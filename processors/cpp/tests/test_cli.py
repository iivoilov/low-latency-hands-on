"""OS-socket processor tests; full-stream L2 is checked against the manifest."""

import json
import pathlib
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


PROCESSOR, PREPARE, PLAY = sys.argv[1:4]


def cli(directory):
    result = subprocess.run([PROCESSOR, '--help'], capture_output=True, text=True, timeout=3)
    assert result.returncode == 0 and 'Two threads:' in result.stdout
    for args in (['--unit', 0], ['--messages', 0], ['--ring-slots', 3], ['--gap-slots', 3], ['--gap-slots', 0],
                 ['--idle-timeout-ms', 0], ['--on-loss', 'overwrite'],
                 ['--group', '127.0.0.1'], ['--unknown', 'x'],
                 ['--interface', '0.0.0.0'], ['--interface', '239.255.0.1'],
                 ['--interface', '255.255.255.255'], ['--max-live-orders', 0],
                 ['--max-live-orders', 100000001], ['--max-trades', 0], ['--max-trades', 100000001],
                 ['--monitor-port', 8765], ['--monitor-port', 0, '--continuous'],
                 ['--monitor-port', 65536, '--continuous']):
        result = subprocess.run([PROCESSOR, *map(str, args)], capture_output=True, text=True, timeout=3)
        assert result.returncode != 0, (args, result.stdout, result.stderr)
    existing = directory / 'existing.json'
    existing.write_text('preserve me')
    result = subprocess.run([PROCESSOR, '--output', str(existing)], capture_output=True, text=True, timeout=3)
    assert result.returncode != 0 and existing.read_text() == 'preserve me'


