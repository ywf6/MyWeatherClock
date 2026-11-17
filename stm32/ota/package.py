# OTA 打包入口：同一应用生成首次烧录 HEX 与后续 .wcota 包，保证两者镜像一致。
# 本文件顶层只定义函数，实际写产物由命令行 main 调用 package(out) 完成。
"""Build the first-install HEX and the OTA file from the same application."""

from pathlib import Path
import re
import struct
import sys
import zlib

BASE = 0x08000000
LAYOUT = Path(__file__).with_name("layout.h").read_text(encoding="utf-8")
APP = int(re.search(r"#define WC_APP_BASE\s+(0x\w+)u", LAYOUT)[1], 16)
LIMIT = int(re.search(r"#define WC_APP_LIMIT\s+(0x\w+)u", LAYOUT)[1], 16)
RECORD = int(re.search(r"#define WC_APP_RECORD\s+(0x\w+)u", LAYOUT)[1], 16)
MAGIC = int(re.search(r"#define WC_OTA_MAGIC\s+(0x\w+)u", LAYOUT)[1], 16)
COOKIE = int(re.search(r"#define WC_COOKIE_ADDR\s+(0x\w+)u", LAYOUT)[1], 16)


# 与 Bootloader/Qt 对齐：检查初始 SP、Thumb 入口以及入口是否在应用镜像范围内。
def vectors_valid(data, base):
    if len(data) < 8:
        return False
    stack, reset = struct.unpack_from("<II", data)
    return (0x20000000 < stack <= COOKIE and stack % 8 == 0
            and reset & 1 and base + 8 <= reset < base + len(data))


def ihex_record(kind, address, data):
    raw = struct.pack(">BHB", len(data), address, kind) + data
    return ":" + (raw + bytes([-sum(raw) & 255])).hex().upper()


def write_hex(path, regions):
    with path.open("w", encoding="ascii") as out:
        upper = None
        for address, data in regions:
            offset = 0
            while offset < len(data):
                current = address + offset
                high, low = divmod(current, 65536)
                if high != upper:
                    print(ihex_record(4, 0, high.to_bytes(2, "big")), file=out)
                    upper = high
                part = data[offset:offset + min(16, 65536 - low)]
                print(ihex_record(0, low, part), file=out)
                offset += len(part)
        print(ihex_record(1, 0, b""), file=out)


# 先验证 Bootloader 和应用长度/向量，再写 12 字节小端包头 magic/size/CRC32 + 原始应用。
# 首次安装 HEX 额外包含 Bootloader、应用有效记录和应用镜像三个区域。
def package(out):
    boot = (out / "boot.bin").read_bytes()
    app = (out / "app" / "weatherclock-page-flow.bin").read_bytes()
    if not 512 <= len(app) <= LIMIT - APP or not vectors_valid(app, APP):
        raise ValueError("Application image size or vector table invalid")
    if len(boot) > RECORD - BASE or not vectors_valid(boot, BASE):
        raise ValueError("Bootloader image size or vector table invalid")
    header = struct.pack("<III", MAGIC, len(app), zlib.crc32(app))
    (out / "weatherclock.wcota").write_bytes(header + app)
    write_hex(out / "weatherclock-first-install.hex",
              [(BASE, boot), (RECORD, header + struct.pack("<I", 0)), (APP, app)])
    print(f"Bootloader {len(boot)} bytes, application {len(app)} bytes, CRC32 {zlib.crc32(app):08x}")


if __name__ == "__main__":
    package(Path(sys.argv[1]))
