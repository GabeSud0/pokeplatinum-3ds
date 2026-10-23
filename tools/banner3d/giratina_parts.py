"""An NSBMD model (+ NSBCA joint animation) as rigidly bound parts: every vertex
belongs to one matrix-stack entry (an "instance"), stored in that entry's local
space, plus that entry's model-space matrix for every animation frame.

Platinum's title-screen Giratina binds every vertex to exactly one joint (no
skin blending), so this reproduces its animation exactly; the banner then
skins each vertex to its one joint, like Pokemon Y's own HOME Menu banner.

Vertices are (instance, local_pos, uv, colour, local_normal).
"""
import linalg
import nitro


def run(model, objmats, want_geometry):
    """Simulate the render commands. Returns (instance matrices, triangles)."""
    I = linalg.eye()
    stack = [(None, I)] * 32
    cur_inst, cur = None, I
    inst_mats, inv_cache = [], {}
    tris = []
    mat = 0

    def new_instance(m):
        inst_mats.append([row[:] for row in m])
        return len(inst_mats) - 1

    def inv(i):
        if i not in inv_cache:
            inv_cache[i] = linalg.inverse(inst_mats[i])
        return inv_cache[i]

    for op, arg in model["ops"]:
        if op == "load":
            cur_inst, cur = stack[arg]
        elif op == "store":
            if cur_inst is None or not linalg.close(inst_mats[cur_inst], cur):
                cur_inst = new_instance(cur)
            stack[arg] = (cur_inst, [row[:] for row in cur])
        elif op == "mulobj":
            cur = linalg.mul(cur, objmats[arg]); cur_inst = None
        elif op == "scaleup":
            cur = linalg.mul(cur, linalg.scale4(model["up"])); cur_inst = None
        elif op == "scaledown":
            cur = linalg.mul(cur, linalg.scale4(model["down"])); cur_inst = None
        elif op == "mat":
            mat = arg
        elif op == "draw":
            m = model["materials"][mat]
            dim = (max(m["width"], 1), max(m["height"], 1))
            if cur_inst is None:
                cur_inst = new_instance(cur)
            g_inst, g_cur = cur_inst, cur
            vtx = [0.0, 0.0, 0.0]; uv = (0.0, 0.0); col = (1.0, 1.0, 1.0); nrm = (0.0, 0.0, 1.0)
            prim, ptype = [], 0

            def flush():
                out, n = [], len(prim)
                if ptype == 0:
                    out = [prim[i:i + 3] for i in range(0, n - 2, 3)]
                elif ptype == 1:
                    for i in range(0, n - 3, 4):
                        q = prim[i:i + 4]; out += [[q[0], q[1], q[2]], [q[0], q[2], q[3]]]
                elif ptype == 2:
                    for i in range(n - 2):
                        out.append([prim[i], prim[i + 1], prim[i + 2]] if i % 2 == 0 else [prim[i], prim[i + 2], prim[i + 1]])
                elif ptype == 3:
                    for i in range(0, n - 3, 2):
                        out += [[prim[i], prim[i + 1], prim[i + 3]], [prim[i], prim[i + 3], prim[i + 2]]]
                for t in out:
                    tris.append((mat, t))

            for c, p in nitro.gpu_cmds(model["pieces"][arg]["cmds"]):
                if c == 0x14:
                    g_inst, g_cur = stack[p[0] & 31]
                    if g_inst is None:
                        g_inst = new_instance(g_cur)
                    continue
                if not want_geometry:
                    continue
                if c == 0x1b:
                    g_cur = linalg.mul(g_cur, linalg.diag([nitro.fx32(p[0]), nitro.fx32(p[1]), nitro.fx32(p[2]), 1.0]))
                elif c == 0x40:
                    flush(); prim = []; ptype = p[0] & 3
                elif c == 0x41:
                    flush(); prim = []
                elif c == 0x22:
                    s, t = nitro.fix(p[0] & 0xFFFF, 1, 11, 4), nitro.fix(p[0] >> 16, 1, 11, 4)
                    uv = (s / dim[0], t / dim[1])
                elif c == 0x20:
                    col = (nitro.bits(p[0], 0, 5) / 31, nitro.bits(p[0], 5, 10) / 31, nitro.bits(p[0], 10, 15) / 31)
                elif c == 0x21:
                    nrm = tuple(nitro.fix(nitro.bits(p[0], 10 * k, 10 * k + 10), 1, 0, 9) for k in range(3))
                elif c in (0x23, 0x24, 0x25, 0x26, 0x27, 0x28):
                    if c == 0x23: vtx = [nitro.fx16(p[0] & 0xFFFF), nitro.fx16(p[0] >> 16), nitro.fx16(p[1] & 0xFFFF)]
                    elif c == 0x24: vtx = [nitro.fix(nitro.bits(p[0], 10 * k, 10 * k + 10), 1, 3, 6) for k in range(3)]
                    elif c == 0x25: vtx = [nitro.fx16(p[0] & 0xFFFF), nitro.fx16(p[0] >> 16), vtx[2]]
                    elif c == 0x26: vtx = [nitro.fx16(p[0] & 0xFFFF), vtx[1], nitro.fx16(p[0] >> 16)]
                    elif c == 0x27: vtx = [vtx[0], nitro.fx16(p[0] & 0xFFFF), nitro.fx16(p[0] >> 16)]
                    elif c == 0x28: vtx = [v + nitro.fix(nitro.bits(p[0], 10 * k, 10 * k + 10), 1, 0, 9) / 8 for k, v in enumerate(vtx)]
                    world = linalg.point(g_cur, vtx)
                    local = linalg.point(inv(g_inst), world)
                    n_w = linalg.apply(linalg.upper3(g_cur), list(nrm))
                    n_l = linalg.normalize(linalg.apply(linalg.upper3(inv(g_inst)), n_w))
                    prim.append((g_inst, local, uv, col, n_l))
            flush()
    return inst_mats, tris


def build(nsbmd_bytes, nsbca_bytes=None):
    """(model, rest instance matrices, triangles, per-frame instance matrices,
    textures {name: (w, h, rgba)})."""
    f = nitro.read_nsbmd(nsbmd_bytes)
    model = f["models"][0]
    anim = nitro.read_nsbca(nsbca_bytes)[0] if nsbca_bytes else None
    inst_rest, tris = run(model, nitro.object_matrices(model), True)
    frames = []
    if anim:
        for fr in range(anim["frames"]):
            mats, _ = run(model, nitro.object_matrices(model, anim, fr), False)
            if len(mats) != len(inst_rest):
                raise ValueError("the animation changes the model's matrix layout")
            frames.append(mats)
    images = {}
    for m in model["materials"]:
        if m["texture"] and m["texture"] not in images:
            t = f["textures"][m["texture"]]
            pal = f["palettes"].get(m["palette"], 0)
            images[m["texture"]] = nitro.decode_texture(t, f["pal_data"], pal)
    return model, inst_rest, tris, frames, images
