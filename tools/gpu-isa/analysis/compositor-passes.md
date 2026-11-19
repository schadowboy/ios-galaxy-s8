# Compositor render passes and their targets (backboardd, capture16)

Agent G, 2026-11-19. Sources: `tools/gpu-isa/data/capture16` (session_N.json deltas, final_*.bin, index.txt), `logs/sweetlog-emu1.txt`,
`core/S8Agx.cpp.reference`, kernel patches 0046 (swap path), 0150, 0153, 0154. Test references: `data/capture9` (test passes, session 31 = 64x64 BGRA clear/draw).
Tools written for this: `tests/replay.py` (state of a region after session N), `tests/scanatt.py`, `tests/scanva.py`, `tests/heads.py`, `tests/pass_test.c` + `tests/pass_test.sh`.
Evidence tags: [M] measured in a dump, [X] cross-checked against my own test passes (capture9/15), [I] inferred, [?] open.

## 0. Headline results

1. The executor's finder fails for two independent reasons. (a) The `fb05050a 00003e00 40000880` / `0305050a 00000000 40000c80` records in res_20 are NOT the
   compositor's render passes: they are a template of NULL attachments (8 slots, target 0x100003200 = a dummy sub-buffer of res_1). The real passes live in
   another 0x8000 ring, res_63 in this capture (0x100650000). (b) The render target is not an AGX resource with stand-in memory at all: it is an IOSurface.
2. A real compositor pass descriptor is the same shape as my test descriptor, only longer. The render target address is in the attachment record
   `{fmt, w1, VA>>4, 0x00000200, 0, 0}` exactly as in the test (`1fe3050a 000007e0 <VA>>4 00000200`): for the display passes `cf8354c2 000171e2 100a5000 00000200`
   -> VA 0x100a50000. It is also encoded in the header word A = `0x40000000 | ((VA & 0xffffffff) >> 2)` (0x40294000 -> 0x100a50000).
3. VA 0x100a50000 is backboardd's resource 101, created with **flags 0x82** and the "user pointer" field (struct +0x38) = **3**, which is the IOSurface id of the
   presented surface ("SWEET FB: swap 9 layer 0 surface 3 1440x2960"). Resources 94/96/97/100/101/103 (flags 0x82, "cpu" 0x2 or 0x3) are the two display
   surfaces (ids 2 and 3). The stand-in treats them as 0x4000-byte zero buffers ("caller memory" branch gives them no stand-in memory), which is why nothing ever
   reaches the screen. Rendering to them means: look up the SweetSurface by id (`sweet_surface_copy(id)`), write through `kernelAddress()`.
4. 'b3a8' is a 4-byte-per-pixel format (bpr 5760 = 1440*4). Its channel layout cannot be proven from the capture; everything we have says treat it as BGRA8 byte order
   (B at byte 0, A at byte 3). See section 3.

## 1. Layout of a compositor pass descriptor

### 1.1 Where the descriptors are
res_63 (VA 0x100650000, 0x8000 bytes) is the pass descriptor ring of this process; a frame writes its descriptors one after another (session 10 starts at +0x0, session 8
at +0x70 - the ring offset moves, old descriptors stay behind until overwritten) [M]. Per frame, 6-8 descriptors of 0xd0-0x1a0 bytes. res_64/65/66 hold per-pass state
blocks (bound-state tables, section 1.5). res_17 holds the uniform-push records that point at the descriptor (section 1.6).

