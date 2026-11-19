#!/usr/bin/env python3
"""Regenerate README figures from reviewed experiment JSON (no new timings)."""

import argparse
import hashlib
import json
from pathlib import Path
from statistics import median

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch


ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    'application': '2026-11-16-ring-quantity-ryzen-results.json',
    'receive': '2026-11-12-hardware-rx-results.json',
    'decoder': '2026-11-14-decoder-breakdown-results.json',
}
COLORS = ['#1565a3', '#b45a08', '#7040a0']


def read(name):
    return json.loads((ROOT / 'docs/experiments' / SOURCES[name]).read_text())


def verify_application(data):
    """Recompute all displayed summaries from individual, validated trial rows."""
    assert data['new_valid_trials'] == data['new_completed_trials'] == 54
    assert len(data['runs']) == 72  # 54 new + 18 explicitly reused controls
    assert all(all(row['checks'].values()) for row in data['affinity_and_state_audit'])
    assert all(row['valid'] and row['latency']['samples'] == 1_000_000
               and not any(row[key] for key in ('kernel_drops', 'ring_drops', 'missing_messages'))
               for row in data['runs'])
    for summary in data['summary']:
        runs = [r for r in data['runs'] if all(r[k] == summary[k]
                for k in ('profile', 'build', 'variant'))]
        assert len(runs) == summary['attempts'] == summary['valid_runs'] == 3
        for metric in ('p50_ns', 'p99_ns', 'p99_9_ns'):
            values = [r['latency'][metric] for r in runs]
            assert median(values) == summary[metric]
            assert min(values) == summary['min_' + metric]
            assert max(values) == summary['max_' + metric]


def save(fig, output, name, preview):
    path = output / (name + '.svg')
    fig.savefig(path, metadata={'Date': None, 'Creator': 'Matplotlib'})
    # Matplotlib emits trailing spaces inside multiline SVG path attributes.
    path.write_text('\n'.join(line.rstrip() for line in path.read_text().splitlines()) + '\n')
    if preview:
        preview.mkdir(parents=True, exist_ok=True)
        fig.savefig(preview / (name + '.png'), dpi=130)
    plt.close(fig)


def application_chart(data, output, preview):
    builds = ['gcc-o3', 'clang-o3', 'rust-o3']
    rows = [next(s for s in data['summary'] if s['profile'] == 'S1'
                 and s['variant'] == 'both' and s['build'] == build) for build in builds]
    fig, ax = plt.subplots(figsize=(7.6, 6.8))
    fig.subplots_adjust(left=.22, right=.97, top=.72, bottom=.30)
    fig.text(.04, .96, 'Application latency: C++ and Rust', fontsize=19, weight='bold', va='top')
    fig.text(.04, .89, '16 Nov 2026 · S1 · 503 stocks · 1M measured updates/run\n'
             '8/20 Mbps · 10 ms bursts / 100 ms · combined variant', fontsize=12, linespacing=1.6, va='top')
    for i, (metric, label, color) in enumerate(zip(
            ('p50_ns', 'p99_ns', 'p99_9_ns'), ('p50', 'p99', 'p99.9'), COLORS)):
        ys = [j * 1.25 + (i - 1) * .27 for j in range(3)]
        values = [r[metric] / 1000 for r in rows]
        errors = [[(r[metric] - r['min_' + metric]) / 1000 for r in rows],
                  [(r['max_' + metric] - r[metric]) / 1000 for r in rows]]
        ax.barh(ys, values, height=.22, color=color, label=label,
                xerr=errors, error_kw={'capsize': 3, 'elinewidth': 1, 'ecolor': '#202b35'})
        for y, value, row in zip(ys, values, rows):
            ax.text(row['max_' + metric] / 1000 + .08, y, f'{value:.2f}', va='center', fontsize=12)
    ax.set_yticks([0, 1.25, 2.5], ['GCC C++\n14.2 O3', 'Clang C++\n19.1 O3', 'Rust\n1.99.1 release'])
    ax.invert_yaxis()
    ax.set_xlim(0, 5.15)
    ax.set_xlabel('RX return → L1 decision (µs); lower is faster', labelpad=9)
    ax.legend(ncols=3, loc='lower left', bbox_to_anchor=(0, 1.02), frameon=False)
    ax.grid(axis='x', alpha=.18)
    ax.set_axisbelow(True)
    fig.text(.04, .14, 'Bars: medians of 3 per-run percentiles. Whiskers: run min–max.\n'
             'Ryzen 9 9950X · Linux sockets · RX 2 / book 3 · no CPU isolation.\n'
             'Source: 2026-11-16-ring-quantity-ryzen-results.json (S1, both).',
             fontsize=10.5, linespacing=1.6, va='top')
    save(fig, output, 'application-latency', preview)


