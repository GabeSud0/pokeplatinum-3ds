# Builder

The builder extracts NitroFS files from **your own legally dumped Pokémon
Platinum cartridge**, entirely on your computer. No ROM or extracted data
is uploaded, committed or included in the project's releases.

## Verify your ROM first

```sh
python builder/verify_rom.py /path/to/platinum.nds
```

Only the supported, clean USA revisions pass the SHA-1 check. Modified,
trimmed or other-region dumps are rejected.

## Extract the NitroFS data pack

From the repository root:

```sh
python builder/extract_rom.py /path/to/platinum.nds
```

By default, the output goes to `data_pack/` (gitignored). To choose
another directory:

```sh
python builder/extract_rom.py /path/to/platinum.nds --output data_pack/
```

The extractor verifies the ROM itself before extracting. It reads the NDS
header, NitroFS file-name table (FNT), and file-allocation table (FAT);
recreates the directory tree and writes `manifest.json`. The manifest contains
a top-level `rom_sha1` (40 lowercase hex characters, from the verified source
ROM) alongside `files`. Each file entry has `id` (NitroSDK file ID), `path`
(relative to the data pack), and `size` (bytes). File IDs are preserved for
the 3DS filesystem port.

The ROM is opened read-only and never modified. Keep the output private:
**never add ROMs, extracted files, data packs or game assets to Git**.

## Pack for the 3DS

After extraction, the source ROM's SHA-1 is already in `data_pack/manifest.json`.
You do **not** need to supply the ROM again:

```sh
python builder/pack.py data_pack/ -o platinum3ds.pak --build-id <id>
```

`--rom /path/to/platinum.nds` or `--rom-sha1 <verified-40-hex-sha1>`
may optionally supply the verified source digest. If an override is supplied,
it **must agree** with the manifest's digest. The packer CLI rejects hashes
outside the supported ROM revisions, including manifest-supplied hashes.

Both `data_pack/` and the generated `.pak` are private outputs; never commit them.

## Tests

```sh
python -m unittest discover -s builder -p 'test_*.py'
```

Tests construct synthetic miniature NDS images; no real game files are used.
Future work will convert game data into faster-to-load formats.
