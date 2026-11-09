"""修复 ArtInChip OTA 元数据 CRC，并按当前 OS-only 协议预检 CPIO。"""

import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

# 从脚本所在工程根目录加载与工具箱相同的 CPIO 白名单校验器。
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolbox" / "Source"))
from toolbox.Source.d133_ota_client import HARDWARE, PRODUCT, validate_cpio


def fix_crc_fields(data, version_override=None):
    """同步修复 ota_info 环境 CRC 和外层 CPIO 成员校验和。"""
    offset = 0
    while offset + 110 <= len(data):
        header = data[offset:offset + 110]
        if header[:6] != b"070702":
            raise ValueError("SDK CPIO 头无效")

        name_size = int(header[94:102], 16)
        file_size = int(header[54:62], 16)
        name_end = offset + 110 + name_size
        raw_name = data[offset + 110:name_end]
        file_start = (name_end + 3) & ~3
        file_end = file_start + file_size

        if raw_name.rstrip(b"\0") == b"ota_info.bin":
            if file_size != 512 or file_end > len(data):
                raise ValueError("ota_info.bin 长度不是 512 字节")

            text = bytes(data[file_start + 4:file_end]).replace(b"\0", b"\n")
            match = re.search(rb'version\s*=\s*"(\d+\.\d+\.\d+)";', text)
            if not match:
                raise ValueError("ota_info.bin 缺少有效版本号")
            current = match.group(1).decode("ascii")
            version = version_override or current
            if not re.fullmatch(r"\d+\.\d+\.\d+", version) or len(version) != len(current):
                raise ValueError("版本号必须为相同宽度的数字语义版本")

            if version_override:
                marker = b'version = "' + current.encode("ascii") + b'";'
                location = data.find(marker, file_start + 4, file_end)
                if location < 0:
                    raise ValueError("ota_info.bin 版本字段定位失败")
                begin = location + len(b'version = "')
                data[begin:begin + len(version)] = version.encode("ascii")

            # mkenvimage 的 CRC 覆盖四字节头之后的全部环境数据。
            struct.pack_into("<I", data, file_start, zlib.crc32(data[file_start + 4:file_end]) & 0xFFFFFFFF)
            # ArtInChip 的 CRC CPIO 解包器还会校验成员字节求和。
            checksum = sum(data[file_start:file_end]) & 0xFFFFFFFF
            data[offset + 102:offset + 110] = f"{checksum:08X}".encode("ascii")
            return version

        if raw_name.rstrip(b"\0") == b"TRAILER!!!":
            break
        offset = (file_end + 3) & ~3

    raise ValueError("CPIO 中没有找到 ota_info.bin")


def prepare(source, output, version=None):
    """生成最终 CPIO 与其公开校验元数据；不接触 Flash 或设备。"""
    data = bytearray(Path(source).read_bytes())
    actual_version = fix_crc_fields(data, version)
    if validate_cpio(data) != actual_version:
        raise ValueError("CPIO 预检返回的版本与元数据不一致")

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    metadata = {
        "product": PRODUCT,
        "hardware": HARDWARE,
        "version": actual_version,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "package": output.name,
        "signature": output.name + ".sig",
    }
    output.with_suffix(".json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(metadata, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="准备 D133CBS OS-only OTA CPIO")
    parser.add_argument("source", help="SDK 输出的 ota.cpio")
    parser.add_argument("output", help="版本化 CPIO 输出路径")
    parser.add_argument("--version", help="可选版本号，必须与环境字段等宽")
    args = parser.parse_args()
    prepare(args.source, args.output, args.version)

