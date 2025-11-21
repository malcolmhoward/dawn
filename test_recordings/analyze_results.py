#!/usr/bin/env python3
"""
Analyze and visualize DAWN ASR benchmark results
Compares GPU vs CPU performance across Whisper models
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path


def load_benchmark(csv_file):
    """Load benchmark results from CSV file"""
    engines = defaultdict(list)

    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['success'] == '1':
                engine_key = row['engine']
                if engine_key == 'Whisper':
                    model_path = row['model']
                    if 'tiny' in model_path:
                        engine_key = 'Whisper tiny'
                    elif 'base' in model_path:
                        engine_key = 'Whisper base'
                    elif 'small' in model_path:
                        engine_key = 'Whisper small'

                engines[engine_key].append({
                    'rtf': float(row['rtf']),
                    'load_time_ms': float(row['load_time_ms']),
                    'trans_time_ms': float(row['transcription_time_ms']),
                    'duration_sec': float(row['duration_sec']),
                    'transcription': row['transcription']
                })

    return engines


def print_statistics(results, title):
    """Print statistical summary of benchmark results"""
    print(f"\n{'='*80}")
    print(f"{title}")
    print(f"{'='*80}")

    for engine in sorted(results.keys()):
        data = results[engine]
        rtfs = [d['rtf'] for d in data]

        avg_rtf = sum(rtfs) / len(rtfs)
        min_rtf = min(rtfs)
        max_rtf = max(rtfs)
        speedup = 1.0 / avg_rtf if avg_rtf > 0 else 0

        status = "✅" if avg_rtf < 1.0 else "⚠️"

        print(f"\n{engine}")
        print(f"  Samples:      {len(data)}")
        print(f"  Avg RTF:      {avg_rtf:.3f} {status} ({speedup:.1f}x realtime)")
        print(f"  Min RTF:      {min_rtf:.3f}")
        print(f"  Max RTF:      {max_rtf:.3f}")


def compare_results(gpu_results, cpu_results):
    """Compare GPU vs CPU performance"""
    print(f"\n{'='*80}")
    print("GPU vs CPU PERFORMANCE COMPARISON")
    print(f"{'='*80}\n")

    print(f"{'Model':<20} {'CPU RTF':>10} {'GPU RTF':>10} {'Speedup':>10} {'Improvement':>12}")
    print(f"{'-'*20} {'-'*10} {'-'*10} {'-'*10} {'-'*12}")

    for engine in sorted(set(gpu_results.keys()) & set(cpu_results.keys())):
        gpu_rtf = sum(d['rtf'] for d in gpu_results[engine]) / len(gpu_results[engine])
        cpu_rtf = sum(d['rtf'] for d in cpu_results[engine]) / len(cpu_results[engine])

        speedup = cpu_rtf / gpu_rtf
        improvement = ((cpu_rtf - gpu_rtf) / cpu_rtf) * 100

        print(f"{engine:<20} {cpu_rtf:>10.3f} {gpu_rtf:>10.3f} {speedup:>9.2f}x {improvement:>11.1f}%")


def main():
    """Main analysis function"""
    results_dir = Path(__file__).parent / "results"
    cpu_baseline_dir = Path(__file__).parent / "results_cpu_baseline_20251121"

    gpu_csv = results_dir / "complete_benchmark.csv"
    cpu_csv = cpu_baseline_dir / "complete_benchmark.csv"

    if not gpu_csv.exists():
        print(f"Error: GPU results not found at {gpu_csv}")
        sys.exit(1)

    print("DAWN ASR Benchmark Analysis")
    print("="*80)
    print(f"GPU results: {gpu_csv}")
    print(f"CPU results: {cpu_csv}")

    # Load results
    gpu_results = load_benchmark(gpu_csv)
    print_statistics(gpu_results, "GPU-ACCELERATED RESULTS")

    if cpu_csv.exists():
        cpu_results = load_benchmark(cpu_csv)
        print_statistics(cpu_results, "CPU BASELINE RESULTS")
        compare_results(gpu_results, cpu_results)
    else:
        print(f"\nNote: CPU baseline not found at {cpu_csv}")

    print("\n" + "="*80)
    print("Analysis complete!")
    print("="*80 + "\n")


if __name__ == "__main__":
    main()
