"""The HOME Menu banner as a glTF scene, laid out like Pokemon Y's banner:
the portal glow at the back, the title-screen Giratina (from the player's own
ROM) in the middle playing its idle, and the logo in front.

    scene.build(giratina_nsbmd_bytes, giratina_nsbca_bytes, art_dir, out_dir)

Coordinates are in the HOME Menu banner camera's space: camera at (0, 1, 44.786)
looking down -Z, 30 degree vertical field of view, near plane 26.5 (pycgfx's
banner-camera.gltf). What the HOME Menu does with a banner, learnt on hardware:

* It spins the whole model one turn every 10 s (faster when you blow into the
  mic). Like Pokemon Y's "y_autorotate" bone, Giratina sits under a bone that
  turns the other way at the same rate (to_cgfx.py writes its exact curve), with
  its pivot on the spin axis, so a spin turns it in place.
* The portal and the logo are screen-facing billboards whose bones sit at the
  root's origin, so a spin never turns or moves them; they are placed outside
  the circle Giratina sweeps when it spins (radius ~13.5).
* The banner is only drawn between the HOME Menu's bars (about y 40 to 212 of
  the 240-pixel screen): everything is kept inside that over the whole loop.
* The logo is drawn 1 texel = 1 screen pixel (224 px wide) so it stays sharp.

Standard library only.
"""
import json
import math
import os
import struct

import giratina_parts
import linalg
import pngio

FPS = 30                      # the title screen advances its animation once per 30 fps frame
KEY_STEP = 4                  # keyframe every 4th frame (size budget)
WING_GLOW_V = -0.4            # fixed texture offset that puts the red glow on the wings
MAX_JOINTS = 7                # joints per part, as in Pokemon Y's banner
GIRATINA_WIDTH = 21.0         # model width in banner units
GIRATINA_POS = (0.0, 2.6, 0.0)  # its pivot: on the spin axis (x = z = 0)
PITCH, YAW = 10.0, -18.0      # degrees: seen slightly from above, turned to the viewer's left
HOVER = 6.0                   # model units, the title screen's gentle bob
AUTOROTATE_SECONDS = 10.0     # the HOME Menu's spin: one turn per 10 s
PORTAL = ("portal.png", 65.0, (0.0, 0.44, -30.0))     # picture, width, centre
LOGO = ("logo.png", 14.9, (0.0, -2.46, 15.0))         # 224 px wide at z = 15


