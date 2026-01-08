#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from scipy import stats
import sys
import os


def linear_fit(x, y):
    """Perform linear regression: y = a + b*x. Returns (slope, intercept, r_squared)."""
    slope, intercept, r_value, p_value, std_err = stats.linregress(x, y)
    return slope, intercept, r_value**2


def estimate_nr_dpus(df_avg):
    """Estimate number of DPUs from bandwidth and latency data.

    From benchmark: bandwidth = size_per_dpu * nrOfDPUs / latency
    So: nrOfDPUs = bandwidth * latency / size_per_dpu

    Units: bandwidth (GB/s), latency (s), size (KB)
    nrOfDPUs = bandwidth (GB/s) * latency (s) * (1024^2 KB/GB) / size (KB)
    """
    # Use send data to estimate (should be same for recv)
    bw = df_avg['send_bw_mean'].values
    lat = df_avg['send_lat_mean'].values
    size_kb = df_avg['test_buffer_kb'].values

    nr_dpus = bw * lat * (1024 ** 2) / size_kb
    return np.median(nr_dpus)  # Use median for robustness


def slope_to_bandwidth(slope, nr_dpus):
    """Convert slope (ms/KB) to total bandwidth (GB/s).

    From: latency (ms) = slope * size_per_dpu (KB)
    And:  latency (s) = size_per_dpu (bytes) * nrOfDPUs / bandwidth (bytes/s)

    slope (ms/KB) = nrOfDPUs * 1000 / bandwidth (GB/s) / (1024^2)
    bandwidth (GB/s) = nrOfDPUs * 1000 / (slope * 1024^2)
    """
    if slope <= 0:
        return float('inf')
    return nr_dpus * 1000 / (slope * 1024 * 1024)


