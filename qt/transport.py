# Python 设备传输适配：让 TCP 与 pyserial 提供相近的 read/read_until/write 接口。
# 文本命令按行读取，音频按明确长度读取；TCP 分段边界不属于应用协议。
"""串口与 TCP 共用的有界、完整读取接口。"""
import socket
import sys
from pathlib import Path
from time import monotonic

sys.path.insert(0, str(Path(__file__).with_name("voice-assets") / "pyserial.zip"))


# 维护剩余字节缓冲，避免一次 socket.recv 取出的后续数据丢失。
class TcpPort:
    def __init__(self, host, port, timeout=3):
        self.timeout = timeout
        self.socket = socket.create_connection((host, port), timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = bytearray()

    # 在超时内尽量凑齐 size 字节，超时可能返回短数据，调用方还需检查实际长度。
    def read(self, size=1):
        deadline = monotonic() + self.timeout
        while len(self.buffer) < size:
            remaining = deadline - monotonic()
            if remaining <= 0:
                break
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(max(4096, size - len(self.buffer)))
            except socket.timeout:
                break
            if not chunk:
                raise OSError("Wi-Fi 连接已断开，请重新连接")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def read_until(self, expected=b"\n", size=256):
        result = bytearray()
        deadline = monotonic() + self.timeout
        previous = self.timeout
        try:
            while len(result) < size and monotonic() < deadline:
                self.timeout = max(.001, deadline - monotonic())
                byte = self.read(1)
                if not byte:
                    break
                result.extend(byte)
                if result.endswith(expected):
                    break
        finally:
            self.timeout = previous
        return bytes(result)

    def write(self, data):
        self.socket.settimeout(self.timeout)
        self.socket.sendall(data)
        return len(data)

    def close(self):
        self.socket.close()


# 上下文管理器统一连接与关闭；建立连接后先 PING/VOICE1 握手。
class Board:
    def __init__(self, port=None, *, host=None, tcp_port=8765):
        self.endpoint = (port, host, tcp_port)
        self.port = None

    def __enter__(self):
        port, host, tcp_port = self.endpoint
        if host:
            self.port = TcpPort(host, tcp_port)
        else:
            import serial
            self.port = serial.Serial(baudrate=460800, timeout=3, write_timeout=3)
            self.port.port = port
            self.port.dtr = self.port.rts = False
            self.port.open()
            self.port.reset_input_buffer()
        try:
            self.port.write(b"PING\n")
            self.expect(b"VOICE1")
        except Exception:
            self.port.close()
            raise
        return self

    def __exit__(self, *exc):
        self.port.close()

    def expect(self, expected):
        received = self.port.read_until(size=256).strip()
        if received != expected:
            detail = received.decode("ascii", "replace") or "连接超时"
            raise ValueError(f"设备应答异常：{detail}")

    # 固定读取 5 秒 PCM16，先读 PCM 长度头，后读二进制，再检查 DONE。
    def record(self):
        size = 5 * 16000 * 2
        self.port.write(b"REC\n")
        self.expect(f"PCM {size}".encode())
        previous = self.port.timeout
        self.port.timeout = 8
        try:
            pcm = self.port.read(size)
        finally:
            self.port.timeout = previous
        if len(pcm) != size:
            raise ValueError("录音接收不完整，请重新连接")
        self.expect(b"DONE")
        return pcm

    def play(self, pcm):
        if not pcm or len(pcm) % 2 or len(pcm) > 30 * 16000 * 2:
            raise ValueError("播报音频长度异常")
        self.port.write(f"PLAY {len(pcm) // 2}\n".encode())
        self.expect(b"READY")
        for offset in range(0, len(pcm), 3840):
            self.port.write(pcm[offset:offset + 3840])
            self.expect(b"OK")
        self.expect(b"DONE")
