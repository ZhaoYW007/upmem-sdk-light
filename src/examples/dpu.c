#include <stdio.h>
#include <stdint.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <assert.h>

__host int64_t DPU_ID;

// Host sets this before each Launch to tell DPU what to expect
__host uint64_t TEST_SEED;
__host uint64_t TEST_SIZE;   // number of uint64_t elements
__host uint64_t ERROR_COUNT; // DPU writes back number of errors found

void StandardOutput() {
    printf("HEAP POINTER ADDR: %p\n", DPU_MRAM_HEAP_POINTER);
    printf("DPU ID is %lld!\n", DPU_ID);
    printf("DPU ID is at %p!\n", &DPU_ID);
}

// Expected value for element j of DPU i with seed s
static uint64_t expected_value(uint64_t dpu_id, uint64_t j, uint64_t seed) {
    return (dpu_id << 48) ^ (j * 2654435761ULL) ^ seed;
}

// Verify MRAM contents match expected pattern, then modify each element
// by adding (DPU_ID << 32) | j so host can verify DPU actually processed it
void verify_and_modify() {
    __mram_ptr uint64_t *mram = (__mram_ptr uint64_t *)DPU_MRAM_HEAP_POINTER;
    uint64_t buf[256]; // 2KB WRAM buffer
    uint64_t errors = 0;
    uint64_t n = TEST_SIZE;

    for (uint64_t base = 0; base < n; base += 256) {
        uint64_t chunk = (n - base < 256) ? (n - base) : 256;
        // Round up to 8-byte aligned transfer size
        uint64_t xfer = (chunk * sizeof(uint64_t) + 7) & ~7ULL;
        mram_read(mram + base, buf, xfer);

        for (uint64_t k = 0; k < chunk; k++) {
            uint64_t j = base + k;
            uint64_t exp = expected_value(DPU_ID, j, TEST_SEED);
            if (buf[k] != exp) {
                if (errors < 4) {
                    printf("DPU %lld: MISMATCH at [%llu] got %llx expected %llx\n",
                           DPU_ID, j, (unsigned long long)buf[k],
                           (unsigned long long)exp);
                }
                errors++;
            }
            // Modify: add DPU-specific tag so host can verify DPU touched it
            buf[k] += ((uint64_t)DPU_ID << 32) | j;
        }

        mram_write(buf, mram + base, xfer);
    }

    ERROR_COUNT = errors;
    if (errors == 0 && DPU_ID % 100 == 0) {
        printf("DPU %lld: verified %llu elements OK\n", DPU_ID,
               (unsigned long long)n);
    }
}

int main() {
    if (me() == 0) {
        if (TEST_SIZE == 0) {
            // First launch: just print info
            StandardOutput();
        } else {
            verify_and_modify();
        }
    }
    return 0;
}
