"""The banner's glTF scene to CGFX, with skyfloogle/pycgfx (fetched at build time
at a pinned commit, never vendored: it has no licence that allows sharing it)
and the fixes it needs to run on a real HOME Menu, applied here at run time so
pycgfx's own files stay untouched.

Fixes found on hardware by daaaaavia/CBC-for-YANBF (MIT licence):
  1. every mesh records its node's bone (mesh_node_name): some banners crash
     without it;
  2. the bone lookup tree is rebuilt after pycgfx moves the bones of BLEND
     materials to the end (otherwise looking up a bone finds the wrong one and
     the HOME Menu crashes);
  3. BLEND materials are marked translucent (pycgfx leaves them "opaque").
Ours, also from hardware tests:
  4. materials are unlit (texture x material colour), whatever the HOME Menu's
     lights do;
  5. the portal and logo bones are screen-facing billboards (as Pokemon Y's
     logo is), and "autorotate" turns -360 degrees over the animation's
     10 seconds against the HOME Menu's own spin (as Pokemon Y's y_autorotate);
  6. each curve loops over its own keys (Giratina's 4-second idle) inside the
     10-second animation.
  7. the two hashes pycgfx writes in each material (Python's hash() of a
     tuple of numbers) are worked out as a 64-bit PC's Python does, on any
     Python: the browser's (Pyodide) is 32-bit, and its hash() gives other
     numbers, so the banner it made wasn't the one tested on the 3DS.
pycgfx and its two libraries are swapped for standard-library stand-ins
(shims/: Pillow and gltflib, just the parts pycgfx calls).
"""
import importlib.util
import itertools
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PYCGFX_COMMIT = "1f78850086f3a77c41e07162e842f97a5bf3c18a"
CGFX_LIMIT = 0x80000          # the HOME Menu's limit for a banner's CGFX
BILLBOARDS = ("portal", "logo")


def _batched(iterable, n):
    # itertools.batched, new in Python 3.12 (pycgfx calls it): for 3.11, as
    # devkitPro's image (Debian 12) has, where build_linux.sh builds
    it = iter(iterable)
    while True:
        batch = tuple(itertools.islice(it, n))
        if not batch:
            return
        yield batch


_MASK64 = (1 << 64) - 1
_HASH_MODULUS = (1 << 61) - 1                  # CPython's _PyHASH_MODULUS, 64-bit
_XXPRIME_1, _XXPRIME_2, _XXPRIME_5 = 11400714785074694791, 14029467366897019727, 2870177450012600261


def hash64(value):
    """hash(value) as a 64-bit CPython (3.8 and later) works it out, for
    integers and tuples of them: what pycgfx's material hashes need (7.)."""
    if type(value) is int:
        h = value % _HASH_MODULUS if value >= 0 else -(-value % _HASH_MODULUS)
        return -2 if h == -1 else h
    if type(value) is tuple:                   # tupleobject.c: xxHash's mixing
        acc = _XXPRIME_5
        for item in value:
            acc = (acc + (hash64(item) & _MASK64) * _XXPRIME_2) & _MASK64
            acc = ((acc << 31) | (acc >> 33)) & _MASK64
            acc = (acc * _XXPRIME_1) & _MASK64
        acc = (acc + (len(value) ^ (_XXPRIME_5 ^ 3527539))) & _MASK64
        if acc == _MASK64:
            return 1546275796
        return acc - (1 << 64) if acc >= 1 << 63 else acc
    raise TypeError(f"hash64: {type(value).__name__} (only integers and tuples of them)")


