/* Host syntax-check stub (NOT used for the iOS build). */
#ifndef STUB_SYS_SYSCTL_H
#define STUB_SYS_SYSCTL_H
#include <stddef.h>
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif
