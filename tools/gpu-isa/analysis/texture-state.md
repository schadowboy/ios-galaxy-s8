# A11 (G10P) texture state, sampler state and texture_sample (Agent I, 2026-11-19)

Evidence: capture9 (scenes 26-30 = sessions 26-30), capture16 (compositor, emulator), G13 reference read as documentation only. IMPORTANT: no
hardware oracle exists for sampled values (the stand-in returns black for TEX scenes, logs/s8mtl_test.log renders 25-29), so "reproducing the hardware"
means: descriptor fields decoded from memory + Metal semantics for filtering. Confidence: V = verified on data, H = hypothesis (consistent with
the data, not separable), U = unknown.

## 1. How the state reaches the shader (V)
- Two push records precede the fragment state record in res_20 (type in the low byte of word 0; address = (w1 << 8) | (w0 >> 24 & 0xfe)):
  - `00100017 01001c00` -> 0x1001c0000 = res_23, the 24-byte TEXTURE STATE record (type 0x17). 0x10 in bits 16-23 = size in 16-bit units?? (0x10 halves = 32 B)
  - `a0100013 01000300` / `60100013 01000301` -> res_4+0xa0 / res_4+0x160, the 8-byte SAMPLER STATE (type 0x13), 0x10 again = 32 B copied (8 words, rest zero).
- res_4 is a ring of 0x40..0xc0-byte slots; two sampler slots (+0xa0, +0x160) alternate between scenes: session 26 uses +0xa0, 27 +0x160, 28 +0xa0, 29 +0x160,
  30 +0xa0. The unused slot keeps the default `000c0000 00000004`. The pushed words land in "sampler state registers" / "texture state registers"
  exactly like the G13's tex_state_store (24 B) / sampler_state_store (8 B) targets; texture_sample then names a state register index.
- res_14 carries a second pair `00000301 <lo>` with the OTHER slot (session 26: 0x...160, session 27: 0x...0a0): the same pointer table seen from the
  command stream; not needed for sampling.

## 2. Texture state record, 6 words (res_23, session 26; same record in all TEX scenes)
`1b44050a 00000600 40001440 00000000 00000000 00000000`
- word2 = VA >> 2 (V): 0x40001440 << 2 = 0x100005100 = res_1+0x5100, the texel bytes (bit 30 is VA bit 32). Render target records use VA >> 4 (`1fe3050a 000007e0 10006000 00000200`).
- word0 low 16 bits 0x050a (record marker, also in render target and pass records) (H); word0 >> 24 = 0x1b = 00 01 10 11: probably the identity channel
  swizzle (2 bits per channel) (H); word0 bits 16-23 = 0x44 (4x4 RGBA8 2D?) U. The only format seen is 4x4 RGBA8 = 1b44.
- (SUPERSEDED by sec. 9: word1 holds height-1 at bit 9 and the high width bits at 0-8) word1 = 0x600 for 4x4 and 0x7e0 for the 64x64 BGRA8 render target (V on the two points): equals 0x800 - (0x800 >> k) for a 2^k square size, i.e.
  (2^k - 1) << (11 - k). Either "size field left-aligned to bit 10" or a log2 + mask encoding (H). Width and height cannot be separated (both square), levels not seen.
- Format: RGBA8 byte order is certain from the memory (R,G,B,A bytes, V); which bits say RGBA8 vs BGRA8 vs R8 is U (render target record has 1fe3 in the top half, BGRA8).
- Layout (V for 4x4): Morton / twiddled, y in the LOW bit: byte offset = 4 * interleave(y0,x0,y1,x1) (pixel (x,y) index: (0,0)=0,(0,1)=1,(1,0)=2,(1,1)=3,(0,2)=4,(2,0)=8). For
  larger textures (tile size, order of tiles, non-square) U; the header implements the G13 rule (interleave the low min(lw,lh) bits, remaining bits of the longer
  side linear above) as a placeholder. Texture pixel contents match the formula of s8mtl_test.m for all 16 texels (checked in tex_test.c).
