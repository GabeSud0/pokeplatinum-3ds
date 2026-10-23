"""A tiny stand-in for the parts of gltflib that pycgfx uses: GLTF.load() of a
.gltf with its .bin/.png resources, then read-only access to the glTF fields
as attributes (a missing field reads as None, as gltflib's dataclasses do),
plus the constructors Material, PBRMetallicRoughness and Sampler.

Vertex attributes keep gltflib's field order (POSITION, NORMAL, TANGENT,
TEXCOORD_0, TEXCOORD_1, COLOR_0, JOINTS_0, WEIGHTS_0, each present or None):
pycgfx lays the vertex data out in that order.
"""
import json
import os

ATTRIBUTE_ORDER = ("POSITION", "NORMAL", "TANGENT", "TEXCOORD_0", "TEXCOORD_1", "COLOR_0", "JOINTS_0", "WEIGHTS_0")


class Obj:
    def __init__(self, **fields):
        self.__dict__.update(fields)

    def __getattr__(self, name):          # only called for fields that aren't there
        if name.startswith("__"):
            raise AttributeError(name)
        return None


class Attributes:
    def __init__(self, d):
        for k in ATTRIBUTE_ORDER:
            self.__dict__[k] = d.get(k)
        for k, v in d.items():
            if k not in self.__dict__:
                self.__dict__[k] = v


def wrap(value, key=None):
    if isinstance(value, dict):
        if key in ("attributes",):
            return Attributes(value)
        if key in ("extensions", "extras"):
            return value                   # pycgfx indexes these like dicts
        return Obj(**{k: wrap(v, k) for k, v in value.items()})
    if isinstance(value, list):
        if key == "targets":
            return [Attributes(v) for v in value]
        return [wrap(v, key) for v in value]
    return value


class Material(Obj):
    pass


class PBRMetallicRoughness(Obj):
    pass


class Sampler(Obj):
    pass


class Accessor(Obj):
    pass


class Resource:
    def __init__(self, uri, data):
        self.uri, self.data = uri, data


class GLTF:
    def __init__(self, model, base_dir):
        self.model, self._base = model, base_dir

    @classmethod
    def load(cls, path, load_file_resources=True):
        with open(path, encoding="utf-8") as f:
            model = wrap(json.load(f))
        return cls(model, os.path.dirname(os.path.abspath(path)))

    def get_resource(self, uri):
        with open(os.path.join(self._base, uri), "rb") as f:
            return Resource(uri, f.read())

    def get_glb_resource(self):
        raise NotImplementedError(".glb is not used by the banner")
