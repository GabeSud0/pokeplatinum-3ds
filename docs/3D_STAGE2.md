# 3D, stage 2: drawing the DS's 3D on the 3DS GPU

Stage 1 (PR #54) built the DS geometry engine (`3ds_port/nitro/src/port_gx3d.c`).
It turns the game's 3D commands into one **scene** per frame: clipped
triangles with their vertices, colours, texture coordinates and polygon
attributes. It also built a software rasterizer that draws that scene on the
PC only. On the 3DS, BG0 shows nothing when 3D is on.

Stage 2 draws the same scene on the 3DS with its GPU (PICA200) through
**citro3d**, and composites it into the 2D renderer as BG0. This file holds
the decisions. Implement it as the PRs listed at the end, one at a time, each
reviewed before the next starts.

## The decisions

### 1. The GPU draws the 3D, the CPU composites it, with one frame of latency (as the DS)

The 2D renderer (`port_render2d.c`) stays on the CPU. The GPU renders each 3D
scene into an off-screen 256×192 colour buffer with a depth/stencil buffer.
It then copies the result with `GX_DisplayTransfer` into a linear (untiled)
buffer in linear memory, as RGBA8. The 2D renderer reads that buffer as BG0,
exactly where it reads `sGx3dPixels` on the PC today.

On the DS, a scene ended by SWAP_BUFFERS during frame N is shown in frame
N+1. Keep that latency: the GPU renders scene N while the game runs frame
N+1, and the 2D renderer uses the newest *finished* 3D image. It never waits
for the GPU. If no image is finished yet, it uses the previous one.

Not chosen, and why:
- Drawing the 2D layers on the GPU too (as textures). That rewrites the
  whole 2D renderer we just made fast and bit-exact.
- Reading the GPU output back synchronously in the same frame. That stalls a
  drawing thread on the GPU every frame.

### 2. One thread owns the GPU: the "3D thread"

All citro3d / GPU calls happen on one thread created by the 3DS program
(`main_3ds.c`): on core 2 on a New 3DS, otherwise on core 0 at the same
priority as the core-0 drawing thread (below the game). It loops:

1. Wait for a new scene.
2. Acquire it.
3. Convert the textures it needs (cached, see 4).
4. Fill a vertex buffer.
5. Render.
6. Display-transfer into the free one of **two** output images.
7. Publish that image (an atomic index) and release the scene.

The drawing threads only ever *read* a published image.

The geometry engine needs the matching change: scenes must be **triple
buffered** (building / newest-finished / in-use-by-GPU), with an atomic
"newest" index, so a SWAP never overwrites a scene the 3D thread is still
reading. The API becomes something like
`const NitroGx3dScene *NitroPort_Gx3dAcquireScene(uint32_t *serial)` and
`NitroPort_Gx3dReleaseScene(...)`.

Never copy a whole scene: one is up to ~600 KB.

On the PC, keep the software rasterizer. Expose it through the same "newest
3D image" interface (`NitroPort_Gx3dImage()`, RGBA8 or RGB555+alpha, 256×192),
rendered synchronously at SWAP or on demand. Then `port_render2d.c` has one
code path for BG0 on both platforms, and the `#if defined(__arm__)` hacks in
it go away.

### 3. Vertices: send clip-space positions, let the GPU do perspective

The scene keeps each vertex's clip coordinates (`clip[4]`, 20.12) next to
its screen position. Send the GPU a position that maps to the **same screen
pixel**, but keeps W so texture and colour interpolation stay
perspective-correct. With `(x, y)` the stage-1 screen position (in pixels,
y down) and `w` the clip W as a float:

```
ndc_x = x / 128 - 1            (render target 256 wide)
ndc_y = 1 - y / 96             (192 high, y up)
pos   = (ndc_x * w, ndc_y * w, depth_ndc * w, w)
```

This folds the DS viewport (already applied in stage 1) into the vertex. The
vertex shader (`.v.pica`, picasso, as in `3ds_port/hello3d`) then just
passes position, colour and texcoord through. Mind the PICA's depth range
(0 near to −1 far): map the DS depth (24-bit Z, or W in W-buffer mode, from
`scene->swap_param` bit 1) into it.

Vertex format: position as 4 floats, colour as 4 unsigned bytes (expand DS
5-bit components: `c<<3 | c>>2`), texcoord as 2 floats in texels divided by
texture size, so 0..1 covers one repeat. Build the buffer in linear memory
(`linearAlloc`), double buffered, sized for the DS's per-frame maximum of
6144 vertices.

Batch triangles in order: start a new draw call only when the GPU state
changes (texture, palette, polygon mode, alpha, depth-write, culling is
already done in stage 1).

### 4. Textures: convert to native PICA formats, cache by content

The PICA has no palettes and none of the DS's compressed formats. Convert
every DS texture to a native format, in the PICA's 8×8 Morton-tiled layout:
- **RGBA5551** for: 4/16/256-colour paletted (colour 0 transparent when
  TEXIMAGE_PARAM bit 29 says so), 4×4 compressed, and direct colour. That
  keeps DS colours exact.
- **RGBA8** for A3I5 and A5I3, which have more alpha levels than 1 bit.

DS sizes are powers of two from 8 to 1024, which the PICA accepts. Map the
DS repeat and flip bits to PICA wrap modes: repeat → `GPU_REPEAT`, repeat with
flip → `GPU_MIRRORED_REPEAT`, no repeat → `GPU_CLAMP_TO_EDGE`. Use nearest
filtering (the DS doesn't filter).

**Decode exactly as the PC rasterizer samples.** Move its decoder into
shared code (e.g. `port_gx3d_texture.c`), used by both the PC rasterizer and
the converter, so the two can't drift apart. *Done:* the converter
(`NitroPort_Gx3dDecodeTextureTiled`) works a row at a time, with the palette
looked up once and eight texels per step; the tests check it texel by texel
against the sampler for every format. (Calling the sampler for each texel
took the 3DS tens of milliseconds for a battle's 256×256 Pokémon sprite
texture, converted again for every frame of a palette fade, such as a
Pokémon coming out of its ball.)

**Cache.** Key: (TEXIMAGE_PARAM address, format and size bits; palette base
for paletted formats). Store a hash of the source bytes: the texture data,
the 4×4 index data for compressed textures, and the palette entries used.
Reuse the converted texture while the hash matches.

Hash only when something may have changed. Keep a global *texture VRAM
generation* counter, bumped whenever:
- the port writes to LCDC VRAM (DMA in `port_dma.c`, the MI copy and fill
  functions in `mi_memory.c`), or
- the VRAMCNT bank registers change.

Within one generation, a cached texture is valid without hashing. Evict the
least recently used textures beyond a fixed budget (e.g. 8 MB of linear
memory).

Texture data is read when the 3D thread converts it, not at SWAP. The game
updates textures in V-blank, as on the DS, so that's close enough. Note it
in the code.

### 5. Polygon modes on the colour combiner (TexEnv)

Polygon attribute bits 4–5:
- **0 modulation:** texture × vertex colour (RGB and alpha).
- **1 decal:** RGB = mix(vertex, texture, texture alpha); alpha = vertex alpha.
- **2 toon / highlight** (DISP3DCNT bit 1 picks): the DS replaces the
  vertex colour with `toon_table[red]`, `red` being the vertex colour's 5-bit red component (32 entries), for toon:
  texture × toon colour; for highlight: texture × vertex + toon colour.
  Do the toon lookup **per vertex on the CPU** when filling the vertex buffer
  (the DS does it per pixel; per vertex is close for smooth shading). Then
  it's modulation, or modulation + add, on the combiner.
- **3 shadow polygons:** stencil (the PICA has an 8-bit stencil with 24-bit
  depth). Polygon ID 0 marks, other IDs draw where marked and the ID
  differs. Last step, and only if the game uses it (see 7).

Untextured polygons (texture format 0): vertex colour only.

### 6. Alpha, order and blending

- **Polygon alpha:** attribute bits 16–20. 0 is wireframe (draw only the
  edges, later), 31 is opaque.
- **Order:** opaque polygons first, then translucent ones (alpha 1–30, or
  translucent texels). That's stage 1's order: keep it, don't re-sort on the
  GPU.
- **Depth writes:** translucent polygons write depth only with attribute
  bit 11. Depth test: "less" by default; bit 14 means "equal". There's no
  tolerance on the GPU, so use LEQUAL for it.
- **Alpha blending** (DISP3DCNT bit 3): normal src-alpha blending. Off means
  translucent pixels are written as is.
- **Alpha test** (DISP3DCNT bit 2) with ALPHA_TEST_REF: the PICA alpha test.
- **Clear:** CLEAR_COLOR (colour, alpha, fog bit) and CLEAR_DEPTH. Clear to
  the clear colour *with its alpha*: clear alpha 0 means BG0 is transparent
  where nothing was drawn. The rear-plane bitmap mode can wait.
- **3D into 2D:** a 3D pixel with alpha 1–30 blends with the layer under BG0
  using its own alpha, whenever that layer is a second target in BLDCNT,
  whatever the effect mode (like semi-transparent OBJs in our compositor).
  Pixels with alpha 0 are transparent. The compositor needs the alpha from
  the 3D image for this, so the image carries 5-bit alpha, not just an
  opaque bit.

### 7. Effects: in order of what the game uses

Before building any of these, log once per run which DISP3DCNT bits and
which polygon modes the game actually uses (a `[gx3d]` line), in the
overworld, a battle and the Pokédex. Then implement only what appears:
- **Fog** (DISP3DCNT bit 7, FOG_COLOR, FOG_OFFSET, the 32-entry FOG_TABLE):
  the PICA has fog with a lookup table indexed by depth. Map the DS table
  into it.
- **Edge marking** (bit 5, EDGE_COLOR by polygon ID): outlines around
  polygons; the field uses it (dark grey outlines round buildings, props and
  the screen's border). *Done* (`port_gx3d_edge.c`), as a pass over the
  finished image: an opaque pixel takes its polygon ID's edge colour when a
  neighbour (left, right, above, below) is of another opaque polygon ID and
  further away; next to the screen's border or a pixel without an opaque
  polygon, the rear plane counts (CLEAR_COLOR's polygon ID, CLEAR_DEPTH).
  With anti-aliasing on (bit 4) an outline is blended 17/32 with the
  neighbour behind it, as melonDS does with the pixel underneath. The PC
  reference keeps each pixel's opaque ID while drawing. The GPU path draws
  the opaque pixels a second time, depth equal, writing only their polygon
  ID to the stencil buffer (translucent passes leave ID + 64 there), and
  reads the depth/stencil buffer back with the image; if what comes back
  isn't IDs and 64-127/255, edge marking turns itself off and says so in
  the log.
- **Anti-aliasing:** skip.
- **Shadow polygons:** see 5.

### 8. Testing

The GPU can't run in CI, so split the code:
- **Pure, host-testable C:**
  - texture decoding (shared with the PC rasterizer) and conversion to
    RGBA5551/RGBA8 + Morton order;
  - the texture cache;
  - the mapping from (polygon attributes, DISP3DCNT) to an abstract "GPU
    state" struct (combiner mode, blend, depth test and write, alpha test);
  - batching;
  - vertex building (positions, toon lookup, texcoord normalization).

  Test it in `3ds_port/nitro/tests`. For every texture format, convert, then
  un-Morton and compare to what the software rasterizer samples. Cover
  state-mapping tables, batch boundaries, cache hits and misses,
  invalidation by the VRAM generation, and the triple buffer under
  concurrent acquire/publish (two threads, as test_render does).
- **A thin 3DS-only backend** (`port_gx3d_pica.c`, `#ifdef __3DS__`): only
  citro3d calls, driven by that state. Compile-checked by the 3DS CI build.
- **Comparison on the 3DS:** a debug key combination writes the newest 3D
  image to the SD card (`sdmc:/3ds/pokeplatinum/3d_frame_NNN.png`, or raw).
  The PC runner writes the reference image for the same frame. Compare them
  in Azahar's overworld.
- **[perf]:** add the 3D thread's ms per scene, triangles drawn, textures
  converted this second, and cache hits.

### 9. Not in stage 2

- Rendering the 3D at a higher resolution than the DS (a natural remaster
  option later: the GPU path makes it easy; keep the 256×192 assumption in
  one place).
- The 3D slider.
- Capture of the 3D output (display capture) for effects.

## The PRs, in order

Each is one PR from its own branch, reviewed before the next starts. Each
keeps every existing test green on host, host32 and the 3DS CI build. SDK
changes only as patches in `#ifdef SDK_PORT_3DS`. No game content.

1. **Plumbing.**
   - Triple-buffered scenes.
   - The newest-3D-image interface, used by `port_render2d.c` on both
     platforms (PC: the software rasterizer behind it).
   - The 3D thread, citro3d set-up, the shader, and the off-screen target +
     display transfer.
   - Untextured triangles in vertex colour, opaque, depth tested.

   Done when the overworld's geometry shows in Azahar as coloured shapes in
   the right places, and the PC output is unchanged.
2. **Textures:** shared decoder, conversion, Morton order, cache with the
   VRAM generation, wrap/flip, modulation and decal modes. Done when the
   overworld looks right in Azahar next to the PC reference.
3. **Alpha:** translucent order, depth-write bit, equal depth test, alpha
   test, blending, clear colour/alpha/depth, and 3D alpha into the 2D
   compositor.
4. **Toon/highlight and the `[gx3d]` feature log;** then fog and shadow
   polygons if the log shows them.
5. **Speed:** measure with [perf] in the overworld and in battle. Fix what
   shows up (vertex building, texture hashing, batch count).
