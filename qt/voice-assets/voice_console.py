# 电脑端音频与百炼接口，同时保留独立 Tk 调试界面。
# Qt 主链路主要复用 setting、wav_from_recording、level_speech 和 ask_cloud，不启动这里的 Tk 窗口。
# API 密钥从本机配置读取；离线唤醒在电脑上执行，云端模型负责本轮语音/文字问答。
"""ESP32-C3 语音助手的 Windows 上位机。"""

import array
import base64
import binascii
import io
import json
import math
import os
from pathlib import Path
import queue
import sys
import threading
from time import monotonic
import tkinter as tk
from tkinter import ttk
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import wave
import winreg

sys.path.insert(0, str(Path(__file__).with_name("pyserial.zip")))
import serial
from wake_word import WAKE_PHRASE, WakeWord


RATE = 16000
CLOUD_RATE = 24000
CAPTURE_BYTES = 5 * RATE * 2
LISTEN_BYTES = 240 * 2
PACKET_BYTES = 1920 * 2
MAX_PLAYBACK_SECONDS = 30
DEFAULT_BASE = "https://dashscope.aliyuncs.com/compatible-mode/v1"
MODEL = "qwen3.5-omni-plus"


class Board:
    def __init__(self, port):
        self.port = serial.Serial(baudrate=460800, timeout=3, write_timeout=3)
        self.port.port = port
        self.port.dtr = False
        self.port.rts = False

    def __enter__(self):
        self.port.open()
        try:
            self.port.reset_input_buffer()
            self.port.write(b"PING\n")
            self.expect(b"VOICE1")
        except Exception:
            self.port.close()
            raise
        return self

    def __exit__(self, *exc):
        self.port.close()

    def expect(self, expected):
        received = self.port.read_until(size=120).strip()
        if received != expected:
            detail = received.decode("ascii", errors="replace") or "串口超时"
            raise ValueError(f"开发板应答异常（{detail}）")

    def record(self):
        self.port.write(b"REC\n")
        self.expect(f"PCM {CAPTURE_BYTES}".encode("ascii"))
        self.port.timeout = 8
        try:
            pcm = self.port.read(CAPTURE_BYTES)
        finally:
            self.port.timeout = 3
        if len(pcm) != CAPTURE_BYTES:
            raise ValueError(f"录音接收不完整（{len(pcm)}/{CAPTURE_BYTES} 字节）")
        self.expect(b"DONE")
        return pcm

    def play(self, pcm):
        if not pcm or len(pcm) % 2 or len(pcm) > RATE * 2 * MAX_PLAYBACK_SECONDS:
            raise ValueError("播报音频长度异常")
        self.port.write(f"PLAY {len(pcm) // 2}\n".encode("ascii"))
        self.expect(b"READY")
        for start in range(0, len(pcm), PACKET_BYTES):
            self.port.write(pcm[start:start + PACKET_BYTES])
            self.expect(b"OK")
        self.expect(b"DONE")

    def listen_packet(self):
        marker = self.port.read(1)
        if marker == b"\x00":
            return None
        if marker != b"\x01":
            raise ValueError("开发板唤醒音频流已中断")
        pcm = self.port.read(LISTEN_BYTES)
        if len(pcm) != LISTEN_BYTES:
            raise ValueError("唤醒音频接收不完整")
        return pcm

    def wait_for_wake(self, detector, stop):
        self.port.write(b"LISTEN\n")
        self.expect(b"STREAM")
        detector.reset()
        keepalive_at = monotonic() + 0.5
        woke = False
        while not stop.is_set():
            now = monotonic()
            if now >= keepalive_at:
                self.port.write(b".")
                keepalive_at = now + 0.5
            pcm = self.listen_packet()
            if pcm is None:
                raise ValueError("开发板已停止发送唤醒音频")
            if detector.feed(pcm):
                woke = True
                break
        self.port.write(b"!")
        while self.listen_packet() is not None:
            pass
        return woke and not stop.is_set()


