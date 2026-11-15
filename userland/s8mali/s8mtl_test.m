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
			};
			int rcount = (int)(sizeof(rvs) / sizeof(rvs[0]));
			for (int r = 0; r < rcount && tex != nil && q != nil; r++) {
				NSError *rerr = nil;
				NSString *rsrc = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
				    "struct V { float4 pos [[position]]; float4 col; };\n"
				    "vertex V vs(uint vid [[vertex_id]]) { float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) }; V o; o.pos = float4(p[vid], 0, 1); o.col = float4(p[vid] * 0.5 + 0.5, 0.25, 1); return o; }\n"
				    "fragment float4 fs(V in [[stage_in]]) { %s }\n", rvs[r].frag];
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
