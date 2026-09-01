"""Writes minimal but spec-compliant uncompressed OpenEXR files.

Exists because ffmpeg's EXR muxer omits required header attributes (notably
pixelAspectRatio), which strict readers reject -- so it cannot be used to
generate test data for an EXR loader.

  python make_test_exr.py <out.exr> <width> <height> <depth|velocity>
"""
import struct
import sys


def write_exr(path, w, h, channels):
    """channels: {name: [float] * (w*h)}; written as 32-bit float, no compression."""
    def attr(name, type_name, data):
        return (name.encode() + b'\0' + type_name.encode() + b'\0' +
                struct.pack('<i', len(data)) + data)

    names = sorted(channels)

    # chlist: per channel  name\0, int32 pixelType(2=FLOAT), uint8 pLinear,
    # 3 reserved bytes, int32 xSampling, int32 ySampling.  Terminated by \0.
    chlist = b''
    for n in names:
        chlist += n.encode() + b'\0' + struct.pack('<iBxxxii', 2, 0, 1, 1)
    chlist += b'\0'

    hdr = b''
    hdr += attr('channels', 'chlist', chlist)
    hdr += attr('compression', 'compression', bytes([0]))          # NO_COMPRESSION
    hdr += attr('dataWindow', 'box2i', struct.pack('<iiii', 0, 0, w - 1, h - 1))
    hdr += attr('displayWindow', 'box2i', struct.pack('<iiii', 0, 0, w - 1, h - 1))
    hdr += attr('lineOrder', 'lineOrder', bytes([0]))              # INCREASING_Y
    hdr += attr('pixelAspectRatio', 'float', struct.pack('<f', 1.0))
    hdr += attr('screenWindowCenter', 'v2f', struct.pack('<ff', 0.0, 0.0))
    hdr += attr('screenWindowWidth', 'float', struct.pack('<f', 1.0))
    hdr += b'\0'

    row_bytes = w * 4 * len(names)
    offset = 8 + len(hdr) + 8 * h            # magic+version, header, offset table
    table = b''
    for _ in range(h):
        table += struct.pack('<q', offset)
        offset += 8 + row_bytes              # int32 y, int32 size, then the row

    body = b''
    for y in range(h):
        body += struct.pack('<ii', y, row_bytes)
        for n in names:
            body += struct.pack('<%df' % w, *channels[n][y * w:(y + 1) * w])

    with open(path, 'wb') as f:
        f.write(struct.pack('<II', 0x01312f76, 2) + hdr + table + body)


if __name__ == '__main__':
    out, w, h, kind = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    n = w * h
    if kind == 'depth':
        # Horizontal ramp 0.1 .. 10.0 -- float range a PNG could not hold.
        r = [0.1 + 9.9 * (i % w) / max(1, w - 1) for i in range(n)]
        write_exr(out, w, h, {'R': r})
    else:
        # Signed motion: +4 px right at the top, -4 px left at the bottom,
        # with a constant -2 px vertical. Negative values are the point.
        x = [8.0 * ((i // w) / max(1, h - 1)) - 4.0 for i in range(n)]
        y = [-2.0] * n
        write_exr(out, w, h, {'R': x, 'G': y})
    print('wrote', out)
