"""WeatherClock test protocol. No GUI or hardware dependencies."""
import re

PAGES = {"BOOT": "正在启动", "HOME": "时钟主页", "FACE": "唤醒表情", "CHAT": "对话页"}


def redact(text):
    # Firmware debug output can contain Wi-Fi credentials and weather API keys.
    text = re.sub(r"(?i)AT\+CWJAP[^\r\n]*", "AT+CWJAP=<已隐藏配网参数>", text)
    text = re.sub(r"(?i)([?&](?:key|api_key|token)=)[^&\s\"']+", r"\1<已隐藏>", text)
    text = re.sub(r"(?i)(bearer\s+)\S+", r"\1<已隐藏>", text)
    return text


class LineDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.discard = False

    def feed(self, data):
        lines = []
        for byte in data:
            if byte == 10:
                if not self.discard:
                    lines.append(redact(self.buffer.decode("utf-8", "replace").rstrip("\r")))
                self.buffer.clear()
                self.discard = False
            elif not self.discard:
                self.buffer.append(byte)
                if len(self.buffer) > 4096:
                    self.buffer.clear()
                    self.discard = True
        return lines


def parse_frame(line):
    marker = line.rfind("@WC1 ")
    if marker < 0:
        return None
    tokens = line[marker + 5:].split()
    if not tokens or tokens[0] not in {"HELLO", "STATE", "ACK", "ERR"}:
        return None
    fields = dict(token.split("=", 1) for token in tokens[1:] if "=" in token)
    kind = tokens[0]
    if kind == "STATE":
        if fields.get("page") not in PAGES:
            return None
        try:
            for key in ("rev", "uptime_ms", "valid", "temp10", "humidity10", "outdoor10", "weather"):
                fields[key] = int(fields[key])
        except (ValueError, KeyError):
            return None
    return kind, fields


class FlowCheck:
    """Observe actual responses: HOME -> FACE -> CHAT -> HOME."""
    def __init__(self, now):
        self.stage = "initial"
        self.deadline = now + 5
        self.face_time = None
        self.elapsed_ms = None
        self.result = None

    def observe(self, page, now):
        if self.result:
            return None
        if self.stage == "initial" and page == "HOME":
            self.stage, self.deadline = "face", now + 5
            return "WAKE"
        if self.stage == "face" and page == "FACE":
            self.face_time = now
            self.stage, self.deadline = "chat", now + 3
        elif self.stage == "chat" and page == "CHAT":
            self.elapsed_ms = round((now - self.face_time) * 1000)
            self.stage, self.deadline = "home", now + 5
            return "HOME"
        elif self.stage == "home" and page == "HOME":
            self.result = "PASS"
        return None

    def check_timeout(self, now):
        if not self.result and now > self.deadline:
            self.result = "TIMEOUT:" + self.stage
        return self.result
