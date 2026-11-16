"""Behavior checks for corpus selection/order; no performance thresholds."""

import json
import subprocess
import sys


binary, fixtures = sys.argv[1:]
base = [binary, '--fixtures', fixtures, '--messages', '23', '--warmup', '23', '--repeats', '1']


def run(*args):
    result = subprocess.run([*base, *map(str, args)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


all_layouts = run()
three = run('--decoders', 'original,packed,zero-copy')
assert {r['decoder'] for r in three['runs']} == {'original', 'packed', 'zero-copy'}
assert len(three['runs']) == 3
assert len({r['checksum'] for r in three['runs']}) == 1
assert {r['decoder'] for r in three['summary']} == {'original', 'packed', 'zero-copy'}
assert set(three['median_paired_speedups_vs_first']) == {'packed', 'zero-copy'}
two = run('--decoders', 'packed,zero-copy')
assert {r['decoder'] for r in two['runs']} == {'packed', 'zero-copy'}
time = run('--type', '0x20', '--length', 6)
assert time['corpus_messages'] == 1 and time['message_type_counts'] == {'32': 1}
assert time['corpus_bytes'] == all_layouts['corpus_bytes']  # Retain original wire placement.
assert len(time['message_start_mod64_counts']) == 64
assert sum(time['message_start_mod64_counts']) == time['corpus_messages']
both_times = run('--type', 32)
assert both_times['corpus_messages'] == 2
selected = run('--type', '0x20', '--type', '0x21')
assert selected['message_type_counts'] == {'32': 2, '33': 1}

variants = [run('--copies', 3, '--order', order) for order in ('input', 'grouped', 'shuffled')]
assert all(v['corpus_messages'] == 69 and v['messages_per_run'] == 69 for v in variants)
assert all(v['corpus_bytes'] == all_layouts['corpus_bytes'] for v in variants)
assert all(v['message_type_counts'] == variants[0]['message_type_counts'] for v in variants)
assert len({v['runs'][0]['checksum'] for v in variants}) == 1
assert len({v['message_order_hash'] for v in variants}) == 3
again = run('--copies', 3, '--order', 'shuffled')
assert again['message_order_hash'] == variants[-1]['message_order_hash']
assert run('--copies', 3, '--order', 'shuffled', '--seed', 43)['message_order_hash'] != again['message_order_hash']

for args in [('--type', 256), ('--type', '0x'), ('--type', '0xff'), ('--length', 1),
             ('--length', 256), ('--copies', 0), ('--copies', 10000000), ('--order', 'unknown'),
             ('--decoders', 'zero-copy'), ('--decoders', 'original,original'),
             ('--decoders', 'original,unknown')]:
    result = subprocess.run([*base, *map(str, args)], capture_output=True, text=True, timeout=5)
    assert result.returncode != 0, args

print('Decoder corpus selection, repetition and ordering checks passed')