def start(report, *args):
    process = subprocess.Popen([PROCESSOR, '--port', '0', '--output', str(report), '--messages', '100000', *map(str, args)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if not select.select([process.stdout], [], [], 5)[0]:
        process.kill()
        stdout, stderr = process.communicate()
        raise AssertionError(('No READY notification', stdout, stderr))
    line = process.stdout.readline()
    match = re.match(r'READY port=(\d+)', line)
    if not match:
        process.kill()
        stdout, stderr = process.communicate()
        raise AssertionError((line, stdout, stderr))
    return process, int(match.group(1))


def finish(process, report, *, success):
    stdout, stderr = process.communicate(timeout=6)
    assert (process.returncode == 0) == success, (process.returncode, stdout, stderr)
    data = json.loads(report.read_text())
    assert data['threads'] == 2 and data['clock'] == 'CLOCK_MONOTONIC'
    assert data['timing_boundary'] == 'recvmsg_return_to_message_l1_decision'
    assert 'rx_to_l2' not in data
    return data, stdout, stderr


def cleanup(process):
    if process.poll() is None:
        process.kill()
        process.communicate()


def message(kind, order=0, quantity=100, price=100000, side=b'B'):
    length = {0x97: 6, 0x21: 34, 0x25: 18, 0x27: 27, 0x29: 14}[kind]
    result = bytearray(length)
    result[0:2] = bytes([length, kind])
    if kind == 0x97:
        return result
    struct.pack_into('<Q', result, 6, order)
    if kind == 0x21:
        result[14:15] = side
        struct.pack_into('<I6sQB', result, 15, quantity, b'TEST  ', price, 1)
    elif kind == 0x25:
        struct.pack_into('<I', result, 14, quantity)
    elif kind == 0x27:
        struct.pack_into('<IQB', result, 14, quantity, price, 1)
    return result


def packet(sequence, *messages):
    payload = b''.join(messages)
    return struct.pack('<HBBI', len(payload) + 8, len(messages), 1, sequence) + payload


def send(port, packets, source='127.0.0.1', group='239.255.0.1'):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as sender:
        sender.bind((source, 0))
        sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton('127.0.0.1'))
        sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        for payload in packets:
            sender.sendto(payload, (group, port))


def filtered_traffic(directory):
    for source in ('127.0.0.1', '127.0.0.2'):
        report = directory / ('filter-' + source + '.json')
        process, port = start(report, '--warmup-messages', 0, '--messages', 1)
        try:
            forged = packet(1, message(0x97), message(0x21, 99))
            hostile = [forged, b'broken', b'x' * 2000]
            send(port, hostile, source=source, group='127.0.0.1') # Unicast is not feed input.
            send(port, hostile, source=source, group='239.255.0.2')
            time.sleep(0.1)
            assert process.poll() is None, 'Unrelated traffic reached PITCH processing'
            send(port, [packet(1, message(0x97), message(0x21, 1))], source=source)
            result, _, _ = finish(process, report, success=True)
            assert result['benchmark_valid'] and result['book_current']
            assert result['max_live_orders'] == 1000000
            if result['implementation'] == 'cpp-os':
                assert result['pitch_decoder'] == 'packed-zero-copy'
                assert result['order_container'] == 'std::unordered_map'
                assert result['order_hash'] == 'SipHash-2-4'
                assert result['max_order_bucket_entries'] is None
            assert result['received_packets'] == 1 and result['protocol_errors'] == 0
            assert result['truncated_packets'] == 0 and result['live_orders'] == 1
        finally:
            cleanup(process)


def hostile_feed(directory):
    initial = packet(1, message(0x97), message(0x21, 1))
    for name, invalid in [('truncated-add', bytes([2, 0x21])),
                          ('truncated-missing-modify', bytes([14, 0x27]) + bytes(12)),
                          ('invalid-add', message(0x21, 2, 0)),
                          ('duplicate-add', message(0x21, 1))]:
        report = directory / (name + '.json')
        process, port = start(report, '--on-loss', 'continue', '--continuous', '--warmup-messages', 0)
        try:
            send(port, [initial, packet(4, invalid)])
            result, _, _ = finish(process, report, success=False)
            assert result['protocol_errors'] == 1 and result['skipped_market_messages'] == 0
            assert not result['book_current'] and result['live_orders'] == 1
        finally:
            cleanup(process)
    report = directory / 'book-limit.json'
    process, port = start(report, '--on-loss', 'continue', '--continuous', '--warmup-messages', 0,
                          '--max-live-orders', 4096)
    try:
        send(port, [packet(1, message(0x97))])
        # Introduce a gap so the resource limit is also exercised with a dirty book.
        sequence = 3
        for first in range(1, 4098, 40):
            messages = [message(0x21, i) for i in range(first, min(first + 40, 4098))]
            send(port, [packet(sequence, *messages)])
            sequence += len(messages)
        result, _, _ = finish(process, report, success=False)
        assert result['live_orders'] == 4096 and result['max_live_orders'] == 4096
        assert result['protocol_errors'] == 1 and result['skipped_market_messages'] == 0
        assert 'limit exceeded' in result['error'] and not result['book_current']
    finally:
        cleanup(process)


def full_stream(directory):
    captures = directory / 'play'
    subprocess.run([PREPARE, '--directory', str(captures), '--messages', '100000'], check=True, capture_output=True, timeout=5)
    capture = next(captures.glob('*.pcap'))
    manifest = json.loads(capture.with_suffix('.json').read_text())
    report = directory / 'full.json'
    process, port = start(report)
    try:
        replay = subprocess.run([PLAY, str(capture), '--interface', '127.0.0.1', '--port', str(port), '--spin'],
                                capture_output=True, text=True, timeout=10)
        assert replay.returncode == 0, (replay.stdout, replay.stderr)
        result, _, _ = finish(process, report, success=True)
        assert result['benchmark_valid'] and result['book_current'] and result['complete']
        assert result['rx_to_l1_decision']['samples'] == manifest['updates'] == 100000
        assert result['warmup_messages'] == manifest['initialization_messages'] == 180
        assert result['market_messages'] == result['applied_market_messages'] == manifest['market_messages']
        assert result['pitch_messages'] == manifest['pitch_messages']
        assert result['processed_packets'] == manifest['packets'] - 1  # Terminal heartbeat has no samples.
        assert result['final_book'] == manifest['final_book'], 'Processor L2 differs from independent play-file reference'
        for field in ('kernel_dropped_packets', 'ring_dropped_packets', 'truncated_packets',
                      'sequence_errors', 'missing_messages', 'protocol_errors', 'skipped_market_messages',
                      'gap_ranges_enqueued', 'gap_ranges_dropped'):
            assert result[field] == 0, (field, result)
        assert result['pending_gap_ranges'] == []
        latency = result['rx_to_l1_decision']
        assert 0 <= latency['min_ns'] <= latency['p50_ns'] <= latency['p99_ns'] <= latency['p99_9_ns'] <= latency['max_ns']
        assert result['queue_wait']['packets'] == result['processed_packets']
        assert 0 < result['ring_high_watermark'] <= result['ring_slots']
        print('100k-update processor run:', latency, 'queue:', result['queue_wait'])
    finally:
        cleanup(process)


def loss_cases(directory):
    initial = packet(1, message(0x97), message(0x21, 1, 10))
    cases = [
        ('strict-gap', [], [initial, packet(4, message(0x21, 2))], 2),
        ('continue-gap', ['--on-loss', 'continue'],
         [initial, packet(4, message(0x25, 99, 1)), packet(5, message(0x21, 2))], 2),
        ('continue-reset', ['--on-loss', 'continue'],
         [initial, packet(4, message(0x25, 99, 1)), packet(5, message(0x27, 1, 20)),
          packet(6, message(0x97), message(0x21, 100, 30, 101000, b'S'))], 3),
        ('gap-queue-full', ['--on-loss', 'continue', '--gap-slots', 1],
         [initial, packet(4, message(0x21, 2)), packet(6, message(0x21, 3))], 3),
        ('truncated', [], [b'x' * 2000], 1),
        ('continue-truncated', ['--on-loss', 'continue'], [b'x' * 2000, initial], 1),
        ('malformed', [], [b'broken'], 1),
        ('idle', [], [initial], 2),
    ]
    for name, args, packets, target in cases:
        report = directory / (name + '.json')
        process, port = start(report, '--warmup-messages', 0, '--messages', target,
                              '--idle-timeout-ms', 150, *args)
        try:
            send(port, packets)
            result, _, _ = finish(process, report, success=False)
            assert not result['benchmark_valid']
            if name in ('strict-gap', 'continue-gap', 'continue-reset'):
                assert result['missing_messages'] == 1 and result['sequence_errors'] == 1
                assert result['gap_ranges_enqueued'] == 1 and result['gap_ranges_dropped'] == 0
                assert len(result['pending_gap_ranges']) == 1
                gap = result['pending_gap_ranges'][0]
                assert (gap['unit'], gap['first_sequence'], gap['last_sequence'], gap['message_count']) == (1, 3, 3, 1)
                assert gap['detected_rx_ns'] > 0
            if name == 'strict-gap':
                assert result['rx_to_l1_decision']['samples'] == 1 and not result['book_current']
            elif name == 'continue-gap':
                assert result['complete'] and not result['book_current']
                assert result['skipped_market_messages'] == 1 and result['error'] == ''
            elif name == 'continue-reset':
                assert result['complete'] and result['book_current']
                assert result['skipped_market_messages'] == 1
                assert result['final_book'] == [{'symbol': 'TEST', 'side': 'S', 'price_1e4': 101000,
                                                 'quantity': 30, 'orders': 1}]
            elif name == 'gap-queue-full':
                assert result['complete'] and not result['book_current']
                assert result['market_messages'] == 3 and result['missing_messages'] == 2
                assert result['gap_ranges_enqueued'] == 1 and result['gap_ranges_dropped'] == 1
                assert len(result['pending_gap_ranges']) == 1
                assert result['pending_gap_ranges'][0]['first_sequence'] == 3
            elif name == 'truncated':
                assert result['truncated_packets'] == 1 and not result['book_current']
                assert result['rx_to_l1_decision']['p50_ns'] is None
            elif name == 'continue-truncated':
                assert result['truncated_packets'] == 1 and result['complete'] and result['book_current']
            elif name == 'malformed':
                assert result['protocol_errors'] == 1
            elif name == 'idle':
                assert not result['complete'] and 'idle timeout' in result['error']
        finally:
            cleanup(process)


def all_equities_messages(directory):
    fixture_path = pathlib.Path(__file__).resolve().parents[2] / 'tests/equities_messages.tsv'
    fixtures = {parts[0]: bytearray.fromhex(parts[1])
                for line in fixture_path.read_text().splitlines() if line and not line.startswith('#')
                for parts in [line.split()]}

    def encoded(name, *, order=None, execution=None, side=None):
        m = fixtures[name].copy()
        if order is not None:
            struct.pack_into('<Q', m, 6, order)
        if execution is not None:
            offset = {'trade_long': 33, 'trade_short': 25, 'trade_expanded': 35,
                      'execute': 18, 'execute_price': 22, 'trade_break': 6}[name]
            struct.pack_into('<Q', m, offset, execution)
        if side is not None:
            m[14] = ord(side)
        return m

    messages = [encoded('time_reference'), encoded('time'), encoded('time_epoch'), encoded('unit_clear'),
                encoded('add_long', order=1), encoded('add_short', order=2, side='S'),
                encoded('add_expanded', order=3), encoded('reduce_long', order=1),
                encoded('reduce_short', order=2), encoded('modify_long', order=1),
                encoded('modify_short', order=2), encoded('execute', order=1, execution=100),
                encoded('execute_price', order=2, execution=101),
                encoded('trade_long', execution=102), encoded('trade_short', execution=103),
                encoded('trade_expanded', execution=104), encoded('trade_break', execution=102),
                encoded('trade_break', execution=103), encoded('status'), encoded('auction_update'),
                encoded('auction_summary'), encoded('retail'), encoded('delete', order=1),
                encoded('delete', order=2), encoded('delete', order=3), encoded('end_session')]
    # All symbol/order/trade messages, including unchanged auction metadata,
    # have a decision sample. Clock/reset/session controls are excluded.
    samples = len(messages) - 5
    report = directory / 'all-equities.json'
    process, port = start(report, '--warmup-messages', 0, '--messages', samples, '--max-trades', 8)
    try:
        send(port, [packet(0), packet(1, *messages)])
        result, _, _ = finish(process, report, success=True)
        assert result['benchmark_valid'] and result['session_ended']
        assert result['unsequenced_heartbeats'] == 1 and result['sequence_errors'] == 0
        assert result['rx_to_l1_decision']['samples'] == samples
        assert result['l1_decisions'] == samples
        assert result['l1_no_updates'] > 0 and result['l1_updates'] > 0
        assert result['retained_trades'] == 3 and result['final_book'] == []
        assert result['feed_time']['epoch_seconds'] == 1700000000
        assert result['time_reference']['trade_date'] == 20261114
        states = {s['symbol']: s for s in result['final_l1']}
        assert set(states) == {'TEST', 'LONGSYMB'}
        for state in states.values():
            assert state['bid'] is None and state['ask'] is None
            assert state['book_current'] and state['trade_history_current']
        assert states['TEST']['last'] == {'execution_id': 101, 'price_1e4': 102700, 'quantity': 200, 'trade_date': 20261114}
        assert states['LONGSYMB']['last'] == {'execution_id': 104, 'price_1e4': 102500, 'quantity': 100, 'trade_date': 20261114}
        assert states['TEST']['trading_status'] == 'T' and states['TEST']['reg_sho'] == '1'
        assert states['TEST']['retail'] == 'A'
        assert states['TEST']['auction_update']['buy_quantity'] == 100
        assert states['TEST']['auction_summary']['quantity'] == 300
    finally:
        cleanup(process)


def continuous(directory):
    report = directory / 'continuous.json'
    process, port = start(report, '--continuous', '--on-loss', 'continue', '--warmup-messages', 0,
                          '--messages', 1, '--idle-timeout-ms', 50)
    try:
        send(port, [packet(1, message(0x97), message(0x21, 1, 50))])
        time.sleep(0.2)
        assert process.poll() is None, 'Continuous mode must survive sample completion and quiet markets'
        before = time.monotonic()
        process.send_signal(signal.SIGTERM)
        result, _, _ = finish(process, report, success=True)
        assert time.monotonic() - before < 1
        assert result['continuous'] and result['complete'] and result['book_current']
        assert not result['benchmark_valid'] and result['rx_to_l1_decision']['samples'] == 1
    finally:
        cleanup(process)


with tempfile.TemporaryDirectory(prefix='os-processor-') as temporary:
    directory = pathlib.Path(temporary)
    if '--network' in sys.argv:
        full_stream(directory)
        all_equities_messages(directory)
        loss_cases(directory)
        continuous(directory)
        filtered_traffic(directory)
        hostile_feed(directory)
    else:
        cli(directory)
print('OS-socket processor checks passed')
