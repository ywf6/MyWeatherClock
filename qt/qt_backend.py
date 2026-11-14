# Qt 的 Python 后台入口：标准输入/输出是与 Qt 约定的协议通道。
# 普通模式读一行 JSON，输出回答、Base64 点阵和 PCM；--wake 模式则长期接收二进制唤醒帧。
# 设备串口/TCP 由 Qt 持有，这里不打开开发板端口；调试输出应写 stderr，避免破坏 stdout 协议。
"""原生 Qt 的语音后台：只负责模型与图像，不访问设备端口。"""
import argparse
import base64
import json
from pathlib import Path
import sys

from voice_link import render_chat, model_input


def encode(data):
    return base64.b64encode(data).decode("ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--voice-dir", type=Path, required=True)
    parser.add_argument("--wake", action="store_true")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stdin.reconfigure(encoding="utf-8")
    sys.path.insert(0, str(args.voice_dir.resolve()))
    # 唤醒协议：R=重置识别流，P=后跟 480 字节 PCM；stdout 仅发送 READY/RESET/WAKE 控制行。
    if args.wake:
        from weatherclock_wake import WeatherClockWakeWord
        detector = WeatherClockWakeWord(args.voice_dir.resolve())
        print("READY", flush=True)
        while marker := sys.stdin.buffer.read(1):
            if marker == b"R":
                detector.reset()
                print("RESET", flush=True)
            elif marker == b"P":
                pcm = sys.stdin.buffer.read(480)
                if len(pcm) != 480:
                    raise ValueError("唤醒音频包不完整")
                if detector.feed(pcm):
                    print("WAKE", flush=True)
            else:
                raise ValueError("唤醒控制帧无效")
        return
    # 普通问答模式：Qt 传入输入内容和 STM32 状态快照，模型调用在本进程中完成。
    request = json.loads(sys.stdin.readline())
    question = request.get("question", "")
    pcm = b""
    if request["action"] == "render":
        answer = request["answer"]
    elif request["action"] == "ask":
        if not (args.voice_dir / "voice_console.py").is_file():
            raise ValueError("请选择包含 voice_console.py 的 ESP32VoiceAI 目录")
        from voice_console import ask_cloud, setting, wav_from_recording, level_speech
        key = setting("DASHSCOPE_API_KEY")
        if not key:
            raise ValueError("本机未配置 DASHSCOPE_API_KEY")
        if request.get("recording"):
            question = wav_from_recording(level_speech(base64.b64decode(request["recording"], validate=True)))
        answer, pcm = ask_cloud(key, model_input(question, request["snapshot"]))
        answer = answer or "已收到语音回答。"
    else:
        raise ValueError("未知语音操作")
    label = question if isinstance(question, str) and question else "语音提问"
    # 把中文问答渲染为 216x208 单色图片，输出恰好 5616 字节的原始点阵。
    pixels = render_chat(label, answer).tobytes()
    print(json.dumps({"ok": True, "answer": answer, "pcm": encode(pcm), "bitmap": encode(pixels)}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False), flush=True)
        sys.exit(1)
