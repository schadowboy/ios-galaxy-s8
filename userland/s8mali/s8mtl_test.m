/*
 * s8mtl_test: does Metal come up on the AGX stand-in? Removable test scaffolding (no OS file is changed).
 * Tries MTLCreateSystemDefaultDevice for about a minute (the stand-in and the compositor start during boot), then logs the
 * device's properties, makes a command queue and a buffer, and tries to compile and run a tiny compute kernel.
 * Log: /var/mobile/s8mtl_test.log (else /tmp/s8mtl_test.log).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

static FILE *lf;
#define LOG(...) do { fprintf(lf, __VA_ARGS__); fflush(lf); } while (0)

int
main(void)
{
	@autoreleasepool {
		lf = fopen("/var/mobile/s8mtl_test.log", "a");
		if (lf == NULL) {
			lf = fopen("/tmp/s8mtl_test.log", "a");
		}
		if (lf == NULL) {
			return 1;
		}
		LOG("S8MTL: start pid %d uid %d\n", (int)getpid(), (int)getuid());
		id<MTLDevice> dev = nil;
		for (int i = 0; i < 12 && dev == nil; i++) {
			sleep(5);
			dev = MTLCreateSystemDefaultDevice();
			LOG("S8MTL: try %d: default device %p\n", i, (__bridge void *)dev);
		}
		if (dev == nil) {
			LOG("S8MTL: no Metal device\n");
			return 2;
		}
		LOG("S8MTL: device name \"%s\" registryID 0x%llx\n", [[dev name] UTF8String], (unsigned long long)[dev registryID]);
		LOG("S8MTL: maxBufferLength %lu recommendedMaxWorkingSetSize %llu\n", (unsigned long)[dev maxBufferLength],
		    (unsigned long long)[dev recommendedMaxWorkingSetSize]);
		LOG("S8MTL: supports family Apple1..4: %d %d %d %d\n", (int)[dev supportsFamily:MTLGPUFamilyApple1],
		    (int)[dev supportsFamily:MTLGPUFamilyApple2], (int)[dev supportsFamily:MTLGPUFamilyApple3],
		    (int)[dev supportsFamily:MTLGPUFamilyApple4]);
		id<MTLCommandQueue> q = [dev newCommandQueue];
		LOG("S8MTL: command queue %p\n", (__bridge void *)q);
		{
			/*
			 * Debug aid: IOGPUMetalDevice keeps the hardware resource pools (set by -[AGXA11FamilyDevice setupDeferred] through
			 * setHwResourcePool:count:) in the ivar _storageCreateParams: { ... [3] = pool array (+0x18), count u32 (+0x20) }.
			 * Read them from this process's own device object and log which pools are missing.
			 */
			Ivar iv = class_getInstanceVariable([(id)dev class], "_storageCreateParams");
			Class c = [(id)dev class];
			while (iv == NULL && c != Nil) {
				c = class_getSuperclass(c);
				iv = c ? class_getInstanceVariable(c, "_storageCreateParams") : NULL;
			}
			if (iv != NULL) {
				uint8_t *base = (uint8_t *)(__bridge void *)dev + ivar_getOffset(iv);
				void **pools = *(void ***)(base + 0x18);
				uint32_t count = *(uint32_t *)(base + 0x20);
				LOG("S8MTL: storageCreateParams at +0x%lx: pool array %p count %u\n", (unsigned long)ivar_getOffset(iv), (void *)pools, count);
				for (uint32_t i = 0; pools != NULL && i < count && i < 64; i++) {
					LOG("S8MTL:   pool[%u] = %p\n", i, pools[i]);
				}
			} else {
				LOG("S8MTL: ivar _storageCreateParams not found\n");
			}
		}
		id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		id<MTLBuffer> buf2 = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		id<MTLBuffer> buf3 = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		LOG("S8MTL: buffer %p contents %p\n", (__bridge void *)buf, buf ? [buf contents] : NULL);
		/*
		 * Second family: one kernel per feature of the AGX compute ISA/command that the first family did not cover. Each entry has its own full
		 * kernel source and dispatch shape; buffers 0,1,2 are bound (0 and 2 outputs / scratch, 1 = input 0..63) and a constant of 5 at index 3.
		 */
		/*
		 * Render family (before the compute kernels so that its submits come first): a 64x64 BGRA target, a full-screen triangle, a constant or
		 * interpolated colour; one command buffer per variant, so the capture shows how a render pass, its clear colour, the pipeline state and a draw
		 * are encoded (and, with the target read back, what a correct result looks like).
		 */
		{
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModeShared;
			id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
			LOG("S8MTL: render target %p\n", (__bridge void *)tex);
			/* the texture of the "TEX:" scenes: 4x4 RGBA8, a known gradient with a varying alpha */
			id<MTLTexture> srctex = nil;
			{
				MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
				sd.usage = MTLTextureUsageShaderRead;
				sd.storageMode = MTLStorageModeShared;
				srctex = [dev newTextureWithDescriptor:sd];
				uint8_t pat[4 * 4 * 4];
				for (int y = 0; y < 4; y++) {
					for (int x = 0; x < 4; x++) {
						uint8_t *p = pat + (y * 4 + x) * 4;
						p[0] = (uint8_t)(x * 80 + 15); p[1] = (uint8_t)(y * 80 + 15); p[2] = (uint8_t)(255 - x * 40 - y * 20); p[3] = (uint8_t)(255 - (x + y) * 30);
					}
				}
				[srctex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:pat bytesPerRow:16];
			}
			struct rv { const char *name; const char *frag; float cr, cg, cb; int draw; int blend; } rvs[] = {
				{ "clear only black", "return float4(1,0,0,1);", 0, 0, 0, 0, 0 },
				{ "clear only 0.25/0.5/0.75", "return float4(1,0,0,1);", 0.25f, 0.5f, 0.75f, 0, 0 },
				{ "red", "return float4(1,0,0,1);", 0, 0, 0, 1, 0 },
				{ "green", "return float4(0,1,0,1);", 0, 0, 0, 1, 0 },
				{ "blue", "return float4(0,0,1,1);", 0, 0, 0, 1, 0 },
				{ "0.5/0.25/0.125", "return float4(0.5,0.25,0.125,1);", 0, 0, 0, 1, 0 },
				{ "interpolated colour", "return in.col;", 0, 0, 0, 1, 0 },
				{ "red over white, no blend", "return float4(1,0,0,1);", 1, 1, 1, 1, 0 },
				{ "red alpha 0.5 blended over white", "return float4(1,0,0,0.5);", 1, 1, 1, 1, 1 },
				/* one operation per scene (ISA calibration: in.col = (x*0.5+0.5, y*0.5+0.5, 0.25, 1) varies over the target) */
				{ "fmul imm", "return in.col * 0.5;", 0, 0, 0, 1, 0 },
				{ "fadd imm", "return in.col + float4(0.125);", 0, 0, 0, 1, 0 },
				{ "fmul vv", "return in.col * in.col;", 0, 0, 0, 1, 0 },
				{ "ffma", "return fma(in.col, float4(0.5), float4(0.25));", 0, 0, 0, 1, 0 },
				{ "swizzle", "return float4(in.col.y, in.col.x, in.col.z, 1);", 0, 0, 0, 1, 0 },
				{ "saturate scale", "return saturate(in.col * 2.0 - 0.5);", 0, 0, 0, 1, 0 },
				{ "mix", "return mix(float4(1,0,0,1), float4(0,0,1,1), in.col.x);", 0, 0, 0, 1, 0 },
				{ "select", "return in.col.x > 0.5 ? float4(1,0,0,1) : float4(0,1,0,1);", 0, 0, 0, 1, 0 },
				{ "sqrt", "return float4(sqrt(in.col.x), in.col.y, in.col.z, 1);", 0, 0, 0, 1, 0 },
				{ "rcp", "return float4(1.0/(in.col.x+1.0), in.col.y, in.col.z, 1);", 0, 0, 0, 1, 0 },
				{ "exp2 log2", "return float4(exp2(in.col.x), log2(in.col.y+1.0), 0, 1);", 0, 0, 0, 1, 0 },
				{ "min max", "return float4(min(in.col.x, 0.5), max(in.col.y, 0.25), 0, 1);", 0, 0, 0, 1, 0 },
				{ "half math", "return float4(half4(in.col) * half4(0.5));", 0, 0, 0, 1, 0 },
				{ "discard", "if (in.col.x < 0.5) discard_fragment(); return float4(1,0,0,1);", 0, 0, 0, 1, 0 },
				{ "sin", "return float4(sin(in.col.x*3.0)*0.5+0.5, 0, 0, 1);", 0, 0, 0, 1, 0 },
				{ "floor", "return float4(floor(in.col.x*4.0)/4.0, 0, 0, 1);", 0, 0, 0, 1, 0 },
				/* round 3: texture sampling ("TEX:" scenes get a 4x4 RGBA8 texture bound at index 0) and integer / conversion forms */
				{ "tex nearest", "TEX:return t.sample(sn, in.col.xy);", 0, 0, 0, 1, 0 },
				{ "tex linear", "TEX:return t.sample(sl, in.col.xy);", 0, 0, 0, 1, 0 },
				{ "tex tint", "TEX:return t.sample(sn, in.col.xy) * float4(1, 0.5, 0.25, 1);", 0, 0, 0, 1, 0 },
				{ "tex premult", "TEX:float4 c = t.sample(sn, in.col.xy); return float4(c.rgb * c.a, c.a);", 0, 0, 0, 1, 0 },
				{ "tex blend over", "TEX:return t.sample(sn, in.col.xy);", 1, 1, 1, 1, 1 },
				{ "uint bitops", "uint u = uint(in.col.x * 255.0); return float4(float(u & 0xF0u) / 255.0, float((u << 1) & 0xFFu) / 255.0, float(u >> 4) / 15.0, 1);", 0, 0, 0, 1, 0 },
				{ "int compare select", "int i = int(in.col.x * 8.0); return i > 3 ? float4(1,0,0,1) : float4(0,0,1,1);", 0, 0, 0, 1, 0 },
				{ "fract abs", "return float4(fract(in.col.x * 4.0), abs(in.col.y - 0.5) * 2.0, 0, 1);", 0, 0, 0, 1, 0 },
				{ "pow", "return float4(pow(in.col.x, 2.2), 0, 0, 1);", 0, 0, 0, 1, 0 },
				{ "unorm pack", "return float4(float(uint(in.col.x * 255.0)) / 255.0, 0, 0, 1);", 0, 0, 0, 1, 0 },
			};
			int rcount = (int)(sizeof(rvs) / sizeof(rvs[0]));
			for (int r = 0; r < rcount && tex != nil && q != nil; r++) {
				NSError *rerr = nil;
				BOOL istex = strncmp(rvs[r].frag, "TEX:", 4) == 0;
				const char *body = istex ? rvs[r].frag + 4 : rvs[r].frag;
				NSString *rsrc = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
				    "struct V { float4 pos [[position]]; float4 col; };\n"
				    "vertex V vs(uint vid [[vertex_id]]) { float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) }; V o; o.pos = float4(p[vid], 0, 1); o.col = float4(p[vid] * 0.5 + 0.5, 0.25, 1); return o; }\n"
				    "fragment float4 fs(V in [[stage_in]]%s) { %s%s }\n",
				    istex ? ", texture2d<float> t [[texture(0)]]" : "",
				    istex ? "constexpr sampler sn(filter::nearest, address::clamp_to_edge); constexpr sampler sl(filter::linear, address::clamp_to_edge); " : "",
				    body];
				LOG("S8MTL: ==== render %d: %s\n", r, rvs[r].name);
				id<MTLLibrary> rl = [dev newLibraryWithSource:rsrc options:nil error:&rerr];
				if (rl == nil) {
					LOG("S8MTL: render library failed: %s\n", rerr ? [[rerr description] UTF8String] : "?");
					continue;
				}
				MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
				pd.vertexFunction = [rl newFunctionWithName:@"vs"];
				pd.fragmentFunction = [rl newFunctionWithName:@"fs"];
				pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				if (rvs[r].blend) {
					pd.colorAttachments[0].blendingEnabled = YES;
					pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
					pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
					pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
					pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				}
				rerr = nil;
				id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:pd error:&rerr];
				if (rps == nil) {
					LOG("S8MTL: render pipeline failed: %s\n", rerr ? [[rerr description] UTF8String] : "?");
					continue;
				}
				MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
				rp.colorAttachments[0].texture = tex;
				rp.colorAttachments[0].loadAction = MTLLoadActionClear;
				rp.colorAttachments[0].storeAction = MTLStoreActionStore;
				rp.colorAttachments[0].clearColor = MTLClearColorMake(rvs[r].cr, rvs[r].cg, rvs[r].cb, 1.0);
				id<MTLCommandBuffer> rcb = [q commandBuffer];
				id<MTLRenderCommandEncoder> renc = [rcb renderCommandEncoderWithDescriptor:rp];
				if (rvs[r].draw) {
					[renc setRenderPipelineState:rps];
					if (istex && srctex != nil) {
						[renc setFragmentTexture:srctex atIndex:0];
					}
					[renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				}
				[renc endEncoding];
				[rcb commit];
				for (int w = 0; w < 5 && [rcb status] < MTLCommandBufferStatusCompleted; w++) {
					sleep(1);
				}
				uint32_t px[4] = { 0, 0, 0, 0 };
				[tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
				LOG("S8MTL: render %d status %ld; pixels (0,0) %08x (1,0) %08x\n", r, (long)[rcb status], px[0], px[1]);
			}
			/*
			 * Round 4, draw-packet scenes (vertex buffers, topologies, viewport, scissor, uniforms, instancing, indexed, two draws): CoreAnimation draws
			 * quads from vertex buffers with these states, the executor needs their encodings. Vertex data: position xy + colour-varying zw.
			 */
			if (tex != nil && q != nil) {
				NSError *e4 = nil;
				NSString *s4 = @"#include <metal_stdlib>\nusing namespace metal;\n"
				    "struct V { float4 pos [[position]]; float4 col; };\n"
				    "vertex V vsb(uint vid [[vertex_id]], uint iid [[instance_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = float4(vb[vid].xy + float2(float(iid) * 0.5, 0.0), 0.0, 1.0); o.col = float4(vb[vid].zw, 0.25, 1.0); return o; }\n"
				    "fragment float4 fsc(V in [[stage_in]]) { return in.col; }\n"
				    "fragment float4 fsu(V in [[stage_in]], constant float4 &u [[buffer(0)]]) { return u; }\n";
				id<MTLLibrary> l4 = [dev newLibraryWithSource:s4 options:nil error:&e4];
				id<MTLRenderPipelineState> pc = nil, pu = nil;

				if (l4 != nil) {
					MTLRenderPipelineDescriptor *p4 = [[MTLRenderPipelineDescriptor alloc] init];
					p4.vertexFunction = [l4 newFunctionWithName:@"vsb"];
					p4.fragmentFunction = [l4 newFunctionWithName:@"fsc"];
					p4.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
					pc = [dev newRenderPipelineStateWithDescriptor:p4 error:&e4];
					p4.fragmentFunction = [l4 newFunctionWithName:@"fsu"];
					pu = [dev newRenderPipelineStateWithDescriptor:p4 error:&e4];
				} else {
					LOG("S8MTL: round 4 library failed: %s\n", e4 ? [[e4 description] UTF8String] : "?");
				}
				float quad[6][4] = { { -0.5f, -0.5f, 0.0f, 0.0f }, { 0.5f, -0.5f, 1.0f, 0.0f }, { -0.5f, 0.5f, 0.0f, 1.0f },
				                     { 0.5f, -0.5f, 1.0f, 0.0f }, { 0.5f, 0.5f, 1.0f, 1.0f }, { -0.5f, 0.5f, 0.0f, 1.0f } };
				float strip[4][4] = { { -0.5f, -0.5f, 0.0f, 0.0f }, { 0.5f, -0.5f, 1.0f, 0.0f }, { -0.5f, 0.5f, 0.0f, 1.0f }, { 0.5f, 0.5f, 1.0f, 1.0f } };
				float shifted[6][4];
				uint16_t idx[6] = { 0, 1, 2, 3, 4, 5 };
				float ucol[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
				id<MTLBuffer> idxbuf = [dev newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];

				for (int i = 0; i < 6; i++) {
					shifted[i][0] = quad[i][0] * 0.5f + 0.3f; shifted[i][1] = quad[i][1] * 0.5f + 0.3f; shifted[i][2] = quad[i][2]; shifted[i][3] = quad[i][3];
				}
				const char *names[] = { "quad 6 verts", "triangle strip 4", "viewport 16,16,32,32", "scissor 8,8,16,16", "uniform colour (fragment bytes)",
				    "instanced x2", "two draws", "indexed quad" };
				for (int k = 0; k < 8 && pc != nil && pu != nil; k++) {
					MTLRenderPassDescriptor *rp4 = [MTLRenderPassDescriptor renderPassDescriptor];

					rp4.colorAttachments[0].texture = tex;
					rp4.colorAttachments[0].loadAction = MTLLoadActionClear;
					rp4.colorAttachments[0].storeAction = MTLStoreActionStore;
					rp4.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
					id<MTLCommandBuffer> cb4 = [q commandBuffer];
					id<MTLRenderCommandEncoder> en4 = [cb4 renderCommandEncoderWithDescriptor:rp4];

					LOG("S8MTL: ==== draw scene %d: %s\n", k, names[k]);
					[en4 setRenderPipelineState:(k == 4 ? pu : pc)];
					if (k == 1) {
						[en4 setVertexBytes:strip length:sizeof(strip) atIndex:0];
						[en4 drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
					} else {
						[en4 setVertexBytes:quad length:sizeof(quad) atIndex:0];
						if (k == 2) {
							MTLViewport vp = { 16, 16, 32, 32, 0, 1 };
							[en4 setViewport:vp];
						}
						if (k == 3) {
							MTLScissorRect sr = { 8, 8, 16, 16 };
							[en4 setScissorRect:sr];
						}
						if (k == 4) {
							[en4 setFragmentBytes:ucol length:sizeof(ucol) atIndex:0];
						}
						if (k == 7) {
							[en4 drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:idxbuf indexBufferOffset:0];
						} else if (k == 5) {
							[en4 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:2];
						} else {
							[en4 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
						}
						if (k == 6) {
							[en4 setVertexBytes:shifted length:sizeof(shifted) atIndex:0];
							[en4 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
						}
					}
					[en4 endEncoding];
					[cb4 commit];
					for (int w = 0; w < 5 && [cb4 status] < MTLCommandBufferStatusCompleted; w++) {
						sleep(1);
					}
					uint32_t p4px[4] = { 0, 0, 0, 0 };
					[tex getBytes:p4px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(32, 32, 2, 2) mipmapLevel:0];
					LOG("S8MTL: draw scene %d status %ld; centre pixels %08x %08x\n", k, (long)[cb4 status], p4px[0], p4px[1]);
				}
			}
		}
		/*
		 * Round 5: minimal vertex programs, one operation apart, to isolate each instruction form of the A11 vertex ISA
		 * (vertex id, buffer load with a stride, plain exports, one multiply, one add).
		 */
		if (tex != nil && q != nil) {
			NSError *e5 = nil;
			NSString *s5 = @"#include <metal_stdlib>\nusing namespace metal;\n"
			    "struct V { float4 pos [[position]]; float4 col; };\n"
			    "vertex V vA(uint vid [[vertex_id]]) { V o; o.pos = float4(0.25, 0.5, 0.0, 1.0); o.col = float4(1.0, 0.0, 0.0, 1.0); return o; }\n"
			    "vertex V vB(uint vid [[vertex_id]]) { V o; o.pos = float4(float(vid) * 0.5, 0.0, 0.0, 1.0); o.col = float4(1.0, 0.0, 0.0, 1.0); return o; }\n"
			    "vertex V vC(uint vid [[vertex_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = vb[vid]; o.col = float4(1.0, 0.0, 0.0, 1.0); return o; }\n"
			    "vertex V vD(uint vid [[vertex_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = float4(vb[vid].xy, 0.0, 1.0); o.col = float4(1.0, 0.0, 0.0, 1.0); return o; }\n"
			    "vertex V vE(uint vid [[vertex_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = float4(vb[vid].xy, 0.0, 1.0); o.col = vb[vid]; return o; }\n"
			    "vertex V vF(uint vid [[vertex_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = float4(vb[vid].xy, 0.0, 1.0); o.col = vb[vid] * 2.0; return o; }\n"
			    "vertex V vG(uint vid [[vertex_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = float4(vb[vid].xy + 0.125, 0.0, 1.0); o.col = vb[vid]; return o; }\n"
			    "vertex V vH(uint vid [[vertex_id]], uint iid [[instance_id]], constant float4 *vb [[buffer(0)]]) { V o; o.pos = float4(vb[vid].xy, 0.0, 1.0); o.col = float4(float(iid), 0.0, 0.0, 1.0); return o; }\n"
			    "fragment float4 fsc(V in [[stage_in]]) { return in.col; }\n";
			id<MTLLibrary> l5 = [dev newLibraryWithSource:s5 options:nil error:&e5];

			if (l5 != nil) {
				const char *vn[] = { "vA", "vB", "vC", "vD", "vE", "vF", "vG", "vH" };
				float tri[3][4] = { { -0.5f, -0.5f, 0.25f, 0.5f }, { 0.5f, -0.5f, 0.75f, 0.125f }, { 0.0f, 0.5f, 0.375f, 0.625f } };

				for (int k = 0; k < 8; k++) {
					MTLRenderPipelineDescriptor *p5 = [[MTLRenderPipelineDescriptor alloc] init];
					NSString *nm = [NSString stringWithUTF8String:vn[k]];

					p5.vertexFunction = [l5 newFunctionWithName:nm];
					p5.fragmentFunction = [l5 newFunctionWithName:@"fsc"];
					p5.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
					id<MTLRenderPipelineState> ps5 = [dev newRenderPipelineStateWithDescriptor:p5 error:&e5];

					if (ps5 == nil) {
						LOG("S8MTL: round 5 pipeline %s failed\n", vn[k]);
						continue;
					}
					MTLRenderPassDescriptor *rp5 = [MTLRenderPassDescriptor renderPassDescriptor];

					rp5.colorAttachments[0].texture = tex;
					rp5.colorAttachments[0].loadAction = MTLLoadActionClear;
					rp5.colorAttachments[0].storeAction = MTLStoreActionStore;
					rp5.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
					id<MTLCommandBuffer> cb5 = [q commandBuffer];
					id<MTLRenderCommandEncoder> en5 = [cb5 renderCommandEncoderWithDescriptor:rp5];

					LOG("S8MTL: ==== vertex scene %s\n", vn[k]);
					[en5 setRenderPipelineState:ps5];
					[en5 setVertexBytes:tri length:sizeof(tri) atIndex:0];
					[en5 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:(k == 7 ? 2 : 1)];
					[en5 endEncoding];
					[cb5 commit];
					for (int w = 0; w < 5 && [cb5 status] < MTLCommandBufferStatusCompleted; w++) {
						sleep(1);
					}
					LOG("S8MTL: vertex scene %s status %ld\n", vn[k], (long)[cb5 status]);
				}
			} else {
				LOG("S8MTL: round 5 library failed: %s\n", e5 ? [[e5 description] UTF8String] : "?");
			}
		}
		struct fam { const char *src; int gx, gy, tx, ty; } fams[] = {
			{ "kernel void kk(device uint *z [[buffer(0)]], device uint *a [[buffer(1)]], uint i [[thread_position_in_grid]]) { a[i] = i; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *z [[buffer(0)]], device uint *y [[buffer(1)]], device uint *a [[buffer(2)]], uint i [[thread_position_in_grid]]) { a[i] = i; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint2 g [[thread_position_in_grid]]) { a[g.x + g.y * 8] = g.x * 10 + g.y; }", 8, 8, 8, 4 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint t [[thread_position_in_threadgroup]], uint g [[threadgroup_position_in_grid]]) { a[g * 16 + t] = g * 100 + t; }", 64, 1, 16, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], device const uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { uint s = 0; for (uint j = 0; j < 4; j++) { s += b[j]; } a[i] = s + i; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], device const uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { a[i] = b[i] > 10 ? 5u : 9u; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { if (i < 8) { a[i] = 1; } else { a[i] = 2; } }", 64, 1, 32, 1 },
			{ "kernel void kk(device ushort *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = (ushort)(i + 1); }", 64, 1, 32, 1 },
			{ "kernel void kk(device uchar *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = (uchar)(i + 1); }", 64, 1, 32, 1 },
			{ "kernel void kk(device float *f [[buffer(0)]], device const uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { f[i] = float(b[i]) * 0.5f; }", 64, 1, 32, 1 },
			{ "kernel void kk(device float *f [[buffer(0)]], device const uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { float x = float(b[i]); f[i] = x > 10.0f ? x : 10.0f; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], constant uint &k [[buffer(3)]], uint i [[thread_position_in_grid]]) { a[i] = i + k; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i / 3; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i % 5; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = min(i, 20u); }", 64, 1, 32, 1 },
			{ "kernel void kk(device float *f [[buffer(0)]], uint i [[thread_position_in_grid]]) { f[i] = sqrt(float(i)); }", 64, 1, 32, 1 },
			{ "kernel void kk(device float *f [[buffer(0)]], uint i [[thread_position_in_grid]]) { f[i] = float(i) * 0.25f + 3.0f; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i * i; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i * i + i; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[2 * i] = i; a[2 * i + 1] = i + 1; }", 32, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = 7; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = 0; }", 64, 1, 32, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i + 1; }", 128, 1, 64, 1 },
			{ "kernel void kk(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i + 1; }", 64, 1, 16, 1 },
		};
		int count = (int)(sizeof(fams) / sizeof(fams[0]));
		for (int n = 0; n < count && q != nil && buf != nil; n++) {
			NSError *err = nil;
			uint32_t *in = (uint32_t *)[buf2 contents];
			for (uint32_t t = 0; t < 64; t++) {
				in[t] = t;
			}
			NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%s\n", fams[n].src];
			LOG("S8MTL: ==== kernel %d grid %dx%d tg %dx%d: %s\n", n, fams[n].gx, fams[n].gy, fams[n].tx, fams[n].ty, fams[n].src);
			id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
			if (lib == nil) {
				LOG("S8MTL: library failed: %s\n", err ? [[err description] UTF8String] : "?");
				continue;
			}
			id<MTLFunction> fn = [lib newFunctionWithName:@"kk"];
			err = nil;
			id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
			if (ps == nil) {
				LOG("S8MTL: pipeline failed: %s\n", err ? [[err description] UTF8String] : "?");
				continue;
			}
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
			uint32_t k5 = 5;
			[enc setComputePipelineState:ps];
			[enc setBuffer:buf offset:0 atIndex:0];
			[enc setBuffer:buf2 offset:0 atIndex:1];
			[enc setBuffer:buf3 offset:0 atIndex:2];
			[enc setBytes:&k5 length:4 atIndex:3];
			[enc dispatchThreads:MTLSizeMake(fams[n].gx, fams[n].gy, 1) threadsPerThreadgroup:MTLSizeMake(fams[n].tx, fams[n].ty, 1)];
			[enc endEncoding];
			[cb commit];
			for (int w = 0; w < 5 && [cb status] < MTLCommandBufferStatusCompleted; w++) {
				sleep(1);
			}
			uint32_t *p = (uint32_t *)[buf contents];
			LOG("S8MTL: kernel %d status %ld a[0..5] = %u %u %u %u %u %u\n", n, (long)[cb status], p[0], p[1], p[2], p[3], p[4], p[5]);
		}
		LOG("S8MTL: done\n");
	}
	return 0;
}