### 1.2 Descriptor, as dwords (session 10, offsets in res_63) [M]
Display pass (target = the presented surface), starts at +0x00:
```
+0x00 fc0554c2 00171e2c 40294000 bfffc000 00000000 000ffff0     head record 1 (6 dwords)
+0x18 fb055982 00171e2c 3bbb8000 8059c000 0eff2280 00000590     head record 2 (same format code word with the 0xeeee0000-based placeholder address)
+0x30 000e0000 00000340                                          marker (same in test pass: +0x18)
+0x38 3f800000 0 0 3f800000 0 0  3f800000 0 0 3f800000 0 0      2 x 6 floats = (1,0,0,1,0,0)  (test pass: one block at +0x20, then the clear colour +0x38)
+0x68 fc0554c2 00171e2c 40294000 bfffc000 00000000 000ffff0     head record 1 again
+0x80 000e0000 00000340  +0x88: 6 floats (1,0,0,1,0,0)
+0xa0 cf8354c2 000171e2 100a5000 00000200 00000000 00000000     ATTACHMENT (target VA 0x100a50000)   <- test: 1fe3050a 000007e0 10006000 00000200
+0xb8 cfe35982 000171e2 0eeee000 80167200 0eff2280 00000590     shadow attachment (placeholder VA 0xeeee0000, skip)
+0xd0 00000000 00000006 cf8354c2 000171e2 100a5000 00000200 ... second copy of the attachment at +0xd8
```
Offscreen target pass (the "plain" class, identical shape to my test pass), +0x5f8 .. in session 10:
```
fb44464a 00007e01 402dc000 0 0 0 | fb44464a 00007e01 3bbb8000 0 0 0 | 000e0000 00000340 | 6 floats | head again | 000e0000 00000340 | 6 floats | 1ff2464a 000007e0 100b7000 00000200 ...
```
Test pass for comparison (capture9 session 31 res_18):
```
+0x00 fb05050a 00007e01 40018000 0 0 0 | +0x18 000e0000 00000340 | +0x20 6 floats (1,0,0,1,0,0) | +0x38 clear RGBA floats (0,0,0,1) | +0x48 1fe3050a 000007e0 10006000 00000200
```

### 1.3 Field by field
| field | meaning | evidence |
|---|---|---|
| head w0 low 16 bits = fmt code (0x050a test BGRA8, 0x464a offscreen, 0x5982 offscreen texture class, 0x54c2 the display surface) | pixel format / layout code of the attachment; identical in the matching attachment record's low 16 bits | [M] every head/attachment pair in capture16 and capture9 (tests/heads.py: `fb05050a`<->`1fe3050a`, `fb44464a`<->`1ff2464a`, `fc0554c2`<->`cf8354c2`). Meaning of the code beyond "same code = same attachment" is [?]. |
| head w0 top byte (0xfb, 0xfc, 0x1b) | flags / count of enabled attachments [?] | 0xfb in test and offscreen, 0xfc for the display surface |
| head w1 = (attachment w1 << 4) \| k, k = low nibble | attachment w1 is the packed descriptor, the head repeats it shifted by 4; k is a small control field: 1 (test, clear+store), 3, 4, 0xc (display surface), 0 (null template, `3e00`). Likely load/store action bits [?] | [X] `00007e01` = (0x7e0<<4)\|1 in test and offscreen; `00171e2c` = (0x171e2<<4)\|0xc; `00009203` = (0x920<<4)\|3 |
| head w2 = A = 0x40000000 \| (VA_lo32 >> 2) | the target base address, second encoding | [M] A=0x4025c000 -> 0x970000 -> res 94 (surface 2) in session 3; 0x4026c000 -> 0x9b0000 -> res 96 (surface 3) in session 4; 0x40294000 -> 0xa50000 -> res 101 (surface 3) in 8/10; test 0x40018000 -> 0x60000 -> 0x100060000 = the 64x64 target [X]. Cross-check A against the attachment VA, they must agree (all 18 pairs in capture16 do). |
| head w3 = B (0xbfffc000, 0x8007c000, 0x80080000, 0) | address-range / size of the attachment allocation, flag bit 31 [?]; 0 in the test | not decoded |
| head w5 = 0x000ffff0 (display, null template) | 0 in offscreen/test | not decoded |
| marker `000e0000 00000340` | constant, same in test | [X] |
| 6 floats (1,0,0,1,0,0) | viewport/transform of the pass in tile space (identity) | [X] same in test at +0x20 |
| clear colour: 4 floats right before the attachment (test: +0x38) | **present only for k with the clear bit**. The display passes and the offscreen `fb44464a` passes have NO clear block (6 floats go straight into the attachment record) | [M] distance head->attachment is 0xa0 for display passes against 0x48 in the test. The 4 floats printed by pass_test.c before such attachments are matrix leftovers, not a clear colour. |
| attachment w0 | fmt code (low 16 bits) + packing | see head w0 |
| attachment w1 (0x7e0 plain, 0x807e0 variant, 0x171e2 display) | packed descriptor [?]; two classes: 0x7e0/0x807e0 = "plain" (test), 0x171e2 = "surface class" | [M] |
| attachment w2 = VA >> 4 | **render target address**, VA = w2 << 4 (all targets are 64 KiB aligned in these captures) | [X] test: 0x10006000<<4 = 0x100060000 = res_11 of capture9, 0x4000 bytes = 64*64*4; compositor: 0x100a5000<<4 = 0x100a50000 = res 101 |
| attachment w3 = 0x00000200 | constant (the finder key) | [X] same in test |
| attachment w4, w5 | 0 | |
| second attachment copy `cfe35982 ... 0eeee000 ...` | shadow with placeholder address base 0xeeee0000, ignore | [M] 0x0eeee000 + n always pairs with the real record |

