"""Local WeatherClock relay for Alibaba Cloud Model Studio."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
import re
import secrets
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
from weather_display import XianWeather, make_bitmap

DEFAULT_BASE = "https://dashscope.aliyuncs.com/compatible-mode/v1"
MAX_QUESTION = 384
MAX_ANSWER = 480
JOB_LIMIT = 16
JOB_TTL = 180


def provider():
    key = os.getenv("DASHSCOPE_API_KEY", "").strip()
    if not key:
        raise ValueError("DASHSCOPE_API_KEY is not configured")
    model = os.getenv("WEATHERCLOCK_MODEL", "qwen-plus")
    base = os.getenv("DASHSCOPE_BASE_URL", DEFAULT_BASE).rstrip("/")
    url = urlsplit(base)
    if (url.scheme != "https" or not url.hostname or not url.hostname.endswith(".aliyuncs.com")
            or url.username or url.password or url.query or url.fragment):
        raise ValueError("DASHSCOPE_BASE_URL must be an Alibaba Cloud HTTPS API base URL")

    def ask(question, indoor=None, outdoor=None):
        context = {
            "indoor": None if indoor is None else {
                "source": "AHT20 实测", "temperature_c": indoor[0] / 10, "humidity_percent": indoor[1] / 10,
            },
            "outdoor": outdoor,
        }
        request = Request(
            base + "/chat/completions",
            data=json.dumps({
                "model": model,
                "messages": [
                    {"role": "system", "content": "你是天气时钟上的文字助手。用简体中文，尽量不超过100字。仅在相关时使用以下数据，区分室内实测和西安室外模型估算。缺失数据写明不可用，不要编造实况或预报。数据：" + json.dumps(context, ensure_ascii=False)},
                    {"role": "user", "content": question},
                ],
                "max_tokens": 180,
                "stream": False,
                "enable_thinking": False,
            }).encode("utf-8"),
            headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"},
        )
        try:
            with urlopen(request, timeout=35) as response:
                payload = json.loads(response.read(65536))
            answer = payload["choices"][0]["message"]["content"]
        except HTTPError as error:
            raise ValueError(f"Model API returned HTTP {error.code}") from None
        except (URLError, TimeoutError, ValueError, KeyError, IndexError, TypeError):
            raise ValueError("Model API did not return a valid answer") from None
        if not isinstance(answer, str) or not answer.strip():
            raise ValueError("Model API returned an empty answer")
        return answer

    return ask


class Relay(ThreadingHTTPServer):
    def __init__(self, address, ask, weather=None):
        super().__init__(address, Handler)
        self.device_key = secrets.token_hex(12)
        self.jobs = {}
        self.lock = threading.Lock()
        self.worker = ThreadPoolExecutor(max_workers=1)
        self.ask = ask
        self.weather = weather

    def reserve(self):
        with self.lock:
            now = time.monotonic()
            self.jobs = {key: job for key, job in self.jobs.items() if now - job[0] < JOB_TTL}
            if len(self.jobs) >= JOB_LIMIT:
                return None
            job_id = secrets.token_hex(8)
            self.jobs[job_id] = (now, "ready", "", None, None)
            return job_id

    def complete(self, job_id, question, indoor):
        outdoor = self.weather.get() if self.weather else None
        try:
            answer = " ".join(self.ask(question, indoor, outdoor).split())
            answer = answer[:160].encode("utf-8")[:MAX_ANSWER].decode("utf-8", errors="ignore")
            state = "done" if answer else "error"
        except ValueError as error:
            state, answer = "error", str(error)
        with self.lock:
            if job_id in self.jobs:
                self.jobs[job_id] = (time.monotonic(), state, answer, indoor, outdoor)

    def server_close(self):
        super().server_close()
        self.worker.shutdown(wait=False, cancel_futures=True)


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(5)
        super().setup()

    def log_message(self, *args):
        # The URL contains a temporary pairing code. Do not log it.
        pass

    def reply(self, status, message):
        data = message.encode("utf-8") if isinstance(message, str) else message
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8" if isinstance(message, str) else "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def route(self):
        parts = self.path.strip("/").split("/")
        if (len(parts) < 3 or parts[0] != "v1" or not parts[1].isascii()
                or not secrets.compare_digest(parts[1], self.server.device_key)):
            return []
        return parts[2:]

    def do_GET(self):
        if self.path == "/health":
            return self.reply(200, "weatherclock-ai")
        route = self.route()
        if route == ["new"]:
            job_id = self.server.reserve()
            return self.reply(200, job_id) if job_id else self.reply(503, "busy")
        if len(route) not in (2, 3) or route[0] != "jobs":
            return self.reply(404, "unknown endpoint")
        with self.server.lock:
            job = self.server.jobs.get(route[1])
        if not job:
            return self.reply(404, "unknown job")
        if len(route) == 3:
            if route[2] != "lcd" or job[1] != "done":
                return self.reply(404, "display not available")
            try:
                return self.reply(200, make_bitmap(job[2], job[3], job[4]))
            except (OSError, ImportError):
                return self.reply(503, "display font or Pillow unavailable")
        return self.reply(200, job[1] + "\n" + job[2])

    def do_POST(self):
        route = self.route()
        if len(route) != 2 or route[0] != "jobs":
            return self.reply(404, "unknown endpoint")
        raw_length = self.headers.get("Content-Length", "")
        if not re.fullmatch(r"[0-9]{1,3}", raw_length) or not 1 <= int(raw_length) <= MAX_QUESTION + 24:
            return self.reply(400, "invalid question length")
        try:
            size = int(raw_length)
            body = self.rfile.read(size)
            question = body.decode("utf-8")
        except (TimeoutError, UnicodeDecodeError):
            return self.reply(400, "invalid question")
        indoor = None
        if question.startswith("WC1 "):
            header, newline, question = question.partition("\n")
            match = re.fullmatch(r"WC1 (-?[0-9]{1,4}) ([0-9]{1,4})", header)
            if match:
                indoor = tuple(map(int, match.groups()))
                if not (-500 <= indoor[0] <= 1500 and 0 <= indoor[1] <= 1000):
                    return self.reply(400, "invalid sensor reading")
            elif header != "WC1 NA" or not newline:
                return self.reply(400, "invalid sensor header")
        question = question.strip()
        if len(body) != size or not question or len(question) > 96 or "\x00" in question:
            return self.reply(400, "invalid question")
        with self.server.lock:
            job = self.server.jobs.get(route[1])
            if job and job[1] == "ready":
                self.server.jobs[route[1]] = (time.monotonic(), "pending", "", None, None)
                self.server.worker.submit(self.server.complete, route[1], question, indoor)
        return self.reply(200, "accepted") if job else self.reply(404, "unknown job")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--mock", action="store_true")
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()
    bind = ipaddress.IPv4Address(args.listen)
    if not bind.is_loopback and not any(bind in ipaddress.ip_network(net) for net in (
            "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")):
        parser.error("Choose a loopback address or a specific LAN interface")
    try:
        ask = (lambda question, indoor=None, outdoor=None: "模拟回答：" + question) if args.mock else provider()
        if args.probe:
            print(ask("请只回复：通信测试成功"))
            return
    except ValueError as error:
        parser.exit(1, str(error) + "\n")
    with Relay((str(bind), args.port), ask, None if args.mock else XianWeather()) as server:
        print(f"Relay listening on {bind}:{server.server_port} ({'mock' if args.mock else 'Alibaba Cloud'})", flush=True)
        print(f"Board setup: /server {bind}:{server.server_port} {server.device_key}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
