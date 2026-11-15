/*
 * s8mtl_test: does Metal come up on the AGX stand-in? Removable test scaffolding (no OS file is changed).
 * Tries MTLCreateSystemDefaultDevice for about a minute (the stand-in and the compositor start during boot), then logs the
 * device's properties, makes a command queue and a buffer, and tries to compile and run a tiny compute kernel.
 * Log: /var/mobile/s8mtl_test.log (else /tmp/s8mtl_test.log).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
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
			NSArray *all = MTLCopyAllDevices();
			LOG("S8MTL: try %d: default device %p, MTLCopyAllDevices %lu\n", i, (__bridge void *)dev, (unsigned long)[all count]);
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
		id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		LOG("S8MTL: buffer %p contents %p\n", (__bridge void *)buf, buf ? [buf contents] : NULL);
		NSError *err = nil;
		NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
		    "kernel void k(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i * 2; }\n";
		id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
		LOG("S8MTL: library %p error %s\n", (__bridge void *)lib, err ? [[err description] UTF8String] : "none");
		if (lib != nil && q != nil && buf != nil) {
			id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
			err = nil;
			id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
			LOG("S8MTL: pipeline %p error %s\n", (__bridge void *)ps, err ? [[err description] UTF8String] : "none");
			if (ps != nil) {
				id<MTLCommandBuffer> cb = [q commandBuffer];
				id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
				[enc setComputePipelineState:ps];
				[enc setBuffer:buf offset:0 atIndex:0];
				[enc dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
				[enc endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				uint32_t *p = (uint32_t *)[buf contents];
				LOG("S8MTL: command buffer status %ld error %s; a[0..3] = %u %u %u %u\n", (long)[cb status],
				    [cb error] ? [[[cb error] description] UTF8String] : "none", p[0], p[1], p[2], p[3]);
			}
		}
		LOG("S8MTL: done\n");
	}
	return 0;
}
