# A11 (G10P) instruction forms decoded for the interpreter (2026-11-19, Agent H)

Implementation: `core/s8gfx_frag.h` (`s8f_len`, `s8f_step_frag`, `s8f_devldst`), `core/s8gfx_sf.h` (`sf_h2f`, `sf_f2h`). Tests: `tests/isa_test.c` (expected values from host C
from the shader source), `tests/isa_walk.c` (structural: every captured program walks to its `08 01` end with `s8f_len`), `tests/corpus_frag.c`, `tests/corpus_run.c`.
Run all: `wsl -u root -d Ubuntu -- bash tests/run_all.sh` (needs `data/scenes.bin`, `data/corpus4.bin`, `data/scenes_raw.bin` made by `tests/isa_export.py`).

Confidence: **P** = proven by a test whose expected values come from the known shader source; **S** = structural (lengths proven by the corpus walk, semantics inferred);
**A** = ASSUMED (best reading; marked `s8f_assume()` in the code, bit set in `ctx->assumed[op >> 5]`).

## 0. Findings that changed the picture
1. **Register/operand layout of the compact forms.** Sources are byte-aligned: A = b2, B = b3, (C = b5 for ffma, b6 for the 8-byte integer forms). Source byte: bits 7:6 =
   `11`/`10` -> register (low 6 bits = half-register index; 32-bit operations use index / 2), `00`/`01` -> immediate (8-bit immediates carry bits 7:6 in b5 bits 5:4).
   Destination in b1: 32-bit ALU op: register `b1 >> 3`; 16-bit op: half index `b1 >> 2`. Operand size: `b4 & 15 == 0` -> 16-bit lanes, else 32-bit
   (g3 `6e .. 05 80`: 32-bit; frag 6 `6e .. 00 80`: 16-bit). [S; the 32-bit side P]
2. **device_load / device_store use the G13 field layout** (applegpu `DeviceLoadStoreInstructionDesc`): F = bit7 | b1[1:0]<<1 | b6[0]<<3 (2 = i32, 4 = u8norm), mask = b6>>4,
   R = b1>>2 (b6 bit 1 = 32-bit regs), offset O = b2>>4 | (b4&15)<<4 (| b7<<8), immediate when b3 bit 0; byte address = O << (log2 elem size + b5[3:2]). The "pack/unpack" of
   g2/g3/g5/g7 is a **store as u8norm x4 at byte 16, load as i32 from the same bytes** (store O=16, load O=4<<2). The old heuristic (0x45 = pack to r[b1/2], 0x05 = unpack) is
   replaced in fragment mode by this generic scratch-memory model (g2 pack_test still passes). [P]
3. **The compositor's fragment programs are half-precision MSAA-resolve-like kernels with no st_tile**: 4 (or 16) raw ld_tile, a tree of f16 ffma (`c6`), f16 fadd (`86`),
   16-bit index math (get_sr `a2/82`, `0e`, `8e`) and the pixel write `31 ...` at the end. See sections 3 and 5.
4. `20 c0 <imm32>` is a 6-byte **literal word** that an imadd with `b4 & 0x60 == 0x60` (6-byte form) takes as its operand (not a 12-byte imad).
5. Apple's compiler turns `x / 255.0` into a multiply by the float reciprocal (programs load 0x3b808081 and 0x3d888889 with mov_imm). Reference code must multiply by `1.0f/255.0f`.

