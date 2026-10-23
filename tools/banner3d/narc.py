"""Read one member of a Nitro archive (NARC: BTAF file table, BTNF names, GMIF data)."""
import struct


def member(data, index):
    if data[:4] != b"NARC":
        raise ValueError("not a NARC")
    pos = struct.unpack_from("<H", data, 12)[0]          # header size
    chunks = {}
    for _ in range(3):
        kind, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        chunks[kind] = (pos, size)
        pos += size
    btaf, _ = chunks[b"BTAF"]
    count = struct.unpack_from("<H", data, btaf + 8)[0]
    if not 0 <= index < count:
        raise ValueError(f"the archive has {count} members, not {index + 1}")
    start, end = struct.unpack_from("<II", data, btaf + 12 + 8 * index)
    gmif, _ = chunks[b"GMIF"]
    return data[gmif + 8 + start:gmif + 8 + end]