def build(nsbmd, nsbca, art_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    model, inst, tris, frames, images = giratina_parts.build(nsbmd, nsbca)
    mats = model["materials"]
    blob, views, accs = bytearray(), [], []

    def acc(vals, kind, comp=5126, minmax=True):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        while len(blob) % 4:
            blob.append(0)
        data = struct.pack("<%d%s" % (len(vals), {5126: "f", 5123: "H", 5121: "B"}[comp]), *vals)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        a = {"bufferView": len(views) - 1, "componentType": comp, "count": len(vals) // n, "type": kind}
        if minmax and kind in ("VEC3", "SCALAR"):
            a["min"] = [float(min(vals[i::n])) for i in range(n)]
            a["max"] = [float(max(vals[i::n])) for i in range(n)]
        accs.append(a)
        return len(accs) - 1

    gl = {"asset": {"version": "2.0", "generator": "pokeplatinum-3ds banner"}, "scene": 0, "scenes": [{"nodes": [0]}],
          "nodes": [{"name": "COMMON", "children": []}], "meshes": [], "materials": [], "textures": [], "images": [],
          "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497},
                       {"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}], "skins": []}

    def node(parent, **kw):
        gl["nodes"].append(kw)
        i = len(gl["nodes"]) - 1
        gl["nodes"][parent].setdefault("children", []).append(i)
        return i

    def texture(w, h, rgba, name, clamp=True):
        pngio.write(os.path.join(out_dir, f"{name}.png"), w, h, rgba)
        gl["images"].append({"uri": f"{name}.png", "name": name})
        gl["textures"].append({"sampler": 1 if clamp else 0, "source": len(gl["images"]) - 1})
        return len(gl["textures"]) - 1

    def material(name, tex, mode):
        m = {"name": name, "alphaMode": mode, "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 1}}
        if tex is not None:
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": tex}
        if mode == "MASK":
            m["alphaCutoff"] = 0.5
        gl["materials"].append(m)
        return len(gl["materials"]) - 1

    def pow2(n):
        return 1 << max(3, math.ceil(math.log2(n)))

    def card(name, picture, width, center, mode):
        w, h, rgba = pngio.read(os.path.join(art_dir, picture))
        tw, th = pow2(w), pow2(h)
        canvas = bytearray(tw * th * 4)
        for y in range(h):
            canvas[y * tw * 4:y * tw * 4 + w * 4] = rgba[y * w * 4:(y + 1) * w * 4]
        mat = material(name, texture(tw, th, canvas, name), mode)
        height = width * h / w
        cx, cy, cz = center
        x0, x1, y0, y1 = cx - width / 2, cx + width / 2, cy - height / 2, cy + height / 2
        u1, v1 = w / tw, h / th
        gl["meshes"].append({"name": name, "primitives": [{"attributes": {
            "POSITION": acc([x0, y0, cz, x1, y0, cz, x1, y1, cz, x0, y1, cz], "VEC3"),
            "NORMAL": acc([0, 0, 1] * 4, "VEC3"), "TEXCOORD_0": acc([0, v1, u1, v1, u1, 0, 0, 0], "VEC2")},
            "indices": acc([0, 1, 2, 0, 2, 3], "SCALAR", 5123), "material": mat}]})
        return node(0, name=name, mesh=len(gl["meshes"]) - 1)

    face = node(0, name="autorotate")

    # ---- back: the portal glow ----
    card("portal", PORTAL[0], PORTAL[1], PORTAL[2], "BLEND")

    # ---- middle: Giratina ----
    world = lambda v: linalg.point(inst[v[0]], v[1])
    pts = [world(v) for _, t in tris for v in t]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    scale = GIRATINA_WIDTH / (hi[0] - lo[0])
    pitch, yaw = math.radians(PITCH), math.radians(YAW)
    q_turn = linalg.quat_mul([math.sin(pitch / 2), 0, 0, math.cos(pitch / 2)], [0, math.sin(yaw / 2), 0, math.cos(yaw / 2)])
    # pivot = the centre of what is seen after the turn
    rt = linalg.quat_matrix(q_turn)
    rp = [linalg.apply(rt, p) for p in pts]
    c = [(min(p[k] for p in rp) + max(p[k] for p in rp)) / 2 for k in range(3)]
    mid = linalg.apply(linalg.transpose(rt), c)
    gira = node(face, name="GiratinaRoot", translation=list(GIRATINA_POS), rotation=q_turn, scale=[scale] * 3)
    center = node(gira, name="GiratinaCenter", translation=[-mid[0], -mid[1], -mid[2]])
    hover = node(center, name="GiratinaHover")

    gtex, gmat = {}, {}
    for m in mats:
        if m["texture"] and m["texture"] not in gtex:
            w, h, rgba = images[m["texture"]]
            opaque = all(rgba[i] == 255 for i in range(3, len(rgba), 4))
            gtex[m["texture"]] = (texture(w, h, rgba, m["texture"], clamp=False), opaque)
    for i, m in enumerate(mats):
        if m["texture"]:
            t, opaque = gtex[m["texture"]]
            gmat[i] = material(f"gira_{m['name']}", t, "OPAQUE" if opaque else "MASK")
        else:
            gmat[i] = material(f"gira_{m['name']}", None, "OPAQUE")

    # joints: one node per matrix instance, posed in model space
    jnodes = []
    for i, M in enumerate(inst):
        t, q, s = linalg.decompose(M)
        jnodes.append(node(hover, name=f"gj{i}", translation=t, rotation=q, scale=s))
    # parts using at most MAX_JOINTS joints each; every vertex follows exactly one joint
    groups = []
    for mat, t in sorted(tris, key=lambda x: sorted({v[0] for v in x[1]})):
        js = {v[0] for v in t}
        for g in groups:
            if len(g[0] | js) <= MAX_JOINTS:
                g[0] |= js
                g[1].setdefault(mat, []).append(t)
                break
        else:
            groups.append([set(js), {mat: [t]}])
    for gi, (js, bymat) in enumerate(groups):
        joints = sorted(js)
        jidx = {j: k for k, j in enumerate(joints)}
        prims = []
        for mat, ts in bymat.items():
            P, N, UV, J, Wt, IDX, seen = [], [], [], [], [], [], {}
            for t in ts:
                for v in t:
                    w = world(v)
                    n = linalg.normalize(linalg.apply(linalg.upper3(inst[v[0]]), v[4]))
                    uv = (v[2][0], v[2][1] + (WING_GLOW_V if mats[mat]["texture"] == "gira01" else 0))
                    key = (v[0], tuple(round(x, 4) for x in w), tuple(round(x, 5) for x in uv), tuple(round(x, 3) for x in n))
                    if key not in seen:
                        seen[key] = len(seen)
                        P += w; N += n; UV += [uv[0], uv[1]]
                        J += [jidx[v[0]], 0, 0, 0]; Wt += [1.0, 0.0, 0.0, 0.0]
                    IDX.append(seen[key])
            prims.append({"attributes": {"POSITION": acc(P, "VEC3"), "NORMAL": acc(N, "VEC3"), "TEXCOORD_0": acc(UV, "VEC2"),
                                         "JOINTS_0": acc(J, "VEC4", 5121, False), "WEIGHTS_0": acc(Wt, "VEC4", 5126, False)},
                          "indices": acc(IDX, "SCALAR", 5123), "material": gmat[mat]})
        ibm = []
        for j in joints:
            inv = linalg.inverse(inst[j])
            ibm += [inv[r][c] for c in range(4) for r in range(4)]        # column-major
        gl["skins"].append({"name": f"part{gi}", "joints": [jnodes[j] for j in joints], "inverseBindMatrices": acc(ibm, "MAT4", minmax=False)})
        gl["meshes"].append({"name": f"gpart{gi}", "primitives": prims})
        node(hover, name=f"gpart{gi}", mesh=len(gl["meshes"]) - 1, skin=len(gl["skins"]) - 1)

    # ---- front: the logo ----
    card("logo", LOGO[0], LOGO[1], LOGO[2], "BLEND")

    # ---- animation ----
    samplers, channels = [], []

    def animate(nd, path, values, kind, t_acc):
        samplers.append({"input": t_acc, "output": acc(values, kind, minmax=False), "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": nd, "path": path}})

    keys = list(range(0, len(frames), KEY_STEP))
    if keys[-1] != len(frames) - 1:
        keys.append(len(frames) - 1)
    t_acc = acc([k / FPS for k in keys], "SCALAR")
    for i, jn in enumerate(jnodes):
        trs = [linalg.decompose(frames[k][i]) for k in keys]
        first = trs[0]
        if all(linalg.close(x[0] + x[1] + x[2], first[0] + first[1] + first[2], atol=1e-4) for x in trs):
            continue                                       # this joint never moves
        if any(not linalg.close(x[0], first[0], atol=1e-3) for x in trs):
            animate(jn, "translation", [c for x in trs for c in x[0]], "VEC3", t_acc)
        if any(not linalg.close(x[1], first[1], atol=1e-5) for x in trs):
            animate(jn, "rotation", [c for x in trs for c in x[1]], "VEC4", t_acc)
        if any(not linalg.close(x[2], first[2], atol=1e-4) for x in trs):
            animate(jn, "scale", [c for x in trs for c in x[2]], "VEC3", t_acc)
    loop = (len(frames) - 1) / FPS
    n = 24
    th = acc([loop * i / n for i in range(n + 1)], "SCALAR")
    animate(hover, "translation", [c for i in range(n + 1) for c in (0.0, HOVER * math.sin(2 * math.pi * i / n), 0.0)], "VEC3", th)
    animate(face, "rotation", [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0], "VEC4", acc([0.0, AUTOROTATE_SECONDS], "SCALAR"))
    gl["animations"] = [{"name": "COMMON", "samplers": samplers, "channels": channels}]

    gl["bufferViews"], gl["accessors"] = views, accs
    gl["buffers"] = [{"uri": "platinum-banner.bin", "byteLength": len(blob)}]
    with open(os.path.join(out_dir, "platinum-banner.bin"), "wb") as f:
        f.write(blob)
    with open(os.path.join(out_dir, "platinum-banner.gltf"), "w", encoding="utf-8") as f:
        json.dump(gl, f)
    return os.path.join(out_dir, "platinum-banner.gltf")
