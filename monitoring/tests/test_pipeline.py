"""Real Valkey + HTTP + WebSocket + both processor exporters; loopback only.

python -m monitoring.tests.test_pipeline --valkey-server /path/to/valkey-server \
    --cpp build/monitor-cpp/processor_cpp --rust build/monitor-rust/debug/processor_rust
"""
import argparse
import asyncio
from contextlib import AsyncExitStack
import json
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

import httpx
from websockets.asyncio.client import connect


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def message(kind, order=0, quantity=100, price=100000, side=b"B", execution=0):
    size = {0x97: 6, 0x21: 34, 0x27: 27, 0x23: 26}[kind]
    data = bytearray(size)
    data[:2] = bytes([size, kind])
    if kind != 0x97:
        struct.pack_into("<Q", data, 6, order)
    if kind == 0x21:
        data[14:15] = side
        struct.pack_into("<I6sQB", data, 15, quantity, b"TEST  ", price, 1)
    elif kind == 0x27:
        struct.pack_into("<IQB", data, 14, quantity, price, 1)
    elif kind == 0x23:
        struct.pack_into("<IQ", data, 14, quantity, execution)
    return data


def send(port, sequence, messages):
    data = b"".join(messages)
    packet = struct.pack("<HBBI", 8 + len(data), len(messages), 1, sequence) + data
    assert len(packet) <= 1472
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        sock.sendto(packet, ("239.255.0.1", port))
    return sequence + len(messages)


async def events(ws, count):
    result = []
    async with asyncio.timeout(5):
        while len(result) < count:
            event = json.loads(await ws.recv())
            assert event["type"] != "error", event
            if event["type"] in ("snapshot", "update"):
                result.append(event)
    return result


async def browser_check(url, output):
    from playwright.async_api import async_playwright
    async with async_playwright() as playwright:
        browser = await playwright.chromium.launch(executable_path="/usr/bin/chromium", headless=True,
                                                   args=["--no-sandbox", "--disable-dev-shm-usage"])
        try:
            page = await browser.new_page(viewport={"width": 1440, "height": 1100}, device_scale_factor=1)
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            await page.goto(url)
            await page.wait_for_function("document.querySelector('#quotes').textContent.includes('10.0000')")
            await page.wait_for_function("document.querySelectorAll('#bids tr').length === 20")
            assert await page.locator("#best-ask").inner_text() == "10.1000"
            assert await page.locator("#banner").is_hidden()
            await page.screenshot(path=str(output / "monitor-desktop.png"), full_page=True)
            # Change a real subscription through the UI and receive its snapshot.
            await page.select_option("#interval", "250")
            await page.locator("button.primary").click()
            await page.wait_for_function("document.querySelector('#interval-badge').textContent === '250 ms / security' && document.querySelector('#best-bid').textContent === '10.0000'")
            await page.set_viewport_size({"width": 390, "height": 844})
            await page.screenshot(path=str(output / "monitor-mobile.png"), full_page=True)
            assert await page.evaluate("document.documentElement.scrollWidth <= innerWidth")
            assert not errors, errors
        finally:
            await browser.close()


