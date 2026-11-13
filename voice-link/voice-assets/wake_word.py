# 电脑端离线关键词检测基类：使用包内 sherpa-onnx 和 ONNX 模型。
# 输入为 16 kHz PCM16；WeatherClockWakeWord 会替换模型/关键词初始化，复用本类流式处理逻辑。
"""用中英双语模型在电脑上检测唤醒词。"""

from array import array
from pathlib import Path
import sys


WAKE_PHRASE = "你好老大"


class WakeWord:
    def __init__(self):
        directory = Path(__file__).resolve().parent
        model = directory / "wake-model"
        keywords = directory / "wake_keywords.txt"
        files = [model / name for name in ("tokens.txt", "encoder.onnx", "decoder.onnx", "joiner.onnx")]
        if not all(path.is_file() for path in [*files, keywords]):
            raise ValueError("离线唤醒模型不完整，请检查语音助手目录")
        runtime = str(directory / "wake-runtime")
        if runtime not in sys.path:
            sys.path.insert(0, runtime)
        try:
            from sherpa_onnx import KeywordSpotter
        except ImportError as exc:
            raise ValueError("无法加载离线唤醒组件，请使用本机的 Python 3.13（64 位）") from exc
        tokens, encoder, decoder, joiner = map(str, files)
        self.detector = KeywordSpotter(
            tokens=tokens, encoder=encoder, decoder=decoder, joiner=joiner,
            keywords_file=str(keywords), num_threads=1,
        )
        self.reset()

    # 新建识别流并清除增益历史，一次唤醒或对话后调用以避免旧音频状态残留。
    def reset(self):
        self.stream = self.detector.create_stream()
        self.peak = 0.0

    # 转换 PCM、调节增益并送入识别流；每次输入可用数据后持续解码，命中关键词则返回 True。
    def feed(self, pcm):
        samples = array("h")
        samples.frombytes(pcm)
        # 提高小信号电平，遇到较响的声音时降低增益，防止削波。
        self.peak = max(max(map(abs, samples)), self.peak * 0.98)
        gain = min(16.0, 24000 / max(self.peak, 1.0)) / 32768
        waveform = [sample * gain for sample in samples]
        self.stream.accept_waveform(16000, waveform)
        while self.detector.is_ready(self.stream):
            self.detector.decode_stream(self.stream)
            if self.detector.get_result(self.stream):
                return True
        return False