def setting(name, default=""):
    value = os.getenv(name, "").strip()
    if value:
        return value
    for root, path in (
        (winreg.HKEY_CURRENT_USER, "Environment"),
        (winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
    ):
        try:
            with winreg.OpenKey(root, path) as key:
                value = winreg.QueryValueEx(key, name)[0].strip()
                if value:
                    return value
        except OSError:
            pass
    return default


# 将单声道 16 kHz PCM16 转为 WAV，并做直流偏移和增益处理，供云端识别。
def wav_from_recording(pcm):
    samples = array.array("h")
    samples.frombytes(pcm)
    # 去直流偏移并提高小信号的音量，限制最大增益以减少底噪放大。
    center = sum(samples) // len(samples)
    peak = max(abs(sample - center) for sample in samples)
    if peak < 2:
        raise ValueError("录音没有有效信号，请靠近麦克风说话并检查接线")
    gain = min(24.0, 22000 / peak)
    adjusted = array.array("h", (int((sample - center) * gain) for sample in samples))
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        output.writeframes(adjusted.tobytes())
    return buffer.getvalue()


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


# 把当前云端返回的 24 kHz PCM16 重采样到 C3 播放使用的 16 kHz，并限制最大音频时长。
def resample_answer(pcm):
    # 百炼流式音频解码后是 24 kHz 单声道 PCM16。
    if len(pcm) < 4 or len(pcm) % 2 or len(pcm) > CLOUD_RATE * 2 * MAX_PLAYBACK_SECONDS:
        raise ValueError("百炼返回的 PCM 音频长度异常")
    samples = array.array("h")
    samples.frombytes(pcm)
    resampled = array.array("h")
    for i in range(len(samples) * RATE // CLOUD_RATE):
        index, fraction = divmod(i * CLOUD_RATE, RATE)
        following = min(index + 1, len(samples) - 1)
        resampled.append((samples[index] * (RATE - fraction) + samples[following] * fraction) // RATE)
    return resampled.tobytes()


# 解析 SSE data 行，累积文本、转写和 Base64 音频，收到 DONE 后统一解码。
# 当前完整回答收齐后才交回上位机播放，并未边生成边播报。
def read_answer(lines):
    text, transcripts, chunks = [], [], []
    finished = False
    for line in lines:
        if not line.startswith(b"data:"):
            continue
        event = line[5:].strip()
        if event == b"[DONE]":
            finished = True
            break
        payload = json.loads(event)
        if "error" in payload:
            raise ValueError("百炼中止了回答")
        for choice in payload.get("choices", []):
            delta = choice.get("delta", {})
            speech = delta.get("audio") or {}
            if delta.get("content"):
                text.append(delta["content"])
            if speech.get("transcript"):
                transcripts.append(speech["transcript"])
            if speech.get("data"):
                chunks.append(speech["data"])
    if not finished or not chunks:
        raise ValueError("百炼未返回完整的语音回答")
    try:
        audio = base64.b64decode("".join(chunks), validate=True)
    except binascii.Error:
        # 兼容每段音频独立进行 Base64 填充的响应。
        audio = b"".join(base64.b64decode(chunk, validate=True) for chunk in chunks)
    return "".join(text or transcripts).strip(), resample_answer(audio)


# 校验阿里云 HTTPS 地址并发起流式模型请求；超时/HTTP 错误转为可显示异常。
def ask_cloud(key, question):
    base = setting("DASHSCOPE_BASE_URL", DEFAULT_BASE).rstrip("/")
    url = urlsplit(base)
    if (url.scheme != "https" or not url.hostname or not url.hostname.endswith(".aliyuncs.com")
            or url.username or url.password or url.query or url.fragment):
        raise ValueError("百炼接口地址必须是阿里云的 HTTPS 地址")
    if isinstance(question, bytes):
        encoded = base64.b64encode(question).decode("ascii")
        question = [{"type": "input_audio", "input_audio": {
            "data": "data:audio/wav;base64," + encoded, "format": "wav",
        }}]
    body = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": "你是语音助手。请用简体中文直接回答用户，最多两句。若听不清，请让用户再说一遍。"},
            {"role": "user", "content": question},
        ],
        "modalities": ["text", "audio"],
        "audio": {"voice": "Tina", "format": "wav"},
        "stream": True,
    }
    request = Request(base + "/chat/completions", data=json.dumps(body).encode("utf-8"), headers={
        "Authorization": "Bearer " + key, "Content-Type": "application/json", "Accept": "text/event-stream",
    })
    try:
        with build_opener(NoRedirect()).open(request, timeout=45) as response:
            return read_answer(response)
    except HTTPError as exc:
        raise ValueError(f"百炼请求失败（HTTP {exc.code}）") from None
    except (URLError, TimeoutError):
        raise ValueError("无法连接百炼，或等待语音回答超时") from None


def level_speech(pcm):
    samples = array.array("h")
    samples.frombytes(pcm)
    rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
    gain = min(3.0, max(1.0, 10000 / max(rms, 1.0)))
    adjusted = array.array("h")
    for sample in samples:
        level = sample * gain
        peak = abs(level)
        if peak > 20000:
            # 平滑压缩大峰值，避免把放大后的 PCM 硬截在最大值。
            level = math.copysign(20000 - 10000 * math.expm1(-(peak - 20000) / 10000), level)
        adjusted.append(int(level))
    return adjusted.tobytes()


def wake_tone():
    frames = RATE // 10
    return array.array("h", (
        int(14000 * math.sin(math.pi * i / frames) * math.sin(math.tau * 880 * i / RATE))
        for i in range(frames)
    )).tobytes()


def main():
    root = tk.Tk()
    root.title("ESP32-C3 语音助手")
    root.minsize(530, 320)
    frame = ttk.Frame(root, padding=18)
    frame.pack(fill="both", expand=True)
    port = tk.StringVar(value="COM7")
    wake_label = f"开启「{WAKE_PHRASE}」唤醒"
    status = tk.StringVar(value=f"可开启「{WAKE_PHRASE}」唤醒，也可以手动录音或输入文字。")
    row = ttk.Frame(frame)
    row.pack(fill="x")
    ttk.Label(row, text="开发板串口").pack(side="left")
    ttk.Entry(row, textvariable=port, width=9).pack(side="left", padx=8)
    ttk.Label(frame, text="录音约 5 秒，由电脑提交给阿里云百炼。回答由开发板喇叭播放。", wraplength=490).pack(anchor="w", pady=10)
    question = ttk.Entry(frame)
    question.pack(fill="x")
    actions = ttk.Frame(frame)
    actions.pack(fill="x", pady=10)
    ttk.Label(frame, textvariable=status, wraplength=490).pack(anchor="w", pady=(0, 8))
    answer = tk.Text(frame, height=5, wrap="word", state="disabled", relief="flat")
    answer.pack(fill="both", expand=True)
    events = queue.Queue()
    wake_stop = None

    def show_answer(value):
        answer.configure(state="normal")
        answer.delete("1.0", "end")
        answer.insert("end", value)
        answer.configure(state="disabled")

    def run_turn(board, prompt, key, stop):
        if prompt is None:
            events.put(("status", "正在录音，请对着麦克风说话（约 5 秒）…"))
            prompt = wav_from_recording(board.record())
        if stop is not None and stop.is_set():
            return
        events.put(("status", "已提交，等待百炼生成语音…"))
        reply, pcm = ask_cloud(key, prompt)
        if stop is not None and stop.is_set():
            return
        events.put(("answer", reply or "已收到语音回答。"))
        events.put(("status", "正在通过开发板播报…"))
        board.play(level_speech(pcm))

    def run(board_port, prompt, key, stop):
        try:
            detector = WakeWord() if stop is not None else None
            with Board(board_port) as board:
                if detector is None:
                    run_turn(board, prompt, key, stop)
                else:
                    while not stop.is_set():
                        events.put(("status", f"本地待唤醒，请说「{WAKE_PHRASE}」。"))
                        if not board.wait_for_wake(detector, stop):
                            break
                        events.put(("status", "已唤醒，提示音后请说问题。"))
                        board.play(wake_tone())
                        if not stop.is_set():
                            run_turn(board, None, key, stop)
            events.put(("done", "已停止唤醒。" if stop is not None else "播报完成。"))
        except (ValueError, EOFError, OSError, RuntimeError, wave.Error) as exc:
            events.put(("done", f"失败：{exc}"))

    def start(mode):
        nonlocal wake_stop
        if mode == "wake" and wake_stop is not None:
            wake_stop.set()
            wake_button.configure(state="disabled")
            status.set("正在停止唤醒…")
            return
        key = setting("DASHSCOPE_API_KEY")
        if not key:
            status.set("本机未配置 DASHSCOPE_API_KEY，请配置后重试。")
            return
        prompt = question.get().strip() if mode == "text" else None
        if prompt == "":
            status.set("请先输入问题。")
            return
        if not port.get().strip():
            status.set("请输入开发板串口号，例如 COM7。")
            return
        for button in buttons:
            button.configure(state="disabled")
        if mode == "wake":
            wake_stop = threading.Event()
            wake_button.configure(text="停止唤醒")
        else:
            wake_button.configure(state="disabled")
        show_answer("")
        status.set("正在连接开发板…")
        threading.Thread(target=run, args=(port.get().strip(), prompt, key, wake_stop), daemon=True).start()

    def poll():
        nonlocal wake_stop
        while not events.empty():
            kind, value = events.get_nowait()
            if kind == "answer":
                show_answer(value)
            else:
                status.set(value)
            if kind == "done":
                for button in buttons:
                    button.configure(state="normal")
                wake_stop = None
                wake_button.configure(state="normal", text=wake_label)
        root.after(50, poll)

    buttons = (
        ttk.Button(actions, text="录音问 AI", command=lambda: start("voice")),
        ttk.Button(actions, text="文字问 AI", command=lambda: start("text")),
    )
    for button in buttons:
        button.pack(side="left", padx=(0, 8))
    wake_button = ttk.Button(actions, text=wake_label, command=lambda: start("wake"))
    wake_button.pack(side="left")

    root.after(50, poll)
    root.mainloop()


if __name__ == "__main__":
    main()
