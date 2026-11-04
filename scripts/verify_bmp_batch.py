"""主机验证 BMP 批读的行边界和上下方向；不替代板载 SD 性能测试。"""
import random


def verify(width, height, bpp, vw, vh, top_down, fallback=False):
    stride = ((width * bpp + 31) // 32) * 4
    ow, oh = min(width, vw), min(height, vh)
    cx, cy = (width - ow) // 2, (height - oh) // 2
    n = ow * (bpp // 8)
    pitch, batch = n, 1
    if stride <= 512 * 1024 and stride <= n * 2:
        pitch, batch = stride, min(128, oh, 512 * 1024 // stride)
    if fallback:
        pitch, batch = n, 1
    data = random.Random(width + bpp).randbytes(stride * height)
    output = [None] * oh
    cursor = -1
    reads = seeks = 0
    for y in range(oh):
        sy = cy + y if top_down else height - cy - oh + y
        off = sy * stride + cx * (bpp // 8)
        if y % batch == 0:
            rows = min(batch, oh - y)
            size = pitch * (rows - 1) + n
            if rows > 1 and y + rows < oh:
                size = pitch * rows
            assert size <= pitch * batch
            assert off + size <= len(data)
            seeks += off != cursor
            block = data[off:off + size]
            cursor = off + size
            reads += 1
        row = y if top_down else oh - 1 - y
        output[row] = block[(y % batch) * pitch:(y % batch) * pitch + n]
    # 独立按显示坐标生成参考，不使用批次读取结果。
    for row in range(oh):
        sy = cy + row if top_down else height - 1 - cy - row
        off = sy * stride + cx * (bpp // 8)
        assert output[row] == data[off:off + n]
    return reads, seeks


if __name__ == '__main__':
    cases = 0
    for bpp in (16, 24, 32):
        for width, height, vw, vh in ((1, 1, 8, 8), (17, 37, 16, 35),
                (1023, 65, 1024, 60), (1201, 67, 1024, 64),
                (4096, 35, 800, 32), (1024, 65, 1024, 65)):
            for top_down in (False, True):
                for fallback in (False, True):
                    verify(width, height, bpp, vw, vh, top_down, fallback)
                    cases += 1
    print('PASS: %d batch boundary/orientation/fallback cases' % cases)
    print('1201x67 BGR24, visible 1024x64: reads/seeks =',
          verify(1201, 67, 24, 1024, 64, False))