Dimensions and per-pass raster state are NOT in the descriptor (the test's 64x64 comes from the kernel command list, section 1.4). The load/store action is not
decoded with certainty (k nibble and the clear-block presence are the best candidates).

### 1.4 Kernel command list (shmem type 1, shmem_100 here, shmem_19 in capture9)
Per pass one 0x820-byte block, `{0x00010000, 0x00000820, <n>, ...}` at the block start [X: test block at +0]. At block+0x690: `{width, height, 1, 8}`, +0x698: 8
(tile 8x8) [X: test 0x40,0x40,1,8,8]. At block+0x30: u64 pointer to the pass's command stream (res_11 + offset here, res_14 in the test).
Session 8 blocks (base, w x h): 0x0 153x64, 0x820 1440x2960, 0x1040 128x74, 0x1860 132x78, 0x2080 128x74, 0x28a0 1440x2960, 0x30c0 64x64, (0x38e0 short block 0x3f8, 32x32),
0x44f0 1440x2960 [M]. The number of 1440x2960 blocks (3) equals the number of display attachments in res_63 (3 distinct, each with 2 copies) in session 8 [M].
Session 1 (first block) is missing from the capture (log ring), so block 0's header is not visible; the block pointer and dims are.
The blocks carry the dims in pixels, the viewport block in res_64/65/66 carries the same numbers as floats (centre/half-scale): res_65 `44340000 44340000 44b90000 c4b90000` = 720,720,1480,-1480 for 1440x2960 [M].

### 1.5 Draw/state side (what the current executor already finds)
Stream (res_11) draw packet: `<0x00NN0000> 6e44pp00 <count> <instances-1> ...` - in the compositor the first dword is 0x00220000/0x00260000/..., not the test's 0x001b0000
(that is the only difference to the finder in executePasses: it matches `d[i]==0x001b0000`). Primitive byte pp = 1, 6, 9 seen. Shader state records `0001001f/2f` (res_16) and
`0011011f/0011851f/0011051f/0011012f` fragment records (res_17, plus program VAs in res_3/res_10 and 0x1000f... ranges) are present; the fragment header differs from my test's
`0009051f` (the finder must accept `0x0011xxxx` too).

### 1.6 Link from the pass state to the descriptor (uniform pushes)
res_17 holds per pass: `{0x00200017 01006500} {0x30100013 01006500} {0x39800003 01006500} 0011011f <frag VA>`: three 2-dword uniform push records (section 23 format: address =
(w1<<8)|((w0>>24)&0xfe), size = (w0>>20)&0x1f halves, dest = (w0>>8)&0xff, type = w0&0xf) whose sources are res_63 +0x00, +0x30 (the 000e0000/340 marker, 1 dword) and
+0x38 (24 halves = the 12 floats). The next pass: `d0200017 01006500, 00100013 01006501, 09800003 01006501` = +0xd0, +0x100, +0x108. So the descriptor start of every pass is
named by a push in the pass's fragment-state area [M, decoded by the section 23 rule; the 0x...17 type-7 record points just before the attachment copy at +0xd8].
This is the reliable way to learn which of the ring's records belong to the CURRENT frame; the ring itself keeps stale descriptors (res_63 in session 9 still shows the
0x100bc0000 pass of the previous frame next to the new ones).

## 2. From a target VA to memory the stand-in can write

