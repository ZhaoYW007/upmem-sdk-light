#include <parlay/parallel.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <vector>

#include "common.h"
#include "pim_interface_header.hpp"
#include "timer.hpp"

extern "C" {
#include <dpu_bank_interface_pmc.h>
}

using namespace std;

// Global CSV file stream
ofstream csv_file;

// Measure host row activations during a transfer using Bank Interface PMC.
// Configures one DPU (DPU 0) to count HOST_ACTIVATE_COMMAND and CYCLES.
void TestRowActivations(PIMInterface *interface, dpu_set_t dpu_set,
                        const vector<size_t> &testSizes) {
    int nrOfDPUs = interface->GetNrOfDPUs();

    size_t maxSize = *max_element(testSizes.begin(), testSizes.end());
    size_t stridePerDPU = (12800 - 4) << 10;
    uint8_t *buffer = (uint8_t *)aligned_alloc(1l << 21, stridePerDPU * nrOfDPUs);
    uint8_t **dpuBuffer = new uint8_t *[nrOfDPUs];
    for (int i = 0; i < nrOfDPUs; i++)
        dpuBuffer[i] = buffer + i * stridePerDPU;

    // Get a handle to DPU 0 for PMC
    dpu_set_t dpu0;
    uint32_t each_dpu;
    DPU_FOREACH(dpu_set, dpu0, each_dpu) {
        if (each_dpu == 0) break;
    }

    printf("=== Row Activation Count (Bank Interface PMC on DPU 0) ===\n");
    printf("%12s %16s %16s %16s %12s\n",
           "BufferKB", "Activations", "Cycles",
           "ExpectedCLs", "Act/CL ratio");

    for (size_t bufferSize : testSizes) {
        // Fill buffer
        parlay::parallel_for(0, nrOfDPUs, [&](size_t i) {
            memset(dpuBuffer[i], 0x42, bufferSize);
        });

        // Configure PMC: counter1 = HOST_ACTIVATE_COMMAND, counter2 = CYCLES
        bank_interface_pmc_config_t config;
        config.mode = BANK_INTERFACE_PMC_32BIT_MODE;
        config.counter_1 = BANK_INTERFACE_PMC_HOST_ACTIVATE_COMMAND;
        config.counter_2 = BANK_INTERFACE_PMC_CYCLES;
        dpu_error_t err = dpu_bank_interface_pmc_enable(dpu0, config);
        if (err != DPU_OK) {
            printf("Bank Interface PMC not available on this hardware (v1A DPUs lack this feature).\n");
            break;
        }

        // Do the transfer
        interface->SendToPIM(dpuBuffer, 0, DPU_MRAM_HEAP_POINTER_NAME, 0,
                             bufferSize, false);

        // Stop and read counters
        DPU_ASSERT(dpu_bank_interface_pmc_stop_counters(dpu0));
        bank_interface_pmc_result_t result;
        DPU_ASSERT(dpu_bank_interface_pmc_read_counters(dpu0, &result));
        DPU_ASSERT(dpu_bank_interface_pmc_disable(dpu0));

        uint32_t activations = result.two_32bits.counter_1;
        uint32_t cycles = result.two_32bits.counter_2;
        // Expected cache lines for this DPU: bufferSize / 8 bytes per DPU per CL
        size_t expected_cls = bufferSize / 8;
        double ratio = expected_cls > 0 ? (double)activations / expected_cls : 0;

        printf("%12zu %16u %16u %16zu %12.4f\n",
               bufferSize / 1024, activations, cycles, expected_cls, ratio);
    }

    delete[] dpuBuffer;
    free(buffer);
}

enum CommunicationDirection { Host2PIM = 0, PIM2Host = 1 };

