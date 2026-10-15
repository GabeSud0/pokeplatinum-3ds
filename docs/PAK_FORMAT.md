# Platinum 3DS data pack format (v1)

`platinum3ds.pak` is a single, little-endian, uncompressed file for the 3DS SD
card. It contains the **player's own extracted NitroFS data**, not assets
shipped with this repository. The packer is `builder/pack.py`; the C runtime
reader is a separate future task.

## Byte layout

All integers are unsigned **little-endian**. There is no native-struct padding
or compression. Offsets are absolute byte offsets from the start of the pack.

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| `0x00` | 8 | bytes | Magic: ASCII `PLATPAK` followed by `00` |
| `0x08` | 4 | `u32` | Format version: **1** |
| `0x0C` | 32 | bytes | UTF-8 build/release ID, NUL-padded; 1–32 bytes, no embedded NUL |
| `0x2C` | 20 | bytes | Source ROM's **raw SHA-1 digest** (not 40 ASCII hex characters) |
| `0x40` | 4 | `u32` | File count `N` |
| `0x44` | `12 × N` | entries | Index sorted by ascending original NitroFS file ID |
| `0x44 + 12 × N` | 0–3 | bytes | Zero padding up to a 4-byte boundary (currently zero bytes) |
| After index | variable | bytes | File data, each file start and pack end aligned to 4 bytes |

The fixed header is **68 bytes (`0x44`)**. Each index entry is **12 bytes**:

| Entry-relative offset | Size | Type | Meaning |
|---:|---:|---|---|
| `+0` | 4 | `u32` | Original NitroFS file ID from `manifest.json` (`id`) |
| `+4` | 4 | `u32` | Absolute file data offset, divisible by 4 |
| `+8` | 4 | `u32` | File data size in bytes, excluding padding |

IDs are not renumbered: missing/gapped IDs are allowed. Each ID occurs once.
Entries are in ascending ID order, so the runtime can binary-search the table
without reading file data. After the index, payloads appear in the same order,
with **zero-filled padding** after each to align the next payload to 4 bytes.
Empty files are allowed; they have size zero and an aligned offset.

All offsets, sizes and counts fit in `u32`, so a v1 pack cannot exceed the
32-bit offset space. No file names are stored in the pack; they are only used
by the builder to locate the extracted input. The reader can use file IDs
for `FS_OpenFile`-style lookups and NARC access.

## Compatibility checks

Before serving any file, the 3DS runtime should:

1. Verify the 8-byte magic and `format_version == 1`.
2. Compare the 32-byte NUL-padded build ID **byte-for-byte** with the ID
   embedded in the running 3DS build. Reject mismatches, not just warn.
3. Validate the source ROM SHA-1 against supported dump hashes (or the
   expected ROM revision), so files from another revision aren't silently used.
4. Bounds-check `N`, the index end, every `offset + size` and alignment against
   the actual pack length. Reject duplicate/unsorted IDs, overlapping entries,
   and malformed/truncated packs. Never trust index offsets before checking.

A version or build-ID mismatch requires rebuilding the pack for that release.
The format version identifies the *binary layout*; the build ID identifies the
*game build's expected data*. SHA-1 identifies the source cartridge revision.
These three values serve different purposes.

`builder/pack.py` exposes `read_pak(path)` for strict host-side validation. It
returns a dictionary containing `version`, `build_id`, `rom_sha1` (hex) and
`files` (a mapping of original NitroFS integer IDs to bytes). This convenience
reader loads the whole pack; a 3DS implementation should stream payloads.

The C reader (`3ds_port/pak/src/pak.c`) streams them, and keeps the parts
of the pack it read last in memory: 64 blocks of 8 KB, the one used
longest ago making way. The game reads many small pieces (a field's load is
about 1900 reads, nine in ten under 256 bytes: archive headers, file
tables, text), and each one a seek and a read of the SD card took about a
millisecond on the 3DS: going between a route and a town, the game lost
about a second. A read of two blocks or more goes straight to the file.
The file is unbuffered: through stdio's buffer (newlib's, 1 KB), a block
or a big read was a request to the SD card per kilobyte.

## Creating a pack locally

From the repository root, first verify and extract **your own** supported ROM:

```sh
python builder/verify_rom.py /path/to/platinum.nds
python builder/extract_rom.py /path/to/platinum.nds -o data_pack/
```

The extractor records the source ROM's SHA-1 in `manifest.json` as a
top-level `"rom_sha1": "<40 lowercase hex>"` field, next to `"files"`.
Because the manifest already identifies the verified source ROM, the standard
packing command needs only a build ID:

```sh
python builder/pack.py data_pack/ -o platinum3ds.pak --build-id <id>
```

The optional `--rom` and `--rom-sha1` flags remain available to supply
the verified source digest explicitly:

```sh
python builder/pack.py data_pack/ -o platinum3ds.pak --build-id <id> --rom /path/to/platinum.nds
python builder/pack.py data_pack/ -o platinum3ds.pak --build-id <id> --rom-sha1 YOUR_VERIFIED_ROM_SHA1
```

An override **must agree** with the manifest digest. The CLI validates both
the manifest digest and any override against supported ROM SHA-1 hashes. If no
source digest is available, packing fails rather than inventing one. The
The
packer verifies every listed file's size, rejects duplicate IDs and unsafe
paths (including symlinks escaping the folder), and writes via a temporary
file to avoid leaving a partially written output.

**Never commit or upload** `.nds`, extracted `data_pack/`, or `.pak` files.
`*.pak` and `data_pack/` are already excluded by `.gitignore`.

Run synthetic tests with:

```sh
python -m unittest discover -s builder -p 'test_*.py'
```
