"""UDP state collector, subscription control API and per-subscriber Valkey publisher."""
import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import json
import os
import re
import socket
import time

from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict, Field
import redis.asyncio as redis
import uvicorn

from .core import Reassembler, Throttler

PREFIX = "low_latency:monitor:"


def channel(subscriber):
    if not re.fullmatch(r"[a-f0-9]{32}", subscriber):
        raise HTTPException(400, "Invalid subscription ID")
    return PREFIX + subscriber


class Watch(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    l1: list[str] = Field(default_factory=list, max_length=256)
    l2: list[str] = Field(default_factory=list, max_length=256)
    interval_ms: int = Field(default=1000, ge=10, le=60000)


class Collector(asyncio.DatagramProtocol):
    def __init__(self, engine, assembler):
        self.engine, self.assembler = engine, assembler
        self.os_errors = 0

    def datagram_received(self, data, addr):
        if addr[0] != "127.0.0.1":
            return
        now = time.monotonic()
        result = self.assembler.feed(data, now)
        if result:
            self.engine.ingest(*result, now)

    def error_received(self, exc):
        self.os_errors += 1


def create_app(udp_port=8765, valkey_url=None):
    engine, assembler = Throttler(), Reassembler()
    broker = redis.from_url(valkey_url or os.environ.get("MONITOR_VALKEY_URL", "redis://127.0.0.1:6379/0"),
                            socket_connect_timeout=2, socket_timeout=2, decode_responses=True)
    healthy = False

    async def pump():
        nonlocal healthy
        next_probe = 0
        while True:
            now = time.monotonic()
            assembler.expire(now)
            try:
                if now >= next_probe:
                    await broker.ping()
                    healthy = True
                    next_probe = now + 1
                for subscriber, sub, key, value in engine.candidates(time.monotonic()):
                    message = engine.event(sub, key, value)
                    listeners = await broker.publish(channel(subscriber), json.dumps(message, separators=(",", ":")))
                    if not listeners:
                        engine.subscribers.pop(subscriber, None)
                        break
                    engine.delivered(sub, key, value, time.monotonic())
            except (redis.RedisError, OSError):
                # Pub/Sub cannot replay. Force clients to reconnect and snapshot.
                healthy = False
                engine.subscribers.clear()
                next_probe = time.monotonic() + 1
            await asyncio.sleep(0.01 if healthy else 0.2)

    @asynccontextmanager
    async def lifespan(app):
        nonlocal healthy
        await broker.ping()
        healthy = True
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.setblocking(False)
            sock.bind(("127.0.0.1", udp_port))
            transport, collector = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: Collector(engine, assembler), sock=sock)
        except BaseException:
            sock.close()
            await broker.aclose()
            raise
        app.state.udp_port = transport.get_extra_info("sockname")[1]
        app.state.engine = engine
        task = asyncio.create_task(pump())
        try:
            yield
        finally:
            transport.close()
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task
            await broker.aclose()

    app = FastAPI(title="Market monitor throttler", lifespan=lifespan)

    @app.get("/health")
    async def health():
        now = time.monotonic()
        fresh = sum(item[2] is not None and now - item[1] < engine.stale_after for item in engine.states.values())
        return JSONResponse({"broker_connected": healthy, "source": str(engine.source),
                             "symbols": sum(item[2] is not None for item in engine.states.values()),
                             "fresh_symbols": fresh, "subscribers": len(engine.subscribers),
                             "udp_invalid": assembler.invalid, "udp_incomplete": assembler.expired,
                             "udp_ignored": engine.ignored, "udp_port": app.state.udp_port}, status_code=200 if healthy else 503)

    @app.get("/symbols")
    async def symbols():
        now = time.monotonic()
        return [{"symbol": name, "source_stale": now - item[1] >= engine.stale_after,
                 "book_current": item[2]["book_current"], "loss_seen": item[2]["loss_seen"]}
                for name, item in sorted(engine.states.items()) if item[2] is not None]

    @app.post("/subscriptions/{subscriber}")
    async def subscribe(subscriber: str, watch: Watch):
        output = channel(subscriber)
        if not healthy:
            raise HTTPException(503, "Valkey unavailable")
        try:
            engine.subscribe(subscriber, watch.l1, watch.l2, watch.interval_ms, time.monotonic())
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        return {"id": subscriber, "channel": output, "lease_seconds": 30}

    @app.post("/subscriptions/{subscriber}/heartbeat")
    async def heartbeat(subscriber: str):
        channel(subscriber)
        sub = engine.subscribers.get(subscriber)
        if sub is None or time.monotonic() >= sub.expires or not healthy:
            raise HTTPException(404, "Subscription expired; reconnect for a snapshot")
        sub.expires = time.monotonic() + 30
        return {"ok": True}

    @app.delete("/subscriptions/{subscriber}")
    async def unsubscribe(subscriber: str):
        channel(subscriber)
        engine.subscribers.pop(subscriber, None)
        return {"ok": True}

    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--udp-port", type=int, default=8765)
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    uvicorn.run(create_app(args.udp_port), host="127.0.0.1", port=args.port, log_level="warning")


if __name__ == "__main__":
    main()