void parse_arguments(int argc, char **argv, int &nr_ranks,
                     string &interfaceType, string &sendMode, string &recvMode) {
    if (argc < 3) {
        fprintf(
            stderr,
            "Usage: %s <nr_ranks> <Interface Type> [Send Mode] [Recv Mode]\n"
            "  Interface Types: direct, UPMEM, broadcast\n"
            "  Send/Recv Modes (direct only): original (default), roundrobin, sequential\n",
            argv[0]);
        exit(1);
    }

    sscanf(argv[1], "%d", &nr_ranks);
    interfaceType = argv[2];
    sendMode = (argc >= 4) ? argv[3] : "original";
    recvMode = (argc >= 5) ? argv[4] : "original";

    if (interfaceType != "direct" && interfaceType != "UPMEM" &&
        interfaceType != "broadcast") {
        fprintf(stderr,
                "Invalid interface type. Please enter 'direct', 'UPMEM', "
                "or 'broadcast'.\n");
        exit(1);
    }

    auto checkMode = [](const string &m, const char *which) {
        if (m != "original" && m != "roundrobin" && m != "sequential") {
            fprintf(stderr,
                    "Invalid %s mode. Please enter 'original', 'roundrobin', "
                    "or 'sequential'.\n", which);
            exit(1);
        }
    };
    checkMode(sendMode, "send");
    checkMode(recvMode, "recv");
}

void WriteCSVHeader() {
    csv_file << "total_buffer_kb,test_buffer_kb,repeat,send_time_s,recv_time_s,"
             << "total_time_s,send_bw_gbps,recv_bw_gbps,send_lat_s,recv_lat_s" << endl;
}

void TestBroadcastThroughput(PIMInterface *interface,
                             const vector<size_t> &testSizes) {
    const double timeLimitPerTest = 2.0;
    const double earlyStopTimeLimitPerTest = 1.0;
    const size_t repeatLimitPerTest = 500;

    int nrOfDPUs = interface->GetNrOfDPUs();

    size_t maxBufferSize = *max_element(testSizes.begin(), testSizes.end());
    uint8_t *broadcastBuffer = (uint8_t *)aligned_alloc(1l << 21, maxBufferSize);

    internal_timer send_timer, total_timer;
    for (size_t bufferSize : testSizes) {
        send_timer.reset();
        total_timer.reset();
        total_timer.start();
        assert(bufferSize % 8 == 0);

        for (size_t repeat = 0; true; repeat++) {
            // Fill broadcast buffer with deterministic data
            parlay::parallel_for(0, bufferSize / 8, [&](size_t j) {
                uint64_t *ptr = (uint64_t *)(broadcastBuffer + j * 8);
                *ptr = parlay::hash64((j << 20) | repeat);
            });

            send_timer.start();
            interface->BroadcastToPIM(broadcastBuffer, DPU_MRAM_HEAP_POINTER_NAME,
                                      0, bufferSize, false);
            send_timer.end();

            if (send_timer.total_time >= timeLimitPerTest ||
                (send_timer.total_time >= earlyStopTimeLimitPerTest &&
                 repeat >= repeatLimitPerTest)) {
                total_timer.end();

                double bandwidth = (double)bufferSize * repeat *
                                   nrOfDPUs / send_timer.total_time;
                double bw_gbps = bandwidth / 1024.0 / 1024.0 / 1024.0;
                double latency = send_timer.total_time / repeat;

                printf(
                    "[Broadcast] Buffer Size: %5lu KB, Repeat: %6lu, "
                    "Time: %8.3lf s, Total Time: %8.3lf s, "
                    "BW: %8.3lf GB/s, Lat: %5g s\n",
                    bufferSize / 1024, repeat, send_timer.total_time,
                    total_timer.total_time, bw_gbps, latency);
                fflush(stdout);
                break;
            }
        }
    }

    free(broadcastBuffer);
}

