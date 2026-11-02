#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""将 24 位 BMP Logo 顺时针旋转并生成 LVGL RGB565 C 资源。"""

import argparse
import struct
from pathlib import Path


def read_bmp_rgb565(path):
    """读取未压缩 24 位 BMP，输出正向横排的 RGB565 像素。"""
    data = Path(path).read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("Logo 必须是有效 BMP 文件")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError("Logo BMP 头格式不受支持")

    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bit_count = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if width <= 0 or signed_height == 0 or planes != 1:
        raise ValueError("Logo BMP 尺寸或平面数无效")
    if bit_count != 24 or compression != 0:
        raise ValueError("Logo 仅支持未压缩 24 位 BMP")

    height = abs(signed_height)
    row_stride = ((width * 3 + 3) // 4) * 4
    if pixel_offset + row_stride * height > len(data):
        raise ValueError("Logo BMP 像素数据不完整")

    # 顺时针旋转后，输出宽为原高、输出高为原宽。
    out_width = height
    out_height = width
    pixels = bytearray(out_width * out_height * 2)
    output_offset = 0

    for out_y in range(out_height):
        source_x = out_y
        for out_x in range(out_width):
            source_y = height - 1 - out_x
            file_row = height - 1 - source_y if signed_height > 0 else source_y
            source_offset = pixel_offset + file_row * row_stride + source_x * 3
            blue, green, red = data[source_offset:source_offset + 3]
            rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            pixels[output_offset] = rgb565 & 0xFF
            pixels[output_offset + 1] = rgb565 >> 8
            output_offset += 2

    # 按顶部工具栏宽高缩小，避免 Logo 撑高应用标题栏。
    target_width = 128
    target_height = 74
    scaled = bytearray(target_width * target_height * 2)
    for target_y in range(target_height):
        source_y = min(out_height - 1,
                       ((target_y * 2 + 1) * out_height) // (target_height * 2))
        for target_x in range(target_width):
            source_x = min(out_width - 1,
                           ((target_x * 2 + 1) * out_width) // (target_width * 2))
            source_offset = (source_y * out_width + source_x) * 2
            target_offset = (target_y * target_width + target_x) * 2
            scaled[target_offset:target_offset + 2] = pixels[source_offset:source_offset + 2]

    return target_width, target_height, bytes(scaled)


def write_lvgl_c(source, output):
    width, height, pixels = read_bmp_rgb565(source)
    rows = []
    for offset in range(0, len(pixels), 16):
        rows.append("    " + ", ".join("0x%02X" % byte for byte in pixels[offset:offset + 16]) + ",")

    content = [
        "/* 由 scripts/convert_hmi_logo.py 自动生成，请修改原始 BMP 后重新构建。 */",
        '#include "hmi_logo.h"',
        "",
        "static const uint8_t hmi_logo_pixels[] = {",
        *rows,
        "};",
        "",
        "const lv_image_dsc_t hmi_logo_image = {",
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_RGB565,",
        "        .flags = 0,",
        "        .w = %u," % width,
        "        .h = %u," % height,
        "        .stride = %u," % (width * 2),
        "        .reserved_2 = 0,",
        "    },",
        "    .data_size = %u," % len(pixels),
        "    .data = hmi_logo_pixels,",
        "    .reserved = NULL,",
        "};",
        "",
    ]
    with Path(output).open("w", encoding="utf-8", newline="\n") as out_file:
        out_file.write("\n".join(content))
    print("Logo 已生成：%u×%u RGB565，顺时针旋转 90 度" % (width, height))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="原始 24 位 BMP Logo")
    parser.add_argument("--output", required=True, help="输出 LVGL C 资源")
    args = parser.parse_args()
    write_lvgl_c(args.input, args.output)
