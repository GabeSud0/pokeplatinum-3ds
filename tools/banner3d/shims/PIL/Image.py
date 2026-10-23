"""See PIL/__init__.py: just what pycgfx calls, on RGBA pictures."""
import builtins
import enum
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import pngio  # noqa: E402  (tools/banner3d/pngio.py)


class Transpose(enum.IntEnum):
    FLIP_LEFT_RIGHT = 0
    FLIP_TOP_BOTTOM = 1


class Image:
    def __init__(self, width, height, rgba):
        self.width, self.height, self._rgba = width, height, bytearray(rgba)
        self.mode = "RGBA"

    @property
    def size(self):
        return (self.width, self.height)

    def convert(self, mode):
        if mode != "RGBA":
            raise NotImplementedError(mode)
        return Image(self.width, self.height, self._rgba)

    def transpose(self, how):
        if how != Transpose.FLIP_TOP_BOTTOM:
            raise NotImplementedError(how)
        row = self.width * 4
        flipped = bytearray()
        for y in range(self.height - 1, -1, -1):
            flipped += self._rgba[y * row:(y + 1) * row]
        return Image(self.width, self.height, flipped)

    def tobytes(self):
        return bytes(self._rgba)

    def getpixel(self, xy):
        i = (xy[1] * self.width + xy[0]) * 4
        return tuple(self._rgba[i:i + 4])

    def putpixel(self, xy, value):
        i = (xy[1] * self.width + xy[0]) * 4
        self._rgba[i:i + len(value)] = bytes(value)

    def resize(self, size):
        raise NotImplementedError("the banner's pictures are already at most 256x256")


def open(fp):  # noqa: A001  (Pillow's name)
    data = fp.read() if hasattr(fp, "read") else builtins.open(fp, "rb").read()
    w, h, rgba = pngio.read_bytes(data)
    return Image(w, h, rgba)
