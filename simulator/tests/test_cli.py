"""Independent PITCH L2 reconstruction, traffic checks, PCAP/CLI and multicast."""

from collections import Counter, defaultdict
import json
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


PREPARE, PLAY = sys.argv[1:3]


def run(program, *args, success=True):
    result = subprocess.run([program, *map(str, args)], capture_output=True, text=True, timeout=10)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result


def prepare(directory, *args):
    run(PREPARE, "--directory", directory, "--messages", 100000, *args)
    paths = list(directory.glob("*.pcap"))
    assert len(paths) == 1
    return paths[0], json.loads(paths[0].with_suffix(".json").read_text())


def payloads(path):
    data = path.read_bytes()
    assert data[:4] == b"\x4d\x3c\xb2\xa1"
    packets = []
    offset = 24
    while offset < len(data):
        sec, ns, captured, original = struct.unpack_from("<IIII", data, offset)
        assert captured == original and ns < 1_000_000_000
        frame = data[offset + 16:offset + 16 + captured]
        assert len(frame) == captured
        assert frame[0] == 0x45 and frame[9] == 17
        checksum = sum(struct.unpack("!10H", frame[:20]))
        while checksum >> 16:
            checksum = (checksum & 0xFFFF) + (checksum >> 16)
        assert checksum == 0xFFFF
        assert struct.unpack_from("!H", frame, 2)[0] == len(frame)
        assert struct.unpack_from("!H", frame, 24)[0] == len(frame) - 20
        packets.append((sec * 1_000_000_000 + ns, frame, frame[28:]))
        offset += 16 + captured
    assert offset == len(data)
    return packets


def write_fixture(path, frames, *, endian="<", nanos=False, link=1, times=None):
    magic = 0xA1B23C4D if nanos else 0xA1B2C3D4
    data = struct.pack(endian + "IHHIIII", magic, 2, 4, 0, 0, 65535, link)
    for i, frame in enumerate(frames):
        timestamp = times[i] if times else i * 1_000_000
        seconds, fraction = divmod(timestamp, 1_000_000_000)
        if not nanos:
            fraction //= 1000
        data += struct.pack(endian + "IIII", 1234 + seconds, fraction, len(frame), len(frame)) + frame
    path.write_bytes(data)


def reconstruct(packets, manifest):
    book = {}
    types = Counter()
    sequence = 1
    time_seconds = 34200
    initialization_checked = False
    for packet_index, (timestamp, _, payload) in enumerate(packets):
        length, count, unit, first = struct.unpack_from("<HBBI", payload)
        assert length == len(payload) and length <= 1472
        assert unit == manifest["unit"] and first == sequence
        offset = 8
        for _ in range(count):
            length, kind = struct.unpack_from("BB", payload, offset)
            msg = payload[offset:offset + length]
            types[kind] += 1
            sequence = sequence % 0xFFFFFFFF + 1
            offset += length
            if kind == 0x20:
                time_seconds = struct.unpack_from("<I", msg, 2)[0]
                assert length == 6
                assert time_seconds == 34200 + timestamp // 1_000_000_000
                continue
            assert struct.unpack_from("<I", msg, 2)[0] == timestamp % 1_000_000_000
            if kind == 0x97:
                assert packet_index == 0
                book.clear()
                continue
            order_id = struct.unpack_from("<Q", msg, 6)[0]
            if kind == 0x21:
                assert length == 34 and order_id not in book
                book[order_id] = [msg[19:25].decode().rstrip(), chr(msg[14]),
                                  struct.unpack_from("<Q", msg, 25)[0],
                                  struct.unpack_from("<I", msg, 15)[0]]
            else:
                assert order_id in book
                order = book[order_id]
                if kind == 0x27:
                    assert length == 27 and msg[26] == 1
                    order[3], order[2] = struct.unpack_from("<IQ", msg, 14)
                elif kind in (0x23, 0x25):
                    quantity = struct.unpack_from("<I", msg, 14)[0]
                    assert quantity > 0 and quantity <= order[3]
                    if kind == 0x25:
                        assert quantity < order[3]
                    order[3] -= quantity
                    if not order[3]:
                        del book[order_id]
                else:
                    assert kind == 0x29 and length == 14
                    del book[order_id]
        assert offset == len(payload)
        if packet_index + 1 == manifest["initialization_packets"]:
            initialized = Counter((s, side, p) for s, side, p, _ in book.values())
            assert len(initialized) == len(manifest["symbols"]) * 2 * manifest["levels_per_side"]
            assert set(initialized.values()) == {manifest["orders_per_level"]}
            initialization_checked = True
    levels = defaultdict(lambda: [0, 0])
    best = {}
    for symbol, side, price, quantity in book.values():
        assert quantity > 0
        levels[symbol, side, price][0] += quantity
        levels[symbol, side, price][1] += 1
        old = best.get((symbol, side), price)
        best[symbol, side] = max(price, old) if side == 'B' else min(price, old)
    expected = {(r["symbol"], r["side"], r["price_1e4"]): [r["quantity"], r["orders"]]
                for r in manifest["final_book"]}
    assert dict(levels) == expected, "Independent decoded L2 book must equal manifest"
    for symbol in manifest["symbols"]:
        assert best[symbol, 'B'] < best[symbol, 'S']
    assert initialization_checked
    assert sum(types[k] for k in (0x21, 0x23, 0x25, 0x27, 0x29)) == manifest["market_messages"]
    assert manifest["market_messages"] == manifest["initialization_messages"] + manifest["updates"]
    assert sum(types.values()) == manifest["pitch_messages"]
    assert min(types[k] for k in (0x21, 0x23, 0x25, 0x27, 0x29)) > 100


