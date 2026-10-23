"""PNG reading and writing with the standard library (zlib), for the banner's
pictures: 8-bit grey, grey+alpha, RGB, RGBA or palette images, not interlaced.
Pictures are (width, height, bytearray of RGBA rows, top row first)."""
import struct
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"


def write(path, width, height, rgba):
    assert len(rgba) == width * height * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgba[y * width * 4:(y + 1) * width * 4]

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def read_bytes(data):
    if data[:8] != SIGNATURE:
        raise ValueError("not a PNG")
    pos, idat, palette, trns = 8, bytearray(), None, None
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        length, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError("only 8-bit, non-interlaced PNGs are supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    out = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if ctype == 6: out += px
            elif ctype == 2: out += px + b"\xff"
            elif ctype == 0: out += bytes([px[0]] * 3) + b"\xff"
            elif ctype == 4: out += bytes([px[0]] * 3) + bytes([px[1]])
            else:
                i = px[0]
                a = trns[i] if trns is not None and i < len(trns) else 255
                out += palette[3 * i:3 * i + 3] + bytes([a])
    return width, height, out


def read(path):
    with open(path, "rb") as f:
        return read_bytes(f.read())
