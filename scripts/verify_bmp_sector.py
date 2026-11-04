"""检查对齐读取布局和原图像素一致性；不代替目标 C 程序上板测试。"""
import argparse
import struct
from pathlib import Path

ALIGN = 4096


def window(data, offset, size):
    start = offset & ~(ALIGN - 1)
    end = min((offset + size + ALIGN - 1) & ~(ALIGN - 1), len(data))
    assert offset >= 0 and offset + size <= len(data)
    assert end - start <= size + 2 * ALIGN
    assert start % ALIGN == 0
    block = data[start:end]
    prefix = offset - start
    assert block[prefix:prefix + size] == data[offset:offset + size]
    return block, prefix


def check_bmp(path):
    data = Path(path).read_bytes()
    offset = struct.unpack_from('<I', data, 10)[0]
    w, signed_h = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    h = abs(signed_h)
    stride = ((w * bpp + 31) // 32) * 4
    assert data[:2] == b'BM' and bpp in (16, 24, 32)
    for vw, vh in ((1024, 600), (800, 1280), (31, 33)):
        ow, oh = min(w, vw), min(h, vh)
        cx, cy = (w - ow) // 2, (h - oh) // 2
        first = cy if signed_h < 0 else h - cy - oh
        visible = ow * (bpp // 8)
        pitch = stride if stride <= visible * 2 else visible
        batch = min(128, oh, 512 * 1024 // stride) if pitch == stride else 1
        for y in range(0, oh, batch):
            rows = min(batch, oh - y)
            pos = offset + (first + y) * stride + cx * (bpp // 8)
            size = pitch * (rows - 1) + visible
            block, prefix = window(data, pos, size)
            for row in range(rows):
                actual = block[prefix + row * pitch:prefix + row * pitch + visible]
                reference = data[pos + row * stride:pos + row * stride + visible]
                assert actual == reference
    print('PASS: original BMP pixel windows, full screen and cropped views')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('bmp')
    args = parser.parse_args()
    # 覆盖扇区首尾、跨界和 EOF，避免预读越界或偏移一字节。
    data = bytes(range(256)) * 48 + b'EOF'
    for offset in (0, 1, 54, 511, 512, 4095, 4096, 8191):
        for size in (1, 511, 512, 4096, len(data) - offset):
            if offset + size <= len(data):
                window(data, offset, size)
    check_bmp(args.bmp)
    print('PASS: sector edges, EOF and buffer capacity checks')