def load_pycgfx(pycgfx_dir):
    if not hasattr(itertools, "batched"):
        itertools.batched = _batched
    shims = os.path.join(HERE, "shims")
    for p in (pycgfx_dir, shims):
        if p not in sys.path:
            sys.path.insert(0, p)
    spec = importlib.util.spec_from_file_location("pycgfx_main", os.path.join(pycgfx_dir, "main.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # 7. (the materials' hash() calls, all in cgfx/mtob.py)
    import cgfx.mtob
    cgfx.mtob.hash = hash64
    return module


def convert(pycgfx_dir, gltf_path):
    pycgfx = load_pycgfx(pycgfx_dir)
    import gltflib
    from cgfx.canm import (FloatAnimationCurve, FloatSegment, InterpolationType, QuantizationType,
                           RepeatMethod, StepLinear64Key)
    from cgfx.mtob import ConstantColorSource, MTOBFlag
    from cgfx.sobj import BillboardMode

    gltf = gltflib.GLTF.load(gltf_path, load_file_resources=True)
    scene = pycgfx.convert_gltf(gltf)
    cmdl = scene.data.models["COMMON"]
    bones = cmdl.skeleton.bones

    # 1. each mesh's node bone (our meshes are named and used by one node each)
    mesh_node = {gltf.model.meshes[n.mesh].name: n.name for n in gltf.model.nodes if n.mesh is not None}
    for sobj_mesh in cmdl.meshes.data.contents:
        mesh_name = next(m for m in mesh_node if sobj_mesh.name.startswith(m + "_"))
        sobj_mesh.mesh_node_name = bones[mesh_node[mesh_name]]
    # 2. the bone lookup tree
    bones.dict.regenerate()
    # 3. translucent BLEND materials, 4. unlit materials
    blend = {m.name for m in (gltf.model.materials or []) if m.alphaMode == "BLEND"}
    for name in cmdl.materials:
        material = cmdl.materials[name]
        if name in blend:
            material.transluscency_kind = 1
        material.flags = MTOBFlag(0)
        stage = material.fragment_shader.texture_combiners[0]
        stage.constant = ConstantColorSource.Diffuse
        stage.src_rgb = stage.src_alpha = 0x0E3
        stage.combine_rgb = stage.combine_alpha = 1
        for stage in material.fragment_shader.texture_combiners[1:]:
            stage.src_rgb = stage.src_alpha = 0x00F
            stage.combine_rgb = stage.combine_alpha = 0
    if list(scene.data.material_animations):
        raise ValueError("material animations are not safe in a HOME Menu banner")
    # 5. billboards
    for n in bones.dict.nodes[1:]:
        if n.content.name in BILLBOARDS:
            n.content.billboard_mode = BillboardMode.Screen
    # 5./6. the animation
    for name in scene.data.skeletal_animations:
        animation = scene.data.skeletal_animations[name]
        animation.looping = True
        for member in animation.member_animations_data:
            bone = animation.member_animations_data[member]
            for curve in vars(bone).values():
                if hasattr(curve, "segments") and curve.segments:
                    curve.start_frame = min(sg.start_frame for sg in curve.segments)
                    curve.end_frame = max(sg.end_frame for sg in curve.segments)
                    curve.pre_repeat_method = curve.post_repeat_method = RepeatMethod.Repeat
            if member == "autorotate":
                frames = float(animation.frame_size)
                bone.rot_x = bone.rot_z = None
                bone.scale_x = bone.scale_y = bone.scale_z = None
                bone.pos_x = bone.pos_y = bone.pos_z = None
                curve, seg = FloatAnimationCurve(), FloatSegment()
                curve.start_frame, curve.end_frame = 0.0, frames
                curve.pre_repeat_method = curve.post_repeat_method = RepeatMethod.Repeat
                seg.start_frame, seg.end_frame = 0.0, frames
                seg.interpolation, seg.quantization = InterpolationType.Linear, QuantizationType.StepLinear64
                seg.keys = [StepLinear64Key(0.0, 0.0), StepLinear64Key(frames, 2 * math.pi)]
                curve.segments.append(seg)
                bone.rot_y = curve
    raw = pycgfx.write(scene)
    if len(raw) > CGFX_LIMIT:
        raise ValueError(f"the banner's CGFX is {len(raw)} bytes, over the HOME Menu's {CGFX_LIMIT}")
    return raw