void TestMRAMThroughput(PIMInterface *interface,
                        const vector<size_t> &testSizes) {
    const double timeLimitPerTest = 2.0;  // 2 seconds
    const double earlyStopTimeLimitPerTest = 1.0;
    const size_t repeatLimitPerTest = 500;

    int nrOfDPUs = interface->GetNrOfDPUs();

    // Find max size and allocate buffer accordingly
    // Add 4KB offset between buffers to avoid cache conflicts
    const size_t CACHE_OFFSET = 4096;
    size_t maxBufferSizePerDPU = *max_element(testSizes.begin(), testSizes.end());
    size_t stridePerDPU = (12800 - 4) << 10;
    assert(maxBufferSizePerDPU <= stridePerDPU);

    size_t buffer_size = stridePerDPU * nrOfDPUs;
    uint8_t *buffer = (uint8_t *)aligned_alloc(1l << 21, buffer_size);

    uint8_t **dpuBuffer = new uint8_t *[nrOfDPUs];

    for (int i = 0; i < nrOfDPUs; i++) {
        dpuBuffer[i] = buffer + i * stridePerDPU;
    }

    auto get_value = [&](size_t i, size_t j, uint64_t repeat) -> uint64_t {
        assert(i < (1 << 12));
        assert(j < (1 << 30));
        assert(repeat < (1 << 20));
        uint64_t combine = (i << 50) | (j << 20) | repeat;
        uint64_t val = parlay::hash64(combine);
        return val;
    };

    internal_timer send_timer, recv_timer, total_timer;
    for (size_t bufferSizePerDPU : testSizes) {
        send_timer.reset();
        recv_timer.reset();
        total_timer.reset();
        total_timer.start();
        assert(bufferSizePerDPU % 4 == 0);

        for (size_t repeat = 0; true; repeat++) {
            parlay::parallel_for(0, nrOfDPUs, [&](size_t i) {
                parlay::parallel_for(0, bufferSizePerDPU / 8, [&](size_t j) {
                    uint64_t* ptr = (uint64_t*)(dpuBuffer[i] + j * 8);
                    *ptr = get_value(i, j, repeat);
                    // dpuBuffer[i][j] = get_value(i, j, repeat);
                });
            });

            send_timer.start();
            // CPU -> PIM.MRAM : Supported by both direct and UPMEM interface.
            interface->SendToPIM(dpuBuffer, 0, DPU_MRAM_HEAP_POINTER_NAME, 0,
                                 bufferSizePerDPU, false);
            
            send_timer.end();

            interface->Launch(false);
            parlay::parallel_for(0, nrOfDPUs, [&](size_t i) {
                parlay::parallel_for(0, bufferSizePerDPU / 8, [&](size_t j) {
                    uint64_t* ptr = (uint64_t*)(dpuBuffer[i] + j * 8);
                    *ptr = get_value(i, j, repeat + 1);
                });
            });

            recv_timer.start();
            // PIM.MRAM -> CPU : Supported by both direct and UPMEM interface.
            interface->ReceiveFromPIM(dpuBuffer, 0, DPU_MRAM_HEAP_POINTER_NAME, 0,
                                      bufferSizePerDPU, false);
            recv_timer.end();

            parlay::parallel_for(0, nrOfDPUs, [&](size_t i) {
                parlay::parallel_for(0, bufferSizePerDPU / 8, [&](size_t j) {
                    uint64_t* ptr = (uint64_t*)(dpuBuffer[i] + j * 8);
                    if (j % 256 == 0) {
                        assert(*ptr == get_value(i, j, repeat) + ((i << 48) + j));
                    } else {
                        assert(*ptr == get_value(i, j, repeat));
                    }

                });
            });

            if ((send_timer.total_time >= timeLimitPerTest &&
                 recv_timer.total_time >= timeLimitPerTest) ||
                (send_timer.total_time >= earlyStopTimeLimitPerTest &&
                 recv_timer.total_time >= earlyStopTimeLimitPerTest &&
                 repeat >= repeatLimitPerTest)) {
                total_timer.end();

                double send_bandwidth = (double)bufferSizePerDPU * repeat *
                                        nrOfDPUs / send_timer.total_time;
                double receive_bandwidth = (double)bufferSizePerDPU * repeat *
                                           nrOfDPUs / recv_timer.total_time;

                double send_bw_gbps = send_bandwidth / 1024.0 / 1024.0 / 1024.0;
                double recv_bw_gbps = receive_bandwidth / 1024.0 / 1024.0 / 1024.0;
                double send_lat = send_timer.total_time / repeat;
                double recv_lat = recv_timer.total_time / repeat;

                // Console output
                printf(
                    "Total Buffer size: %5lu KB, Test Buffer "
                    "Size: %5lu KB, Repeat: %6lu, Send Time: %8.3lf s, Recv "
                    "Time: %8.3lf s, Total Time: %8.3lf s, "
                    "Send BW: %8.3lf GB/s, Recv BW: %8.3lf GB/s, Send Lat: %5g s, Recv Lat: %5g s\n",
                    maxBufferSizePerDPU / 1024,
                    bufferSizePerDPU / 1024, repeat, send_timer.total_time,
                    recv_timer.total_time, total_timer.total_time,
                    send_bw_gbps, recv_bw_gbps, send_lat, recv_lat);
                fflush(stdout);

                // CSV output
                csv_file << maxBufferSizePerDPU / 1024 << ","
                         << bufferSizePerDPU / 1024 << ","
                         << repeat << ","
                         << send_timer.total_time << ","
                         << recv_timer.total_time << ","
                         << total_timer.total_time << ","
                         << send_bw_gbps << ","
                         << recv_bw_gbps << ","
                         << send_lat << ","
                         << recv_lat << endl;

                interface->PrintAndResetTimingStats();

                break;
            }
        }
    }

    delete[] dpuBuffer;
    free(buffer);
}

