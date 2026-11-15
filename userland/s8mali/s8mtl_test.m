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
		LOG("S8MTL: buffer %p contents %p\n", (__bridge void *)buf, buf ? [buf contents] : NULL);
		/*
		 * A family of tiny kernels, each differing from its neighbours in ONE thing, so the AGX machine code can be decoded by comparing the
		 * binaries (one line of the program heap per kernel, captured by the kernel side at every submit). Dispatched over 64 threads with
		 * buffer 0 = output, buffer 1 = input (0..63). The kernel NUMBER n is logged with its source; session n+1 of the capture is kernel n.
		 */
		static const char *bodies[] = {
			"a[i] = i + 0;", "a[i] = i + 1;", "a[i] = i + 2;", "a[i] = i + 3;", "a[i] = i + 4;", "a[i] = i + 7;", "a[i] = i + 100;",
			"a[i] = i + 255;", "a[i] = i + 256;", "a[i] = i + 65535;",
			"a[i] = i * 1;", "a[i] = i * 2;", "a[i] = i * 3;", "a[i] = i * 4;", "a[i] = i * 5;", "a[i] = i * 6;", "a[i] = i * 7;", "a[i] = i * 10;",
			"a[i] = i << 1;", "a[i] = i << 3;", "a[i] = i >> 1;", "a[i] = i >> 2;",
			"a[i] = i & 15;", "a[i] = i | 16;", "a[i] = i ^ 255;",
			"a[i] = 100 - i;", "a[i] = i - 1;",
			"device float *f = (device float *)a; f[i] = float(i) * 2.0f;", "device float *f = (device float *)a; f[i] = float(i) + 1.0f;",
			"a[i] = b[i];", "a[i] = b[i] + b[i];",
		};
		int count = (int)(sizeof(bodies) / sizeof(bodies[0]));
		for (int n = 0; n < count && q != nil && buf != nil; n++) {
			NSError *err = nil;
			uint32_t *in = (uint32_t *)[buf2 contents];
			for (uint32_t t = 0; t < 64; t++) {
				in[t] = t;
			}
			NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
			    "kernel void kk(device uint *a [[buffer(0)]], device const uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { %s }\n",
			    bodies[n]];
			LOG("S8MTL: ==== kernel %d: %s\n", n, bodies[n]);
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
			[enc setComputePipelineState:ps];
			[enc setBuffer:buf offset:0 atIndex:0];
			[enc setBuffer:buf2 offset:0 atIndex:1];
			[enc dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
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