1. Resource create (selector 9, struct 0x68 B): flags at +0, user pointer at +0x38, size at +0x48. `flags 0x80` + a real user pointer = caller memory (textures backed by app memory,
   e.g. res 95/99 0x30000 at cpu 0x107630000, res 106/124 0x3c000). `flags 0x82` + pointer in {2,3,5} = **IOSurface id**: resources 94 (id 2), 96 (3), 97 (3), 100 (2), 101 (3),
   103 (2); the ids equal the `surface N` of the SWEET FB swap lines (2 and 3; swap 9 presents surface 3 after the session-9 passes rendered into resource 101 = VA 0x100a50000) [M].
   Resource 121 (id 5) belongs to SpringBoard's own client (surface id 5 is its LA08 icon texture, `SWEET SURF: SpringBoard create id 5`). The size field is 0 for these
   (stand-in default 0x4000) - the real surface size is in the SweetSurface.
2. Session-by-session confirmation of "target VA = the surface that is shown next" [M]: session 3 -> 0x100970000 (res 94, id 2) and swap 3 shows surface 2; session 4 ->
   0x1009b0000 (res 96, id 3) and swap 4 shows surface 3; sessions 8/10 -> 0x100a50000 (res 101, id 3) and swap 9/11 show surface 3.
3. Display path (patch 0046 `AppleCLCD::swap`): `sweet_surface_copy(id)`; the layer is copied with `memcpy` row by row from `s->kernelAddress() + s->offset + y*s->bytesPerRow` into the
   panel buffer `fPixels + y*fRowBytes`, `w*4` bytes per row, **no format conversion** (only `bpe == 4` is accepted; patch 0153's zero-copy flip additionally requires
   pixelFormat 'BGRA' and a pool slot, which 'b3a8' surfaces never get, so they take this copy). Patch 0154 (`sweetfb_shot=<s>`) reads the same memory as B,G,R,x bytes and prints
   thumbnails: a ready-made check that the GPU pixels arrived (`SWEET SHOT:` lines, tools/shot-decode.py).
4. So the stand-in must: (a) in the selector-9 handler remember `fResSurf[n-1] = *(uint64*)(in+0x38)` when `(flags & 0x82) == 0x82`; (b) when an attachment VA falls in such a resource,
   `SweetSurface *s = sweet_surface_copy(id)`, `uint8_t *px = (uint8_t*)s->kernelAddress() + s->offset`, stride `s->bytesPerRow`, width/height `s->width/height`, pixel (x,y) at
   `px + y*stride + 4*x`; `s->release()` afterwards (SweetSurface is declared in the patch-0046 header with `kernelAddress()`, `memory`, `bytesPerRow`, `offset`).
   Pixel data must be written before the completion records are posted (as the current code already does for the test).
5. Layout of the surface target is linear (it is scanned out/copied with bpr rows). For the offscreen targets (res_106..: 0x9400 = 128*74*4 bytes, no row padding) the layout is
   unknown: the executor can render them linear and the compositor's later sampling of them would then be self-consistent only if the sampler path is also executed by us (it is: our
   own fragment interpreter does the texture reads), so linear is fine as long as BOTH sides are ours.
6. Offscreen passes of the "plain" class write to ordinary stand-in resources (VA 0x100b60000 = res 115 0xc000 / 0x100b70000 = res 116 0x8000 in session 8); `findResMem` already finds them.
   The `fb055982`/`3fe35982 ... <VA>>4 8001f200 <VA2>>4 00000070` class (VA 0x100ac0000.. etc., 128x74) uses a different attachment record shape `{fmt, w1, VA>>4, 0x8001f200|0x80020200, VA2>>4, 0x70}`;
   VA2 - VA = 0x9400 = width*height*4, so VA2 is the end of the pixel data (a second plane / metadata after it) [M for the numbers, [?] for the meaning].

## 3. Format 'b3a8' (0x62336138)

- Measured: IOSurface 1440x2960, `bytesPerRow 5760` -> 4 bytes per element [M, SWEET FB lines]. The display copy treats every 4-byte element as opaque.
- The attachment format code of that surface is 0x54c2 (head `fc0554c2`, attachment `cf8354c2`), different from the BGRA8 code of my test target (0x050a) and from the
  offscreen codes 0x464a / 0x5982 [M]. So the GPU does NOT treat it as plain BGRA8Unorm (it is a different pixel-format encoding), but it is also 4 bytes per pixel.