int main(int argc, char **argv) {
    int nr_ranks;
    string interfaceType;
    string sendMode, recvMode;

    parse_arguments(argc, argv, nr_ranks, interfaceType, sendMode, recvMode);

    auto parseMode = [](const string &s) {
        if (s == "roundrobin") return DirectPIMInterface::AccessMode::RoundRobin;
        if (s == "sequential") return DirectPIMInterface::AccessMode::Sequential;
        return DirectPIMInterface::AccessMode::Original;
    };

    // To Allocate: identify the number of RANKS you want, or use
    // DPU_ALLOCATE_ALL to allocate all possible.
    PIMInterface *pimInterface;
    if (interfaceType == "direct") {
        DirectPIMInterface *direct = new DirectPIMInterface(nr_ranks, "dpu_benchmark");
        direct->SetSendMode(parseMode(sendMode));
        direct->SetRecvMode(parseMode(recvMode));
        cout << "SendMode: " << sendMode << ", RecvMode: " << recvMode << endl;
        pimInterface = direct;
    } else {
        // Both UPMEM and broadcast use UPMEMInterface
        pimInterface = new UPMEMInterface(nr_ranks, "dpu_benchmark");
    }

    int nrOfDPUs = pimInterface->GetNrOfDPUs();
    uint8_t **dpuIDs = new uint8_t *[nrOfDPUs];
    for (int i = 0; i < nrOfDPUs; i++) {
        dpuIDs[i] = new uint8_t[16];  // two 64-bit integers
        uint64_t *id = (uint64_t *)dpuIDs[i];
        *id = i;
    }

    // CPU -> PIM.WRAM : Not supported by direct interface. Use UPMEM interface.
    pimInterface->SendToPIMByUPMEM(dpuIDs, 0, "DPU_ID", 0, sizeof(uint64_t),
                                   false);

    // PIM.WRAM -> CPU : Supported by both direct and UPMEM interface.
    pimInterface->ReceiveFromPIM(dpuIDs, 0, "DPU_ID", 0, sizeof(uint64_t), false);
    for (int i = 0; i < nrOfDPUs; i++) {
        uint64_t *id = (uint64_t *)dpuIDs[i];
        assert(*id == (uint64_t)i);
    }

    // Execute : will call the UPMEM interface.
    pimInterface->Launch(false);
    pimInterface->PrintLog([](int i) { return (i % 100) == 0; });

    cout << "PIM is running correctly" << endl;

    // Generate test sizes: 1KB, 2KB, 4KB, ..., 8MB
    vector<size_t> testSizes;
    for (size_t size = 1 << 10; size <= 8 << 20; size <<= 1) {
        testSizes.push_back(size);
    }

    if (interfaceType == "broadcast") {
        // Broadcast benchmark: extend to 32MB (4x of scatter max)
        testSizes.push_back(16 << 20);
        testSizes.push_back(32 << 20);
        cout << "=== Broadcast Benchmark (UPMEM dpu_broadcast_to) ===" << endl;
        TestBroadcastThroughput(pimInterface, testSizes);
    } else {
        // Scatter/gather benchmark: per-DPU data via direct or UPMEM interface
        cout << "=== Scatter/Gather Benchmark (" << interfaceType << ") ===" << endl;
        csv_file.open("benchmark_results.csv");
        WriteCSVHeader();
        TestMRAMThroughput(pimInterface, testSizes);
        csv_file.close();
        cout << "Results saved to benchmark_results.csv" << endl;

        // Row activation measurement
        TestRowActivations(pimInterface, pimInterface->GetDpuSet(), testSizes);
    }

    for (int i = 0; i < nrOfDPUs; i++) {
        delete[] dpuIDs[i];
    }
    delete[] dpuIDs;

    return 0;
}