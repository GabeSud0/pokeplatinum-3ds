"""Minimal reader for Nintendo DS NitroSystem 3D files (NSBMD models, NSBCA joint
animations), written for turning Platinum's title-screen Giratina into a 3DS HOME
Menu banner. Format knowledge follows apicula (github.com/scurest/apicula).

Matrices are 4x4 lists of rows in column-vector convention (p' = M p), see
linalg.py. Standard library only.
"""
import math
import struct

import linalg


def bits(x, lo, hi):
    return (x >> lo) & ((1 << (hi - lo)) - 1)


def fix(x, sign, ib, fb):
    n = sign + ib + fb
    x = bits(x, 0, n)
    if sign and x & (1 << (ib + fb)):
        x -= 1 << n
    return x / float(1 << fb)


fx32 = lambda x: fix(x, 1, 19, 12)
fx16 = lambda x: fix(x, 1, 3, 12)


class Buf:
    def __init__(self, data):
        self.d = data

    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from("<H", self.d, o)[0]
    def u32(self, o): return struct.unpack_from("<I", self.d, o)[0]


def info_block(b, o, datum_size):
    """Return list of (data_offset_in_file, name) for a Nitro info block at o."""
    count = b.u8(o + 1)
    data_hdr = o + 12 + 4 * count            # header, subheader, per-entry u32s
    size_of_datum = b.u16(data_hdr)
    assert size_of_datum == datum_size, (size_of_datum, datum_size)
    data0 = data_hdr + 4
    names0 = data0 + count * size_of_datum
    out = []
    for i in range(count):
        name = b.d[names0 + 16 * i: names0 + 16 * i + 16].rstrip(b"\0").decode("latin1")
        out.append((data0 + i * size_of_datum, name))
    return out


def sections(data, stamp):
    b = Buf(data)
    assert data[:4] == stamp, data[:4]
    n = b.u16(14)
    return b, [b.u32(16 + 4 * i) for i in range(n)]


def mat3_cols(*v):
    """cgmath Matrix3::new argument order (column-major) -> 3x3."""
    return linalg.cols3(*[float(x) for x in v])


def pivot_mat(select, neg, a, b):
    if select >= 9:
        return mat3_cols(-a, 0, 0, 0, 0, 0, 0, 0, 0)
    o = -1.0 if neg & 1 else 1.0
    c = -b if neg & 2 else b
    d = -a if neg & 4 else a
    return {
        0: mat3_cols(o, 0, 0, 0, a, b, 0, c, d), 1: mat3_cols(0, o, 0, a, 0, b, c, 0, d),
        2: mat3_cols(0, 0, o, a, b, 0, c, d, 0), 3: mat3_cols(0, a, b, o, 0, 0, 0, c, d),
        4: mat3_cols(a, 0, b, 0, o, 0, c, 0, d), 5: mat3_cols(a, b, 0, 0, 0, o, c, d, 0),
        6: mat3_cols(0, a, b, 0, c, d, o, 0, 0), 7: mat3_cols(a, 0, b, c, 0, d, 0, o, 0),
        8: mat3_cols(a, b, 0, c, d, 0, 0, 0, o)}[select]


def basis_mat(in0, in1, in2, in3, in4):
    inp = [in4, in0, in1, in2, in3]
    out = [0] * 6
    for i in range(5):
        out[i] = bits(inp[i], 3, 16)
        out[5] = (out[5] << 3) | bits(inp[i], 0, 3)
    f = lambda x: fix(x, 1, 0, 12)
    a = [f(out[1]), f(out[2]), f(out[3])]
    bb = [f(out[4]), f(out[0]), f(out[5])]
    c = linalg.cross(a, bb)
    return [[a[i], bb[i], c[i]] for i in range(3)]


trs = linalg.trs


# ----------------------------------------------------------------- model --------

RC_LEN = {0x00: 0, 0x01: 0, 0x02: 2, 0x03: 1, 0x04: 1, 0x05: 1, 0x06: 3, 0x07: 1, 0x08: 1,
          0x0b: 0, 0x0c: 2, 0x0d: 2, 0x24: 1, 0x26: 4, 0x2b: 0, 0x40: 0, 0x44: 1, 0x46: 4,
          0x47: 2, 0x66: 5, 0x80: 0}


