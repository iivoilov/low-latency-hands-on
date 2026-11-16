"""Shared C++/Rust Cboe recovery integration tests; fabricated PCAPs only."""
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import threading

PROCESSOR, PREPARE, SERVE = sys.argv[1:4]
ENV = dict(os.environ, PITCH_SESSION='0001', PITCH_USERNAME='TEST', PITCH_PASSWORD='lab')


def line(process, timeout=8):
    if not select.select([process.stdout], [], [], timeout)[0]:
        raise AssertionError('Timed out waiting for service output')
    value = process.stdout.readline()
    if not value:
        raise AssertionError(('Unexpected process exit', process.communicate(timeout=2)))
    return value


def udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def terminate(process):
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            return process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            return process.communicate()
    return ('', '')


def packet(sequence, *messages):
    data = b''.join(messages)
    return struct.pack('<HBBI', len(data)+8, len(messages), 1, sequence)+data


def order(kind, identifier=1, quantity=100):
    size = {0x97: 6, 0x21: 34, 0x27: 27, 0x23: 26, 0x25: 18}[kind]
    b = bytearray(size)
    b[0:2] = bytes([size, kind])
    if kind != 0x97:
        struct.pack_into('<Q', b, 6, identifier)
    if kind == 0x21:
        b[14] = ord('B')
        struct.pack_into('<I6sQB', b, 15, quantity, b'TEST  ', 100000, 1)
    elif kind == 0x27:
        struct.pack_into('<IQB', b, 14, quantity, 100000, 1)
    elif kind in (0x23, 0x25):
        struct.pack_into('<I', b, 14, quantity)
        if kind == 0x23:
            struct.pack_into('<Q', b, 18, 123)
    return bytes(b)