def plot_single_result(csv_file, output_dir):
    """Plot results from a single CSV file."""
    df = pd.read_csv(csv_file)

    # Average results across multiple runs
    df_avg = df.groupby('test_buffer_kb').agg({
        'send_bw_gbps': ['mean', 'std'],
        'recv_bw_gbps': ['mean', 'std'],
        'send_lat_s': ['mean', 'std'],
        'recv_lat_s': ['mean', 'std']
    }).reset_index()
    df_avg.columns = ['test_buffer_kb', 'send_bw_mean', 'send_bw_std',
                      'recv_bw_mean', 'recv_bw_std',
                      'send_lat_mean', 'send_lat_std',
                      'recv_lat_mean', 'recv_lat_std']

    # Create figure with 2 subplots
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))

    basename = os.path.basename(csv_file).replace('.csv', '')

    # Plot 1: Bandwidth vs Buffer Size
    ax1 = axes[0]
    ax1.errorbar(df_avg['test_buffer_kb'], df_avg['send_bw_mean'], yerr=df_avg['send_bw_std'],
                 fmt='b-o', label='Send (Host->PIM)', linewidth=2, markersize=8, capsize=3)
    ax1.errorbar(df_avg['test_buffer_kb'], df_avg['recv_bw_mean'], yerr=df_avg['recv_bw_std'],
                 fmt='r-s', label='Recv (PIM->Host)', linewidth=2, markersize=8, capsize=3)
    ax1.set_xscale('log', base=2)
    ax1.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax1.set_ylabel('Bandwidth (GB/s)', fontsize=12)
    ax1.set_title(f'{basename} - Bandwidth vs Buffer Size', fontsize=14)
    ax1.legend(fontsize=10)
    ax1.grid(True, alpha=0.3)

    # Plot 2: Latency vs Buffer Size with linear fitting
    ax2 = axes[1]

    # Data for fitting (in KB and ms)
    x_kb = df_avg['test_buffer_kb'].values
    send_lat_ms = df_avg['send_lat_mean'].values * 1000
    recv_lat_ms = df_avg['recv_lat_mean'].values * 1000

    ax2.errorbar(x_kb, send_lat_ms, yerr=df_avg['send_lat_std'] * 1000,
                 fmt='b-o', label='Send (Host->PIM)', linewidth=2, markersize=8, capsize=3)
    ax2.errorbar(x_kb, recv_lat_ms, yerr=df_avg['recv_lat_std'] * 1000,
                 fmt='r-s', label='Recv (PIM->Host)', linewidth=2, markersize=8, capsize=3)

    # Estimate number of DPUs from data
    nr_dpus = estimate_nr_dpus(df_avg)

    # Linear fit: latency = base_lat + size / bandwidth
    send_slope, send_intercept, send_r2 = linear_fit(x_kb, send_lat_ms)
    recv_slope, recv_intercept, recv_r2 = linear_fit(x_kb, recv_lat_ms)

    # Plot fitted lines
    x_fit = np.linspace(x_kb.min(), x_kb.max(), 100)
    ax2.plot(x_fit, send_intercept + send_slope * x_fit, 'b--', alpha=0.7, linewidth=1.5)
    ax2.plot(x_fit, recv_intercept + recv_slope * x_fit, 'r--', alpha=0.7, linewidth=1.5)

    # Convert slope to total bandwidth
    send_bw = slope_to_bandwidth(send_slope, nr_dpus)
    recv_bw = slope_to_bandwidth(recv_slope, nr_dpus)

    textstr = (f'nrDPUs={int(nr_dpus)}\n'
               f'Send: lat = {send_intercept:.3f} + {send_slope:.6f}*size (ms)\n'
               f'       BW={send_bw:.2f} GB/s, R²={send_r2:.4f}\n'
               f'Recv: lat = {recv_intercept:.3f} + {recv_slope:.6f}*size (ms)\n'
               f'       BW={recv_bw:.2f} GB/s, R²={recv_r2:.4f}')
    ax2.text(0.02, 0.98, textstr, transform=ax2.transAxes, fontsize=9,
             verticalalignment='top', fontfamily='monospace',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))

    ax2.set_xscale('log', base=2)
    ax2.set_yscale('log')
    ax2.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax2.set_ylabel('Latency (ms)', fontsize=12)
    ax2.set_title(f'{basename} - Latency vs Buffer Size', fontsize=14)
    ax2.legend(fontsize=10, loc='lower right')
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()

    # Save figure
    output_file = os.path.join(output_dir, f'{basename}.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f'Plot saved to {output_file}')
    plt.close()

    return df_avg


def plot_comparison(direct_csv, upmem_csv, output_dir):
    """Plot comparison between direct and UPMEM interfaces."""

    # Read and process both datasets
    def process_csv(csv_file):
        df = pd.read_csv(csv_file)
        df_avg = df.groupby('test_buffer_kb').agg({
            'send_bw_gbps': ['mean', 'std'],
            'recv_bw_gbps': ['mean', 'std'],
            'send_lat_s': ['mean', 'std'],
            'recv_lat_s': ['mean', 'std']
        }).reset_index()
        df_avg.columns = ['test_buffer_kb', 'send_bw_mean', 'send_bw_std',
                          'recv_bw_mean', 'recv_bw_std',
                          'send_lat_mean', 'send_lat_std',
                          'recv_lat_mean', 'recv_lat_std']
        return df_avg

    direct_avg = process_csv(direct_csv)
    upmem_avg = process_csv(upmem_csv)

    # Create comparison figure with 2x2 subplots
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Plot 1: Send Bandwidth Comparison
    ax1 = axes[0, 0]
    ax1.errorbar(direct_avg['test_buffer_kb'], direct_avg['send_bw_mean'], yerr=direct_avg['send_bw_std'],
                 fmt='b-o', label='Direct', linewidth=2, markersize=6, capsize=3)
    ax1.errorbar(upmem_avg['test_buffer_kb'], upmem_avg['send_bw_mean'], yerr=upmem_avg['send_bw_std'],
                 fmt='r-s', label='UPMEM', linewidth=2, markersize=6, capsize=3)
    ax1.set_xscale('log', base=2)
    ax1.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax1.set_ylabel('Bandwidth (GB/s)', fontsize=12)
    ax1.set_title('Send Bandwidth (Host->PIM)', fontsize=14)
    ax1.legend(fontsize=10)
    ax1.grid(True, alpha=0.3)

    # Plot 2: Recv Bandwidth Comparison
    ax2 = axes[0, 1]
    ax2.errorbar(direct_avg['test_buffer_kb'], direct_avg['recv_bw_mean'], yerr=direct_avg['recv_bw_std'],
                 fmt='b-o', label='Direct', linewidth=2, markersize=6, capsize=3)
    ax2.errorbar(upmem_avg['test_buffer_kb'], upmem_avg['recv_bw_mean'], yerr=upmem_avg['recv_bw_std'],
                 fmt='r-s', label='UPMEM', linewidth=2, markersize=6, capsize=3)
    ax2.set_xscale('log', base=2)
    ax2.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax2.set_ylabel('Bandwidth (GB/s)', fontsize=12)
    ax2.set_title('Recv Bandwidth (PIM->Host)', fontsize=14)
    ax2.legend(fontsize=10)
    ax2.grid(True, alpha=0.3)

    # Plot 3: Send Latency Comparison with fitting
    ax3 = axes[1, 0]
    x_direct = direct_avg['test_buffer_kb'].values
    x_upmem = upmem_avg['test_buffer_kb'].values

    direct_send_lat = direct_avg['send_lat_mean'].values * 1000
    upmem_send_lat = upmem_avg['send_lat_mean'].values * 1000

    ax3.errorbar(x_direct, direct_send_lat, yerr=direct_avg['send_lat_std'] * 1000,
                 fmt='b-o', label='Direct', linewidth=2, markersize=6, capsize=3)
    ax3.errorbar(x_upmem, upmem_send_lat, yerr=upmem_avg['send_lat_std'] * 1000,
                 fmt='r-s', label='UPMEM', linewidth=2, markersize=6, capsize=3)

    # Estimate nrDPUs from each dataset
    nr_dpus_direct = estimate_nr_dpus(direct_avg)
    nr_dpus_upmem = estimate_nr_dpus(upmem_avg)

    # Fit and plot
    d_slope, d_intercept, d_r2 = linear_fit(x_direct, direct_send_lat)
    u_slope, u_intercept, u_r2 = linear_fit(x_upmem, upmem_send_lat)

    x_fit = np.linspace(min(x_direct.min(), x_upmem.min()), max(x_direct.max(), x_upmem.max()), 100)
    ax3.plot(x_fit, d_intercept + d_slope * x_fit, 'b--', alpha=0.7, linewidth=1.5)
    ax3.plot(x_fit, u_intercept + u_slope * x_fit, 'r--', alpha=0.7, linewidth=1.5)

    d_bw = slope_to_bandwidth(d_slope, nr_dpus_direct)
    u_bw = slope_to_bandwidth(u_slope, nr_dpus_upmem)

    textstr = (f'Direct: {d_intercept:.2f}+{d_slope:.5f}*x, BW={d_bw:.1f}GB/s\n'
               f'UPMEM: {u_intercept:.2f}+{u_slope:.5f}*x, BW={u_bw:.1f}GB/s')
    ax3.text(0.02, 0.98, textstr, transform=ax3.transAxes, fontsize=8,
             verticalalignment='top', fontfamily='monospace',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))

    ax3.set_xscale('log', base=2)
    ax3.set_yscale('log')
    ax3.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax3.set_ylabel('Latency (ms)', fontsize=12)
    ax3.set_title('Send Latency (Host->PIM)', fontsize=14)
    ax3.legend(fontsize=10, loc='lower right')
    ax3.grid(True, alpha=0.3)

    # Plot 4: Recv Latency Comparison with fitting
    ax4 = axes[1, 1]
    direct_recv_lat = direct_avg['recv_lat_mean'].values * 1000
    upmem_recv_lat = upmem_avg['recv_lat_mean'].values * 1000

    ax4.errorbar(x_direct, direct_recv_lat, yerr=direct_avg['recv_lat_std'] * 1000,
                 fmt='b-o', label='Direct', linewidth=2, markersize=6, capsize=3)
    ax4.errorbar(x_upmem, upmem_recv_lat, yerr=upmem_avg['recv_lat_std'] * 1000,
                 fmt='r-s', label='UPMEM', linewidth=2, markersize=6, capsize=3)

    # Fit and plot
    d_slope, d_intercept, d_r2 = linear_fit(x_direct, direct_recv_lat)
    u_slope, u_intercept, u_r2 = linear_fit(x_upmem, upmem_recv_lat)

    ax4.plot(x_fit, d_intercept + d_slope * x_fit, 'b--', alpha=0.7, linewidth=1.5)
    ax4.plot(x_fit, u_intercept + u_slope * x_fit, 'r--', alpha=0.7, linewidth=1.5)

    d_bw = slope_to_bandwidth(d_slope, nr_dpus_direct)
    u_bw = slope_to_bandwidth(u_slope, nr_dpus_upmem)

    textstr = (f'Direct: {d_intercept:.2f}+{d_slope:.5f}*x, BW={d_bw:.1f}GB/s\n'
               f'UPMEM: {u_intercept:.2f}+{u_slope:.5f}*x, BW={u_bw:.1f}GB/s')
    ax4.text(0.02, 0.98, textstr, transform=ax4.transAxes, fontsize=8,
             verticalalignment='top', fontfamily='monospace',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))

    ax4.set_xscale('log', base=2)
    ax4.set_yscale('log')
    ax4.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax4.set_ylabel('Latency (ms)', fontsize=12)
    ax4.set_title('Recv Latency (PIM->Host)', fontsize=14)
    ax4.legend(fontsize=10, loc='lower right')
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()

    # Save comparison figure
    output_file = os.path.join(output_dir, 'comparison.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f'Comparison plot saved to {output_file}')

    pdf_file = os.path.join(output_dir, 'comparison.pdf')
    plt.savefig(pdf_file, bbox_inches='tight')
    print(f'Comparison plot saved to {pdf_file}')

    plt.close()


def main():
    if len(sys.argv) == 4:
        # Comparison mode: direct_csv upmem_csv output_dir
        direct_csv = sys.argv[1]
        upmem_csv = sys.argv[2]
        output_dir = sys.argv[3]

        os.makedirs(output_dir, exist_ok=True)

        if not os.path.exists(direct_csv):
            print(f'Error: {direct_csv} not found')
            sys.exit(1)
        if not os.path.exists(upmem_csv):
            print(f'Error: {upmem_csv} not found')
            sys.exit(1)

        # Plot individual results
        plot_single_result(direct_csv, output_dir)
        plot_single_result(upmem_csv, output_dir)

        # Plot comparison
        plot_comparison(direct_csv, upmem_csv, output_dir)

    elif len(sys.argv) == 2:
        # Single file mode
        csv_file = sys.argv[1]
        if not os.path.exists(csv_file):
            print(f'Error: {csv_file} not found')
            sys.exit(1)
        output_dir = os.path.dirname(csv_file) or '.'
        plot_single_result(csv_file, output_dir)

    else:
        print('Usage:')
        print('  Single file:  python plot_results.py <csv_file>')
        print('  Comparison:   python plot_results.py <direct_csv> <upmem_csv> <output_dir>')
        sys.exit(1)


if __name__ == '__main__':
    main()