- The compositor capture (capture16) shows only `0305050a 00000000 40000c80` (38x) and `fb05050a 00003e00 40000880` (6x) records (attachments / pass targets), no
  second texture record, so no second data point for the texture state exists there. Its word1 `00003e00` = 31 << 9 hints that the same size encoding applies with other bit positions: not resolved.

## 3. Sampler state, 2 words (res_4)
nearest (sessions 26, 28, 29, 30): `000e0000 00c00780`; linear (session 27): `028e0000 00c00780`; unused/default slot: `000c0000 00000004`.
- Filter (V as a pair): linear sets bits 23 AND 25 of word 0. The test sampler sets magnify and minify together, so which bit is mag / min is H (bit 23 = mag, bit 25 = min,
  bit 24 = mip filter?). The compositor uses the same two words (sec. 30 of agx-kernel-interface.md), so the compositor's samplers are clamp_to_edge nearest/linear.
- Address mode: all captured samplers are clamp_to_edge (V: constexpr samplers in s8mtl_test.m); repeat/mirror/border encodings U (probably in word0 bits 17-19 / word1 bits 7-10, 22-23: these differ between the
  default sampler (`000c0000 00000004`) and the test sampler, but the test changes several things at once). LOD min/max, anisotropy, compare: U.

## 4. texture_sample instruction (0x21, 10 bytes `21 d 00 41 0f c8 00 80 00 20`)
- byte 1 = destination (reg*8+1, like the fmul dest: (b1-1)/8 = first of the result registers), byte 4 low nibble = component mask (0f RGBA, 07 RGB), byte 5 low 6 bits = coordinate register in
  half units (c8 -> half 8 = r4,r5 filled by `51 03 81 00`; compositor variants c0 / c5 / c6 with byte 6 = 10: 16-bit coordinates / other start) (V for mask and coordinates in my scenes), bytes 7-9 `80 00 20` constant in
  all ~100 compositor instances matched (capture16 res_68..76), and in every scene (no lod / offset / compare forms).
- Texture index and sampler index: byte 2 (0x00 mine; compositor 0x00, 0x11, 0x02, 0x13, 0x26) and byte 3 (0x41 mine; compositor 0x6c, 0x74, 0xec, 0x70, 0x5e, 0x24, ...) vary and are the only candidates.
  All my scenes use texture 0 / sampler 0 so the split is U. The state register file is filled by the 0x17 / 0x13 pushes, so the index selects among the pushed records in push order.
  Stand-in rule that works for every captured scene: index 0 = the (single) texture and the (single) sampler pushed before the fragment record; to be confirmed with experiment E4.
