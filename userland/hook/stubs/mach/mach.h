/* Host syntax-check stub (NOT used for the iOS build): what s8mali_drm.c takes from <mach/mach.h>. */
#ifndef STUB_MACH_H
#define STUB_MACH_H
#include <stdint.h>
typedef unsigned int mach_port_t;
typedef mach_port_t task_port_t;
typedef int kern_return_t;
typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;
mach_port_t mach_task_self(void);
#endif
