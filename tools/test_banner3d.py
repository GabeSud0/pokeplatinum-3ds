"""tools/banner3d (the HOME Menu's 3D banner) on made-up data only: no ROM, no
game files. The pycgfx part runs when build/pycgfx is there (CI fetches it:
make_banner.py --fetch-pycgfx-only) on Python 3.11+, and is skipped otherwise."""
import hashlib
import json
import math
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "banner3d"))
import linalg  # noqa: E402
import narc  # noqa: E402
import nitro  # noqa: E402
import pngio  # noqa: E402

PYCGFX = os.path.join(os.path.dirname(HERE), "build", "pycgfx")
# What to_cgfx makes of synthetic_scene (Python 3.11 to 3.13; pycgfx at to_cgfx.PYCGFX_COMMIT)
SYNTHETIC_CGFX_SHA1 = "1d50ebe1e74d3382f54b2b992d3492fa4cf82a25"


class PNG(unittest.TestCase):
    def test_round_trip(self):
        w, h = 5, 3
        rgba = bytearray()
        for y in range(h):
            for x in range(w):
                rgba += bytes((x * 40, y * 80, (x + y) * 20, 255 - x))
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "a.png")
            pngio.write(path, w, h, rgba)
            self.assertEqual(pngio.read(path), (w, h, rgba))


class LinAlg(unittest.TestCase):
    def test_inverse(self):
        m = linalg.trs([1, 2, 3], linalg.quat_matrix(linalg.normalize([0.1, 0.7, -0.2, 0.6])), [2, 3, 0.5])
        self.assertTrue(linalg.close(linalg.mul(m, linalg.inverse(m)), linalg.eye(), atol=1e-12))

    def test_decompose_round_trip(self):
        q = linalg.normalize([0.3, -0.4, 0.1, 0.85])
        m = linalg.trs([4, -5, 6], linalg.quat_matrix(q), [1.5, 2.0, 0.75])
        t, q2, s = linalg.decompose(m)
        self.assertTrue(linalg.close(t, [4, -5, 6]))
        self.assertTrue(linalg.close(s, [1.5, 2.0, 0.75]))
        if q2[3] * q[3] < 0:
            q2 = [-x for x in q2]
        self.assertTrue(linalg.close(q2, q, atol=1e-9))

    def test_nearest_rotation_of_a_rounded_matrix(self):
        r = linalg.quat_matrix(linalg.normalize([0.2, 0.5, -0.3, 0.8]))
        rounded = [[round(x * 4096) / 4096 for x in row] for row in r]      # DS fixed point
        n = linalg.nearest_rotation(rounded)
        self.assertTrue(linalg.close(linalg.mul(linalg.transpose(n), n), linalg.eye(3), atol=1e-12))
        self.assertTrue(linalg.close(n, r, atol=1e-3))


class Nitro(unittest.TestCase):
    def test_fixed_point(self):
        self.assertEqual(nitro.fx32(0x1000), 1.0)
        self.assertEqual(nitro.fx32(0xFFFFF000), -1.0)
        self.assertEqual(nitro.fx16(0x8000), -8.0)

    def test_pivot_matrices_are_rotations(self):
        a, b = math.cos(0.4), math.sin(0.4)
        for select in range(9):
            m = nitro.pivot_mat(select, 2, a, b)          # neg bit 1: the rotation sign pattern
            self.assertTrue(linalg.close(linalg.mul(linalg.transpose(m), m), linalg.eye(3), atol=1e-12), select)

    def test_narc_member(self):
        files = [b"first", b"second!!"]
        data = b"".join(f + b"\0" * (-len(f) % 4) for f in files)
        offs, pos = [], 0
        for f in files:
            offs.append((pos, pos + len(f))); pos += len(f) + (-len(f) % 4)
        btaf = b"BTAF" + struct.pack("<IHH", 12 + 8 * len(files), len(files), 0) + b"".join(struct.pack("<II", *o) for o in offs)
        btnf = b"BTNF" + struct.pack("<I", 16) + struct.pack("<IHH", 4, 0, 1)
        gmif = b"GMIF" + struct.pack("<I", 8 + len(data)) + data
        body = btaf + btnf + gmif
        archive = b"NARC" + struct.pack("<HHIHH", 0xFFFE, 0x0100, 16 + len(body), 16, 3) + body
        self.assertEqual(narc.member(archive, 0), b"first")
        self.assertEqual(narc.member(archive, 1), b"second!!")
        with self.assertRaises(ValueError):
            narc.member(archive, 2)