async def main(args):
    with tempfile.TemporaryDirectory(prefix="market-monitor-test-") as temporary:
        directory = Path(temporary)
        processes, handles = [], []

        def start(command, name, env=None, pipe=False):
            log = open(directory / (name + ".log"), "w+")
            handles.append(log)
            process = subprocess.Popen(command, stdout=subprocess.PIPE if pipe else log, stderr=log,
                                       text=True, env=env)
            processes.append(process)
            return process

        def stop(process):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)

        valkey_port, udp_port, throttle_port, web_port = free_port(), free_port(socket.SOCK_DGRAM), free_port(), free_port()
        valkey_command = [str(Path(args.valkey_server).resolve()), "--bind", "127.0.0.1", "--port", str(valkey_port),
                          "--save", "", "--appendonly", "no", "--loglevel", "warning"]
        import os
        environment = dict(os.environ, MONITOR_VALKEY_URL=f"redis://127.0.0.1:{valkey_port}/0",
                           MONITOR_THROTTLER_URL=f"http://127.0.0.1:{throttle_port}")
        broker = start(valkey_command, "valkey")

        async def broker_ready():
            import redis.asyncio as redis
            test_broker = redis.from_url(environment["MONITOR_VALKEY_URL"])
            try:
                async with asyncio.timeout(10):
                    while True:
                        try:
                            await test_broker.ping()
                            return
                        except redis.RedisError:
                            await asyncio.sleep(.05)
            finally:
                await test_broker.aclose()

        async def ready(client, path, predicate=lambda value: True):
            async with asyncio.timeout(10):
                while True:
                    try:
                        response = await client.get(path)
                        if response.is_success and predicate(response.json()):
                            return response.json()
                    except httpx.HTTPError:
                        pass
                    await asyncio.sleep(.05)

        try:
            # Wait for the broker before launching services that fail fast on startup.
            await broker_ready()
            start([sys.executable, "-m", "monitoring", "--udp-port", str(udp_port), "--control-port", str(throttle_port),
                   "--port", str(web_port)], "services", environment)
            async with httpx.AsyncClient(base_url=f"http://127.0.0.1:{web_port}", trust_env=False) as client:
                await ready(client, "/api/health")
                uri = f"ws://127.0.0.1:{web_port}/ws"
                for implementation, binary in [("cpp", args.cpp), ("rust", args.rust)]:
                    report = directory / (implementation + ".json")
                    process = start([str(Path(binary).resolve()), "--continuous", "--on-loss", "continue", "--port", "0",
                                     "--monitor-port", str(udp_port), "--messages", "100000", "--warmup-messages", "0",
                                     "--max-live-orders", "1000", "--max-trades", "1000", "--output", str(report)], implementation, pipe=True)
                    assert select.select([process.stdout], [], [], 8)[0], "No processor READY"
                    line = process.stdout.readline()
                    assert line.startswith("READY port="), line
                    port = int(line.split()[1].split("=")[1])
                    orders = [message(0x97), message(0x21, 1), message(0x21, 2, price=101000, side=b"S")]
                    for i in range(20):
                        orders.extend([message(0x21, 3 + i * 2, price=99999 - i),
                                       message(0x21, 4 + i * 2, price=101001 + i, side=b"S")])
                    previous_source = (await client.get("/api/health")).json()["source"]
                    sequence = send(port, 1, orders)
                    await ready(client, "/api/health", lambda health: health["source"] != previous_source and health["symbols"] == 1)
                    if args.browser and implementation == "cpp":
                        output = Path("build/monitor-preview")
                        output.mkdir(parents=True, exist_ok=True)
                        await browser_check(f"http://127.0.0.1:{web_port}", output)
                        await ready(client, "/api/health", lambda health: health["subscribers"] == 0)
                    async with AsyncExitStack() as stack:
                        a = await stack.enter_async_context(connect(uri))
                        await a.send(json.dumps({"l1": ["TEST"], "l2": ["TEST"], "interval_ms": 200}))
                        initial = await events(a, 2)
                        assert all(e["type"] == "snapshot" and e["state"]["book_current"] for e in initial), initial
                        l1, l2 = [e["state"]["data"] for e in initial]
                        assert l1["bid"]["price_1e4"] == "100000" and l1["ask"]["price_1e4"] == "101000"
                        assert len(l2["bids"]) == len(l2["asks"]) == 21
                        b = await stack.enter_async_context(connect(uri))
                        await b.send(json.dumps({"l1": ["TEST", "UNKNOWN"], "interval_ms": 600}))
                        snapshots = await events(b, 2)
                        assert all(e["type"] == "snapshot" for e in snapshots)
                        assert snapshots[-1]["symbol"] == "UNKNOWN" and not snapshots[-1]["available"]
                        assert (await client.get("/api/health")).json()["subscribers"] == 2
                        await asyncio.sleep(.03)
                        changed_at = time.monotonic()
                        sequence = send(port, sequence, [message(0x27, 1, quantity=200)])
                        first = await events(a, 2)
                        assert time.monotonic() - changed_at >= .12
                        assert first[0]["state"]["data"]["bid"]["quantity"] == "200"
                        sequence = send(port, sequence, [message(0x27, 1, quantity=300)])
                        second = await events(a, 2)
                        assert second[0]["state"]["data"]["bid"]["quantity"] == "300"
                        slower = await events(b, 1)
                        assert slower[0]["state"]["data"]["bid"]["quantity"] == "300", slower
                        # Exact last-trade IDs cross the JS safe-integer boundary.
                        sequence = send(port, sequence, [message(0x23, 1, quantity=100, execution=2**63 + 555)])
                        execution = await events(a, 2)
                        assert execution[0]["state"]["data"]["last"]["execution_id"] == str(2**63 + 555)
                        # Below-top L2 change must not generate an L1 update.
                        sequence = send(port, sequence, [message(0x27, 3, quantity=222, price=99999)])
                        deep = await events(a, 1)
                        assert deep[0]["stream"] == "l2"
                        assert deep[0]["state"]["data"]["bids"][1]["quantity"] == "222"
                        try:
                            await asyncio.wait_for(a.recv(), .25)
                            raise AssertionError("Unchanged L1 was published")
                        except TimeoutError:
                            pass
                        # A bounded gap invalidates quotes/history; Unit Clear only repairs quotes.
                        sequence = send(port, sequence + 1, [message(0x27, 2, quantity=201, price=101000)])
                        dirty = await events(a, 2)
                        assert all(not e["state"]["book_current"] and e["state"]["loss_seen"] for e in dirty)
                        sequence = send(port, sequence, [message(0x97)])
                        cleared = await events(a, 2)
                        assert all(e["state"]["book_current"] and not e["state"]["trade_history_current"] for e in cleared)
                        assert cleared[0]["state"]["data"]["bid"] is None
                        assert cleared[0]["state"]["data"]["last"]["execution_id"] == str(2**63 + 555)
                        assert cleared[1]["state"]["data"] == {"bids": [], "asks": []}
                        stop(process)
                        final = json.loads(report.read_text())
                        assert final["monitoring_enabled"] and not final["benchmark_valid"]
                        stale = await events(a, 2)
                        assert all(e["source_stale"] for e in stale)
                    await ready(client, "/api/health", lambda health: health["subscribers"] == 0)
                    print(f"{implementation}: processor → UDP → throttler → Valkey → WebSocket passed", flush=True)
                # Reconnect snapshots are real; a broker interruption terminates the old subscription.
                async with connect(uri) as ws:
                    await ws.send(json.dumps({"l1": ["TEST"]}))
                    assert (await events(ws, 1))[0]["type"] == "snapshot"
                    stop(broker)
                    async with asyncio.timeout(10):
                        while True:
                            try:
                                event = json.loads(await ws.recv())
                                if event["type"] == "error":
                                    break
                            except Exception as e:
                                from websockets.exceptions import ConnectionClosed
                                if isinstance(e, ConnectionClosed):
                                    break
                                raise
                broker = start(valkey_command, "valkey-restarted")
                # /health reports the throttler's last periodic probe. It can
                # still be green briefly while a restarted broker is booting.
                await broker_ready()
                await ready(client, "/api/health")
                async with connect(uri) as ws:
                    await ws.send(json.dumps({"l1": ["TEST"]}))
                    assert (await events(ws, 1))[0]["type"] == "snapshot"
                print("Valkey interruption, cleanup and reconnect snapshot passed", flush=True)
        except BaseException:
            for handle in handles:
                handle.flush()
                handle.seek(0)
                print(f"--- {handle.name} ---\n{handle.read()[-8000:]}", file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                stop(process)
            for handle in handles:
                handle.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--valkey-server", required=True)
    parser.add_argument("--cpp", required=True)
    parser.add_argument("--rust", required=True)
    parser.add_argument("--browser", action="store_true", help="Use installed Playwright and /usr/bin/chromium")
    asyncio.run(main(parser.parse_args()))