def check_schedule(packets, manifest):
    period = manifest['burst_every_ms'] * 1_000_000
    burst = manifest['burst_ms'] * 1_000_000
    average = manifest['cycle_average_mbps'] / 1000  # Bits per ns.
    peak = manifest['burst_mbps'] / 1000
    background = manifest['background_mbps'] / 1000
    assert abs(average - (background * (period - burst) + peak * burst) / period) < 1e-12

    def capacity(timestamp):
        cycles, phase = divmod(timestamp, period)
        return cycles * average * period + min(phase, burst) * peak + max(phase - burst, 0) * background

    bits = 0
    peak_gaps = []
    background_gaps = []
    for i, (timestamp, _, payload) in enumerate(packets):
        if i + 1 == len(packets):
            # Terminal heartbeat closes the byte budget and scheduled duration.
            bits += len(payload) * 8
            assert len(payload) == 8 and payload[2] == 0
            assert abs(capacity(timestamp) - bits) < 0.1
        else:
            assert abs(capacity(timestamp) - bits) < 0.1
            bits += len(payload) * 8
            next_time = packets[i + 1][0]
            if i + 2 < len(packets) and timestamp // period == next_time // period:
                rate = len(payload) * 8000 / (next_time - timestamp)
                if timestamp % period < burst and next_time % period <= burst:
                    peak_gaps.append(rate)
                elif timestamp % period >= burst:
                    background_gaps.append(rate)
    assert bits // 8 == manifest['payload_bytes']
    assert packets[-1][0] == manifest['duration_ns']
    actual = bits * 1000 / packets[-1][0]
    assert abs(actual - manifest['finite_average_mbps']) < 1e-9
    if manifest['burst_ms']:
        assert peak_gaps and background_gaps
        assert abs(sum(peak_gaps) / len(peak_gaps) - manifest['burst_mbps']) < 0.002
    assert abs(sum(background_gaps) / len(background_gaps) - manifest['background_mbps']) < 0.002
    assert abs(actual - manifest['cycle_average_mbps']) / manifest['cycle_average_mbps'] < 0.04


def offline(directory):
    source, manifest = prepare(directory / "first")
    duplicate, duplicate_manifest = prepare(directory / "duplicate")
    assert source.read_bytes() == duplicate.read_bytes() and manifest == duplicate_manifest
    assert manifest['updates'] == 100000
    assert manifest['format_version'] == 2
    assert manifest['background_mbps'] == 8 and manifest['cycle_average_mbps'] == 9.2
    assert source.name == 'pitch-v2-100k-base8m-peak20m-b10ms-e100ms_sym3_seed42.pcap'
    assert manifest['pcap'] == source.name
    original = source.read_bytes()
    run(PREPARE, '--directory', directory / 'first', '--messages', 100000, success=False)
    assert source.read_bytes() == original
    run(PREPARE, '--directory', directory / 'default')
    default_path = directory / 'default' / 'pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed42.json'
    default = json.loads(default_path.read_text())
    assert default['updates'] == 1000000 and default['initialization_messages'] == 180
    packets = payloads(source)
    reconstruct(packets, manifest)
    check_schedule(packets, manifest)
    assert f"messages={manifest['pitch_messages']} " in run(PLAY, source, '--no-send').stdout
    run(PLAY, source, '--no-send', '--filter-port', 1, success=False)

    for label, args in [('constant', ['--background-mbps', 4, '--burst-ms', 0]),
                        ('long-burst', ['--background-mbps', 12, '--burst-ms', 25, '--burst-every-ms', 250]),
                        ('frequent', ['--burst-ms', 50])]:
        path, profile = prepare(directory / label, *args)
        check_schedule(payloads(path), profile)

    fixture = directory / 'fixture.pcap'
    for endian in ('<', '>'):
        for nanos in (False, True):
            for tags in (b'', b'\x81\x00\x00\x01', b'\x88\xa8\x00\x01\x81\x00\x00\x02'):
                ethernet = b'\x00' * 12 + tags + b'\x08\x00'
                frames = [b'\x00' * 12 + b'\x08\x06' + b'\x00' * 46]
                frames += [ethernet + frame + b'\x00' * 5 for _, frame, _ in packets[:2]]
                write_fixture(fixture, frames, endian=endian, nanos=nanos)
                assert 'loaded_packets=2 ' in run(PLAY, fixture, '--no-send').stdout
            write_fixture(fixture, [packets[0][1]], endian=endian, nanos=nanos, link=101)
            run(PLAY, fixture, '--no-send')
    for position, value in ((6, 0x20), (24, 0), (28, 0), (30, 255), (36, 0)):
        broken = bytearray(packets[0][1])
        broken[position] = value
        write_fixture(fixture, [broken], link=101)
        run(PLAY, fixture, '--no-send', success=False)
    for length in (0, 10, 25, len(original) - 1):
        fixture.write_bytes(original[:length])
        run(PLAY, fixture, '--no-send', success=False)
    write_fixture(fixture, [p[1] for p in packets[:2]], times=[1_000_000_000, 0], link=101)
    run(PLAY, fixture, '--no-send', success=False)
    for options in (["--seed", "0"], ["--events", "0"], ["--events", "41"],
                    ["--unit", "0"], ["--symbols", "TOOLONG"], ["--symbols", "A,"],
                    ["--symbols", "A,A"], ["--messages", "0"], ["--messages", "-1"],
                    ["--background-mbps", "nan"], ["--background-mbps", "1junk"],
                    ["--background-mbps", "21"], ["--burst-ms", "100"], ["--burst-every-ms", "0"],
                    ["--levels", "0"], ["--levels", "100", "--orders-per-level", "100"]):
        run(PREPARE, '--directory', directory / 'invalid', *options, success=False)
    for options in (['--group', '127.0.0.1'], ['--port', 0], ['--ttl', 256], ['--unknown', 'x']):
        run(PLAY, source, '--no-send', *options, success=False)
    run(PREPARE, '--average-mbps', 8, success=False)  # Reject the obsolete interpretation.
    run(PLAY, '--no-send', success=False)
    assert 'Usage:' in run(PREPARE, '--help').stdout and 'Usage:' in run(PLAY, '--help').stdout