- I found no 'b3a8' string in the extracted Metal/IOGPU/AGXMetalA11 binaries (scratch/agx/dylibs) and no earlier note in this tree; the name is not in the CoreVideo list I can
  verify offline. [?] whether it is 10-bit-per-channel extended range or 8-bit with colour-managed alpha.
- Working rule for the executor: treat the pixel as 32-bit little-endian word `A<<24 | R<<16 | G<<8 | B` (bytes B,G,R,A), i.e. BGRA8 byte order, converting float RGBA with the
  existing `agx_f32_unorm8` (clamp to [0,1], x255, round half up; verified against the hardware pack/unpack round trip, tests/pack_test.c). Reason: the panel scan-out path of this
  project expects BGRA bytes (0061: SWEET_FB_FORMAT_BGRA), the display copy does no conversion, and the earlier CPU-composited frames (same surfaces, same copy) displayed
  correctly. If colours come out wrong (blue/red swap or wrong gamma/brightness) the pixel is not BGRA8: next candidate B10G10R10A2 packed (`A<<30|R<<20|G<<10|B`) - the
  sweetfb_shot thumbnails of one test frame with a known fill colour decide this in one boot (fill the surface with 0xff0000ff = red in BGRA8; if the panel shows red it is BGRA8).
- Verification plan, no rendering needed: first executor step = fill every surface-class attachment with a constant (e.g. magenta B=0xff,G=0,R=0xff,A=0xff) at pass execution; the
  emulator screen or SWEET SHOT must turn magenta. That validates the whole chain (attachment scan, VA->id mapping, surface memory, swap) independent of shaders.

## 4. The scan rule for the executor (C, matches tests/pass_test.c)

```c
/* attachment record: 6 dwords { fmt, w1, VA>>4, 0x200, 0, 0 } */
static bool agx_att(const uint32_t *d, uint64_t i, uint64_t nd, uint64_t *va, uint32_t *fmt)
{
	if (i + 6 > nd || d[i + 3] != 0x200 || d[i] == 0 || d[i + 1] == 0) return false;
	if (d[i + 2] < 0x10000000u || d[i + 2] >= 0x20000000u) return false;   /* VA in 0x1_0000_0000..0x2_0000_0000 (>>4); skips the 0x0eeee000 shadow copies */
	*va = (uint64_t)d[i + 2] << 4;
	*fmt = d[i];
	return true;
}
```
Use:
1. Scan every shmem-backed resource (same loop as the current pass finder) with `agx_att`. This finds the test pass too (`1fe3050a 000007e0`), so one rule replaces the
   `d[i]==0xfb05050a && d[i+1]==0x7e01 && ...` match.
2. Classify the target: `findResRegion(va)` for ordinary resources (offscreen); if `fResSurf[n-1] != 0` the target is the IOSurface (section 2). Skip attachments whose VA has no
   resource (shadow copies, stale).
3. The head record is found by scanning backwards <= 0x60 bytes for a dword with `(w & 0xffff) == (fmt & 0xffff)` and top byte >= 0xf8; confirm `head.A == 0x40000000 | ((va & 0xffffffff) >> 2)`.
   If a clear block exists (test-class head, distance head->attachment == 0x48, equivalently `k` low nibble of head w1 has bit 0 set) the 4 floats at attachment-0x10 are the clear RGBA
   (test: `d[att-4..att-1]`). If there is no clear block the pass loads: do not clear, keep the previous surface content (the first compositor pass of a frame usually has k=0xc).
4. De-duplicate: the two (sometimes three) records with the same fmt+VA within 0x40 bytes are copies of one pass: execute the pass once (use the first one that follows a head).
5. Which descriptors are current: ring entries from older frames remain. Use the uniform pushes of res_17 (section 1.6): a descriptor is current if a push record `(w0&0xf)==7, hi byte of
   w1 == 1` names an address within 0x10 bytes before one of its attachment copies. Cheaper fallback already used by the executor: `agx_rec_changed(n, offset, kind, hash of 6 words +
   the 6 words before)` and execute only the records that changed since the last scan; the ring rewrites every record each frame, so "changed since last scan" also works when the
   descriptor of two consecutive frames is identical only because the same slot was re-used (then force-execute once per submit: the submit itself is the trigger).