class Shims(unittest.TestCase):
    def test_gltflib_attribute_order_and_missing_fields(self):
        sys.path.insert(0, os.path.join(HERE, "banner3d", "shims"))
        try:
            import gltflib
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "s.gltf")
                with open(path, "w") as f:
                    json.dump({"meshes": [{"primitives": [{"attributes": {"TEXCOORD_0": 2, "POSITION": 0, "JOINTS_0": 3}}]}],
                               "nodes": [{"name": "a"}]}, f)
                g = gltflib.GLTF.load(path)
                attrs = list(g.model.meshes[0].primitives[0].attributes.__dict__.items())
                self.assertEqual([k for k, _ in attrs][:8], list(gltflib.ATTRIBUTE_ORDER))
                self.assertEqual([k for k, v in attrs if v is not None], ["POSITION", "TEXCOORD_0", "JOINTS_0"])
                self.assertIsNone(g.model.nodes[0].mesh)
                self.assertIsNone(g.model.extensionsUsed)
        finally:
            sys.path.remove(os.path.join(HERE, "banner3d", "shims"))


def synthetic_scene(out):
    """Two cards (portal, logo), an autorotate bone over 10 s and a skinned,
    animated quad on two joints: the shapes of the real banner, made up."""
    blob, views, accs = bytearray(), [], []

    def acc(vals, kind, comp=5126):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        while len(blob) % 4:
            blob.append(0)
        data = struct.pack("<%d%s" % (len(vals), {5126: "f", 5123: "H", 5121: "B"}[comp]), *vals)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}); blob.extend(data)
        a = {"bufferView": len(views) - 1, "componentType": comp, "count": len(vals) // n, "type": kind}
        if kind in ("VEC3", "SCALAR") and comp == 5126:
            a["min"] = [min(vals[i::n]) for i in range(n)]; a["max"] = [max(vals[i::n]) for i in range(n)]
        accs.append(a); return len(accs) - 1

    pngio.write(os.path.join(out, "t.png"), 8, 8, bytearray([200, 100, 50, 255] * 64))
    quad = lambda z: {"POSITION": acc([-1, -1, z, 1, -1, z, 1, 1, z, -1, 1, z], "VEC3"), "NORMAL": acc([0, 0, 1] * 4, "VEC3"),
                      "TEXCOORD_0": acc([0, 1, 1, 1, 1, 0, 0, 0], "VEC2")}
    idx = lambda: acc([0, 1, 2, 0, 2, 3], "SCALAR", 5123)
    mat = lambda name, mode: {"name": name, "alphaMode": mode, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}
    skinned = quad(0)
    skinned["JOINTS_0"] = acc([0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0], "VEC4", 5121)
    skinned["WEIGHTS_0"] = acc([1.0, 0, 0, 0] * 4, "VEC4")
    t = acc([0.0, 1.0, 2.0], "SCALAR")
    g = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"name": "COMMON", "children": [1, 2, 6]},
                   {"name": "portal", "mesh": 0},
                   {"name": "autorotate", "children": [3, 4, 5]},
                   {"name": "j0"}, {"name": "j1", "translation": [0, 1, 0]},
                   {"name": "body", "mesh": 1, "skin": 0},
                   {"name": "logo", "mesh": 2}],
         "skins": [{"joints": [3, 4], "inverseBindMatrices": acc([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                                                                  1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1], "MAT4")}],
         "meshes": [{"name": "portal", "primitives": [{"attributes": quad(-30), "indices": idx(), "material": 0}]},
                    {"name": "body", "primitives": [{"attributes": skinned, "indices": idx(), "material": 1}]},
                    {"name": "logo", "primitives": [{"attributes": quad(15), "indices": idx(), "material": 2}]}],
         "materials": [mat("portal", "BLEND"), mat("body", "OPAQUE"), mat("logo", "BLEND")],
         "textures": [{"source": 0, "sampler": 0}], "samplers": [{"magFilter": 9729, "minFilter": 9729}],
         "images": [{"uri": "t.png", "name": "t"}],
         "animations": [{"name": "COMMON", "samplers": [
             {"input": t, "output": acc([0, 1, 0, 0, 1.2, 0, 0, 1, 0], "VEC3"), "interpolation": "LINEAR"},
             {"input": acc([0.0, 10.0], "SCALAR"), "output": acc([0, 0, 0, 1, 0, 0, 0, 1], "VEC4"), "interpolation": "LINEAR"}],
             "channels": [{"sampler": 0, "target": {"node": 4, "path": "translation"}},
                          {"sampler": 1, "target": {"node": 2, "path": "rotation"}}]}]}
    g["bufferViews"], g["accessors"] = views, accs
    g["buffers"] = [{"uri": "s.bin", "byteLength": len(blob)}]
    with open(os.path.join(out, "s.bin"), "wb") as f:
        f.write(blob)
    with open(os.path.join(out, "s.gltf"), "w") as f:
        json.dump(g, f)
    return os.path.join(out, "s.gltf")


