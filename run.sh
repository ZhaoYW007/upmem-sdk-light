#!/bin/bash

set -e

# Check SDK version
echo "Checking UPMEM SDK version..."
SDK_VERSION=$(dpu-diag 2>/dev/null | grep -oP 'SDK version:\s*\K[\d.]+' || echo "unknown")
REQUIRED_VERSION="2025.1.0"

if [ "$SDK_VERSION" != "$REQUIRED_VERSION" ]; then
    echo "Error: SDK version mismatch!"
    echo "  Required: $REQUIRED_VERSION"
    echo "  Found:    $SDK_VERSION"
    echo "Please update the UPMEM SDK."
    exit 1
fi
echo "SDK version: $SDK_VERSION (OK)"

# Ask for number of ranks
read -p "Enter the number of RANKS to test: " NR_RANKS

if ! [[ "$NR_RANKS" =~ ^[0-9]+$ ]]; then
    echo "Error: Please enter a valid number"
    exit 1
fi

echo "Testing with $NR_RANKS ranks..."

# Create directories
mkdir -p results
mkdir -p plot

# Build (Release mode)
rm -rf ./build
mkdir build
cd build
UPMEM_HOME=/usr/share/upmem cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Test direct interface
echo "=== Testing direct interface ==="
echo "Command: ./benchmark $NR_RANKS direct"
numactl --interleave=all ./benchmark $NR_RANKS direct
mv benchmark_results.csv ../results/direct_results.csv
echo "Direct interface test done"

# Test UPMEM interface
echo "=== Testing UPMEM interface ==="
echo "Command: ./benchmark $NR_RANKS UPMEM"
numactl --interleave=all ./benchmark $NR_RANKS UPMEM
mv benchmark_results.csv ../results/upmem_results.csv
echo "UPMEM interface test done"

# Generate plots
cd ..
echo "=== Generating plots ==="
python3 src/benchmark/plot_results.py results/direct_results.csv results/upmem_results.csv plot/

echo "=== All done ==="
echo "Results saved to: results/"
echo "Plots saved to: plot/"
