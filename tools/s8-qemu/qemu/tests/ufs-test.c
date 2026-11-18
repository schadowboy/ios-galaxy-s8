/*
 * Bare-metal aarch64 test for the exynos-ufs QEMU device (machine s8-ufs-test).
 * Mirrors the real software: bsd/dev/sweet_ufs.c (+0124 Exynos quirks) and the loader's
 * shim/dreamlte/ufs_probe.c: byte-unit UTRD fields, 16-byte PRDT entries of <= 4 KiB, VS nexus
 * bit, HCE reads 0 / HCS=0xf from the S-Boot state, W-LUN 0xd0 START STOP UNIT, polled doorbell.
 * Also exercises the full Linux-style init (HCE reset, UIC link startup, NOP, query).
 * Backing image: block b, byte j = (b*131 + j*7 + (b>>8)) & 0xff, unless rewritten by the test.
 * Exit code via semihosting: 0 = PASS.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

#define HCI     0x11120000UL
#define VS      0x11121100UL
#define UART    0x09000000UL
#define DMA     0x88000000UL   /* inside RAM (256 MiB at 0x80000000; stack at 0x8fff0000) */
#define BLK     4096u

static inline u32 rd(u64 a) { return *(volatile u32 *)a; }
static inline void wr(u64 a, u32 v) { *(volatile u32 *)a = v; }

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

