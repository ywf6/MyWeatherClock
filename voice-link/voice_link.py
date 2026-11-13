# Python 联调入口，同时向 Qt 后台提供 model_input 和 render_chat。
# Qt 日常链路不执行本文件的 main()；直接运行本文件时，可通过 transport.Board 独立连接 C3。
"""Use the existing C3 audio assistant with the WeatherClock display."""
import argparse
import base64
from pathlib import Path
import sys
from time import monotonic, sleep

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host-console"))
from ai_text import render_chat
from protocol import parse_frame

# Seniverse weather codes from the original STM32 weather feed.
# https://docs.seniverse.com/api/start/code.html
SENIVERSE_CONDITIONS = (
    "晴", "晴", "晴", "晴", "多云", "晴间多云", "晴间多云", "大部多云", "大部多云", "阴",
    "阵雨", "雷阵雨", "雷阵雨伴有冰雹", "小雨", "中雨", "大雨", "暴雨", "大暴雨", "特大暴雨", "冻雨",
    "雨夹雪", "阵雪", "小雪", "中雪", "大雪", "暴雪", "浮尘", "扬沙", "沙尘暴", "强沙尘暴",
    "雾", "霾", "风", "大风", "飓风", "热带风暴", "龙卷风", "冷", "热",
)


# 把 STM32 valid 位对应的室内实测/西安室外实况加入模型上下文，区分数据来源。
# 语音输入组合文本上下文与 WAV；当前快照没有读数时间戳，不能据 valid 判断数据是否过期。
def model_input(question, snapshot):
    indoor = "室内 AHT20 温湿度暂无数据。"
    if snapshot["valid"] & 4:
        indoor = (f"室内 AHT20 实测 {snapshot['temp10'] / 10:.1f} 摄氏度，"
                  f"相对湿度 {snapshot['humidity10'] / 10:.1f}%。")
    outdoor = "西安室外天气实况暂无数据。"
    if snapshot["valid"] & 8:
        code = snapshot["weather"]
        condition = SENIVERSE_CONDITIONS[code] if 0 <= code < len(SENIVERSE_CONDITIONS) else "天气现象未知"
        outdoor = f"西安室外心知天气实况 {condition}，{snapshot['outdoor10'] / 10:.1f} 摄氏度。"
    context = (
        "以下是梅花板最近一次有效读数。仅在问题涉及天气或温湿度时参考。"
        "用户未指定城市时，室外天气按西安回答。区分室内实测和室外实况。"
        "询问天气或出行且有对应室外实况时，先报天气和温度，再给一条实用的出行建议。"
        "按室外温度建议穿衣，实况正在下雨时可建议携带雨具。"
        "其他城市、未来预报和降水概率没有数据时直接说明，勿从实况推断预报。\n"
        f"{indoor}\n{outdoor}"
    )
    if isinstance(question, str):
        return f"{context}\n\n用户问题：{question}"
    encoded = base64.b64encode(question).decode("ascii")
    return [
        {"type": "text", "text": context + "\n请回答后附录音中的问题。"},
        {"type": "input_audio", "input_audio": {"data": "data:audio/wav;base64," + encoded, "format": "wav"}},
    ]


def play_wake_reply(board):
    board.port.write(b"WAKE_REPLY\n")
    board.expect(b"DONE")


# Python 版本的 WC/@WC1/WCEND 请求应答封装，语义与 Qt 的 exchange 相同。
def exchange(board, command):
    board.port.write(f"WC @WC1 {command}\n".encode("ascii"))
    frames = []
    for _ in range(5):
        line = board.port.read_until(size=256)
        if not line.endswith(b"\n"):
            raise ValueError("外置 C3 未返回完整串口帧")
        if line.strip() == b"WCEND":
            break
        frame = parse_frame(line.decode("ascii", errors="replace"))
        if not frame:
            raise ValueError("外置 C3 返回了无法识别的串口帧")
        frames.append(frame)
    else:
        raise ValueError("外置 C3 回复缺少结束标记")
    for kind, fields in frames:
        if kind == "ERR":
            raise ValueError("梅花板通信失败 " + fields.get("reason", "UNKNOWN"))
    if not frames:
        raise ValueError("梅花板返回空回复")
    return frames


def status(board):
    [(kind, fields)] = exchange(board, "STATUS")
    if kind != "STATE":
        raise ValueError("梅花板未返回状态")
    return fields


def ack(board, command):
    [(kind, fields)] = exchange(board, command)
    if kind != "ACK" or fields.get("cmd") != command.split()[0]:
        raise ValueError("梅花板未确认命令 " + command.split()[0])
    return fields


def start_chat(board, from_key=False):
    current = status(board)
    if current["page"] == "CHAT":
        return current["rev"]
    if current["page"] == "HOME":
        if from_key:
            raise ValueError("按键唤醒已取消")
        ack(board, "WAKE")
    elif current["page"] != "FACE":
        raise ValueError("梅花板仍在启动")
    saw_face = current["page"] == "FACE"
    deadline = monotonic() + 4
    while monotonic() < deadline:
        current = status(board)
        if current["page"] == "CHAT":
            return current["rev"]
        if current["page"] == "FACE":
            saw_face = True
        elif current["page"] == "HOME" and saw_face:
            raise ValueError("唤醒已取消")
        sleep(.05)
    raise ValueError("梅花板没有进入对话页")


