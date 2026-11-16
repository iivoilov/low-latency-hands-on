"""Exercise real repeated playback, producer sessions and child shutdown."""

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

from monitoring.core import HEADER


def events(log):
    log.seek(0)
    result = []
    for line in log:
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict) and "cycle" in value:
            result.append(value)
    return result


def gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("processor")
    parser.add_argument("prepare")
    parser.add_argument("player")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="monitor-replay-test-") as temporary:
        directory = Path(temporary)
        subprocess.run([args.prepare, "--directory", str(directory), "--messages", "100000",
                        "--symbols", "TEST", "--levels", "2", "--orders-per-level", "1",
                        "--background-mbps", "4", "--burst-mbps", "4", "--burst-ms", "0"],
                       check=True, capture_output=True, text=True)
        pcap = next(directory.glob("*.pcap"))
        manifest = json.loads(pcap.with_suffix(".json").read_text())
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as monitor:
            monitor.bind(("127.0.0.1", 0))
            monitor.settimeout(.05)
            command = [sys.executable, "-m", "monitoring.replay", str(pcap), "--processor", args.processor,
                       "--player", args.player, "--monitor-port", str(monitor.getsockname()[1])]
            with tempfile.TemporaryFile(mode="w+") as log:
                process = subprocess.Popen([*command, "--cycles", "2"], stdout=log, stderr=log)
                sources = set()
                try:
                    deadline = time.monotonic() + 30
                    while process.poll() is None and time.monotonic() < deadline:
                        try:
                            data = monitor.recv(2048)
                        except socket.timeout:
                            continue
                        magic, source, *_ = HEADER.unpack_from(data)
                        assert magic == b"LLM1"
                        sources.add(source)
                    code = process.wait(timeout=2)
                    log.seek(0)
                    assert code == 0, log.read()
                    recorded = events(log)
                    assert len(sources) == 2, sources
                    playing = [e for e in recorded if e["status"] == "playing"]
                    complete = [e for e in recorded if e["status"] == "complete"]
                    assert len(playing) == len(complete) == 2, recorded
                    assert playing[0]["processor_pid"] != playing[1]["processor_pid"]
                    for event in complete:
                        assert event["market_messages"] == manifest["market_messages"], event
                        assert event["protocol_errors"] == event["missing_messages"] == 0, event
                        assert event["ring_dropped_packets"] == 0 and event["book_current"], event
                    assert complete[0]["retained_trades"] == complete[1]["retained_trades"] > 0
                    assert all(gone(e[key]) for e in playing for key in ("processor_pid", "player_pid"))
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=7)

            for suffix, interrupt in [([], True), (["--player", "/bin/false"], False)]:
                with tempfile.TemporaryFile(mode="w+") as log:
                    process = subprocess.Popen([*command, *suffix], stdout=log, stderr=log)
                    try:
                        deadline = time.monotonic() + 10
                        playing = []
                        while not playing and process.poll() is None and time.monotonic() < deadline:
                            playing = [e for e in events(log) if e["status"] == "playing"]
                            time.sleep(.02)
                        assert len(playing) == 1, events(log)
                        if interrupt:
                            process.send_signal(signal.SIGINT)
                        assert process.wait(timeout=7) == (0 if interrupt else 1)
                        assert all(gone(playing[0][key]) for key in ("processor_pid", "player_pid"))
                        assert len([e for e in events(log) if e["status"] == "playing"]) == 1
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            process.wait(timeout=7)
    print("Monitor replay: two fresh sessions, repeated trades, Ctrl-C and failure cleanup passed")


if __name__ == "__main__":
    main()
