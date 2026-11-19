# The compositor's AGX command stream (backboardd, capture17): packet layout and draw list

Agent J, 2026-11-19. Data: `data/capture17` (rebuild of the stand-in's dump of backboardd in the QEMU emulator, sessions 2..10), `logs/sweetlog-emu2.txt`.
Code: `core/s8gfx_stream.h` (decoder, integer-only, callback reader), `tests/stream_decode.c` (host test and draw-list printer), `tests/stream_test.sh` (runs it on
sessions 8, 9, 10), `tests/replay_dump.py` (session-N snapshot of every region), `tests/stream_py.py` (token dumper / exploration, Python).
Evidence tags: [M] measured in the dump and reproduced by the test, [X] cross-checked against another structure of the capture, [H] hypothesis (consistent, not separable),
[?] unknown.

## 0. Headline

1. The stream in res_11 (VA 0x100130000, 32 KiB ring, rewritten from +0x40 every frame) is **not a flat list of draws**. It is a chain of *pass streams*; a pass stream is
   entered through the start pointer of its block in the kernel command list (shmem type 1, block +0x38), begins with a LINK, and consists of **LOAD records** (copy N dwords
   from a state ring into the hardware), **vertex-stage launch records** and **draw packets**. Per draw there is no vertex/fragment address in the stream itself: the stream
   points at *slots* in two state rings (res_16 vertex, res_17 fragment), and each slot is a list of push records followed by a state record carrying the program address.
2. The decoder produces for every draw: primitive, count, instances, index VA, vertex program entry, fragment program entry (+ program block, + tail), the full push lists
   (vertex uniforms incl. the 64-bit **vertex-buffer pointer**, fragment uniforms, texture-state and sampler-state pushes), viewport + tile scissor, blend flag, sampler load,
   and the pass's **render target VA** (decoded from the pass's store/load slots). On session 8 of capture17: 10 passes, 31 draws, 0 unknown stream words, 0 unknown loads;
   31 of 31 vertex buffers found by the decoder hold plausible quad vertices in pixel space (full-screen (0,0)-(1440,2960) quads, icon rects, 128x74 blur tiles ...).
3. Pass streams skip a per-pass **clear draw** (a `7e44` draw whose head record is overwritten by the LINK). All 10 passes of every captured frame skip it, i.e. nothing in this
   capture is cleared by a draw; the "clear" colour/load program is the pass's slot A (section 5).
4. What stops execution is no longer the stream: it is (a) program bytes missing from the capture (res_59 / res_3 / parts of res_72..77: 13 of 31 vertex and ~8 fragment programs
   are all-zero in the dump, section 8), (b) fragment ISA forms not in `s8f_len` (f16 `76 7a f6 fa f9`, `2a`, `56`, `c4`...), (c) texture format / tiling of the layer textures,
   (d) the 'b3a8' surface format, (e) caller-memory texture pixels are not visible to the dump. Section 9.

## 1. Where things live (capture17, backboardd; ids = resource numbers of that process)

| region | VA | role |
|---|---|---|
| shmem_100 (type 1) | none (kernel command list) | 0x820-byte pass blocks; +0x38 stream start (u64), +0x690/+0x694 width/height, +0x2a8 flags, +0x2b0 / +0x2d0 fragment slot pointers A and B |
| res_11 | 0x100130000 (0x8000) | the pass streams (this document) |
| res_16 | 0x100180000 (0x8000) | VERTEX slots: pushes + `0001xx1f/2f/3f` state record |
| res_17 | 0x100190000 (0x8000) | FRAGMENT slots: pushes + `0011xx1f/2f/3f` state record |
| res_25 | 0x100280000 (0x8000) | LOAD source records (pipeline state, fragment bind, vertex info, constants) |
| res_19 | 0x100220000 (0x8000) | table of `{0x20, 0|1}` draw-slot pairs |
| res_4 | 0x100030000 (0x10000) | sampler-state ring (LOAD3 sources and 0x13 push sources) |
| res_20 / res_23 | 0x100230000 / ... | texture-state + sampler-state records pushed by the fragment slots (0x17 / 0x13) |
| res_48..57, 62, 64..66 | 0x1005..0x1006.. (0x8000 each) | per-pass state block: viewport block (LOAD9 source, +0x398 / +0x3e0 ...), vertex-buffer pointer pairs (+0x390 + 8n) |
| res_63 | 0x100650000 (0x8000) | per-pass render-target records (texture-state form and attachment form), pass-load uniforms |
| res_98 | 0x1009d0000 (0x40000) | uniform / constants ring |
| res_103 | 0x100a70000 (0x40000) | vertex data + index data ring (vertices stride 0x30, u16 indices behind them) |
| res_10, res_59, res_72..77 | 0x1000e0000 (0x50000), 0x100610000, 0x1007a0000.. | program heaps (vertex + fragment programs, blocks) |
| res_3 | 0x100020000 | builtin programs (0x200f0 clear vertex shader, 0x2cf58 / 0x2f358 tails): NOT in the capture |