def receive_chart(application, receive, output, preview):
    runs = receive['runs']
    assert len(runs) == 3 and all(r['valid'] and r['packets'] == 125024 for r in runs)
    p50 = [r['nic_user_p50_ns'] / 1000 for r in runs]
    p999 = [r['nic_user_p99_9_ns'] / 1_000_000 for r in runs]
    rows = [s for s in application['summary'] if s['profile'] == 'S1' and s['variant'] == 'both']
    app50 = [r['p50_ns'] / 1000 for r in rows]
    app999 = [r['p99_9_ns'] / 1000 for r in rows]
    fig = plt.figure(figsize=(7.6, 6.9))
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set(xlim=(0, 1), ylim=(.23, 1.015))
    ax.axis('off')
    ax.text(.04, .96, 'Two timing boundaries', fontsize=20, weight='bold')
    ax.text(.04, .918, 'Architecture only · stage heights are not elapsed time', fontsize=12)
    stages = [('NIC RX timestamp', .79), ('Driver + kernel', .67),
              ('recvmsg returns', .55), ('Ring → decode → L2', .39), ('L1 decision', .27)]
    for label, y in stages:
        ax.add_patch(FancyBboxPatch((.055, y), .40, .065, boxstyle='round,pad=0.012',
                                   facecolor='#edf3f8', edgecolor='#627789'))
        ax.text(.255, y + .0325, label, ha='center', va='center', fontsize=12)
    for (_, top), (_, bottom) in zip(stages, stages[1:]):
        ax.annotate('', xy=(.255, bottom + .08), xytext=(.255, top - .012),
                    arrowprops={'arrowstyle': '->', 'color': '#627789', 'lw': 1.5})
    ax.annotate('', xy=(.49, .82), xytext=(.49, .58),
                arrowprops={'arrowstyle': '|-|', 'color': COLORS[0], 'lw': 2})
    ax.annotate('', xy=(.49, .58), xytext=(.49, .30),
                arrowprops={'arrowstyle': '|-|', 'color': COLORS[1], 'lw': 2})
    ax.text(.54, .84, 'RX-only probe · 12 Nov 2026', color=COLORS[0], fontsize=12, weight='bold')
    ax.text(.54, .798, f'EPYC 7742 / Intel X550\nP2: 4/20 Mbps, 5/100 ms\n'
            f'3 runs × 125,024 packets\np50: {min(p50):.1f}–{max(p50):.1f} µs\n'
            f'p99.9: {min(p999):.3f}–{max(p999):.3f} ms',
            va='top', fontsize=12, linespacing=1.6)
    ax.text(.54, .53, 'Application · 16 Nov 2026', color=COLORS[1], fontsize=12, weight='bold')
    ax.text(.54, .488, f'Ryzen 9 9950X / r8169\nS1: 8/20 Mbps, 10/100 ms\n'
            f'3 runs/build × 1M updates\np50: {min(app50):.2f}–{max(app50):.2f} µs\n'
            f'p99.9: {min(app999):.2f}–{max(app999):.2f} µs',
            va='top', fontsize=12, linespacing=1.6)
    save(fig, output, 'receive-boundaries', preview)


def decoder_chart(data, output, preview):
    names = ['order-fixtures-1024-input', 'order-fixtures-1024-grouped',
             'order-fixtures-1024-shuffled']
    rows = [next(c for c in data['cases'] if c['name'] == name) for name in names]
    for row in rows:
        averages = row['run_averages']
        old = median(r['ns_per_message'] for r in averages if r['decoder'] == 'original')
        new = median(r['ns_per_message'] for r in averages if r['decoder'] == 'packed')
        assert old == row['original_ns'] and new == row['packed_ns']
        assert abs(100 * (new / old - 1) - row['median_time_change_pct']) < 1e-9
        assert row['pairs'] == 9
    assert len({json.dumps(r['counts'], sort_keys=True) for r in rows}) == 1
    assert len({json.dumps(r['message_start_mod64_counts']) for r in rows}) == 1
    fig, ax = plt.subplots(figsize=(7.6, 6.4))
    fig.subplots_adjust(left=.22, right=.94, top=.71, bottom=.38)
    fig.text(.04, .96, 'One decoder change, opposite outcomes', fontsize=18, weight='bold', va='top')
    fig.text(.04, .88, '14 Nov 2026 · same five order layouts and backing bytes\n'
             'Bulk-packed copy vs byte decoder · only message order changes',
             fontsize=12, linespacing=1.6, va='top')
    values = [r['median_time_change_pct'] for r in rows]
    ax.barh(range(3), values, color=['#b04c29' if v > 0 else '#147565' for v in values], height=.55)
    for y, value, row in zip(range(3), values, rows):
        ax.text(value + (1 if value > 0 else -1), y, f'{value:+.1f}%', va='center',
                ha='left' if value > 0 else 'right', fontsize=14, weight='bold')
    ax.set_yticks(range(3), ['Cyclic', 'Grouped', 'Shuffled'])
    ax.invert_yaxis()
    ax.axvline(0, color='#384854', lw=1)
    ax.set_xlim(-32, 34)
    ax.set_xlabel('Change in decode time (%)\n← faster                         slower →', labelpad=9)
    ax.grid(axis='x', alpha=.18)
    ax.set_axisbelow(True)
    fig.text(.04, .17, 'Ratio of medians of 9 paired batch averages; not latency percentiles.\n'
             'Core i9-13900K · WSL2 · GCC 12.2 O3 · guest CPU 3, not isolated.\n'
             'Historical decoder microbenchmark; no network or book processing.\n'
             'Source: 2026-11-14-decoder-breakdown-results.json (order-fixtures).',
             fontsize=10.5, linespacing=1.6, va='top')
    save(fig, output, 'decoder-ordering', preview)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/images')
    parser.add_argument('--preview-directory', type=Path, help='Optional untracked PNG previews')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 12,
                         'svg.fonttype': 'none', 'svg.hashsalt': 'low-latency-readme',
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'figure.facecolor': 'white', 'axes.facecolor': 'white',
                         'text.color': '#20313d', 'axes.labelcolor': '#20313d'})
    application, receive, decoder = (read(name) for name in ('application', 'receive', 'decoder'))
    verify_application(application)
    application_chart(application, args.output, args.preview_directory)
    receive_chart(application, receive, args.output, args.preview_directory)
    decoder_chart(decoder, args.output, args.preview_directory)
    print(json.dumps({SOURCES[k]: hashlib.sha256(
        (ROOT / 'docs/experiments' / SOURCES[k]).read_bytes()).hexdigest() for k in SOURCES}, indent=2))


if __name__ == '__main__':
    main()