## 1. Lengths (proven structurally, `s8f_len`)
All 47 vertex and 6 fragment programs of corpus4.bin that are complete in their 768-byte window walk exactly to `08 01`: **44/47 vertex, 4/6 fragment** (frag 7 is cut by the
window, frag 4 is not a fragment program: the entry points into another shader's data). Rules: 2 B: `38`, `75 xx`. 4 B: `08 01`, `48`/`58`, export `01`/`81`, the mov family
(low 6 bits `02` or `22`: `02 42 82 a2 c2 e2`), `12 b1 imm16`, `92 b1 imm16`, `23`, `1e` when b3 = 0, b1 odd, b2 >= 0xc0 (`1e 13 cc 00`). 6 B: `52`/`d2` mov_imm, `4a 8a 0a 86 46 7e fe 3e be de 1a 5a 26 20`,
and the 6/8 families `0e 4e 8e ce 6e 2e ae ee 05 45 85 c5` (8 B when b5 bit 7 is set), `1e 5e 9e` (6 B when `b4 & 0x60 == 0x60`, else 8 B). 8 B: `c6 ca 79 09 89 31 16`, iter `51` (4 B when b1 bit 0). 10 B: `21`.

## 2. Integer / bit forms (all P unless noted; evidence: g3 g5 g7 round 8, scenes 31 and 35, bit-exact over 2000-3000 random inputs)
| form | bytes | semantics | evidence |
|---|---|---|---|
| bitop `7e` | `7e D A B tt|7 00` | r[D>>3] = tt(A, B); tt = b4 >> 4 in G13 order (bit0 ~a~b, bit1 a~b, bit2 ~ab, bit3 ab): and = 8, xor = 6, or = e; 8-bit imm B = b3 \| (b5>>4 & 3)<<6 | g3 (and 0x7f7f7f7f), g5 (xor), g7 (and), scene 31 (and 0xf0); all 16 tables checked against the formula (only 8, 6 and the immediate form seen in programs) |
| iadd `0e/4e` 6 B | `4e D A B 15 00` | 32-bit: r[D>>3] = A + B; (b4 low nibble 0: 16-bit h[D>>2] = A + B, A) | g7 (`4e 00 c0 c2 15 00`) P for 32-bit |
| bfeil `6e` 8 B | `6e D A B 05 m 80 C` -> D = (a & ~mask) \| ((b >> c) & mask), mask = (1<<m)-1, m = b5 & 0x3f (0 = all) | g3 (shift by 1), scene 31 (>> 4): P; 16-bit form (frag 6): A |
| bfi `2e` 8 B | D = (a & ~(mask<<c)) \| ((b & mask) << c) | scene 31 `(u<<1)&0xFF` = `2e .. 87 01` (m = 7: (u&0x7f)<<1): P |
| convert `be` 6 B | `be D mode src b4 00`: mode (b2) 8 = f_to_u32 (trunc, sat), 9 = f_to_s32, 10 = u32_to_f, 11 = s32_to_f; modes 0/6 (u8/u16 -> f) A | scene 31, 35 (uint(x*255) truncates), compute kernels (`be 12 0a 86 61 00`): P for 8, 10 |
| mov_imm f16 `92 b1 lo hi` 4 B | r[b1>>1] = float(f16 imm) (255.0 = 0x5bf8) | scene 31/35 run correctly: P as used; field split A |
| mov_imm16 `12 b1 lo hi` | h[b1] = imm16 | length only (vertex prog 0) A |
| device_load/store | section 0.2 | P for u8norm x4 and i32 x1 |
| fmul register source | `4a D A B 04 00` with b3 >= 0xc0 = register (scene 35 `4a 03 c0 c2 04 00` = r0*r1) | scene 35 P |
| imadd `1e` 6 B + `20 c0 imm` | vertex address multiply; vertex stage sets stride = literal | A (vertex prog 0 now continues; the program has a loop `9e .. be ff ff ff` that stops it) |
| imadd `8e` 8 B, 16-bit | h[D>>2] = A*B + C (C = b6) | frag 5/6; A (arithmetic ok, role = pixel address) |
| `1e 13 cc 00`/`23 c4 c0 00`, `26`, `12`, `16`, `de`, `fe`, `75` | length decoded, executed as no-ops | A, semantics unknown |

## 3. Fragment f16 / tile forms (A: consistent but not proven by a known shader)
* ld_tile four-way `79 R 02 b3 00 b5 fc 01`: dst = half index b1>>2, loads 4 f16 unorm channels (R G B A) of the pixel word of sample j = b3[3:2] | b5[3:2]<<2. The four loads of
  frag 5 write h12.., h8.., h4.., h0.. (b3,b5 = (0,0),(4,0),(0,4),(4,4)); frag 7 has 16. The split of b3/b5 into format and sample fields (G13: F at bits 24-27, sample mask at 42) and the
  channel order are ASSUMED. The float form `79 02 .. fa ff` is unchanged (scene 9, P).
* `c6` f16 ffma: h[b1>>2] = h[b2&63] * minifloat(b3) + c, c = h[b5&63] when b5 bits 7:6 = 11 else minifloat(b5) (0 for b5 = 0). `86` f16 fadd: h[b1>>2] = h[b2&63] + (b3 reg or minifloat).
  **Consistency test**: running frag 5 on 16 random per-sample tile words yields exactly the mean of the four addressed samples (+-2e-3) for R G B A over 500 trials, which is what the c6
  tree (16 ops, weights 0x10 = 0.25) computes if these readings are right. frag 6 uses weight 0x04 (1/16), frag 7 weight 0x01 (1/64) with b7 = 04 on some ops: unexplained (a resolve with different
  weighting or a further blur step); the interpreter just uses minifloat(b3).
* `a2/82 b1 sr 00`: h[b1] = sr[b2] (get_sr). sr 0x30/0x31 = tile pixel x/y (G13 numbering), sr 0/1/6/8 unknown; the executor must fill `ctx->sr[]`. `02 04 04 00` = copy h4 to h4 (no-op).
* `31 ...` (8 B) at the end of every compositor program, preceded by `0e`/`8e` index math (x + y*stride) in h16..h21: interpreted as the pixel write of h0..h3 (f16 RGBA) to the render
  target; the interpreter sets `stored` and `out[] = float(h0..h3)`. This is why `corpus_frag` counts frag 5 and 6 as running to a pixel store. The coordinate and format operands are ignored (A).

## 4. Coverage (tests/corpus_*.c on data/corpus4.bin)
| metric | before | after |
|---|---|---|
| `corpus_run` vertex programs run to completion | 43 / 47 | **44 / 47** (prog 12: `1e 13 cc 00`/`23` pair; prog 0 now passes the imad and stops at a loop jump `9e`) |
| `corpus_frag` programs run to a pixel store | 0 / 6 | **2 / 6** (frag 5, frag 6) |
| `corpus_frag` programs reaching the end instruction | 0 / 6 | **4 / 6** (+ the two empty programs) |
| structural walk (`isa_walk`) fragment / vertex | n/a | 4 / 6, 44 / 47 |
Remaining: frag 7 (window ends before its end: 768-byte capture too short), frag 4 (the record's entry lands in another shader; not a fragment program), vertex prog 0 (loop/jump `9e`), vertex 33 and 35 (entry inside a device_load; start finder).

## 5. What is still ASSUMED and the phone/emulator shaders that settle it
All shaders are fragment functions on a 4x MSAA BGRA8 target or a normal target as stated; capture the program (s8agxprogs) and compare the output pixels with the expected values.
1. **ld_tile four-way sample/format split and channel order; c6/86 semantics; the 0x31 write.** Render a full-screen quad into a 4x MSAA target with a fragment shader that outputs
   different constants per sample using `[[sample_id]]`-based colour (`float4 f(V v [[stage_in]], uint s [[sample_id]]) { return float4(s*0.25h, 1.0 - s*0.25, 0.5, 1.0); }`) and `MTLStoreActionMultisampleResolve`; the compositor-style resolve program
   is generated by the driver for the resolve. Expected resolve pixel (0.375, 0.625, 0.5, 1.0). Also vary one channel only to confirm R G B A order.
2. **f16 ALU forms (c6 86 and the weights 0x04/0x01).** `fragment half4 h1(V v [[stage_in]]) { half4 a = half4(v.col); half4 b = a * 0.0625h + half4(0.25h); half4 c = b + a; return float4(c); }`
   with col = (0.5, 0.25, 0.75, 1): expected (0.78125, 0.390625, 1.0625 -> clamped 1.0, 1.3125 -> 1.0); then the same with 0.015625h and a register addend to see which b3/b7 values appear.
3. **16-bit index math (0e 8e a2 82 31 operands).** A fragment shader that writes the pixel position `float4(float(pos.x)/64.0, float(pos.y)/64.0, 0, 1)` using `[[position]]`, and
   `uint2 p = uint2(pos.xy); return float4(float(p.y * 64 + p.x) / 4096.0, 0, 0, 1)`: confirms sr 0x30/0x31 = tile-local x/y and the imadd role.
4. **bfeil 16-bit form, imad 16-bit, iadd 16-bit, icmpsel `16 ..`, `1e`/`23` 4-byte pair (64-bit multiply?).** `kernel void k(device ushort *a, uint i [[thread_position_in_grid]]) { a[i] = (ushort)((i >> 2) * 3 + 5); }`
   and `uint64_t`/`mulhi` variants: `a[i] = (uint)mulhi(i, 0x9e3779b9u)`; plus `int i = ...; return i > 3 ? ... : ...` (scene 32) for icmpsel.
5. **The vertex imad literal as stride, `fe`, `26`, `12`, `16`, `de`, `75`.** A vertex shader with two interleaved streams of strides 28 and 44 bytes (buffer(0) with `stride 44`), positions fetched with `vid * stride`; compare the literal in the `20 c0` word with the stride.

## 6. Second pass: the compositor corpus (2026-11-19, tests/comp_run.c over data/corpus_comp.bin = tests/frag_forms.c -o)
Corpus: 46 distinct compositor fragment programs with a determined start (11 block images, the rest by walk), exported by `bash tests/r9.sh`. Runner: `tests/comp_run.c` (`bash tests/r12.sh [-v]`, built with
-fsanitize=address,undefined) executes each program with synthetic inputs (varyings, tile word, uniforms 0.5f, 16x16 texture, sr[48..49]) and counts executed opcodes. **Result: 38/46 reach the end
instruction, 35 execute a pixel store, 17 sample the texture unit; 5 run with proven forms only; 8 stop** (3 records that start in data (`80 00 ..`), 4 programs at the unary function op `0a`, 1 at `c4`).
Walk lengths: `s8f_len` now knows `f9 f6 76 fa 7a` (8 B, lands on every block entry) and `1a/5a` (6 B).

New proven fact (P, scene 35 / 31 still bit-exact): float ALU sources have a size bit. For `4a/8a`: **b4 bit 5 = B is a 32-bit register; b3 >= 0x80 without it = a 16-bit register (half index b3 & 63) converted to f32**; `92 b1 lo hi` therefore
writes the raw half h[b1] (not a converted f32 as in section 2): `92 02 f8 5b` = h2 = 255.0 read as a 16-bit source by `4a 03 c0 c2 04 00`. The earlier "r1 = float" reading passed the same tests by accident.

| form | status | semantics implemented | evidence / why ASSUMED |
|---|---|---|---|
| `c6` (212x) | A | f16 ffma D = a*b + c, a = h[b2], b = h[b3] if b3 >= 0x80 else minifloat(b3), c likewise from b5, negate-a = b4 bit 1 | MSAA-resolve tree consistency (section 3); `c6 .. 93 00 c1` (b3 a register) in the blend programs; the 0x04/0x01 weights and b4/b6 bits (`20`, `04`) unexplained |
| `46`/`86` | A | f16 fmul / fadd, b4 bit 1 negates a: `86 1a c6 30 02 00` = 1.0 - h6 (minifloat 0x30 = 1.0) | the 1 - alpha idiom of the blend programs; 46 = fmul only by the 0x06 + bits 7:6 pattern of 4a/8a/ca |
| `f6`/`76` (56x each) | A | f16 multiply D = h[b2] * h[b3]; b5/b6 repeat b2/b3 in every one of the 120 instances (ignored) | used with the f16 pair constant loaded by `d2 .. 06 ba 01 3d` (h0 = 0xba06 = -0.753, h1 = 0x3d01 = 1.251) as f6 = low half, 76 = high half; true operation (fma(a,b,a)?) not settled |
| `fa`/`7a`/`1a`/`5a` | A | f32 multiply of r[b2/2] * r[b3/2] (duplicated operands) | texture-coordinate transform ops before texture_sample |
| `f9` (38x) | A | four f16 unorm channels of the pixel word (same as the four-way `79`); `f8 ff`/`78 ff` = one sample, `fc 01`/`7c 01` = samples via b3/b5 | length proven by the walk, semantics as for 79 |
| `75` (18x) | A | no-op (`75 2a`, `75 29` pairs before the end) | nothing observable |
| `31` | A | pixel write of h0..h3 (f16) -> out[] | unchanged; coordinates `d0` operand and `0a`/`0e` byte (format?) ignored |
| `21` texture_sample | A | dst r[b1 >> 2] (32-bit), channel mask b4 & 15 into contiguous registers, unit (b5 & 63) % ntex, coordinates = registers of the last iter, via `s8t_sample`; short iter `51 b1 b2 00` writes the varyings like the long form | scenes 26/28/29 (tex nearest/tint/premult) reproduce the host reference bit-exact on a 4x4 Morton texture (P for that mapping on those scenes; the compositor's unit/coordinate fields b5/b6 are not mapped: in them b6 = iter destination half index + 4) |
| `58` signal_pix, `e2`/`22`, 16-bit bitop `7e` | A | no-op, get_sr -> r[b1 >> 1], 16-bit truth table | length/structure |
| `71`, `2a`, `56`, `80`, `c4`, `0a` unary | open | `71` appears only in data-like records (`71 01 00 11 00 74` tables) at unknown starts: probably not an instruction; `0a` unary is 4 B in scene 19 (`0a 01 c0 44` = rcp?) but 6 B elsewhere in the compositor; `80`/`2a`/`56`/`c4` are mid-instruction stops of mis-started records | |

API additions (all optional; old callers unaffected): `ctx->texd[8]/texs[8]/ntex/trd/trdctx` (texture units, descriptors from `s8t_desc_decode`/`s8t_desc_make`, texel reader), `ctx->hit[256]` (executed instruction counts), `ctx->last_iter`,
`s8f_walk_lands()` and **`s8f_start_image(m, len, block, entry, &start)`** (1 = block + 0x40 verified by the length walk reaching the entry or entry + 2 and the end, 2 = earliest start in [entry - 0x100, entry], 3 = block + 0x40 unverified);
tested on the corpus (11/11 block images, 21/21 headerless). `s8gfx_frag.h` now includes `s8gfx_tex.h`; all three kernel headers compile as C++ (`g++ -fsyntax-only -x c++`).

Experiments that would settle the ASSUMED compositor forms (fragment shaders captured with s8agxprogs, outputs read back; tex = 2x2 RGBA8 texture, constants chosen so every candidate formula gives a different pixel):
1. f6/76/46/86/c6: `half4 d = half4(dst [[color(0)]]); half a = half(in.col.a); return float4(d * (half(1) - a) + half4(in.col) * a);` and `return float4(d * half4(0.75h) * half4(1.25h))`: dump the program, vary one constant at a time; expected for d = 0.5, col = (0.25, 0.5, 0.75, 0.5): (0.375, 0.5, 0.625, 0.5) / 0.46875.
2. 0x21 unit/coordinate fields: two textures bound at slots 0 and 3 with different colours, `t3.sample(s3, uv)`, `t0.sample(s0, uv * 0.5)`; the b5/b6 bytes of the two sample instructions identify slot and coordinate register.
3. unary `0a`: `float4(sqrt(x), 1.0/x, exp2(x), log2(x))` with x = 0.25 on separate shaders (one function each), read the function-code byte of each.
4. `31` write: a fragment shader on a non-MSAA target outputting a constant, plus a variant with `[[color(1)]]` second attachment: the extra bytes in `31 01 .. 0a 10 d0 00 04` vs `31 01 0e 0a 00 ..` select the target/format.
5. f9/79 sample/format split: MSAA 4x resolve of a target with distinct per-sample values (`[[sample_id]]`), expected resolved mean.
