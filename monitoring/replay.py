"""Repeat a prepared feed with a fresh monitor processor on each pass."""

import argparse
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


def terminate(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcap", type=Path, help="Prepared PCAP with a same-stem JSON manifest")
    parser.add_argument("--processor", type=Path, default=Path("build/monitor-cpp/processor_cpp"))
    parser.add_argument("--player", type=Path, default=Path("build/monitor-cpp/play_feed"))
    parser.add_argument("--monitor-port", type=int, default=8765)
    parser.add_argument("--interface", default="127.0.0.1")
    parser.add_argument("--cycles", type=int, default=0, help="Number of passes; 0 repeats until Ctrl-C (default)")
    args = parser.parse_args()
    if not 1 <= args.monitor_port <= 65535 or args.cycles < 0:
        parser.error("Use a monitor port 1..65535 and nonnegative cycle count")
    manifest = json.loads(args.pcap.with_suffix(".json").read_text())
    processor_binary, player_binary = str(args.processor.resolve()), str(args.player.resolve())
    stopped = False

    def interrupt(signum, frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    cycle = 0
    while not stopped and (not args.cycles or cycle < args.cycles):
        cycle += 1
        processor = player = None
        with tempfile.TemporaryDirectory(prefix="pitch-monitor-replay-") as temporary:
            report = Path(temporary) / "processor.json"
            with tempfile.TemporaryFile(mode="w+") as processor_log:
                def failure(message):
                    processor_log.seek(0)
                    return RuntimeError(message + "\n" + processor_log.read()[-4000:])

                try:
                    # Each child has its own process group. Ctrl-C reaches this
                    # supervisor, which stops both owned children in finally.
                    processor = subprocess.Popen([
                        processor_binary, "--continuous", "--on-loss", "continue",
                        "--interface", args.interface, "--port", "0",
                        "--monitor-port", str(args.monitor_port), "--unit", str(manifest["unit"]),
                        "--warmup-messages", str(manifest["initialization_messages"]),
                        "--messages", str(manifest["updates"]), "--output", str(report),
                    ], stdout=processor_log, stderr=processor_log, start_new_session=True)
                    deadline = time.monotonic() + 10
                    ready = None
                    while not stopped:
                        processor_log.seek(0)
                        ready = re.match(r"READY port=(\d+)", processor_log.readline())
                        if processor.poll() is not None:
                            raise failure("Monitor processor failed to start")
                        if ready:
                            break
                        if time.monotonic() >= deadline:
                            raise failure("Monitor processor readiness timed out")
                        time.sleep(.05)
                    if stopped:
                        break
                    player = subprocess.Popen([
                        player_binary, str(args.pcap), "--interface", args.interface,
                        "--port", ready.group(1),
                    ], start_new_session=True)
                    print(json.dumps({"cycle": cycle, "status": "playing", "processor_pid": processor.pid,
                                      "player_pid": player.pid}), flush=True)
                    while not stopped and player.poll() is None:
                        if processor.poll() is not None:
                            raise failure("Monitor processor exited during replay")
                        time.sleep(.05)
                    if stopped:
                        break
                    if player.returncode != 0:
                        raise RuntimeError(f"Feed player exited with status {player.returncode}")
                    # Allow the receiver to consume its trailing queued packets.
                    # This display loop is not a benchmark completion barrier.
                    deadline = time.monotonic() + .25
                    while not stopped and time.monotonic() < deadline:
                        time.sleep(.05)
                    terminate(processor)
                    if not stopped:
                        if processor.returncode != 0:
                            raise failure("Monitor processor failed")
                        result = json.loads(report.read_text())
                        print(json.dumps({"cycle": cycle, "status": "complete", **{
                            key: result[key] for key in ("market_messages", "book_current", "protocol_errors",
                                                        "missing_messages", "kernel_dropped_packets",
                                                        "ring_dropped_packets", "truncated_packets", "retained_trades")
                        }}), flush=True)
                finally:
                    terminate(player)
                    terminate(processor)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