class Hash64(unittest.TestCase):
    def test_a_64_bit_pythons_hash(self):
        import random
        import to_cgfx
        rng = random.Random(1)
        values = [0, 1, -1, -2, 2**61 - 1, 2**61, -(2**61), 2**64 + 5, -(2**70)]
        tuples = [(), (0,), (-1,), tuple(values), ((1, 2), (3, (4,)))]
        tuples += [tuple(rng.randrange(-2**40, 2**40) for _ in range(rng.randrange(1, 40))) for _ in range(200)]
        # the numbers this test expects are a 64-bit CPython's own (its hash() is the reference there)
        if sys.maxsize == 2**63 - 1 and sys.implementation.name == "cpython":
            for v in values + tuples:
                self.assertEqual(to_cgfx.hash64(v), hash(v), v)
        self.assertEqual(to_cgfx.hash64((1, 2, 3)), 529344067295497451)
        self.assertEqual(to_cgfx.hash64(-1), -2)
        with self.assertRaises(TypeError):
            to_cgfx.hash64(1.5)


@unittest.skipUnless(sys.version_info >= (3, 11) and os.path.isfile(os.path.join(PYCGFX, "main.py")),
                     "needs Python 3.11+ and build/pycgfx (make_banner.py --fetch-pycgfx-only)")
class Convert(unittest.TestCase):
    def test_synthetic_scene_to_cgfx(self):
        import to_cgfx
        with tempfile.TemporaryDirectory() as tmp:
            raw = to_cgfx.convert(PYCGFX, synthetic_scene(tmp))
        self.assertEqual(raw[:4], b"CGFX")
        self.assertLessEqual(len(raw), to_cgfx.CGFX_LIMIT)
        self.assertEqual(struct.unpack_from("<I", raw, 12)[0], len(raw))
        # the autorotate track: one linear turn over the whole 600-frame animation
        key = struct.pack("<ff", 600.0, 2 * math.pi)
        self.assertIn(key, raw)
        self.assertIn(b"autorotate\0", raw)
        # The same bytes on every Python, and in the browser (tools/pyodide_test.mjs
        # runs this test in Pyodide): the web builder makes the banner there.
        self.assertEqual(hashlib.sha1(raw).hexdigest(), SYNTHETIC_CGFX_SHA1)


if __name__ == "__main__":
    unittest.main()
