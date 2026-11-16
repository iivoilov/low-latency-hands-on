"""Start the local throttler and web app together; stop both on interruption."""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="Web app bind address")
    parser.add_argument("--port", type=int, default=8767, help="Web app HTTP/WebSocket port")
    parser.add_argument("--control-port", type=int, default=8766, help="Local throttler HTTP port")
    parser.add_argument("--udp-port", type=int, default=8765, help="Local processor telemetry UDP port")
    args = parser.parse_args()
    if any(not 1 <= port <= 65535 for port in (args.port, args.control_port, args.udp_port)) or args.port == args.control_port:
        parser.error("Use ports 1..65535 and distinct web/control ports")
    environment = dict(os.environ, MONITOR_THROTTLER_URL=f"http://127.0.0.1:{args.control_port}")
    processes = []
    stopped = False

    def interrupt(signum, frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        throttler = subprocess.Popen([sys.executable, "-m", "monitoring.throttler", "--port", str(args.control_port),
                                      "--udp-port", str(args.udp_port)], env=environment)
        processes.append(throttler)
        deadline = time.monotonic() + 10
        while not stopped:
            if throttler.poll() is not None:
                raise RuntimeError("Throttler startup failed; check its error above and MONITOR_VALKEY_URL")
            try:
                with opener.open(environment["MONITOR_THROTTLER_URL"] + "/health", timeout=1) as response:
                    if json.load(response)["broker_connected"]:
                        break
            except (urllib.error.URLError, OSError):
                pass
            if time.monotonic() >= deadline:
                raise RuntimeError("Throttler startup timed out")
            time.sleep(.1)
        if stopped:
            return
        processes.append(subprocess.Popen([sys.executable, "-m", "monitoring.app", "--host", args.host, "--port", str(args.port)], env=environment))
        print(f"Market monitor: http://{args.host}:{args.port} (processor --monitor-port {args.udp_port})", flush=True)
        while not stopped:
            if any(process.poll() is not None for process in processes):
                raise RuntimeError("A monitoring service exited; stopping the other service")
            time.sleep(.2)
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