- Reference: G13 texture_sample is 0x31 (the compositor's `31 01 00 0a 10 d0 00 04 75 2a 75 29` is NOT this form on the A11, it appears next to ld_tile in the blend shaders: still unknown).

## 5. API (tools/gpu-isa/core/s8gfx_tex.h)
```
struct s8t_desc {uint64_t va; uint32_t width,height,levels,fmt,layout,pitch; int known;};   fmt S8T_FMT_RGBA8/BGRA8/R8, layout MORTON/LINEAR
struct s8t_sampler {int mag_linear, min_linear; uint32_t wrap_s, wrap_t;};                   wrap CLAMP_EDGE/REPEAT/MIRROR/ZERO
int  s8t_desc_decode(struct s8t_desc*, const uint32_t w[6]);     // 6-word record (0x17 push target); 0 = recognised
void s8t_desc_make(struct s8t_desc*, va, w, h, fmt, layout, pitch);  // caller supplies the facts when the decode is uncertain
void s8t_sampler_decode(struct s8t_sampler*, const uint32_t w[2]);   // 2-word record (0x13 push target)
void s8t_sample(const struct s8t_desc*, const struct s8t_sampler*, f32b u, f32b v, uint32_t (*rd)(void*, uint64_t va), void *ctx, f32b out[4]);
```
`rd` returns the aligned 32-bit word at a GPU VA (same contract as s8f_ctx.rd). Level 0 only; nearest when both filter bits are clear, bilinear otherwise
(without LOD computation min and mag cannot be told apart). Model: 16.16 fixed texel position, 8-bit truncated weights, result = sum(w*texel8)/(65536*255) correctly rounded; nearest = floor(u*w).
Integration in the interpreter (not done, Agent H owns s8gfx_frag.h): on `21` read dest = (b1-1)/8, mask = b4 & 15, coords = r[(b5 & 0x3f)/2] and +1; call s8t_sample with the pushed texture / sampler; write the masked channels to r[dest+k] (k counts only the set mask bits, as in G13: dense packing, to be checked by E5);
the `38 00` wait that follows is a no-op.

## 6. Tests (tools/gpu-isa/tests/tex_test.c, run tests/tex_test.sh in WSL): 8310 checks, 0 failures
Descriptor decode from capture9 res_23 (VA, 4x4, 64x64 record), sampler words from res_4, Morton fetch of all 16 texels vs the formula of the test app, nearest and linear at texel centres = the texel
bytes/255 bit-exact, bilinear at all 9 inner corner midpoints = sum/1020 bit-exact, clamp outside [0,1], repeat periodicity, 2000 random coordinates vs a double reference (nearest exact, bilinear within 2/255), layout helpers.
These are semantic checks; there is no hardware pixel for textured scenes yet (scenes 26-30 read back black on the stand-in).

## 7. Remaining uncertainty
Width/height separately; levels/mips; format bits (BGRA8, R8, RG8, half/float, compressed); tiling for sizes > 4x4 (compositor layers are screen sized: this is the biggest risk);
mag/min split; wrap modes; texture/sampler index bytes; bilinear rounding/weight precision of the hardware (8 vs more bits); LOD/mip selection (needed for downscaled layers).

## 8. Extra experiments (add to s8mtl_test.m; each is a TEX-style scene: fragment samples at in.col.xy, `t` bound at index 0 unless noted)
Common helper: texture `mk(w,h,fmt)` filled with a position-coded pattern (R = x*(255/(w-1)), G = y*..., B = (x^y)*16+1, A = 255-(x+y)*8 clipped), then compare descriptor words and texel memory between scenes (use the capture diff
tooling: only res_23 / res_4 / texel region change).
```objc
// E1 size field: same sampler (nearest), textures 2x2, 8x8, 16x16, 32x16, 16x32 (RGBA8). Settles: word1 size encoding, width vs height split, non-square Morton layout, tile size growth (memory order of 16x16, 32x16).
int dims[][2] = {{2,2},{8,8},{16,16},{32,16},{16,32},{64,64},{128,64}};
// E2 format: 4x4 textures of MTLPixelFormatBGRA8Unorm, R8Unorm, RG8Unorm, RGBA16Float, RGBA8Unorm_sRGB, A8Unorm, RGB10A2Unorm. Settles: which word0 bits are format/swizzle, texel size, sRGB flag
// E3 sampler fields: one 4x4 texture, one shader per sampler (constexpr in source or MTLSamplerDescriptor, bound with setFragmentSamplerState:atIndex:0):
//   filters: {min nearest/mag linear}, {min linear/mag nearest}, mip_filter nearest/linear (needs mipmapped textures, see E6)
//   address: clamp_to_edge, repeat, mirrored_repeat, clamp_to_zero, clamp_to_border(opaque black), separate s/t:  r=repeat t=clamp
//   "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.col.xy * 2.0 - 0.5); }"   // coordinates -0.5..1.5 exercise the wrap
//   plus max_anisotropy(2,4,16), lod_clamp(0,2) and compare function for depth: one scene per change so exactly one field moves in the sampler words.
// E4 indices: two textures + two samplers bound at (0,1) / (0,1), shader samples texture 1 with sampler 0 and texture 0 with sampler 1, summing results with different weights:
//   "return t1.sample(s0, uv) * 0.75 + t0.sample(s1, uv) * 0.25;"  Settles: which of byte 2 / byte 3 of 0x21 is the texture index and which the sampler index (swap indices between two scenes and diff the instruction); also three textures at slots 0, 2, 5 (gaps).
// E5 mask: t.sample(...).g only (mask 02), .ba (0c), .rgb (07), float2 from an RG texture; settles the dense register packing of masked results.
// E6 mips: mipmapped 16x16 texture (levels 5), each level a flat colour (level k = colour k), shader samples with level(0), level(2), bias(1), and plain sample with a minifying quad (scale the quad so one texel < pixel);
//   settles levels field, LOD instruction operand (lod bytes of 0x21 beyond '80 00 20'), mip base address layout, auto-LOD gradient inputs.
// E7 big texture (compositor shapes): a 256x256 and 1024x512 RGBA8 texture with t.sample(sn, in.col.xy) read through the tile at 4 chosen pixels; settles tile size and tile order. Pair with an experiment that
//   uses texture_load (t.read(uint2(x,y))) with integer coordinates to confirm addressing independent of filtering (opcode 0x71 on G13).
// E8 rounding: 2x2 texture {0,255}: sample linear at u = 0.5 + k/256 for k = 0..255 via a gradient quad; the bilinear weight quantisation (8 vs 6 vs 16 bit) shows as step widths in the result (needs the real GPU or the executor's pixel read-back).
```
E1+E2+E3 are the cheapest and settle the descriptor; E7 settles the tiling that the compositor depends on.

## 9. Real texture records of the compositor (capture21 session 9, comp_render.c TEXDUMP=1; 2026-11-19) SUPERSEDES the size guesses of sec. 2
Fields now decoded from 12 distinct real records (V = checked on texel memory / geometry of the passes):
```
w0 bits 0-15   type code. bit 3 = layout (1 Morton, 0 linear). (code & ~8): 0x0502 RGBA8, 0x5982 BGRA8, 0x0002 R8, 0x8002 A8, 0x4642 RGBA16F, 0x0142 LA8 (L,A 2 B), 0x54c2 BGRA8 (display surface)
w0 bits 16-26  constant per format family (0x305 for the 0x5982 / 0x0502 / 0x54c2 records, 0x344 for 0x4642/0x464a and the capture9 test texture): channel swizzle / sRGB flags, undecoded
(w0 >> 27) | (w1 & 0x1ff) << 5      = width - 1   (14 bits)   V: 4x4, 153x64, 128x74, 132x78, 696x437, 694x435, 694x79, 1440x2960, 256x1, 128x1, 64x64, 1x1
(w1 >> 9) & 0x3fff                  = height - 1  (14 bits)   V (same list)
w2 = VA >> 2, bit 30 = VA bit 32     V (every record)
w3 bit 31 set, bits 12+ ~ (align4(width in 4-byte words) - 4): 124, 128, 692, 152 ... for the BGRA8 records; NOT the pitch (the 153x64 widget has tight 612-byte rows); 0x3fffc000 for the display surface. Undecoded.
w4 = (VA + size_of_pixels, 64-byte aligned) >> 4 and w5 = a size (0x70, 0x80, 0x2b0 ...): present for samples of earlier passes' targets (lossless-compression metadata pointer?); ignored: the stand-in renders plain linear targets.
```
The old "word1 = 0x800 - (0x800 >> k)" rule was a coincidence of the two square points: 0x600 is height-1 = 3 at bit 9, and the 64x64 target record `1fe3050a 000007e0` is a render-target/PBE record of another layout (its w1 0x7e0 does not decode this way).

Evidence per record (all linear unless stated):
| w0 / w1 | decoded | texel memory check |
|---|---|---|
| c3055982 00007e04 (VA 0x100a60000) | BGRA8 153x64 | `59 c7 34 ff` = B,G,R,A = iOS green #34C759; first non-zero rows 612 bytes apart = tight 153-px stride; rendered as a clean green bar (before: 103x63 with pitch 412 gave stripes) |
| fb055982 00009203 / 1b055982 00009a04 / bb055982 00036815 / ab055982 00036415 | BGRA8 128x74 / 132x78 / 696x437 / 694x435 | = the target sizes of passes 2, 3, 5, 0; w4 - VA = pixel bytes rounded up to 64 |
| a8b68002 00009c15 / f8b68002 00030415 | A8 694x79 / 704x387 (descriptor width = row stride; the header at -0x40 gives the real size) | agrees with Agent J's A8 findings (height 79 and 387) |
| fb444642 00000007 (0x100c90000) | RGBA16F 256x1, 2048-byte row | texels `fc3b 0000 0000 fc3b | f43b 0000 0000 f43b ...` = (0.9990, 0, 0, 0.9990), (0.9966, ..): a 256-entry half table (dither / ramp) |
| fcb4000a 00000003 (0x100002100) | R8 128x1 (Morton = linear for one row) | 00 1d 37 50 66 7b 8d 9e ... ff ff ff fe ... 00: a bell-shaped (Gaussian-like) weight curve, 1 byte per entry |
| fc0554c2 00171e2c (0x100ed0000) | BGRA8 1440x2960 | exactly the display surface (pass 9 draw 6 reads it back: the dither / resolve pass) |
| 03050502 00000000 (0x100eb0000) | RGBA8 1x1 | texel `ff ff ff ff` = white: the home-indicator bar (draws 3, 4: 516x15 at (462,2921)) samples a 1x1 white texture; with the correct 1x1 the bar renders white (before: black) |
| fb44464a 00007e01 (0x100e60000) | code 0x464a = RGBA16F, MORTON, 64x64 | = pass 8's 55x55 target viewed as 64x64 (memory not in the capture, pass 8's vertex programs fail anyway) |
| 2ca40142 00017207 (0x100ea0000) | LA8 230x186?? | IOSurface of the LA08 icon: texels not in the capture. The decoded 230x186 and the 90x90 quad do not agree (w3 hints a 120-word = 240-px stride): UNRESOLVED, keep the synthetic gradient until texels are captured |

