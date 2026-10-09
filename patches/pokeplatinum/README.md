# Patches to pokeplatinum

Changes to the decompilation live here as numbered patch files, applied in
order by `tools/bootstrap.py`:

```
0001-short-description.patch
0002-another-change.patch
```

To make one: edit the code in `build/upstream/`, commit there, then run
`git format-patch -1 -o ../../patches/pokeplatinum/` from inside
`build/upstream/` and renumber the file if needed.