def parse_render_cmds(b, o):
    ops = []
    while True:
        op = b.u8(o); o += 1
        if op == 0x09:
            cnt = b.u8(o + 1); p = b.d[o:o + 2 + 3 * cnt]; o += 2 + 3 * cnt
            terms = [(p[2 + 3 * i], p[3 + 3 * i], p[4 + 3 * i] / 256.0) for i in range(cnt)]
            ops.append(("skin", terms)); ops.append(("store", p[0])); continue
        n = RC_LEN[op]; p = b.d[o:o + n]; o += n
        if op == 0x01:
            return ops
        if op == 0x03: ops.append(("load", p[0]))
        elif op in (0x04, 0x24, 0x44): ops.append(("mat", p[0]))
        elif op == 0x05: ops.append(("draw", p[0]))
        elif op in (0x06, 0x26, 0x46, 0x66):
            store = p[3] if op in (0x26,) else (p[3] if op == 0x66 else None)
            load = p[3] if op == 0x46 else (p[4] if op == 0x66 else None)
            if load is not None: ops.append(("load", load))
            ops.append(("mulobj", p[0]))
            if store is not None: ops.append(("store", store))
        elif op == 0x0b: ops.append(("scaleup", None))
        elif op == 0x2b: ops.append(("scaledown", None))


def read_object(b, o):
    flags = b.u16(o); m0 = b.u16(o + 2); o += 4
    t = r = s = None
    if not bits(flags, 0, 1):
        t = [fx32(b.u32(o + 4 * i)) for i in range(3)]; o += 12
    if bits(flags, 3, 4):
        a, bb = fx16(b.u16(o)), fx16(b.u16(o + 2)); o += 4
        r = pivot_mat(bits(flags, 4, 8), bits(flags, 8, 12), a, bb)
    elif not bits(flags, 1, 2):
        m = [b.u16(o + 2 * i) for i in range(8)]; o += 16
        r = mat3_cols(fx16(m0), *[fx16(x) for x in m])
    if not bits(flags, 2, 3):
        s = [fx32(b.u32(o + 4 * i)) for i in range(3)]; o += 12
    return {"t": t, "r": r, "s": s, "matrix": trs(t, r, s)}


