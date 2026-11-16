"""HTTP UI/API and a real Valkey subscription for each browser WebSocket."""
import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import json
import os
from pathlib import Path
import uuid
from urllib.parse import urlsplit

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import httpx
import redis.asyncio as redis
import uvicorn

from .throttler import Watch, channel


def create_app(throttler_url=None, valkey_url=None):
    static = Path(__file__).parent / "static"

    @asynccontextmanager
    async def lifespan(app):
        app.state.http = httpx.AsyncClient(base_url=throttler_url or os.environ.get("MONITOR_THROTTLER_URL", "http://127.0.0.1:8766"), timeout=3, trust_env=False)
        app.state.broker = redis.from_url(valkey_url or os.environ.get("MONITOR_VALKEY_URL", "redis://127.0.0.1:6379/0"),
                                         socket_connect_timeout=2, decode_responses=True)
        try:
            await app.state.broker.ping()
            yield
        finally:
            await app.state.http.aclose()
            await app.state.broker.aclose()

    app = FastAPI(title="PITCH market monitor", lifespan=lifespan)
    app.mount("/static", StaticFiles(directory=static), name="static")

    @app.get("/")
    async def index():
        return FileResponse(static / "index.html")

    async def upstream(path):
        try:
            response = await app.state.http.get(path)
            response.raise_for_status()
            return response.json()
        except (httpx.HTTPError, ValueError) as e:
            raise HTTPException(503, "Throttler unavailable") from e

    @app.get("/api/symbols")
    async def symbols():
        return await upstream("/symbols")

    @app.get("/api/health")
    async def health():
        return await upstream("/health")

    @app.websocket("/ws")
    async def stream(ws: WebSocket):
        origin = ws.headers.get("origin")
        if origin and urlsplit(origin).netloc != ws.headers.get("host"):
            await ws.close(code=1008)
            return
        await ws.accept()
        subscriber = uuid.uuid4().hex
        pubsub = app.state.broker.pubsub()
        tasks = []
        registered = False
        try:
            request = await asyncio.wait_for(ws.receive_json(), 10)
            watch = Watch.model_validate(request)
            # Wait for broker ACK before asking the throttler to publish the
            # snapshot. subscribe() alone writes the command without reading ACK.
            await pubsub.subscribe(channel(subscriber))
            async def acknowledgement():
                while True:
                    message = await pubsub.get_message(ignore_subscribe_messages=False, timeout=1)
                    if message and message["type"] == "subscribe":
                        return
            await asyncio.wait_for(acknowledgement(), 3)
            response = await app.state.http.post("/subscriptions/" + subscriber, json=watch.model_dump())
            response.raise_for_status()
            registered = True
            await ws.send_json({"type": "subscribed", "id": subscriber, "interval_ms": watch.interval_ms})

            async def forward():
                expected = 1
                while True:
                    message = await pubsub.get_message(ignore_subscribe_messages=True, timeout=1)
                    if message is None:
                        continue
                    if message["type"] != "message":
                        continue
                    event = json.loads(message["data"])
                    if event["seq"] != expected:
                        raise RuntimeError("Pub/Sub gap; reconnect for a snapshot")
                    expected += 1
                    # A slow browser is disconnected, then snapshots on reconnect.
                    await asyncio.wait_for(ws.send_json(event), 2)

            async def renew():
                while True:
                    await asyncio.sleep(5)
                    response = await app.state.http.post("/subscriptions/" + subscriber + "/heartbeat")
                    response.raise_for_status()
                    await asyncio.wait_for(ws.send_json({"type": "heartbeat"}), 2)

            async def disconnected():
                while True:
                    await ws.receive_text()

            tasks = [asyncio.create_task(coro()) for coro in (forward, renew, disconnected)]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except WebSocketDisconnect:
            pass
        except (ValueError, TypeError, httpx.HTTPError, redis.RedisError, OSError, RuntimeError, TimeoutError):
            with suppress(RuntimeError, WebSocketDisconnect, OSError):
                await asyncio.wait_for(ws.send_json({"type": "error", "message": "Stream interrupted or invalid subscription. Reconnect to obtain a fresh snapshot."}), 2)
        finally:
            for task in tasks:
                task.cancel()
            for task in tasks:
                with suppress(asyncio.CancelledError, Exception):
                    await task
            if registered:
                with suppress(httpx.HTTPError):
                    await app.state.http.delete("/subscriptions/" + subscriber)
            await pubsub.aclose()
            with suppress(RuntimeError, WebSocketDisconnect, OSError):
                await ws.close(code=1011)

    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8767)
    args = parser.parse_args()
    uvicorn.run(create_app(), host=args.host, port=args.port, log_level="warning", ws_max_size=65536, ws_max_queue=16)


if __name__ == "__main__":
    main()
