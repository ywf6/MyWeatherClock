# WeatherClock 专用唤醒配置：复用 WakeWord 的 reset/feed，但加载项目自己的 wake_keywords.txt。
# 区分此包装类与 voice-assets/wake_word.py 的独立示例默认唤醒词；Qt 使用本类。
"""WeatherClock-specific keyword configuration for the existing detector."""
from pathlib import Path
import sys

from wake_word import WakeWord

KEYWORDS = Path(__file__).with_name("wake_keywords.txt")
WAKE_PHRASE = KEYWORDS.read_text(encoding="utf-8").strip().split("@", 1)[1]


class WeatherClockWakeWord(WakeWord):
    def __init__(self, voice_dir):
        model = voice_dir / "wake-model"
        files = [model / name for name in ("tokens.txt", "encoder.onnx", "decoder.onnx", "joiner.onnx")]
        if not all(path.is_file() for path in files):
            raise ValueError("离线唤醒模型不完整，请检查 ESP32VoiceAI 目录")
        sys.path.insert(0, str(voice_dir / "wake-runtime"))
        try:
            from sherpa_onnx import KeywordSpotter
        except ImportError as error:
            raise ValueError("无法加载唤醒组件，请使用本机 64 位 Python 3.13") from error
        tokens, encoder, decoder, joiner = map(str, files)
        self.detector = KeywordSpotter(
            tokens=tokens, encoder=encoder, decoder=decoder, joiner=joiner,
            keywords_file=str(KEYWORDS), num_threads=1,
        )
        self.reset()