def read_material(b, o):
    dif_amb, spe_emi, poly_attr, poly_mask, tex_param = (b.u32(o + 4 + 4 * i) for i in range(5))
    misc = b.u16(o + 30)
    w, h = b.u16(o + 32), b.u16(o + 34)
    texmtx = linalg.eye()
    if misc & 1:                                   # texture matrix: optional scale / rotation / translation
        q = o + 44
        sx = sy = 1.0; tx = ty = 0.0
        if not misc & 2: sx, sy = fx32(b.u32(q)), fx32(b.u32(q + 4)); q += 8
        if not misc & 4: q += 4                    # rotation (unused here)
        if not misc & 8: tx, ty = fx32(b.u32(q)), fx32(b.u32(q + 4)); q += 8
        texmtx = [[sx, 0.0, 0.0, tx * w], [0.0, sy, 0.0, ty * h], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
    rgb = lambda x: [bits(x, 0, 5) / 31, bits(x, 5, 10) / 31, bits(x, 10, 15) / 31]
    return {"params": tex_param & poly_mask, "width": w, "height": h,
            "diffuse": rgb(bits(dif_amb, 0, 15)), "diffuse_is_vcol": bool(bits(dif_amb, 15, 16)),
            "ambient": rgb(bits(dif_amb, 16, 31)), "emission": rgb(bits(spe_emi, 16, 31)),
            "alpha": bits(poly_attr, 16, 21) / 31, "poly_attr": poly_attr,
            "cull_back": bits(poly_attr, 6, 7) == 0, "cull_front": bits(poly_attr, 7, 8) == 0,
            "has_texmtx": bool(misc & 1), "texmtx": texmtx, "misc": misc, "texture": None, "palette": None}


def read_model(b, o):
    sbc, mats, pieces, invb = (b.u32(o + 4 + 4 * i) for i in range(4))
    num_obj, num_mat, num_piece = b.u8(o + 23), b.u8(o + 24), b.u8(o + 25)
    up, down = fx32(b.u32(o + 28)), fx32(b.u32(o + 32))
    info = dict(num_verts=b.u16(o + 36), num_surfs=b.u16(o + 38), num_tris=b.u16(o + 40), num_quads=b.u16(o + 42))
    objs_o = o + 64
    objects = [dict(read_object(b, off_pos := objs_o + b.u32(dp)), name=n) for dp, n in info_block(b, objs_o, 4)]
    # materials
    mo = o + mats
    tex_pair, pal_pair = b.u16(mo), b.u16(mo + 2)
    materials = [dict(read_material(b, mo + b.u32(dp)), name=n) for dp, n in info_block(b, mo + 4, 4)]
    for dp, n in info_block(b, mo + tex_pair, 4):
        off, num = b.u16(dp), b.u8(dp + 2)
        for i in range(num): materials[b.u8(mo + off + i)]["texture"] = n
    for dp, n in info_block(b, mo + pal_pair, 4):
        off, num = b.u16(dp), b.u8(dp + 2)
        for i in range(num): materials[b.u8(mo + off + i)]["palette"] = n
    # pieces
    po = o + pieces
    plist = []
    for dp, n in info_block(b, po, 4):
        q = po + b.u32(dp)
        cmds_off, cmds_len = b.u32(q + 8), b.u32(q + 12)
        plist.append({"name": n, "cmds": b.d[q + cmds_off: q + cmds_off + cmds_len]})
    # inverse binds
    inv = []
    io = o + invb
    for i in range(num_obj):
        if io + 84 > len(b.d): break
        e = [fx32(b.u32(io + 4 * k)) for k in range(12)]
        m = linalg.eye()
        for c in range(4):
            for r in range(3):
                m[r][c] = e[3 * c + r]
        inv.append(m); io += 84
    return {"objects": objects, "materials": materials, "pieces": plist, "inv_binds": inv,
            "ops": parse_render_cmds(b, o + sbc), "up": up, "down": down, "info": info}


def read_tex0(b, o):
    tex_off = b.u16(o + 14); tex_block = b.u32(o + 20)
    c1, c2 = b.u32(o + 36), b.u32(o + 40)
    pal_len = b.u16(o + 48) << 3; pal_dict = b.u32(o + 52); pal_block = b.u32(o + 56)
    pal_data = b.d[o + pal_block: o + pal_block + pal_len]
    bpp = [0, 8, 2, 4, 8, 2, 8, 16]
    textures = {}
    for dp, n in info_block(b, o + tex_off, 8):
        params = b.u32(dp)
        fmt = bits(params, 26, 29); w = 8 << bits(params, 20, 23); h = 8 << bits(params, 23, 26)
        off = bits(params, 0, 16) << 3; ln = w * h * bpp[fmt] // 8
        if fmt == 5:
            d1 = b.d[o + c1 + off: o + c1 + off + ln]; d2 = b.d[o + c2 + off // 2: o + c2 + off // 2 + ln // 2]
        else:
            d1 = b.d[o + tex_block + off: o + tex_block + off + ln]; d2 = b""
        textures[n] = {"params": params, "fmt": fmt, "w": w, "h": h, "d1": d1, "d2": d2}
    palettes = {n: b.u16(dp) << 3 for dp, n in info_block(b, o + pal_dict, 4)}
    return textures, palettes, pal_data


def rgb555(c, a=255):
    r, g, bb = bits(c, 0, 5), bits(c, 5, 10), bits(c, 10, 15)
    e = lambda x: (x << 3) | (x >> 2)
    return (e(r), e(g), e(bb), a)


def decode_texture(t, pal_data, pal_off):
    w, h, fmt, d = t["w"], t["h"], t["fmt"], t["d1"]
    c0t = bool(bits(t["params"], 29, 30))
    pal = lambda i: struct.unpack_from("<H", pal_data, pal_off + 2 * i)[0] if pal_off + 2 * i + 2 <= len(pal_data) else 0
    px = []
    if fmt in (2, 3, 4):
        bpp = {2: 2, 3: 4, 4: 8}[fmt]
        for byte in d:
            for k in range(0, 8, bpp):
                u = bits(byte, k, k + bpp)
                px.append(rgb555(pal(u), 0 if (u == 0 and c0t) else 255))
    elif fmt == 1:
        for x in d:
            a = bits(x, 5, 8); a5 = (a << 2) | (a >> 1)
            px.append(rgb555(pal(bits(x, 0, 5)), (a5 << 3) | (a5 >> 2)))
    elif fmt == 6:
        for x in d:
            a5 = bits(x, 3, 8)
            px.append(rgb555(pal(bits(x, 0, 3)), (a5 << 3) | (a5 >> 2)))
    elif fmt == 7:
        for i in range(w * h):
            c = struct.unpack_from("<H", d, 2 * i)[0]
            px.append(rgb555(c, 255 if c & 0x8000 else 0))
    elif fmt == 5:
        d2 = t["d2"]; nbx = w // 4
        img = [[(0, 0, 0, 0)] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                bi = nbx * (y // 4) + x // 4
                block = struct.unpack_from("<I", d, 4 * bi)[0]; extra = struct.unpack_from("<H", d2, 2 * bi)[0]
                texel = bits(block, 2 * (4 * (y % 4) + x % 4), 2 * (4 * (y % 4) + x % 4) + 2)
                mode = bits(extra, 14, 16); pa = bits(extra, 0, 14) << 1
                col = lambda n: rgb555(pal(pa + n))
                mix = lambda p, q, wp, wq, d: tuple(int((wp * a + wq * b) / d) for a, b in zip(p, q))
                if texel == 3 and mode in (0, 1): v = (0, 0, 0, 0)
                elif mode == 1 and texel == 2: v = mix(col(0), col(1), 1, 1, 2)
                elif mode == 3 and texel == 2: v = mix(col(1), col(0), 3, 5, 8)
                elif mode == 3 and texel == 3: v = mix(col(0), col(1), 3, 5, 8)
                else: v = col(texel)
                img[y][x] = v
        return w, h, bytearray(c for row in img for p in row for c in p)
    return w, h, bytearray(c for p in px for c in p)


def read_nsbmd(data):
    b, offs = sections(data, b"BMD0")
    out = {"models": [], "textures": {}, "palettes": {}, "pal_data": b""}
    for so in offs:
        st = data[so:so + 4]
        if st == b"MDL0":
            for dp, n in info_block(b, so + 8, 4):
                out["models"].append(dict(read_model(b, so + b.u32(dp)), name=n))
        elif st == b"TEX0":
            out["textures"], out["palettes"], out["pal_data"] = read_tex0(b, so)
    return out


# ----------------------------------------------------------------- GPU commands ---

GPU_N = {0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12, 0x18: 16,
         0x19: 12, 0x1a: 9, 0x1b: 3, 0x1c: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1,
         0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2a: 1, 0x2b: 1, 0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1,
         0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1}


def gpu_cmds(cmds):
    i = 0
    while i + 4 <= len(cmds):
        ops = cmds[i:i + 4]; i += 4
        for op in ops:
            n = GPU_N.get(op)
            if n is None:
                return
            params = struct.unpack_from("<%dI" % n, cmds, i) if n else (); i += 4 * n
            yield op, params


# ----------------------------------------------------------------- animation -----

def read_nsbca(data):
    b, offs = sections(data, b"BCA0")
    anims = []
    for so in offs:
        if data[so:so + 4] != b"JNT0": continue
        for dp, n in info_block(b, so + 8, 4):
            anims.append(dict(read_anim(b, so + b.u32(dp)), name=n))
    return anims


def read_anim(b, base):
    assert b.d[base:base + 4] == b"J\0AC"
    nframes, nobj = b.u16(base + 4), b.u16(base + 6)
    pivot, basis = base + b.u32(base + 12), base + b.u32(base + 16)
    curves = []

    def fetch(x):
        idx = bits(x, 0, 15)
        if bits(x, 15, 16):
            sn, a, bb = (b.u16(pivot + 6 * idx + 2 * k) for k in range(3))
            return pivot_mat(bits(sn, 0, 4), bits(sn, 4, 8), fx16(a), fx16(bb))
        return basis_mat(*(b.u16(basis + 10 * idx + 2 * k) for k in range(5)))

    def info(x):
        return dict(start=bits(x, 0, 16), end=bits(x, 16, 28), width=bits(x, 28, 30), rate=bits(x, 30, 32))

    for k in range(nobj):
        o = base + b.u16(base + 20 + 2 * k)
        flags = b.u16(o); o += 4
        c = {"t": [None] * 3, "r": None, "s": [None] * 3, "flags": flags}
        curves.append(c)
        if bits(flags, 0, 1):          # not animated
            continue
        if bits(flags, 1, 3) == 0:
            for i in range(3):
                if bits(flags, 3 + i, 4 + i):
                    c["t"][i] = ("const", fx32(b.u32(o))); o += 4
                else:
                    inf = info(b.u32(o)); off = base + b.u32(o + 4); o += 8
                    n = (inf["end"] - inf["start"]) >> inf["rate"]
                    vals = [fx32(b.u32(off + 4 * j)) for j in range(n)] if inf["width"] == 0 else \
                           [fx16(b.u16(off + 2 * j)) for j in range(n)]
                    c["t"][i] = ("samples", inf, vals)
        if bits(flags, 6, 8) == 0:
            if bits(flags, 8, 9):
                c["r"] = ("const", fetch(b.u16(o))); o += 4
            else:
                inf = info(b.u32(o)); off = base + b.u32(o + 4); o += 8
                n = (inf["end"] - inf["start"]) >> inf["rate"]
                c["r"] = ("samples", inf, [fetch(b.u16(off + 2 * j)) for j in range(n)])
        if bits(flags, 9, 11) == 0:
            for i in range(3):
                if bits(flags, 11 + i, 12 + i):
                    c["s"][i] = ("const", fx32(b.u32(o))); o += 8
                else:
                    inf = info(b.u32(o)); off = base + b.u32(o + 4); o += 8
                    n = (inf["end"] - inf["start"]) >> inf["rate"]
                    vals = [fx32(b.u32(off + 8 * j)) for j in range(n)] if inf["width"] == 0 else \
                           [fx16(b.u16(off + 4 * j)) for j in range(n)]
                    c["s"][i] = ("samples", inf, vals)
    return {"frames": nframes, "curves": curves}


def sample(curve, default, frame):
    if curve is None:
        return default
    if curve[0] == "const":
        return curve[1]
    _, inf, vals = curve
    if not vals:
        return default
    if frame <= inf["start"]:
        return vals[0]
    pos = (frame - inf["start"]) / float(1 << inf["rate"])
    lo = int(math.floor(pos))
    if lo >= len(vals) - 1:
        return vals[-1]
    g = pos - lo
    return lerp(vals[lo], vals[lo + 1], g)


def lerp(a, b, g):
    if isinstance(a, list):
        return [lerp(x, y, g) for x, y in zip(a, b)]
    return a * (1 - g) + b * g


def object_matrices(model, anim=None, frame=0):
    """NitroSystem joint-animation SRT flags: bit0 whole identity; T: bit1 identity,
    bit2 use model's base value; R: bit6 identity, bit7 base; S: bit9 identity, bit10 base."""
    mats = []
    for i, ob in enumerate(model["objects"]):
        if anim is None or i >= len(anim["curves"]):
            mats.append(ob["matrix"]); continue
        c = anim["curves"][i]; f = c["flags"]
        if f & 1:
            mats.append(linalg.eye()); continue
        if f & 2: t = [0.0, 0.0, 0.0]
        elif f & 4: t = ob["t"] or [0.0, 0.0, 0.0]
        else: t = [sample(c["t"][k], 0.0, frame) for k in range(3)]
        if f & 0x40: r = linalg.eye(3)
        elif f & 0x80: r = ob["r"] if ob["r"] is not None else linalg.eye(3)
        else: r = sample(c["r"], linalg.eye(3), frame)
        if f & 0x200: s_ = [1.0, 1.0, 1.0]
        elif f & 0x400: s_ = ob["s"] or [1.0, 1.0, 1.0]
        else: s_ = [sample(c["s"][k], 1.0, frame) for k in range(3)]
        mats.append(trs(t, r, s_))
    return mats