## 2. Stream grammar [M for the grammar, see column for the meaning]

Dwords, little endian, all pointers are the low 32 bits of a GPU VA 0x1_0000_0000 | lo [M: every pointer resolves inside a dumped resource].

| first dword | length | record | fields |
|---|---|---|---|
| `80000001` | 2 | LINK | w1 = target lo. First record of every pass stream (start pointer of the block); the link was written over the pass's first A record (section 5) |
| `c0000000` | 1 | END | after the pass's closing LOAD3 `02000000 ..` |
| `(n<<8)\|1` | 2 | LOAD n | w1 = source lo; n dwords are copied from the source; the kind is told by n and the first source dword (table 3) |
| `48004000` / `48002000` | 2 + 2 | A + B | A = {`0x48000000 | thread class (0x4000 / 0x2000)`, 2 or 4}; B = {vertex slot pointer `slot|4`, register class (4,6,8,0xa,0xc,0x11)}. One per draw, before the draw's loads [M]. Class 0x2000 appears exactly with register class >= 0xa [M]: occupancy pair (A.w0 low half = threads, B.w1 = registers) [H] |
| `0x5000xxxx 0000ffff` | 2 | index prefix | before every `7e42` draw (`50004000`, `50002000`, `5000c060` before `7e44`). The low half equals A's. `ffff` = index mask / restart [H] |
| `6e44pp00` | 5 | non-indexed draw | {hdr, count, instances-1, 0, trailer}; pp = primitive: 9 = triangle strip (the 4-vertex rect draws), 1 = ? (count 814, even counts: lines [H]; seen only in session 8's first frame) |
| `7e42pp01` | 6 | indexed draw | {hdr, index VA lo, count, instances-1, 0, trailer}; pp = 6 triangle list; 16-bit indices [H: indices read as u16 give max 3 / 15 / 31 / 35 for 6 / 24 / 48 / 54 indices = quads, all < vertex count] |
| `7e440001` | 6 | "clear" draw | {hdr, 0x260000 (res at VA 0x100260000: not dumped), 0, 0, 0, 0x200}; always in the skipped chunk |
| `20000160` | 1 | barrier | between the skipped chunk and the body [H] |

Trailer values (0xaa indexed, 0x1fe strip, 0x100 prim 1, 0x200 clear) are not understood [?]; they do not matter for execution so far.

### 2.1 LOAD kinds (table 3) [M for field positions; meaning per column]

| n | first dword | source ring | fields | meaning |
|---|---|---|---|---|
| 9 | `0x00000c00` | per-pass block (res_48.. +0x398) | [0xc00][`0x8000_0000 \| xmin<<16 \| xmax`][`ymin<<16 \| ymax`][tx][sx][ty][sy][0.0][1.0] | viewport block. Words 1/2 are the 32-pixel **tile bounds of the pass region**: 1440x2960 -> xmax 44 ymax 92 (`8000002c 0000005c`), 128x74 -> 3 / 2, 55x55 -> 1 / 1, partial regions `80230027 00520057` [M, [X] tile = 32 px: 1440/32 = 45, 2960/32 = 92.5]. Floats = centre/half-extent, y flipped: 1440x2960 -> 720 720 1480 -1480; 128x74 -> 64 64 37 -37 [M, [X] agrees with capture16 and the test viewports] |
| 6 | `0x0000001f` | res_25 | [0x1f][0x00050000 (0x00240000 in old frames)][ctl][0x0e000000][ctl][0x0e000000] | pipeline state. ctl = `002e0f00` (blend off) / `012e0f00` (blend on) / `102e0f00`: bit 24 = blending [M: 17 of 31 draws have it; the draw-by-draw list below shows it follows the blend shaders (ld_tile programs)] [H for the exact meaning of the other bits] |
| 5 | `0x000000c0` | res_25 | [0xc0][flags][fragment slot `ptr\|k`][program tail VA][register class 2/3/4/7] | **fragment bind**: w2 -> fragment slot in res_17 (section 4); w3 = end of the program image (a few 0x10 bytes after the program; 0 for some) [X: entry+0x70..0x190]; w1 flags `0x38002 / 0x78212 / 0xfff80002 / 0xffff822c` [?] |
| 3 | `0x02000000` | res_25 | [0x02000000][`xmax*4<<16 \| ymax`][0] | pass-closing tile bounds, then END |
| 3 | `0x00210000` | res_25 | [0x00210000][0x1e3ce508][0x000004a0] | constant (same in every draw) [?] |
| 3 | other | res_4 | [0x000c0000 or 0][1 word: 0 / 2 / 4][0 / 0x400] | **sampler binding**: w1 = 0 with no texture, 2 with one texture state (11 of 17), 4 with two samplers; w2 = 0x400 for the pass-through (display copy) samplers. The sampler STATE itself comes by the 0x13 push [H] |
| 2 | `0x00020000` | res_25 | [0x00020000][0x0N0F0000] | vertex info: N = B's register class, F = 1 (5 for N = 0x11) [M] |
| 2 | `0x00000020` | res_19 | [0x20][0 or 1] | per-draw slot pair (ring index 0x00220000 + 8n, +8 per draw that loads it) [?] |

State persists: a LOAD stays in force until the next LOAD of the same kind. The first draw of a pass loads everything; later draws of the same pass re-load only what changed
(typically B, the fragment bind when the pipeline changes, the sampler, the pipeline state). The decoder keeps a persistent state and copies it into every draw.

## 3. Slots (vertex and fragment) [M]

A slot is found from a pointer `p`: slot start = `p & ~0x1f`, `p & 0x1f` = k (4 for every vertex slot, 2..5 for fragment slots; meaning [?]). It is a run of **push records**
(2 dwords), then one **state record**, then optional trailers:

```
push   w0 w1                  type = w0 & 0xff:  03 / 33 uniform   13 sampler state   17 texture state
                              source VA = (w1 << 8) | (w0 >> 24 & 0xfe)      w1 >> 24 == 1 (VA bit 32)
                              size      = (w0 >> 20) & 0x1f units (halves for uniforms, 0 = 32)
                              dest      = (w0 >> 8) & 0xff  (register half index for uniforms; state index for 13/17)
state  hdr entry|f [extra] [0x108]  [c0400038 block]
                              hdr 0x0001xx1f/2f/3f = vertex program, 0x0011xx1f/2f/3f = fragment program (b1 bit 2 = extra pointer, b1 bit 7 seen with the 851f class)
                              entry = word & ~3 (flags in the low bits: 2 for fragment programs, 0 for vertex)
                              extra = res_3 pointer 0x2cf58 / 0x2e358 / 0x2f358 (fragment tail program, not in the capture)
                              block = c0400038 <VA>: start of the program image (header block + code), see section 7
```

Uniform pushes (type 03) land in the shader's uniform file at **half-register index `dest`** (the stand-in's `uni[]`, rule already used by the executor: `uni[dest + 2j] = word j`).
Measured roles [M unless noted]:

| vertex push | meaning |
|---|---|
| first push, 4 halves (dest 0, or 4 in the res_75 programs) | source = per-pass block + 0x390 + 8n: **two dwords {vertex buffer VA lo, 1} = a 64-bit pointer**. Vertices: stride 0x30 (index data starts 4 x 0x30 = 0xc0 after the vertex data) [M on 31/31 draws: floats (x,y,0,1) of the first 4 vertices are quad corners in pixel space] |
| dest 0xc or 0x8, 32 halves (64 B), source half-aligned (...82, ...c2) | transform block (matrix and constants, f16/f32 mix) [H] |
| dest 0x2c/0x10/0x14, 8 halves | per-draw constants such as tint (1.0 1.0 ...) [H] |
| dest 0x18, 2 halves, source res_14+4 | constant [?] |

Fragment pushes: uniform (03) at dest 0 4 halves = pointer pair to the per-draw fragment uniform block (res_103 ring), more uniforms at dest 4/8/0x16/0x18...; **0x17 = texture state**
(6 dwords at the source: see section 6), **0x13 = sampler state** (`028e0000 00c00780` linear, `000e0000 00c00780` nearest, `200e0000 00c00789` another, 2 dwords; the following
two dwords belong to the next record) [X: Agent I's sampler words].

## 4. Pass blocks and the render target [M]

The kernel command list block of a pass (marker `{0x818, 0x7e8, 0x30, 1, stream lo, 1}` at block +0x28; the first block of a capture can lose its header, so the marker is used):

| block offset | field |
|---|---|
| +0x38 | stream start VA (LINK) |
| +0x690 / +0x694 | width, height in pixels (694x435, 1440x2960, 128x74, 132x78, 696x437, 64x64, 55x55 seen) |
| +0x2a8 | flags (`0x78002`, `0x38212`, same words as LOAD5.w1) |
| +0x2b0 | **slot A** pointer (res_17): the pass's **load/clear program**: either a uniform push of the clear colour (res_63 +0x138.. = (0,0,0,0) for the offscreen passes) or a texture push of the target itself (display passes: the previous contents are *loaded* through a texture read) |
| +0x2d0 | **slot B** pointer (res_17): the pass's **store program**; its 0x17 push names the render-target record |
| +0x2d8 | pointer into res_18 (+0x10, +0x18, +0x20 ... per pass) |

**Render target of a pass** = the first 0x17 push of slot B (else slot A): its 6-dword record is either the attachment form `{fmt, w1, VA>>4, 0x200 | 0x8001f200 ...}` (form 1) or the
texture-state form `{fmt, w1, 0x40000000 | VA>>2, ...}` (form 2); both give the same address [M: 10 of 10 passes, session 8: 100ab0000 (res_104, 694x435), 100a50000 (display surface, 1440x2960,
fmt cf8354c2), 100c90000, 100cb0000, 100ca0000, 100cc0000, 100e40000, 100e50000 ...]. Agent G's attachment rule is the same record, now reached through the pass block. fmt/w1: 0x5982 / 0x464a class
codes as in compositor-passes.md; w1 = 0x171e2 for the display surface, 0x3641 for 694x435 (0x3641 = 0x1b2 << 5 | 1 ... [?]).

## 5. The head LINK and the skipped "clear" chunk [M, H]

Every pass stream is `[A record patched into LINK][chunk X][20000160][body][closing LOAD3 + END]`. The first record of the stream is the LINK `80000001 <body>`; the 2 words behind it
(`B`: vertex slot pointer of the chunk) belong to a chunk whose **A record was overwritten by the LINK**: a full-screen `7e440001 00260000 ...` draw with the builtin vertex program
`0x1000200f0` (res_3) and the pass's slot A as fragment program. The chunk has no LOAD5. It is executed only when the head is not patched, i.e. when the pass **clears**; all 10 passes in
all captured frames are patched (load-action = load / don't care), so the executor does not need it for this capture. For a pass whose stream starts with `A` instead of `LINK`, scan from the
start pointer as usual: the draw comes out as `S8S_DRAW_OTHER` with `fs.found == 0`; use `s8s_parse_slot(slotA)` as its fragment program and the slot A uniform as the clear colour.

## 6. Textures and samplers per draw [M for the pointers, H for the fields]

The fragment slot's 0x17 push gives a texture-state record; the record decodes with the same two forms as the render target (`s8s_texrec_va`). On session 8: icon layers sample
res_105 / res_106 (VA 0x100c30040, 0x100c70040: caller memory, texel bytes NOT in the dump), the blur chain samples earlier passes' targets (pass 2 reads the display surface
0x100a50000, pass 3 reads 0x100c90000, pass 4 reads 0x100cb0000, pass 5 reads 0x100ca0000: pass order is the dependency order), SpringBoard's own LA08 surface 0x100e90000 (IOSurface 5).
Record words: `28b68002 00030412 4030c010 80094000 0 000ffff0` (icon, 4030c010<<2 = 0xc30040), `fb444642 00000007 40320000 801fc000 ...`; w1 = size/format field [?] (Agent I: 0x600 for 4x4, 0x7e0
for 64x64; the compositor's w1 = 0x30412 / 0x9c15 / 0x7 / 0x171e2 are not covered by that rule).

## 7. Program addresses (what the state record's entry means) [M for the vertex rule, M/H for the fragment rule]

* **Vertex**: `entry` is 0x30 bytes inside the program (a 12-instruction constant/fetch prologue before it). `s8v_find_start(entry)` returns entry-48 and the `s8f_len` walk from there reaches the
  `08 01` end for **every captured vertex program** (session 8: 18 of 18 with non-zero bytes; the other 13 are zero in the dump). [M]
* **Fragment**: when the state record is followed by `c0400038 <block>`, the block is a **program image**: a 0x40-byte header (`0d000105 0012c000 028a0038 200400c0 10400402 00080108 00080008..`) and the code
  at **block+0x40**. A walk from block+0x40 with `s8f_len` (+ f9 f6 fa 76 7a as 8-byte forms) lands exactly on `entry` in 8 of the 17 session-8 fragment programs with a block (12 of 27 in session 9, 6 of 15 in session 10), usually at
  block+0x70 (code +0x30, right after the first texture sample and its `38 00` wait), also +0x50 / +0x60; 7 of those continue to a `08 01`. The code before the entry is the tile / texture preamble
  (`48 wait_pix`, `f9 ld_tile`, `51 iter`, `21 texture_sample`, `38 00`), the code after it the shader proper. [M for 8; the rest stop at forms `s8f_len` does not know (2a, 56, c4) or whose bytes are zero]
  A block can hold several shaders separated by `08 00` padding (blocks e1c40, e2d00): the entry is the later one.
* `extra` (0x2cf58 / 0x2e358 / 0x2f358) and `tail` (LOAD5.w3) are not decoded; tail = image end + small table (`00000303 0000000c 0001011d`) [H].
* Programs without a block record (res_59 / res_75 0x1008xxxxx, 0x1000e00f2 ...) are verified by search only (`search start +0 end +28` = the constant-colour 28-byte programs `42 00 08 00 .. 89 .. 08 01`).

## 8. Draw list of capture17

`tests/stream_test.sh` replays sessions 8, 9, 10 (session 10 == `final_res_*.bin`; the final files are a mix of sessions only for rings that session 10 did not rewrite, which is why session 8 is
the cleanest frame: a full rewrite of res_11 from +0x40). Session 8, 10 passes / 31 draws (`DL` lines of the test; prim 6 = triangle list, 9 = strip; vs/fs = entry VAs; vb = vertex buffer, ib = index buffer;
tex = number of texture pushes, texture0 = its address):

```
pass 0 (target 100ab0000, 694x435)
 p0.0 lst prim9 n4  vs 1006144e0 fs 100618f80 vb 100a777c0 (rect: 373,303,1067,738)            blend 0
 p0.1 idx prim6 n6  vs 1000e1830 fs 1000e16b0 vb 100a77800 ib 100a778c0 tex 100c30040          blend 1
 p0.2 idx prim6 n6  vs 1000e1830 fs 1000e16b0 vb 100a778f0 ib 100a779b0 tex 100c70040          blend 1
 p0.3 idx prim6 n6  vs 1000e1b30 fs 1000e19b0 vb 100a77aa0 ib 100a77b60 (no tex)               blend 0
pass 1 (target 100a50000 = display surface, 1440x2960)
 p1.0 idx n6  vs 10061eb30 fs 10061ea00 vb 100a77c80 (0,0)-(1440,2960)  p1.1 same, vb 100a77d70   p1.2 vs 1000e1f30 fs 1000e1cf0 tex 100c80000 blend 1
pass 2 (100c90000, 128x74): p2.0 n54 tex 100a50000 | p2.1 n24 | p2.2 n24 | p2.3 n6 blend 1
pass 3 (100cb0000, 132x78): p3.0 n48 | p3.1 n6 tex 100c90000     pass 4 (100ca0000): p4.0 n6 tex 100cb0000     pass 5 (100cc0000, 696x437): p5.0 n6 tex 100ca0000
pass 6 (100a50000): p6.0 lst prim9 n4 | p6.1 tex 100c30040 | p6.2 tex 100c70040 | p6.3 tex 100cc0000 | p6.4 tex 100ab0000   all blend 1
pass 7 (100e40000, 64x64): p7.0 n54 tex 100a50000           pass 8 (100e50000, 55x55): p8.0..2 n6 blend 1 (vs 100617af0 fs 100800670)
pass 9 (100a50000): p9.0 tex 100e50000 | p9.1 | p9.2 tex 100e90000 | p9.3 n30 tex 100ea0000 | p9.4 n24 | p9.5 tex 100a60000 | p9.6 (0,0)-(1440,2960) blend 1
```
(the full list with every address is the output of `tests/stream_test.sh`, `V=-v` adds all pushes).

Checks run by the test (session 8 / 9 / 10):
| check | result |
|---|---|
| passes found / with a render target | 10 / 10, 10 / 10, 10 / 10 |
| draws, unknown stream words, unknown loads | 31 / 35 / 28 draws, 0 unknown words (1 in a stale session-10 pass), 0 unknown loads |
| vertex programs that walk to `08 01` (start = `s8v_find_start`) | 18 ok, 0 bad, 13 zero in the dump / 25 ok, 1 bad, 9 zero / 15 ok, 1 bad, 12 zero |
| fragment programs (walk from block+0x40 to entry; else search) | 11 / 15 / 9 verified, rest hit unknown forms or are zero |
| index data | all indices < vertex count (max 3 / 15 / 31 / 35), 29 / 33 / 27 indexed draws |
| vertex buffers (first 4 positions finite, |x|,|y| < 8000, mapped) | 31 of 31 / 34 of 34 / 27 of 27 |
| `g++ -fsyntax-only -x c++ core/s8gfx_stream.h` | clean |

## 9. What remains unknown, and what blocks executing the compositor draws

Unknown stream fields [?]: trailer words (0xaa, 0x1fe, 0x100, 0x200); A.w1 (2 / 4) and B.w1 meaning beyond "register class"; the k bits (ptr & 0x1f); LOAD5.w1 flags; the constant LOAD3 `00210000 1e3ce508 000004a0`;
LOAD2 `0x20` pair; prim code 1 (814-vertex draw); 16-bit index assumption; blend = ctl bit 24 is only correlated; what `20000160` does; the `0x260000` buffer.

Blockers for executing the draws, in order:
1. **Missing program bytes**: 13 of 31 vertex and 8 fragment entries point into res_59 / res_3 / res_72..77, whose first (full) dump was lost with session 1's log wrap and whose later deltas only carry 24 changed lines.
   Dump rule change (core/S8Agx.cpp.reference `dumpSparse` + the `fSubmitDumps` block, ~line 1353): (a) make the first full dump of a region land in a session that survives: flush the console ring before the
   first dump (the `s8agxskip=N` path only delays it), (b) stop spending the 9000-line budget on the texture/target regions: skip regions whose id is a render target or surface (res_1, res_104, res_112, res_117,
   res_68..71 pattern 0x30000 pixel data) via a boot-arg list, e.g. `s8agxres=3,4,10,11,14,16,17,18,19,20,23,25,35,48..57,59,62..66,72..77,98,102,103` and a per-region cap of 4096 lines for the program heaps
   (res_10 0x50000, res_59, res_72..77 0x30000 each, res_3), (c) log `S8AGX: resource` lines for every id (the VA table is complete), (d) dump the **caller-memory texture resources** (0x80 flags: res_95, 99, 105, 106, 107,
   119, 121, 122 = layer textures) through the IOMemoryDescriptor of the user pointer, the dump currently only sees `fShmemBuf`, and the resource at VA 0x100260000 (the clear draw's rect).
2. **Fragment ISA**: the compositor's fragment programs use f16 ALU forms `f6 fa 76 7a` (8 bytes, verified only by the walk landing on the entry), `f9` ld_tile with bit 7, `2a`, `56`, `c4`, `c3`, `c2` data forms,
   MSAA/resolve-like four-way `ld_tile`; `s8f_len` must be extended before the interpreter can walk them (Agent H).
3. **Texture state + tiling** of the layer textures (sizes 0x3c000 / 0x10000 caller memory, formats `8002` / `4642` / `5982` codes, w1 size fields), not decoded (Agent I's tiling for > 4x4 is a placeholder).
4. **Vertex programs**: uniform-file layout is now known (pushes, halves), vertex buffer = push 0 pointer, stride 0x30: the interpreter's fetch (`ctx->stride`) must take the 64-bit base from `uni[dest], uni[dest+1]`.
5. **Render target format** 'b3a8' (display surface, fmt code 0x54c2) and the offscreen codes: first milestone = BGRA fill already shows the chain; per-format pack needs the colour test.
6. Pass order = dependency order of the block list [M: textures of pass k+1 are targets of pass k]; the executor must run the passes in command-list order and write each target before its readers.

## 10. API summary (`core/s8gfx_stream.h`)

```
typedef int (*s8s_rd_fn)(void *ctx, uint64_t va, uint32_t *out);       /* 1 = mapped (agxRd + findResMem gives this) */
int  s8s_find_passes(const uint32_t *cmdlist, uint32_t ndw, struct s8s_pass *out, int max);   /* kernel command list (shmem type 1) -> passes */
void s8s_pass_target(s8s_rd_fn, void *ctx, struct s8s_pass *p);                              /* render target of a pass (slots A/B) */
int  s8s_scan(s8s_rd_fn, void *ctx, uint64_t stream_va, uint32_t pass, struct s8s_draw *out, int max, struct s8s_stats *st);  /* draw list of one pass */
void s8s_parse_slot(s8s_rd_fn, void *ctx, uint32_t ptr, struct s8s_prog *pg);                /* vertex / fragment slot: pushes + program entry */
int  s8s_texrec_va(const uint32_t w[6], uint64_t *va);                                        /* texture-state / attachment record -> address */
```
`struct s8s_draw` carries pass, stream address, kind, prim, count, instances, index VA, A/B words, vertex and fragment program (`struct s8s_prog`: entry, flags, extra, block, push list), fragment bind words,
pipeline words + blend flag, sampler words, viewport floats and tile bounds. Memory use: about 1.3 KB per draw; the kernel should call `s8s_scan` once per pass with a pass-local buffer.

## 11. capture18 re-run (coordinator's second capture, `s8agxnores=10,117,103,98,68,69,70,71`)

Run: `wsl -u root -d Ubuntu -- env CAP=data/capture18 SESS="11 14" MERGE="/tmp/s8s/capture17/s10" bash tests/stream_test.sh` (`CAP`, `SESS`, `MERGE`, `V` env options; `MERGE` fills zero bytes of the program
heaps res_3 / 10 / 59 / 68..77 from another snapshot). Per-draw lines `DL ... vs <entry>[walk-ok|no-bytes|FAIL] fs <entry>[walk-ok|entry-ok|no-bytes|unwalked]` in `/tmp/stream_capture18_s<N>.txt`
(copies of the verbose output of sessions 11 and 14: `logs/_stream_capture18_s11.txt`, `_s14.txt`).

* **Session 1 is intact** (res_3, res_59, res_72..76, res_4, res_11..25 first dumps), but the skip list removed regions that hold programs: **res_10 (0x1000e0000, program heap, 0x50000) and res_68..71
  (0x1006b0000..0x100770000, program heaps, 4 x 0x30000)**; res_98 is the capture17 uniform ring (in this capture the ring is res_102 at 0x100a60000, which is dumped) and res_103 is now IOSurface 2 (0x100aa0000).
  Resource ids moved between captures (the process creates resources in a different order); skip lists by id must be recomputed per capture, or better by VA: **keep 0x1000e0000 and 0x1006b0000..0x100770000 dumped,
  skip only 0x100a60000-style rings if the budget needs it**.
* The stream decodes as before: 10 live passes (the blocks with a head LINK; the following 14 blocks `776x24` at 0x5f18.. are stale / other-type blocks: they have no LINK, no END or an empty state, count them out),
  per frame 68 draws (session 11) and 103 (session 14), all vertex buffers plausible in session 11 (59/59) and session 12.
* Program bytes per draw (session 11, 68 draws, incl. the stale ones): vertex entries 23 walk to `08 01`, 12 FAIL (7 are the stale passes' empty slots, 5 are `100617b30` / `10061c8f0` (the res_75-block programs) whose walk from `s8v_find_start` fails: partial page or an unknown form, not examined further), **31 no bytes**. Fragment entries: 9 verified by the entry walk, 24 unwalked (unknown forms, or block-less start), **33 no bytes**. Session 14: vertex 32 ok / 14 FAIL / 56 no bytes; fragment 4 / 37 / 61.
  The "no bytes" ones all lie in 0x1006df000-0x1006dffff, 0x10070e000-0x10070ffff, 0x10073e000-0x10073ffff (res_68..71, skipped) and, for 18 of capture17's, res_10: **so the new dump rule made program coverage worse for
  the compositor draws, not better; programs of res_3 / res_59 / res_72..76 are present now** (res_59 programs 0x1006139b0, 0x100619ff0, 0x10061ee70 walk fine).
* Fragment program forms, per-program disassembly and the work list: `analysis/compositor-frag-forms.md`. Sampled textures, their resources and IOSurface ids, and the descriptor words: section 4 of that file.

## 12. Host reproduction of execComposite (tests/comp_replay.c, tests/comp_replay.sh)

`wsl -u root -d Ubuntu -- bash tests/comp_replay.sh` runs the kernel's per-draw logic on capture17 session 8 and capture18 sessions 11, 14 (program heaps merged from capture17 s10); `-v` prints vertex exports,
`-t <pass> <draw>` / `-tf <pass> <draw>` trace the first vertex / the first fragment pixel instruction by instruction (hook `S8F_TRACE` in `s8f_run`, host only). Output: `logs/_comp_*.txt`.

Bugs found and fixed (the kernel glue needs no change for 1 and 2 beyond taking the new headers; 3-5 are glue):
1. **Uniform push source address** (`s8s_push_decode`): the low byte is `(w0 >> 24) & 0xfc`, NOT `& 0xfe`; bit 1 is a flag (`0x22`, `0xb2`, `0x82` ...). With `0xfe` every transform block was read 2 bytes off, so all matrices
   came out as f32 garbage (w = 0, positions -inf/nan, 0 pixels). `s8s_push.flags` keeps the bit. Data after the fix: the transform block is f32 `{1/1440, 0, ..., -1/1480, ..., -1.0, 1.0, 0, 1.0}`.
2. **Vertex stage address/fetch** (`s8f_run`, rd != 0): `8e <D> ca 30 26 80 c0 00` is a 64-bit imadd `D = r5 * 0x30 + r1:r0` (r5 = vertex id, stride = immediate, r0:r1 = base copied by `c2 00 00 00` / `c2 02 02 00`), and
   `05/85 ..` device_load uses the G13 layout: offset `O = b2>>4 | (b4&15)<<4 | b7<<8` shifted by `log2(element size) + b5[3:2]` (second stream: `85 20 48 05 00 c8 f0 00` = 16-bit elements at +0x20), base = the imadd result pair
   (not b5, not `uni[0] + vid*stride + b2`). The vertex id is pre-loaded in r5 (new: `s8f_run` sets `r[5] = vid` when `rd != 0`; kernel glue unchanged since it sets `c.vid`). The old `uni[0]`-based path remains for the
   `0e` programs of the test app (run_all.sh all green).
3. **Fragment image start** (`s8f_start_image`): a program image can hold several shaders (`08 01`, `08 00` padding, next shader); the returned start is now the start of the shader that contains the entry (was block+0x40, i.e.
   the tile-preamble shader, which ended without a store). The unverified fallback (return 3) skips complete shaders before the entry too.
4. Kernel glue: `execComposite` skips `d->count > 4000` etc. fine; do NOT treat `halves` of pushes as bytes; texture descriptors: `s8t_desc_decode` accepts only format low half `0x050a` and `w[2] << 2`: the compositor's records are other
   formats (0x8002, 0x4642, 0x5982, 0x54c2 ...) and attachment form 1 (`VA >> 4`): use `s8s_texrec_va` for the address and `s8t_desc_make` with the pass / surface size until Agent I decodes w1.
5. Programs whose bytes are zero in the capture (res_10 / res_68..71 skipped by the dump rule) cannot run; in the live kernel they exist, so the "vertices failed, op 0x00 at +0" lines of the capture replay are a capture artefact.

Results after the fixes: capture17 s8: 18 of 18 vertex programs with bytes run (all vertices on screen: full-screen quads (0,0)-(1440,2960), icons (429,350)-(1011,737), blur taps), fragment: 16 of 31 draws store pixels (13 have no
program bytes, 2 stop at unknown forms 0x56, 0xc4). capture18 s11: 13 vertex ok, fragment 7 stored, stops at 0x0a (funop) and 0xc0. Remaining interpreter gaps: (a) the 4-vertex strip `6e44 prim 9` rect draws (vs 0x1006144e0): all 4 vertices
come out at (-x0, -y0): the vid-dependent corner selection (integer ops) is not executed; (b) stored colours of the textured shaders are tiny (3e-5, 0.0078, 0.03) because f16 ops (`c6 f6 76`) and the 32-bit texture_sample
results share the register file with different conventions; (c) unknown ops 0x56, 0xc4, 0x0a, 0xc0 (see compositor-frag-forms.md).

## 13. Host frame renderer (tests/comp_render.c, tests/comp_render.sh, tests/ppm2png.py)

Run (WSL): `wsl -u root -d Ubuntu -- bash tests/comp_render.sh` renders capture17 s8, capture18 s11 and s14 (program heaps merged from capture17 s10) to `logs/_frame_<name>.png` (display target), `logs/_frame_<name>_t<VA>.png`
(every pass target; offscreen layers over a grey checker) and `logs/_render_<name>.txt` (per pass / draw: program entries and start kind, texture bindings, quad bbox, vertices ok, pixels covered / stored, first output pixel, unknown ops).
One frame: `bash tests/comp_render.sh <name> <capture dir name> <session> [<merge capture>:<session>] -- [flags]`, e.g. `... bash tests/comp_render.sh f19 capture19 14 capture17:10 -- -all -v`.
Flags: `-all` all targets, `-v` texture records, `-synth` ignore texel memory, `-keepfull` do not skip the full-screen dither pass, `-selfblend` force the hardware blend even for programs that read the destination, `-bg AARRGGBB`.
Logic = execComposite (pre-pass bbox, R8 0x8002 layer textures sized to the quad, constant / destination-only fast paths, premultiplied source-over for blend=1) with these host differences, which the kernel should adopt:
* pass targets are separate linear BGRA8 buffers (pitch = pass width * 4) keyed by VA; a texture whose address is a previous pass's target is bound as LINEAR BGRA8 with THAT target's width/height/pitch
  (the kernel's Morton + pass-size guess is wrong for blur chains);
* texel memory that is zero / missing gets a synthetic checker (visible), real texels are used when the snapshot has them (capture19 type-9 regions);
* programs that read the destination (`ld_tile` 0x79 / 0xf9) blend themselves: no second hardware blend; a full-screen blend pass whose program reads the destination (the last draw of every display pass, fs 0x1000e4220: f16 dither / resolve) is skipped
  by default because the f16 forms are not decoded and it writes opaque black over the whole frame.
Findings on the capture17 session-8 frame: geometry plausible (a 694x79 header bar over a 582x387 body at (373,303)-(1067,738), both white because the R8 mask texels are synthetic, a 103x63 widget at (1071,55), a 90x90 coloured icon
at (1158,2666) from IOSurface 5, a 516x15 home-indicator bar at (462,2921), the full-screen backdrop = vertex colour (0,0,0,1) i.e. black). Missing: every draw whose vertex/fragment bytes are zero in the capture (blur chain passes 2-5, 7, 8: 13 of 31 draws).
