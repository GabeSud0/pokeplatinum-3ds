"""A tiny stand-in for the parts of Pillow that pycgfx uses on the banner's
pictures (Image.open of a PNG, .width/.height, .convert("RGBA"),
.transpose(FLIP_TOP_BOTTOM), .tobytes()), so the banner builds on any Python
3.12 without compiled packages. Only on sys.path while the banner is made."""
