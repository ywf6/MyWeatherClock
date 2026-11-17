"""Loopback dashboard and in-memory view of the Xiaozhi gateway."""
from collections import deque
from contextlib import asynccontextmanager
from pathlib import Path
from time import time

from aiohttp import web

ASSETS = Path(__file__).with_name("web")


class Dashboard:
    def __init__(self, backend, model):
        self.backend = backend
        self.model = model if backend == "dashscope" else None
        self.started_at = time()
        self.devices = {}
        self.history = deque(maxlen=12)
        self.completed = 0

    def connect(self, session_id, label):
        self.devices[session_id] = {
            "label": label[:80],
            "phase": "idle",
            "tools_ready": False,
        }

    def update(self, session_id, **fields):
        if session_id in self.devices:
            self.devices[session_id].update(fields)

    def disconnect(self, session_id):
        self.devices.pop(session_id, None)

    def complete(self, session_id, answer, seconds, dt, screen):
        self.completed += 1
        self.history.appendleft({
            "id": self.completed,
            "answer": answer[:1600],
            "at": time(),
            "input_seconds": round(seconds, 2),
            "elapsed_seconds": round(dt, 2),
            "screen": screen,
        })
        self.update(session_id, phase="done")

    def snapshot(self):
        return {
            "backend": self.backend,
            "model": self.model,
            "started_at": self.started_at,
            "devices": list(self.devices.values()),
            "completed": self.completed,
            "history": list(self.history),
        }


@asynccontextmanager
async def serve_dashboard(dashboard, port):
    @web.middleware
    async def local_only(request, handler):
        if request.host.lower() not in {f"127.0.0.1:{port}", f"localhost:{port}"}:
            raise web.HTTPForbidden()
        response = await handler(request)
        response.headers["Cache-Control"] = "no-store"
        response.headers["Content-Security-Policy"] = "default-src 'self'; frame-ancestors 'none'; base-uri 'none'"
        return response

    async def status(_request):
        return web.json_response(dashboard.snapshot())

    async def index(_request):
        return web.FileResponse(ASSETS / "index.html")

    async def css(_request):
        return web.FileResponse(ASSETS / "dashboard.css")

    async def script(_request):
        return web.FileResponse(ASSETS / "dashboard.js")

    app = web.Application(middlewares=[local_only])
    app.add_routes([
        web.get("/", index),
        web.get("/dashboard.css", css),
        web.get("/dashboard.js", script),
        web.get("/api/status", status),
    ])
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    try:
        await web.TCPSite(runner, "127.0.0.1", port).start()
        yield
    finally:
        await runner.cleanup()