def scripted(directory, *, rollover=False, malformed=False):
    """Independent tiny exchange peer checks overlap, exact-once executions and rollover."""
    port, gap_port = udp_port(), udp_port()
    stop = threading.Event()
    requested = threading.Event()
    errors = []
    listeners = []
    workers = []
    process = None

    def send_udp(payload, replay=False):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton('127.0.0.1'))
            sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            sender.sendto(payload, ('239.255.0.2' if replay else '239.255.0.1', gap_port if replay else port))

    def peer(listener, spin):
        try:
            listener.settimeout(3)
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(.1)
                data = bytearray()
                logged_in = False
                while not stop.is_set():
                    try:
                        more = connection.recv(4096)
                    except socket.timeout:
                        continue
                    if not more:
                        return
                    data.extend(more)
                    while len(data) >= 8:
                        size, count, unit, sequence = struct.unpack_from('<HBBI', data)
                        assert size >= 8 and sequence == 0 and unit == 0
                        if len(data) < size:
                            break
                        message = bytes(data[8:size])
                        del data[:size]
                        if not count:
                            continue
                        assert count == 1 and message[0] == len(message)

                        def respond(value):
                            frame = struct.pack('<HBBI', len(value)+8, 1, 0, 0)+value
                            for b in frame:
                                connection.sendall(bytes([b]))

                        if not logged_in:
                            assert message == b'\x16\x010001TEST  lab       '
                            if malformed:
                                connection.sendall(b'\x07\x00\x01\x00\x00\x00\x00\x00')
                                continue
                            respond(b'\x03\x02A')
                            logged_in = True
                            if spin:
                                respond(struct.pack('<BBI', 6, 0x80, 0xffffffff-2))
                        elif spin:
                            assert message[1] == 0x81
                            image = struct.unpack_from('<I', message, 2)[0]
                            respond(struct.pack('<BBIIB', 11, 0x82, image, 1, ord('A')))
                            respond(order(0x21))
                            respond(struct.pack('<BBI', 6, 0x83, image))
                            requested.set()
                        else:
                            assert message[1] == 3
                            _, _, unit, first, count = struct.unpack('<BBBIH', message)
                            if rollover:
                                assert (unit, first, count) == (1, 0xffffffff, 2), (first, count)
                                send_udp(packet(first, order(0x25, quantity=10), order(0x23, quantity=10)), True)
                            else:
                                assert (unit, first, count) == (1, 3, 1), (first, count)
                                # Past message 2, recovered 3, and overlapping buffered 4/5.
                                send_udp(packet(2, order(0x21), order(0x21, 2), order(0x27, 2, 30), order(0x23, 2, 10)), True)
                            time.sleep(.03)  # replay is allowed before its acknowledgment
                            respond(struct.pack('<BBBIHB', 10, 4, unit, first, count, ord('A')))
                            if not rollover:
                                requested.set()
        except Exception as e:
            if not stop.is_set():
                errors.append(e)

    name = 'malformed-control' if malformed else 'rollover' if rollover else 'overlap'
    try:
        for spin in ([False, True] if rollover else [False]):
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listeners.append(listener)
            worker = threading.Thread(target=peer, args=(listener, spin), daemon=True)
            worker.start()
            workers.append(worker)
        report = directory / (name+'.json')
        args = [PROCESSOR, '--continuous', '--port', str(port), '--gap-port', str(gap_port),
                '--grp', '127.0.0.1:'+str(listeners[0].getsockname()[1]), '--warmup-messages', '0',
                '--messages', '100000', '--max-live-orders', '1000', '--max-trades', '1000', '--output', str(report)]
        if rollover:
            args += ['--spin-server', '127.0.0.1:'+str(listeners[1].getsockname()[1])]
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=ENV)
        assert line(process).startswith('READY ')
        if malformed:
            stdout, stderr = process.communicate(timeout=4)
            assert process.returncode != 0, (stdout, stderr)
        else:
            if rollover:
                send_udp(packet(0xffffffff-1, order(0x27, quantity=90)))
                assert requested.wait(2), errors
                requested.clear()
                time.sleep(.08)
                send_udp(packet(2, order(0x27, quantity=50)))
            else:
                send_udp(packet(1, order(0x97), order(0x21)))
                send_udp(packet(4, order(0x27, 2, 30), order(0x23, 2, 10)))
            time.sleep(.25)
            stdout, stderr = terminate(process)
            assert process.returncode == 0, (stdout, stderr, errors)
        data = json.loads(report.read_text())
        assert not errors, errors
        if malformed:
            assert not data['book_current'] and 'TCP frame length' in data['error']
        else:
            assert data['book_current'] and not data['error'] and not data['recovery']['active'], data
            assert data['retained_trades'] == 1 and data['recovery']['requests'] == 1, data
            assert data['final_book'][0]['quantity'] == (50 if rollover else 120), data
            assert data['trade_history_current'] == (not rollover)
            if not rollover:
                assert data['recovery']['duplicates'] == 3 and data['l1_decisions'] == 4, data
                assert data['rx_to_l1_decision']['samples'] == 1, data
            else:
                assert data['rx_to_l1_decision']['samples'] == 0, data
        print(name, data['recovery'], flush=True)
    finally:
        terminate(process)
        stop.set()
        for worker in workers:
            worker.join(timeout=1)
        for listener in listeners:
            listener.close()


def trial(directory, pcap, name, server_args=(), processor_args=(), *, spin=False, failure=False, join=False):
    port, gap_port = udp_port(), udp_port()
    while gap_port == port:
        gap_port = udp_port()
    server = processor = None
    report = directory / (name+'.json')
    try:
        server = subprocess.Popen([SERVE, str(pcap), '--port', str(port), '--gap-port', str(gap_port),
                                   '--grp-port', '0', '--spin-port', '0', '--start-delay-ms', '400',
                                   '--checkpoint-messages', '127', *server_args],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=ENV)
        ready = line(server)
        match = re.match(r'READY grp_port=(\d+) spin_port=(\d+)', ready)
        assert match, ready
        if join:
            time.sleep(.6)
        args = [PROCESSOR, '--continuous', '--port', str(port), '--gap-port', str(gap_port),
                '--grp', '127.0.0.1:'+match[1], '--max-live-orders', '1000', '--max-trades', '100000',
                '--messages', '100000', '--warmup-messages', '180', '--recovery-timeout-ms', '3000',
                '--output', str(report)]
        if spin:
            args += ['--spin-server', '127.0.0.1:'+match[2]]
        processor = subprocess.Popen([*args, *processor_args], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, env=ENV)
        assert line(processor).startswith('READY ')
        assert line(server, 12).startswith('stream_finished ')
        if failure:
            stdout, stderr = processor.communicate(timeout=5)
            assert processor.returncode != 0, (stdout, stderr)
        else:
            time.sleep(1.4)  # next advertised image + catch-up, including a lost tail
            stdout, stderr = terminate(processor)
            assert processor.returncode == 0, (stdout, stderr)
        data = json.loads(report.read_text())
        assert not data['benchmark_valid']
        if failure:
            assert data['error'] and not data['book_current'], data
        else:
            assert not data['error'] and data['book_current'] and not data['recovery']['active'], data
            expected = json.loads(pcap.with_suffix('.json').read_text())['final_book']
            assert data['final_book'] == expected, (name, data['final_book'], expected)
        print(name, data['recovery'], flush=True)
        return data
    finally:
        terminate(processor)
        terminate(server)


