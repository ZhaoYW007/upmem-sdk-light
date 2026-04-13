#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <parlay/parallel.h>

#include "pim_interface_header.hpp"
using namespace std;

// Must match DPU-side expected_value()
static uint64_t expected_value(uint64_t dpu_id, uint64_t j, uint64_t seed) {
    return (dpu_id << 48) ^ (j * 2654435761ULL) ^ seed;
}

// Test WRAM round-trip: send DPU IDs, read back, verify
void TestWRAM(PIMInterface *pim, int nr_dpus) {
    printf("=== WRAM round-trip test ===\n");
    uint8_t **bufs = new uint8_t *[nr_dpus];
    for (int i = 0; i < nr_dpus; i++) {
        bufs[i] = new uint8_t[16];
        *(uint64_t *)bufs[i] = (uint64_t)i;
    }

    pim->SendToPIMByUPMEM(bufs, 0, "DPU_ID", 0, sizeof(uint64_t), false);

    // Clear and read back
    for (int i = 0; i < nr_dpus; i++)
        *(uint64_t *)bufs[i] = 0xDEAD;

    pim->ReceiveFromPIM(bufs, 0, "DPU_ID", 0, sizeof(uint64_t), false);

    int errors = 0;
    for (int i = 0; i < nr_dpus; i++) {
        uint64_t val = *(uint64_t *)bufs[i];
        if (val != (uint64_t)i) {
            if (errors < 10)
                printf("  WRAM FAIL: DPU %d got %lx expected %lx\n", i,
                       (unsigned long)val, (unsigned long)i);
            errors++;
        }
    }
    printf("  WRAM: %d/%d DPUs correct\n", nr_dpus - errors, nr_dpus);

    for (int i = 0; i < nr_dpus; i++) delete[] bufs[i];
    delete[] bufs;
}

// Test MRAM send -> DPU verify+modify -> receive -> host verify
void TestMRAMWithDPU(PIMInterface *pim, int nr_dpus, size_t buffer_size,
                     uint64_t seed) {
    size_t n_elems = buffer_size / sizeof(uint64_t);
    printf("=== MRAM correctness test: %zu KB, seed=%lu ===\n",
           buffer_size / 1024, (unsigned long)seed);

    // Allocate buffers (2MB aligned for direct interface)
    size_t stride = buffer_size;
    uint8_t *pool =
        (uint8_t *)aligned_alloc(1 << 21, stride * nr_dpus);
    uint8_t **bufs = new uint8_t *[nr_dpus];
    for (int i = 0; i < nr_dpus; i++)
        bufs[i] = pool + i * stride;

    // Fill with deterministic pattern
    parlay::parallel_for(0, nr_dpus, [&](size_t i) {
        uint64_t *p = (uint64_t *)bufs[i];
        for (size_t j = 0; j < n_elems; j++)
            p[j] = expected_value(i, j, seed);
    });

    // Tell DPUs what to expect
    uint8_t **seed_bufs = new uint8_t *[nr_dpus];
    uint8_t **size_bufs = new uint8_t *[nr_dpus];
    for (int i = 0; i < nr_dpus; i++) {
        seed_bufs[i] = new uint8_t[8];
        size_bufs[i] = new uint8_t[8];
        *(uint64_t *)seed_bufs[i] = seed;
        *(uint64_t *)size_bufs[i] = n_elems;
    }
    pim->SendToPIMByUPMEM(seed_bufs, 0, "TEST_SEED", 0, sizeof(uint64_t), false);
    pim->SendToPIMByUPMEM(size_bufs, 0, "TEST_SIZE", 0, sizeof(uint64_t), false);

    // Send data to MRAM
    pim->SendToPIM(bufs, 0, DPU_MRAM_HEAP_POINTER_NAME, 0, buffer_size, false);

    // Launch DPUs — they verify the pattern and modify each element
    pim->Launch(false);
    pim->PrintLog([](int i) { return (i % 100) == 0; });

    // Check DPU-side error counts
    uint8_t **err_bufs = new uint8_t *[nr_dpus];
    for (int i = 0; i < nr_dpus; i++) {
        err_bufs[i] = new uint8_t[8];
        *(uint64_t *)err_bufs[i] = 0;
    }
    pim->ReceiveFromPIM(err_bufs, 0, "ERROR_COUNT", 0, sizeof(uint64_t), false);

    int dpu_errors = 0;
    for (int i = 0; i < nr_dpus; i++) {
        uint64_t e = *(uint64_t *)err_bufs[i];
        if (e > 0) {
            if (dpu_errors < 10)
                printf("  DPU %d reported %lu errors\n", i, (unsigned long)e);
            dpu_errors++;
        }
    }
    printf("  DPU-side verification: %d/%d DPUs error-free\n",
           nr_dpus - dpu_errors, nr_dpus);

    // Clear host buffers and receive DPU-modified data
    parlay::parallel_for(0, nr_dpus, [&](size_t i) {
        memset(bufs[i], 0xFF, buffer_size);
    });

    pim->ReceiveFromPIM(bufs, 0, DPU_MRAM_HEAP_POINTER_NAME, 0, buffer_size, false);

    // Verify host-side: each element should be original + DPU tag
    int host_err_dpus = 0;
    parlay::parallel_for(0, nr_dpus, [&](size_t i) {
        uint64_t *p = (uint64_t *)bufs[i];
        int errs = 0;
        for (size_t j = 0; j < n_elems; j++) {
            uint64_t exp = expected_value(i, j, seed) + ((uint64_t)i << 32) + j;
            if (p[j] != exp) {
                if (errs < 4)
                    printf("  HOST FAIL: DPU %zu elem %zu got %lx expected %lx\n",
                           i, j, (unsigned long)p[j], (unsigned long)exp);
                errs++;
            }
        }
        if (errs > 0)
            __atomic_fetch_add(&host_err_dpus, 1, __ATOMIC_RELAXED);
    });
    printf("  Host-side verification: %d/%d DPUs correct\n",
           nr_dpus - host_err_dpus, nr_dpus);

    // Cleanup
    for (int i = 0; i < nr_dpus; i++) {
        delete[] seed_bufs[i];
        delete[] size_bufs[i];
        delete[] err_bufs[i];
    }
    delete[] seed_bufs;
    delete[] size_bufs;
    delete[] err_bufs;
    delete[] bufs;
    free(pool);
}

int main(int argc, char **argv) {
    int nr_ranks = 10;
    if (argc >= 2) sscanf(argv[1], "%d", &nr_ranks);

    DirectPIMInterface pimInterface(nr_ranks, "dpu_example");
    int nr_dpus = pimInterface.GetNrOfDPUs();

    // Initial launch to print DPU info (TEST_SIZE=0 → just prints)
    pimInterface.Launch(false);
    pimInterface.PrintLog([](int i) { return (i % 100) == 0; });

    // 1. WRAM round-trip
    TestWRAM(&pimInterface, nr_dpus);

    // 2. MRAM correctness at various sizes
    vector<size_t> test_sizes = {
        1 << 10,   // 1 KB
        8 << 10,   // 8 KB
        64 << 10,  // 64 KB
        512 << 10, // 512 KB
        4 << 20,   // 4 MB
    };

    for (size_t sz : test_sizes) {
        TestMRAMWithDPU(&pimInterface, nr_dpus, sz, sz * 42 + 7);
    }

    printf("\n=== All correctness tests completed ===\n");
    return 0;
}
