"""Xi'an weather context and the 240x320 display format."""

import json
from pathlib import Path
import time
from urllib.parse import urlencode
from urllib.request import urlopen


WEATHER_URL = "https://api.open-meteo.com/v1/forecast?" + urlencode({
    "latitude": 34.26,
    "longitude": 108.94,
    "current": "temperature_2m,relative_humidity_2m,weather_code,precipitation",
    "timezone": "Asia/Shanghai",
    "forecast_days": 1,
})
WEATHER_CODES = {
    0: "晴", 1: "大致晴朗", 2: "多云", 3: "阴", 45: "雾", 48: "雾凇",
    51: "毛毛雨", 53: "毛毛雨", 55: "毛毛雨", 56: "冻毛毛雨", 57: "冻毛毛雨",
    61: "小雨", 63: "中雨", 65: "大雨", 66: "冻雨", 67: "冻雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "雪粒",
    80: "阵雨", 81: "阵雨", 82: "强阵雨", 85: "阵雪", 86: "强阵雪",
    95: "雷暴", 96: "雷暴伴冰雹", 99: "雷暴伴冰雹",
}


class XianWeather:
    def __init__(self):
        self.last_read = float("-inf")
        self.cached = None

    def get(self):
        if time.monotonic() - self.last_read < 300:
            return self.cached
        try:
            with urlopen(WEATHER_URL, timeout=5) as response:
                data = json.loads(response.read(16384))["current"]
            current = {
                "city": "西安",
                "source": "Open-Meteo 模型估算",
                "time": data["time"],
                "temperature_c": data["temperature_2m"],
                "humidity_percent": data["relative_humidity_2m"],
                "precipitation_mm": data["precipitation"],
                "condition": WEATHER_CODES.get(data["weather_code"], "未知"),
            }
            if not isinstance(current["time"], str) or not isinstance(current["temperature_c"], (float, int)):
                raise ValueError("Incomplete current weather")
        except (OSError, ValueError, KeyError, TypeError):
            self.cached = None
            print("[WEATHER] Xi'an current conditions unavailable", flush=True)
        else:
            self.cached = current
            print(f"[WEATHER] Xi'an {current['time']} {current['temperature_c']} C, Open-Meteo", flush=True)
        self.last_read = time.monotonic()
        return self.cached


def make_bitmap(answer, indoor, outdoor):
    from PIL import Image, ImageDraw, ImageFont

    font_path = Path("C:/Windows/Fonts/msyh.ttc")
    font = ImageFont.truetype(str(font_path), 17)
    small = ImageFont.truetype(str(font_path), 14)
    image = Image.new("1", (240, 320))
    draw = ImageDraw.Draw(image)
    draw.text((10, 6), "WeatherClock AI", font=font, fill=1)
    indoor_label = "室内 AHT20 未读到"
    if indoor is not None:
        indoor_label = f"室内 {indoor[0] / 10:.1f} °C   {indoor[1] / 10:.1f}% RH"
    draw.text((10, 33), indoor_label, font=small, fill=1)
    if outdoor is None:
        draw.text((10, 54), "西安天气暂不可用", font=small, fill=1)
    else:
        label = f"西安 {outdoor['temperature_c']:.1f} °C  {outdoor['condition']}"
        draw.text((10, 54), label, font=small, fill=1)
        timestamp = outdoor["time"].replace("T", " ")
        draw.text((10, 74), f"{timestamp}  模型估算", font=small, fill=1)
    draw.line((10, 99, 229, 99), fill=1)
    lines, line = [], ""
    for char in answer:
        if draw.textlength(line + char, font=font) > 220:
            lines.append(line)
            line = ""
        line += char
    if line:
        lines.append(line)
    if len(lines) > 9:
        lines = lines[:9]
        while draw.textlength(lines[-1] + "…", font=font) > 220:
            lines[-1] = lines[-1][:-1]
        lines[-1] += "…"
    for index, line in enumerate(lines):
        draw.text((10, 107 + index * 23), line, font=font, fill=1)
    return image.tobytes()
