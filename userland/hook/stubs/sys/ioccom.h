/* Host syntax-check stub (NOT used for the iOS build): Darwin's <sys/ioccom.h> request encoding. */
#ifndef STUB_SYS_IOCCOM_H
#define STUB_SYS_IOCCOM_H
#define IOCPARM_MASK 0x1fff
#define IOCPARM_LEN(x) (((x) >> 16) & IOCPARM_MASK)
#define IOC_OUT 0x40000000UL
#define IOC_IN 0x80000000UL
#define IOC_INOUT (IOC_IN | IOC_OUT)
#define _IOC(inout, group, num, len) ((unsigned long)((inout) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num)))
#define _IOW(g, n, t) _IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif
