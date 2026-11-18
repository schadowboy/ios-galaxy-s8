/* Bare-metal test for the Agent E devices on the s8-a5xelte machine (EL1, MMU off, load at 0x80080000).
 *  - fills the panel buffer (0xcc000000, 1440x2960 XRGB) with a test pattern,
 *  - waits for the DECON frame-done flag like the kernel does,
 *  - reads touch BOOT_STATUS/DEVICE_ID, fuel gauge SOC/VCELL/CURRENT, charger INT_OK over the HSI2C stubs,
 *  - reads the RTC through SPEEDY,
 *  - polls touch events and key GPIOs forever, printing them on the PL011 (0x13800000).
 */
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned long u64;
#define W(a) (*(volatile u32 *)(u64)(a))
#define UART 0x13800000UL
#define FB   0xcc000000UL

static void putc_(char c) { W(UART) = c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex(u32 v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }

static int i2c_xfer(u64 bus, u32 addr, const u8 *w, u32 wl, u8 *r, u32 rl)
{
    W(bus + 0x24) = W(bus + 0x24);
    W(bus + 0x70) = addr << 10;
    if (wl) {
        W(bus + 0x04) = 3;
        W(bus + 0x00) = 0x89;
        W(bus + 0x44) = (rl ? 0 : 1u << 17) | wl | 1u << 31;
        for (u32 i = 0; i < wl; i++) W(bus + 0x34) = w[i];
        u32 s = W(bus + 0x24);
        W(bus + 0x24) = s;
        if (s & 0xf00) return -1;
    }
    if (rl) {
        W(bus + 0x44) = (1u << 16) | (1u << 17) | rl | 1u << 31;
        for (u32 i = 0; i < rl; i++) {
            if (W(bus + 0x30) & (1u << 24)) return -2;
            r[i] = W(bus + 0x38);
        }
        u32 s = W(bus + 0x24);
        W(bus + 0x24) = s;
        if (s & 0xf00) return -3;
    }
    return 0;
}

static u32 rd16(u8 reg)
{
    u8 b[2] = {0, 0};
    i2c_xfer(0x10970000, 0x36, &reg, 1, b, 2);
    return b[0] | b[1] << 8;
}

void main(void)
{
    puts_("\nFBTEST start\n");
    /* pattern: 8 colour bars, white frame, black 1125x2436 centre outline */
    static const u32 bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
    for (u32 y = 0; y < 2960; y++)
        for (u32 x = 0; x < 1440; x++) {
            u32 c = bars[x * 8 / 1440];
            if (y < 4 || y >= 2956 || x < 4 || x >= 1436) c = 0xffffff;
            if ((y == 262 || y == 2697) && x >= 157 && x < 1282) c = 0xff8000;
            if ((x == 157 || x == 1281) && y >= 262 && y < 2698) c = 0xff8000;
            if (y >= 1400 && y < 1560) c = (x * 255 / 1440) * 0x010101;   /* gradient strip */
            W(FB + (u64)y * 5760 + x * 4) = c;
        }
    puts_("pattern written\n");

    W(0x12860040) |= (1u << 13) | 1;
    W(0x1286004c) = 1u << 13;
    u32 n = 0;
    while (!(W(0x1286004c) & (1u << 13)) && n < 100000000) n++;
    puts_("DECON frame-done after "); hex(n); puts_(" polls, pending="); hex(W(0x1286004c)); putc_('\n');

    u8 cmd, b[8] = {0};
    cmd = 0x55; int rc = i2c_xfer(0x108e0000, 0x48, &cmd, 1, b, 1);
    puts_("touch BOOT_STATUS rc="); hex(rc); puts_(" val="); hex(b[0]); putc_('\n');
    cmd = 0x52; rc = i2c_xfer(0x108e0000, 0x48, &cmd, 1, b, 3);
    puts_("touch DEVICE_ID "); hex(b[0] << 16 | b[1] << 8 | b[2]); putc_('\n');
    u8 absent = 0; rc = i2c_xfer(0x108e0000, 0x49, &absent, 1, b, 1);
    puts_("touch absent-slave rc (expect nonzero) "); hex(rc); putc_('\n');
    puts_("FG SOCREP "); hex(rd16(0x06)); puts_(" VCELL "); hex(rd16(0x09)); puts_(" CURRENT "); hex(rd16(0x0a)); putc_('\n');
    cmd = 0xb2; i2c_xfer(0x10970000, 0x69, &cmd, 1, b, 1);
    puts_("CHG INT_OK "); hex(b[0]); putc_('\n');

    W(0x15b50008) = 1u << 19 | 2u << 15 | 0x04u << 7;
    puts_("RTC sec "); hex(W(0x15b50038)); puts_(" min ");
    W(0x15b50008) = 1u << 19 | 2u << 15 | 0x05u << 7;
    hex(W(0x15b50038)); putc_('\n');
    puts_("polling touch/keys (use QMP touch/key)\n");

    cmd = 0x30; u8 fn[3] = {0x30, 1, 0}; i2c_xfer(0x108e0000, 0x48, fn, 3, 0, 0);
    cmd = 0x10; i2c_xfer(0x108e0000, 0x48, &cmd, 1, 0, 0);
    u32 lastk = 0xffff;
    for (;;) {
        if (!(W(0x164b0044) & 1)) {
            u8 e[8]; cmd = 0x60;
            if (!i2c_xfer(0x108e0000, 0x48, &cmd, 1, e, 8) && e[0]) {
                u32 x = e[1] << 4 | e[3] >> 4, y = e[2] << 4 | (e[3] & 15);
                puts_("TOUCH state="); hex(e[0] >> 6); puts_(" id="); hex(((e[0] >> 2) & 15) - 1);
                puts_(" x="); hex(x); puts_(" y="); hex(y); puts_(" left="); hex(e[7] & 0x3f); putc_('\n');
            }
        }
        u32 k = (W(0x164b0024) & 0xff) | (W(0x164b0064) & 0xff) << 8;
        if (k != lastk) { puts_("KEYS "); hex(k); putc_('\n'); lastk = k; }
    }
}