static void putc_(char c) { *(volatile u8 *)UART = c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(u64 v) { puts_("0x"); for (int i = 60; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }

static void exit_(int code)
{
    static u64 blk[2];
    blk[0] = 0x20026; blk[1] = code;
    register u64 x0 __asm__("x0") = 0x20;
    register u64 x1 __asm__("x1") = (u64)blk;
    __asm__ volatile("hlt #0xf000" :: "r"(x0), "r"(x1) : "memory");
    for (;;) ;
}

static int fails;
static u32 last_st;
static void check(const char *what, int ok)
{
    puts_(ok ? "  ok   " : "  FAIL ");
    puts_(what);
    if (!ok) { puts_(" [last st="); puthex(last_st); puts_("]"); }
    puts_("\n");
    if (!ok) fails++;
}

/* DMA layout */
#define D_UTRL  (DMA + 0x0000)
#define D_UCD   (DMA + 0x1000)  /* cmd 512 | resp 512 | PRDT @1024 */
#define D_DATA  (DMA + 0x10000)
#define D_DATA2 (DMA + 0x200000)
#define D_TM    (DMA + 0x2000)

static u8 tag;

static u8 pattern(u64 b, u32 j) { return (u8)(b * 131 + j * 7 + (b >> 8)); }

/* one SCSI command, slot 0. dir: 0 none, 1 in, 2 out. Returns ocs | resp<<8 | status<<16 */
static u32 scsi_lun(u8 lun, const u8 *cdb, int cl, u32 len, int dir, u64 data)
{
    volatile u8 *ucd = (volatile u8 *)D_UCD;
    volatile u32 *utrd = (volatile u32 *)D_UTRL;
    for (int i = 0; i < 1024 + 16 * 300; i++) ucd[i] = 0;
    ucd[0] = 1;
    ucd[1] = dir == 1 ? 0x40 : dir == 2 ? 0x20 : 0;
    ucd[2] = lun;
    ucd[3] = ++tag;
    ucd[12] = len >> 24; ucd[13] = len >> 16; ucd[14] = len >> 8; ucd[15] = len;
    for (int i = 0; i < cl; i++) ucd[16 + i] = cdb[i];
    u32 n = 0;
    for (u32 done = 0; done < len; done += 4096, n++) {
        u32 chunk = len - done < 4096 ? len - done : 4096;
        volatile u32 *e = (volatile u32 *)(D_UCD + 1024 + n * 16);
        e[0] = (u32)(data + done);
        e[1] = (u32)((data + done) >> 32);
        e[3] = chunk - 1;
    }
    utrd[0] = (1u << 28) | ((dir ? (dir == 2 ? 1u : 2u) : 0u) << 25) | (1u << 24);
    utrd[1] = 0;
    utrd[2] = 0x0f;
    utrd[3] = 0;
    utrd[4] = (u32)D_UCD; utrd[5] = (u32)(D_UCD >> 32);
    utrd[6] = (512u << 16) | 512u;
    utrd[7] = (1024u << 16) | (n * 16);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(VS + 0x40, rd(VS + 0x40) | 1);
    wr(HCI + 0x58, 1);
    for (long t = 0; rd(HCI + 0x58) & 1; t++) if (t > 1000000) return 0xdead;
    wr(HCI + 0x20, rd(HCI + 0x20));
    return last_st = (utrd[2] & 0xff) | (u32)ucd[512 + 6] << 8 | (u32)ucd[512 + 7] << 16;
}

static u32 scsi(const u8 *cdb, int cl, u32 len, int dir, u64 data) { return scsi_lun(0, cdb, cl, len, dir, data); }

static u32 rw10(u8 op, u32 lba, u32 cnt, u64 data)
{
    u8 c[10] = { op, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, cnt >> 8, cnt, 0 };
    return scsi(c, 10, cnt * BLK, op == 0x2a ? 2 : 1, data);
}

static int verify(u64 data, u32 lba, u32 cnt)
{
    volatile u8 *d = (volatile u8 *)data;
    for (u32 b = 0; b < cnt; b++)
        for (u32 j = 0; j < BLK; j++)
            if (d[b * BLK + j] != pattern(lba + b, j)) { puts_("   mismatch at blk "); puthex(lba + b); puts_(" off "); puthex(j); puts_("\n"); return 0; }
    return 1;
}

/* UPIU query in slot 0 (device management). Returns response code; value/data out. */
static u32 query(u8 fn_op, u8 idn, u8 idx, u32 value, u16 len, u32 *val_out, u8 **data)
{
    volatile u8 *ucd = (volatile u8 *)D_UCD;
    volatile u32 *utrd = (volatile u32 *)D_UTRL;
    for (int i = 0; i < 1024; i++) ucd[i] = 0;
    ucd[0] = 0x16; ucd[1] = 0; ucd[3] = ++tag; ucd[5] = (fn_op >= 4 && fn_op != 5) ? 0x81 : 0x01;
    ucd[12] = fn_op; ucd[13] = idn; ucd[14] = idx;
    ucd[18] = len >> 8; ucd[19] = len;
    ucd[20] = value >> 24; ucd[21] = value >> 16; ucd[22] = value >> 8; ucd[23] = value;
    utrd[0] = (2u << 28) | (1u << 24);
    utrd[1] = 0; utrd[2] = 0x0f; utrd[3] = 0;
    utrd[4] = (u32)D_UCD; utrd[5] = 0;
    utrd[6] = (512u << 16) | 512u;
    utrd[7] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    wr(HCI + 0x58, 1);
    for (long t = 0; rd(HCI + 0x58) & 1; t++) if (t > 1000000) return 0xdead;
    wr(HCI + 0x20, rd(HCI + 0x20));
    volatile u8 *r = ucd + 512;
    if (val_out) *val_out = (u32)r[20] << 24 | r[21] << 16 | r[22] << 8 | r[23];
    if (data) *data = (u8 *)(r + 32);
    return (utrd[2] & 0xff) ? 0xee00 | (utrd[2] & 0xff) : r[6];
}

static u32 uic(u32 cmd, u32 a1, u32 a2, u32 a3, u32 *res3)
{
    wr(HCI + 0x94, a1); wr(HCI + 0x98, a2); wr(HCI + 0x9c, a3);
    wr(HCI + 0x90, cmd);
    for (long t = 0; !(rd(HCI + 0x20) & (1 << 10)); t++) if (t > 100000) return 0xdead;
    wr(HCI + 0x20, 1 << 10);
    if (res3) *res3 = rd(HCI + 0x9c);
    return rd(HCI + 0x98) & 0xff;
}

void main(void)
{
    u32 st, v;
    u8 *dp;

    puts_("ufs-test: start\n");
    /* ---- S-Boot state, as the loader probe and the kernel driver see it ---- */
    check("CAP == 0x1303ff0f", rd(HCI + 0x00) == 0x1303ff0f);
    check("VER == 0x210", rd(HCI + 0x08) == 0x210);
    check("HCS == 0xf (DP|UTRLRDY|UTMRLRDY|UCRDY)", rd(HCI + 0x30) == 0xf);
    check("HCE reads 0 (USE_OF_HCE)", rd(HCI + 0x34) == 0);
    check("UTRLRSR == 1, DBR == 0, IS == 0, IE == 0", rd(HCI + 0x60) == 1 && rd(HCI + 0x58) == 0 && rd(HCI + 0x20) == 0 && rd(HCI + 0x24) == 0);
    check("VS TXPRDT/RXPRDT == 0xc, NEXUS == 0xffff", rd(VS) == 0xc && rd(VS + 4) == 0xc && rd(VS + 0x40) == 0xffff);
    check("UFSPRSECURITY desc type != 3 (16 B PRDT)", ((rd(0x11130010UL) >> 19) & 3) != 3);

    /* sweet_ufs.c ufs_take_over_host */
    wr(HCI + 0x24, 0); wr(HCI + 0x20, rd(HCI + 0x20));
    wr(HCI + 0x60, 0);
    wr(HCI + 0x50, (u32)D_UTRL); wr(HCI + 0x54, (u32)(D_UTRL >> 32));
    wr(HCI + 0x60, 1);
    check("host takes our UTRL", rd(HCI + 0x50) == (u32)D_UTRL && rd(HCI + 0x54) == 0 && (rd(HCI + 0x60) & 1));

    /* START STOP UNIT ACTIVE to the device W-LUN 0xd0, then TUR */
    static const u8 ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    st = scsi_lun(0xd0, ssu, 6, 0, 0, 0);
    check("SSU(ACTIVE) to W-LUN 0xd0 succeeds", st == 0);
    static const u8 tur[6] = { 0 };
    st = scsi(tur, 6, 0, 0, 0);
    check("TEST UNIT READY", st == 0);
    static const u8 rs[6] = { 0x03, 0, 0, 0, 18, 0 };
    st = scsi(rs, 6, 18, 1, D_DATA);
    check("REQUEST SENSE", st == 0 && (((volatile u8 *)D_DATA)[2] & 0xf) == 0);

    /* INQUIRY */
    static const u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    st = scsi(inq, 6, 36, 1, D_DATA);
    check("INQUIRY (SAMSUNG disk)", st == 0 && ((volatile u8 *)D_DATA)[0] == 0 && ((volatile u8 *)D_DATA)[8] == 'S');

    /* READ CAPACITY(10) and (16) */
    static const u8 rc10[10] = { 0x25 };
    st = scsi(rc10, 10, 8, 1, D_DATA);
    volatile u8 *d = (volatile u8 *)D_DATA;
    u32 last = (u32)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
    u32 bl = (u32)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
    check("READ CAPACITY(10): block size 4096", st == 0 && bl == 4096);
    puts_("   last LBA "); puthex(last); puts_("\n");
    static const u8 rc16[16] = { 0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0 };
    st = scsi(rc16, 16, 32, 1, D_DATA);
    u64 last16 = 0;
    for (int i = 0; i < 8; i++) last16 = last16 << 8 | d[i];
    check("READ CAPACITY(16) == (10)", st == 0 && last16 == last);

    /* ---- GPT-less reads: sector 0, then the last 1025 blocks (kernel console ring area) ---- */
    st = rw10(0x28, 0, 1, D_DATA);
    check("READ(10) LBA 0", st == 0 && verify(D_DATA, 0, 1));
    u32 first = last + 1 - 1025;
    st = rw10(0x28, first, 256, D_DATA);
    check("READ(10) last 1025 blocks (part 1, 256 blk)", st == 0 && verify(D_DATA, first, 256));
    st = rw10(0x28, first + 256, 256, D_DATA2);
    check("READ(10) last 1025 blocks (part 2)", st == 0 && verify(D_DATA2, first + 256, 256));
    st = rw10(0x28, last, 1, D_DATA);
    check("READ(10) final block", st == 0 && verify(D_DATA, last, 1));
    st = rw10(0x28, last, 2, D_DATA);
    check("READ(10) past the end -> CHECK CONDITION", st == (2u << 16));
    st = rw10(0x28, 0xfffffff0u, 1, D_DATA);
    check("READ(10) LBA far out of range -> CHECK CONDITION", st == (2u << 16));

    /* ---- write + read back (kernel writes console ring + loader log) ---- */
    volatile u8 *w = (volatile u8 *)D_DATA2;
    for (u32 j = 0; j < BLK * 16; j++) w[j] = (u8)(0xa5 ^ (j * 3));
    u32 wl = last - 1024;   /* 16 blocks inside the console ring area */
    st = rw10(0x2a, wl, 16, D_DATA2);
    check("WRITE(10) 16 blocks", st == 0);
    static const u8 sync[10] = { 0x35 };
    st = scsi(sync, 10, 0, 0, 0);
    check("SYNCHRONIZE CACHE(10)", st == 0);
    for (u32 j = 0; j < BLK * 16; j++) ((volatile u8 *)D_DATA)[j] = 0;
    st = rw10(0x28, wl, 16, D_DATA);
    int same = st == 0;
    for (u32 j = 0; same && j < BLK * 16; j++) same = ((volatile u8 *)D_DATA)[j] == (u8)(0xa5 ^ (j * 3));
    check("READ back == written", same);
    /* READ(16) / WRITE(16) */
    u8 r16[16] = { 0x88, 0, 0, 0, 0, 0, wl >> 24, wl >> 16, wl >> 8, wl, 0, 0, 0, 1, 0, 0 };
    st = scsi(r16, 16, BLK, 1, D_DATA);
    check("READ(16) one block", st == 0 && ((volatile u8 *)D_DATA)[5] == (u8)(0xa5 ^ 15));
    u8 w16[16] = { 0x8a, 0, 0, 0, 0, 0, wl >> 24, wl >> 16, wl >> 8, wl, 0, 0, 0, 1, 0, 0 };
    for (u32 j = 0; j < BLK; j++) ((volatile u8 *)D_DATA)[j] = 0x5a;
    st = scsi(w16, 16, BLK, 2, D_DATA);
    check("WRITE(16) one block (0x5a fill at ring-1024)", st == 0);

    /* DMA above 4 GiB (kernel UFS bounce buffer lives at 0x883a88000 on the phone) */
    st = rw10(0x28, 5, 4, 0x883a88000UL);
    check("READ(10) into a buffer above 4 GiB", st == 0 && verify(0x883a88000UL, 5, 4));

    /* unsupported opcode */
    static const u8 bad[6] = { 0xee };
    st = scsi(bad, 6, 0, 0, 0);
    check("unknown opcode -> CHECK CONDITION", st == (2u << 16));
    st = scsi(rs, 6, 18, 1, D_DATA);
    check("REQUEST SENSE works after a failed command", st == 0);

    /* ---- Linux-style paths: NOP, query, UTMR, UTRLCLR (inverted) ---- */
    {
        volatile u8 *ucd = (volatile u8 *)D_UCD;
        volatile u32 *utrd = (volatile u32 *)D_UTRL;
        for (int i = 0; i < 1024; i++) ucd[i] = 0;
        ucd[0] = 0; ucd[3] = 0x77;
        utrd[0] = (2u << 28) | (1u << 24); utrd[1] = 0; utrd[2] = 0xf; utrd[3] = 0;
        utrd[4] = (u32)D_UCD; utrd[5] = 0; utrd[6] = (512u << 16) | 512u; utrd[7] = 0;
        wr(HCI + 0x58, 1);
        for (long t = 0; rd(HCI + 0x58) & 1 && t < 100000; t++) ;
        wr(HCI + 0x20, rd(HCI + 0x20));
        check("NOP OUT -> NOP IN (0x20)", ucd[512] == 0x20 && ucd[515] == 0x77 && (utrd[2] & 0xff) == 0);
    }
    st = query(0x01, 0, 0, 0, 0x40, &v, &dp);
    check("QUERY read device descriptor (len 0x40, spec 2.10, 1 LU)", st == 0 && dp[0] == 0x40 && dp[6] == 1 && dp[16] == 0x02 && dp[17] == 0x10);
    st = query(0x01, 2, 0, 0, 0x2d, &v, &dp);
    u64 lb = 0;
    for (int i = 0; i < 8; i++) lb = lb << 8 | dp[11 + i];
    check("QUERY read unit descriptor 0 (4 KiB block, count == capacity)", st == 0 && dp[3] == 1 && dp[10] == 12 && lb == (u64)last + 1);
    st = query(0x01, 7, 0, 0, 0x48, &v, &dp);
    check("QUERY read geometry descriptor", st == 0 && dp[0] == 0x48);
    st = query(0x01, 2, 9, 0, 0x2d, &v, &dp);
    check("QUERY read unit descriptor 9 -> invalid index", st == 0xfc);
    st = query(0x06, 1, 0, 0, 0, &v, 0);
    check("QUERY set flag fDeviceInit", st == 0);
    st = query(0x05, 1, 0, 0, 0, &v, 0);
    check("QUERY read flag fDeviceInit -> 0 (init complete)", st == 0 && v == 0);
    st = query(0x03, 0, 0, 0, 0, &v, 0);
    check("QUERY read attr bBootLunEn", st == 0 && v == 1);
    st = query(0x04, 0, 0, 1, 0, &v, 0);
    st = query(0x03, 0, 0, 0, 0, &v, 0);
    check("QUERY write/read attr", st == 0 && v == 1);

    /* ---- full Linux-style bring-up: HCE reset, link startup, DME get/set, power mode ---- */
    wr(HCI + 0x34, 0);
    check("HCE=0 resets: HCS == 0", rd(HCI + 0x30) == 0);
    wr(HCI + 0x34, 1);
    check("HCE=1 reads 1, UCRDY set", rd(HCI + 0x34) == 1 && (rd(HCI + 0x30) & 8));
    check("UIC DME_LINKSTARTUP", uic(0x16, 0, 0, 0, 0) == 0 && (rd(HCI + 0x30) & 1));
    wr(HCI + 0x20, rd(HCI + 0x20));
    check("DME_GET PA_AvailTxDataLanes == 2", uic(0x01, 0x15200000, 0, 0, &v) == 0 && v == 2);
    check("DME_SET/GET round trip", uic(0x02, 0x15a10000, 0, 0x1234, 0) == 0 && uic(0x01, 0x15a10000, 0, 0, &v) == 0 && v == 0x1234);
    check("DME_SET PA_PWRMODE -> IS.UPMS and UPMCRS=local", uic(0x02, 0x15710000, 0, 0x11, 0) == 0 && (rd(HCI + 0x20) & 0x10) && ((rd(HCI + 0x30) >> 8) & 7) == 1);
    wr(HCI + 0x20, rd(HCI + 0x20));
    wr(HCI + 0x50, (u32)D_UTRL); wr(HCI + 0x54, 0);
    wr(HCI + 0x70, (u32)D_TM); wr(HCI + 0x74, 0);
    wr(HCI + 0x60, 1); wr(HCI + 0x80, 1);
    {
        volatile u8 *tm = (volatile u8 *)D_TM;
        for (int i = 0; i < 80; i++) tm[i] = 0;
        tm[16 + 3] = 0x42;
        wr(HCI + 0x78, 1);
        check("UTMR doorbell completes, IS.UTMRCS, response 0x24", rd(HCI + 0x78) == 0 && (rd(HCI + 0x20) & 0x200) && tm[48] == 0x24);
        wr(HCI + 0x20, 0x200);
    }
    /* interrupt line: enable UTRCS, run a command and see IS set (line level read via IS&IE) */
    wr(HCI + 0x24, 1);
    st = rw10(0x28, 3, 1, D_DATA);
    check("command with IE=1 (IRQ path enabled) still completes, ",st == 0 && verify(D_DATA, 3, 1));

    puts_(fails ? "ufs-test: FAILED\n" : "ufs-test: PASS\n");
    exit_(fails ? 1 : 0);
}

__asm__(
    ".section .text.boot\n"
    ".global _start\n"
    "_start:\n"
    "  ldr x0, =0x8fff0000\n"
    "  mov sp, x0\n"
    "  bl main\n"
    "1: b 1b\n");