6. Dimensions: from the surface (`s->width x s->height`) for surface targets; for offscreen passes from the kernel command-list block (dims at block+0x690/+0x694; find blocks by
   the dword pair `0x00010000, 0x00000820`), paired with the descriptors in order of the command list vs. order of the pushes in res_17 [?: pairing by order is unverified, see open items].
7. Draw-packet match: relax `d[i] == 0x001b0000` to `(d[i] & 0xffff) == 0 && (d[i] >> 16) < 0x100 && (d[i+1] >> 16) == 0x6e44`; fragment record headers `0x0011011f/0x0011851f/0x0011051f/0x0011012f` next to the test's `0x0009051f`.

## 5. Open uncertainties
1. Pass <-> kernel block pairing (which block's draws belong to which descriptor). Order in the ring is not the kernel block order in session 8 (the first ring pass after the head is a 128x74 offscreen pass
   while block 0 is 153x64). The res_17 push records are the intended link but the decode of `0x...17` type-7 pushes is partial (sections 1.6 / 23).
2. Load/store actions: k (head w1 low nibble) and presence of the clear block are consistent with the test (k=1 clear) but there is no compositor ground truth; 0xc is unexplained.
3. The meaning of attachment w1 (0x7e0, 0x807e0, 0x171e2), head B (0xbfffc000 etc.), the 0x590/0x167200 words of the shadow records and the fmt codes 0x54c2/0x5982/0x464a.
4. Offscreen target memory layout (linear vs twiddled): irrelevant while both producer and consumer are ours, but the compositor's CPU-visible textures (caller memory 0x80 resources: icons, 0x3c000 etc.) are written by iOS in
   the real layout; sampling them needs the texture-descriptor decode (section 24 of agx-kernel-interface.md), not covered here.
5. Whether surface-class attachments are always the full surface (dims = surface) or can be partial (the 3 full-screen passes per frame plus smaller passes onto the same surface would be dirty-region updates).
6. 'b3a8' channel layout (section 3).
7. The capture misses session 1 (log ring wrap) and per-region caps (1100 lines, 9000 total) cut res_10/res_72.. but none of the regions used above is cut.

## 6. Extra capture that would settle 1, 2, 3, 6
Recipe = agx-kernel-interface.md section 30 (run.sh ... `s8agxexec=backboardd s8agxdump=backboardd s8agxskip=0`), with these changes in S8Agx.cpp (constants, not done by me):
- dump only res_11/16/17/20/25/63/64/65/66 and shmem_100 (skip the program heaps res_10/72-77, which eat the 9000-line cap), and print the whole selector-9 struct (112 B) for flags 0x82 resources;
- log one line per submit: for each kernel block (dword pair 0x00010000/0x820): base offset, w x h, stream pointer; plus every attachment the new scan finds (res, offset, fmt, w1, VA, resource id, surface id);
- start the dump at the first frame after SpringBoard launch (`s8agxskip=<n>`), and flush the console ring earlier (the first session was lost);
- for item 2/6: run once with the executor filling every surface attachment with a flat colour per pass (pass index -> colour) and take `sweetfb_shot=2`; the colour of each screen region
  reveals pass order and pixel format at once.

## 7. Recommended executor change (summary)
1. `agx_att` scan replaces the test-layout match; execute passes per attachment (de-duplicated, section 4).
2. Selector 9: record IOSurface ids (`flags & 0x82 == 0x82`, id = `*(u64*)(in+0x38)`) per resource; `sweet_surface_copy` + `kernelAddress()` for those targets.
3. First milestone: fill the display surface attachment with a constant and see it on the emulated screen (and in SWEET SHOT); then run the existing draw interpreter against these targets,
   with `0x001b0000` relaxed and the compositor's fragment record headers accepted.
4. Do not treat the res_20 `fb05050a 00003e00` records as passes (null-attachment templates).