def network(directory):
    source, manifest = prepare(directory / 'network')
    packets = payloads(source)
    expected = [p[2] for p in packets]
    duration = packets[-1][0] / 1e9
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as receiver:
        receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        receiver.bind(('0.0.0.0', 0))
        port = receiver.getsockname()[1]
        receiver.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            socket.inet_aton('239.255.0.1') + socket.inet_aton('127.0.0.1'))
        receiver.settimeout(3)
        for pacing in ([], ['--spin']):
            process = subprocess.Popen([PLAY, str(source), '--interface', '127.0.0.1', '--port', str(port), *pacing],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                received = []
                arrivals = []
                for _ in expected:
                    received.append(receiver.recvfrom(2048)[0])
                    arrivals.append(time.monotonic())
                elapsed = arrivals[-1] - arrivals[0]
                stdout, stderr = process.communicate(timeout=5)
                assert process.returncode == 0, (stdout, stderr)
                assert received == expected, 'Player changed or lost PITCH payloads'
                assert duration * 0.98 <= elapsed <= duration + 1, (duration, elapsed, stdout)
                # Check burst/background arrival rates as well as total duration;
                # a uniform-rate player with the same average must not pass.
                phases = {'burst': [0, 0.0], 'background': [0, 0.0]}
                period = manifest['burst_every_ms'] * 1_000_000
                burst = manifest['burst_ms'] * 1_000_000
                for i in range(len(packets) - 2):
                    start, end = packets[i][0], packets[i + 1][0]
                    if start // period != end // period:
                        continue
                    if start % period < burst and end % period <= burst:
                        phase = 'burst'
                    elif start % period >= burst:
                        phase = 'background'
                    else:
                        continue
                    phases[phase][0] += len(expected[i]) * 8
                    phases[phase][1] += arrivals[i + 1] - arrivals[i]
                rates = {phase: bits / seconds / 1e6 for phase, (bits, seconds) in phases.items()}
                assert 12 < rates['burst'] < 30, (rates, stdout)
                assert 7 < rates['background'] < 9, (rates, stdout)
                assert rates['burst'] > 1.8 * rates['background'], (rates, stdout)
                print('Multicast phases:', rates, 'elapsed_seconds=', elapsed, stdout.strip())
                assert f"sent_packets={len(expected)} " in stdout
                assert 'schedule_slip_ns=' in stdout
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        long_gap = directory / 'long-gap.pcap'
        write_fixture(long_gap, [p[1] for p in packets[:2]], times=[0, 5_000_000_000], link=101)
        process = subprocess.Popen([PLAY, str(long_gap), '--port', str(port)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            receiver.recvfrom(2048)
            start = time.monotonic()
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=2)
            assert process.returncode == 0 and time.monotonic() - start < 1, (stdout, stderr)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


with tempfile.TemporaryDirectory(prefix='low-latency-cli-') as temporary:
    if '--network' in sys.argv:
        network(pathlib.Path(temporary))
    else:
        offline(pathlib.Path(temporary))
print('CLI checks passed')