Not texture state: `0305050a 00000000 40000c80` and `fb05050a 00003e00 40000880` (pass attachments, decode as 1x1 / 32x1 nonsense).

Sampler words: only 000e0000/00c00780 (nearest) and 028e0000/00c00780 (linear) occur in this capture too (draw 5 of pass 9 = 028e0000, linear).

### Glue for the kernel (S8Agx.cpp execComposite, around line 1182)
Replace the "size/format words are not decoded yet" branch by the descriptor. `s8t_desc_decode()` now returns 0 and fills va, width, height, fmt, layout and a tight pitch for every real record, so:
```
if (r8tva == 0 && ok == 0 && s8s_texrec_va(w6, &tva) == 2) {          // form 2 = w2 bit 30: a real texture record
    struct s8t_desc *td = &dc->tmpl.texd[dc->tmpl.ntex];                // already decoded: va, W x H, fmt, layout, pitch = W * bpp
    // a texture that IS an IOSurface or an earlier pass's target keeps the surface's real pitch (it may be padded):
    for each fVm[m] containing td->va with surf: td->pitch = ss->bytesPerRow; td->layout = LINEAR; (size and format stay from the descriptor)
    // everything else (caller memory such as the 1x1 white, the half / R8 tables, the 153x64 widget) uses the descriptor as decoded
} else { existing 0x8002 / attachment-form handling }
```
Do not force S8T_FMT_BGRA8 / the pass size any more (they were wrong for the widget, the 1x1 white and the tables). s8t_sample returns RGBA16F texels as floats directly (half conversion in the header), so the dither table and the bell curve are usable as soon as Agent H's f16 ops exist.

### Remaining (updated)
w0 bits 16-26 and the exact meaning of w3 / w4 / w5 (compression?); Morton tiling for big textures (no big Morton texture in any capture: all compositor textures are linear, only tiny tables and 464a are Morton); mips; wrap / min-mag split; the LA8 icon geometry (needs texels). Experiments E1-E3 of sec. 8 remain the way to pin the format code bits (E2) and the sampler bits (E3).