def main():
    with tempfile.TemporaryDirectory(prefix='pitch-recovery-') as tmp:
        directory = Path(tmp)
        subprocess.run([PREPARE, '--directory', str(directory), '--messages', '100000', '--seed', '73',
                        '--background-mbps', '8', '--burst-mbps', '8', '--burst-ms', '0'],
                       check=True, capture_output=True, timeout=5)
        pcap = next(directory.glob('*.pcap'))
        baseline = trial(directory, pcap, 'no-loss')
        gap = trial(directory, pcap, 'grp', ['--drop-packets', '40-65', '--replay-before-ack', '--tcp-fragment-bytes', '1'])
        assert gap['recovery']['requests'] >= 3 and gap['recovery']['spins'] == 0
        assert gap['trade_history_current'] and gap['final_l1'] == baseline['final_l1']
        snapshot = trial(directory, pcap, 'spin-reject', ['--drop-packets', '40-65', '--reject-grp'], spin=True)
        assert snapshot['recovery']['spins'] == 1 and not snapshot['trade_history_current']
        snapshot = trial(directory, pcap, 'spin-timeout', ['--drop-packets', '40', '--drop-replay-once'],
                         ['--recovery-timeout-ms', '1800', '--recovery-buffer', '200000'], spin=True)
        assert snapshot['recovery']['spins'] == 1 and snapshot['recovery']['timeouts'] == 1
        snapshot = trial(directory, pcap, 'spin-join', spin=True, join=True)
        assert snapshot['recovery']['spins'] == 1 and not snapshot['trade_history_current']
        snapshot = trial(directory, pcap, 'spin-large', ['--drop-packets', '40-200'],
                         ['--recovery-max-gap', '100'], spin=True)
        assert snapshot['recovery']['spins'] == 1 and snapshot['recovery']['requests'] == 0
        data = pcap.read_bytes()
        offset, index, last_update_packet = 24, 0, 0
        while offset < len(data):
            size = struct.unpack_from('<I', data, offset+8)[0]
            frame = data[offset+16:offset+16+size]
            index += 1
            if frame[30]:  # fabricated LINKTYPE_RAW IPv4(20) + UDP(8) + PITCH Count(2)
                last_update_packet = index
            offset += 16+size
        tail = trial(directory, pcap, 'tail-gap', ['--drop-packets', str(last_update_packet)])
        assert tail['recovery']['requests'] == 1 and tail['trade_history_current']
        exhausted = trial(directory, pcap, 'bounded-spin-failure', ['--drop-packets', '40-65'],
                          ['--recovery-buffer', '128'], spin=True, failure=True)
        assert exhausted['recovery']['buffer_overflows'] == 1 and exhausted['recovery']['active']
        trial(directory, pcap, 'reject-without-spin', ['--drop-packets', '40', '--reject-grp'], failure=True)
        scripted(directory)
        scripted(directory, rollover=True)
        scripted(directory, malformed=True)
        # Reject before network setup when recovery is requested without continuous mode.
        invalid = subprocess.run([PROCESSOR, '--grp', '127.0.0.1:1'], capture_output=True, timeout=3)
        assert invalid.returncode != 0


if __name__ == '__main__':
    main()