# 使用与 Qt 相同的 LCDSTART/LCDATA/LCDEND 协议，按 96 字节分块上传点阵。
def display(board, rev, question, answer):
    pixels = render_chat(question, answer).tobytes()
    # The transaction is sequential and protected by the STM32 page revision.
    transfer_id = rev
    ack(board, f"LCDSTART {transfer_id} {rev} {sum(pixels)}")
    for offset in range(0, len(pixels), 96):
        block = pixels[offset:offset + 96]
        reply = ack(board, f"LCDDATA {transfer_id} {offset} {block.hex()}")
        if reply.get("next") != str(offset + len(block)):
            raise ValueError("梅花板接收点阵时偏移量不一致")
    ack(board, f"LCDEND {transfer_id}")


# 在音频流中同时检查离线唤醒结果和 K2 标记，停止流并读完结束标记后才发送其他命令。
def wait_for_trigger(board, detector, packet_size, stop=None):
    board.port.write(b"WC_LISTEN\n")
    board.expect(b"STREAM")
    detector.reset()
    trigger = None
    keepalive_at = monotonic() + .5
    while True:
        if trigger is None and stop is not None and stop.is_set():
            trigger = "停止"
            board.port.write(b"!")
        if trigger is None and monotonic() >= keepalive_at:
            board.port.write(b".")
            keepalive_at = monotonic() + .5
        marker = board.port.read(1)
        if marker == b"\x00":
            if trigger is None:
                raise ValueError("开发板已停止发送唤醒音频")
            return None if trigger == "停止" else trigger
        if marker == b"\x02":
            detected = "K2"
        elif marker == b"\x01":
            pcm = board.port.read(packet_size)
            if len(pcm) != packet_size:
                raise ValueError("唤醒音频接收不完整")
            detected = "语音" if trigger is None and detector.feed(pcm) else None
        else:
            raise ValueError("开发板唤醒音频流已中断")
        if trigger is None and detected is not None:
            trigger = detected
            board.port.write(b"!")


def main():
    parser = argparse.ArgumentParser(description="外置 ESP32-C3 语音和梅花板联调")
    connection = parser.add_mutually_exclusive_group(required=True)
    connection.add_argument("--port", help="外置 C3 的 USB 串口")
    connection.add_argument("--host", help="外置 C3 的 Wi-Fi 地址")
    parser.add_argument("--tcp-port", type=int, default=8765)
    parser.add_argument("--voice-dir", type=Path, default=Path(__file__).with_name("voice-assets"))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--probe", action="store_true", help="串口、切页、显示和提示语测试")
    mode.add_argument("--text", help="使用文字发起一次真实问答并播报")
    args = parser.parse_args()
    voice_dir = args.voice_dir.resolve()
    sys.path.insert(0, str(voice_dir))
    from voice_console import LISTEN_BYTES, ask_cloud, level_speech, setting, wav_from_recording
    from transport import Board

    key = None if args.probe else setting("DASHSCOPE_API_KEY")
    if not args.probe and not key:
        raise ValueError("本机没有配置 DASHSCOPE_API_KEY")
    with Board(args.port, host=args.host, tcp_port=args.tcp_port) as board:
        hello = exchange(board, "HELLO")
        if not any(k == "HELLO" and "voice_uart" in f.get("caps", "").split(",") for k, f in hello):
            raise ValueError("请为梅花板烧录支持语音串口的 STM32 固件")
        if args.probe:
            rev = start_chat(board)
            display(board, rev, "外置 C3 联调", "串口与中文点阵接收正常。")
            play_wake_reply(board)
            print("梅花板确认接收点阵，C3 确认播放「我在」")
            return

        def turn(text, from_key=False):
            rev = start_chat(board, from_key=from_key)
            ack(board, f"THINK {rev}")
            label = text or "语音提问"
            try:
                if text is None:
                    play_wake_reply(board)
                    print("请说话，录音 5 秒", flush=True)
                    text = wav_from_recording(level_speech(board.record()))
                snapshot = status(board)
                if snapshot["page"] != "CHAT" or snapshot["rev"] != rev:
                    print("已离开对话页")
                    return
                answer, pcm = ask_cloud(key, model_input(text, snapshot))
            except ValueError as error:
                page = status(board)
                if page["page"] == "CHAT" and page["rev"] == rev:
                    display(board, rev, label, str(error))
                raise
            page = status(board)
            if page["page"] != "CHAT" or page["rev"] != rev:
                print("已离开对话页")
                return
            display(board, rev, label, answer or "已收到语音回答。")
            page = status(board)
            if pcm and page["page"] == "CHAT" and page["rev"] == rev:
                board.play(pcm)
            print(answer or "已收到语音回答。", flush=True)

        if args.text is not None:
            if not args.text.strip():
                raise ValueError("请输入问题")
            turn(args.text)
            return
        if not any(k == "HELLO" and "voice_key2" in f.get("caps", "").split(",") for k, f in hello):
            raise ValueError("请更新梅花板的 K2 唤醒固件")
        from weatherclock_wake import WAKE_PHRASE, WeatherClockWakeWord
        detector = WeatherClockWakeWord(voice_dir)
        while True:
            print(f"等待唤醒词「{WAKE_PHRASE}」或按 K2", flush=True)
            trigger = wait_for_trigger(board, detector, LISTEN_BYTES)
            print(f"{trigger}唤醒", flush=True)
            try:
                turn(None, from_key=trigger == "K2")
            except ValueError as error:
                print(error, file=sys.stderr, flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (ValueError, OSError) as error:
        sys.exit(str(error))
