# UPMEM SDK Light

A lightweight, high-performance interface for UPMEM Processing-In-Memory (PIM) hardware. This library provides a direct memory access interface that bypasses the standard UPMEM API overhead, achieving significantly higher bandwidth for Host-PIM data transfers.

## Features

- **Direct Interface**: High-performance direct memory-mapped access to DPU MRAM, bypassing UPMEM API overhead
- **UPMEM Interface**: Standard UPMEM API wrapper for compatibility and WRAM access
- **Unified API**: Common interface for both direct and UPMEM backends
- **Parallel Transfers**: Utilizes [ParlayLib](https://github.com/cmuparlay/parlaylib) for parallel data preparation and transfers
- **Benchmark Suite**: Built-in tools for measuring Host-PIM bandwidth and latency

## Performance

The direct interface provides higher bandwidth compared to the standard UPMEM API, especially for MRAM transfers:

| Interface | Send (Host->PIM) | Recv (PIM->Host) |
|-----------|------------------|------------------|
| Direct    | ~16 GB/s         | ~7 GB/s          |
| UPMEM     | ~14 GB/s         | ~6 GB/s          |

*Results measured on 40 ranks (2560 DPUs) with varying buffer sizes.*

## Prerequisites

1. **UPMEM SDK 2025.1.0** - The latest UPMEM SDK must be installed and configured
   ```bash
   # Verify installation
   dpu-diag
   ```

2. **Build Tools**
   - CMake >= 3.13
   - GCC with C++17 support
   - pthread library

3. **Python Dependencies** (for plotting)
   ```bash
   pip3 install pandas matplotlib numpy scipy
   ```

## Installation

```bash
# Clone the repository with submodules
git clone --recursive https://github.com/your-repo/upmem-sdk-light.git
cd upmem-sdk-light

# Build
mkdir build && cd build
UPMEM_HOME=/usr/share/upmem cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

## Usage

### Quick Start

```bash
# Run the example
./build/example

# Run benchmark (40 ranks, direct interface)
./build/benchmark 40 direct

# Run benchmark (40 ranks, UPMEM interface)
./build/benchmark 40 UPMEM
```

### Automated Benchmark

```bash
# Run full benchmark suite with both interfaces and generate plots
./run.sh
# Enter the number of ranks when prompted
```

Results are saved to:
- `results/` - CSV files with raw benchmark data
- `plot/` - PNG and PDF plots with bandwidth/latency comparisons

### API Usage

```cpp
#include "pim_interface_header.hpp"

// Create interface (allocates DPUs and loads binary)
// Options: DirectPIMInterface or UPMEMInterface
PIMInterface* pim = new DirectPIMInterface(nr_ranks, "dpu_binary");

// Allocate buffers (one per DPU)
uint8_t** buffers = new uint8_t*[pim->GetNrOfDPUs()];
for (int i = 0; i < pim->GetNrOfDPUs(); i++) {
    buffers[i] = (uint8_t*)aligned_alloc(64, buffer_size);
}

// Send data to PIM (Host -> DPU MRAM)
pim->SendToPIM(buffers, buffer_offset,
               DPU_MRAM_HEAP_POINTER_NAME, symbol_offset,
               length, async);

// Launch DPU program
pim->Launch(async);

// Receive data from PIM (DPU MRAM -> Host)
pim->ReceiveFromPIM(buffers, buffer_offset,
                    DPU_MRAM_HEAP_POINTER_NAME, symbol_offset,
                    length, async);

// Print DPU logs (with filter)
pim->PrintLog([](int i) { return i % 100 == 0; });

// Cleanup
delete pim;
```

### Interface Comparison

| Feature | DirectPIMInterface | UPMEMInterface |
|---------|-------------------|----------------|
| MRAM Read | Direct memory-mapped | UPMEM API |
| MRAM Write | Direct memory-mapped | UPMEM API |
| WRAM Read | UPMEM API fallback | UPMEM API |
| WRAM Write | Not supported* | UPMEM API |
| Performance | Higher | Standard |

*Use `SendToPIMByUPMEM()` for WRAM writes with DirectPIMInterface.

## Project Structure

```
upmem-sdk-light/
├── src/
│   ├── pim_interface/          # Core interface library
│   │   ├── pim_interface.hpp   # Base interface class
│   │   ├── direct_interface.hpp # Direct memory-mapped interface
│   │   ├── upmem_interface.hpp # Standard UPMEM API wrapper
│   │   └── pim_interface_header.hpp # Unified header
│   ├── benchmark/              # Benchmark suite
│   │   ├── host.cpp            # Host-side benchmark
│   │   ├── dpu.c               # DPU-side program
│   │   └── plot_results.py     # Results visualization
│   ├── examples/               # Usage examples
│   │   ├── host.cpp
│   │   └── dpu.c
│   └── third_party/
│       ├── upmem-sdk/          # UPMEM SDK source (modified)
│       └── parlaylib/          # Parallel algorithms library
├── CMakeLists.txt
├── run.sh                      # Automated benchmark script
└── README.md
```

## Technical Notes

### SDK Modifications

The included `third_party/upmem-sdk` is based on UPMEM SDK 2025.1.0 with one modification:
- In `dpu_region_address_translation.h`, the member variable `void *private;` was renamed to `void *privatedata;` for C++ compatibility.

### Direct Interface Implementation

The direct interface achieves higher performance by:
1. Memory-mapping the DPU region directly
2. Computing MRAM address translations in software
3. Using parallel memory copies with ParlayLib
4. Bypassing the UPMEM API serialization overhead

### Cache Considerations

Buffer allocation uses a stride of `(12800 - 4) KB` per DPU to avoid cache line conflicts when accessing different DPUs' data sequentially.

## License

See LICENSE file for details.

## Acknowledgments

- [UPMEM](https://www.upmem.com/) for the PIM hardware and SDK
- [ParlayLib](https://github.com/cmuparlay/parlaylib) for parallel algorithms
