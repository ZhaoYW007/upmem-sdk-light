#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
import sys
import os

def plot_benchmark_results(csv_file):
    # Read CSV data
    df = pd.read_csv(csv_file)

    # Average results across runs
    df_avg = df.groupby('test_buffer_kb').agg({
        'send_bw_gbps': 'mean',
        'recv_bw_gbps': 'mean',
        'send_lat_s': 'mean',
        'recv_lat_s': 'mean'
    }).reset_index()

    # Create figure with 2 subplots
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))

    # Plot 1: Bandwidth vs Buffer Size
    ax1 = axes[0]
    ax1.plot(df_avg['test_buffer_kb'], df_avg['send_bw_gbps'], 'b-o', label='Send (Host→PIM)', linewidth=2, markersize=8)
    ax1.plot(df_avg['test_buffer_kb'], df_avg['recv_bw_gbps'], 'r-s', label='Recv (PIM→Host)', linewidth=2, markersize=8)
    ax1.set_xscale('log', base=2)
    ax1.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax1.set_ylabel('Bandwidth (GB/s)', fontsize=12)
    ax1.set_title('UPMEM PIM Bandwidth vs Buffer Size', fontsize=14)
    ax1.legend(fontsize=10)
    ax1.grid(True, alpha=0.3)
    ax1.set_xticks(df_avg['test_buffer_kb'])
    ax1.set_xticklabels([f'{x}' for x in df_avg['test_buffer_kb']])

    # Plot 2: Latency vs Buffer Size
    ax2 = axes[1]
    ax2.plot(df_avg['test_buffer_kb'], df_avg['send_lat_s'] * 1000, 'b-o', label='Send (Host→PIM)', linewidth=2, markersize=8)
    ax2.plot(df_avg['test_buffer_kb'], df_avg['recv_lat_s'] * 1000, 'r-s', label='Recv (PIM→Host)', linewidth=2, markersize=8)
    ax2.set_xscale('log', base=2)
    ax2.set_yscale('log')
    ax2.set_xlabel('Buffer Size per DPU (KB)', fontsize=12)
    ax2.set_ylabel('Latency (ms)', fontsize=12)
    ax2.set_title('UPMEM PIM Latency vs Buffer Size', fontsize=14)
    ax2.legend(fontsize=10)
    ax2.grid(True, alpha=0.3)
    ax2.set_xticks(df_avg['test_buffer_kb'])
    ax2.set_xticklabels([f'{x}' for x in df_avg['test_buffer_kb']])

    plt.tight_layout()

    # Save figure
    output_file = csv_file.replace('.csv', '.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f'Plot saved to {output_file}')

    # Also save as PDF for high quality
    pdf_file = csv_file.replace('.csv', '.pdf')
    plt.savefig(pdf_file, bbox_inches='tight')
    print(f'Plot saved to {pdf_file}')

    plt.show()

if __name__ == '__main__':
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]
    else:
        csv_file = 'benchmark_results.csv'

    if not os.path.exists(csv_file):
        print(f'Error: {csv_file} not found')
        print('Usage: python plot_results.py [csv_file]')
        sys.exit(1)

    plot_benchmark_results(csv_file)
