/*
 * Exynos 8895 (Galaxy S8) UFS host controller (UFSHCI 2.1 + Samsung vendor
 * registers) with a built-in single-LUN UFS device backed by a block drive.
 *
 * Models exactly what the s8-qemu guest software uses (see tools/s8-qemu/docs/
 * ufs-exynos.md for the register contract):
 *   - xnu patches 0028/0030/0034/0069/0124 (bsd/dev/sweet_ufs.c): polled, one
 *     SCSI command at a time in slot 0, takes over the host state left by S-Boot
 *   - shim/dreamlte/ufs_probe.c (loader)
 *   - Samsung 4.4 ufs-exynos.c / ufshcd.c (UIC, query, NOP, TM) for completeness.
 *
 * Exynos quirks reproduced (selectable by properties, default = real hardware):
 *   byte-utrd      UTRD response/PRDT offsets+lengths are bytes, PRDT length is
 *                  entries*entry size (UFSHCD_QUIRK_BROKEN_DWORD_UTRD)
 *   inverted-clr   UTRLCLR: bit set = clear that slot (BROKEN_REQ_LIST_CLR)
 *   booted         come out of reset in the state S-Boot leaves: link up, lists
 *                  ready, UTRLRSR=1, HCE reads 0 (USE_OF_HCE)
 *   start-stopped  LU stays "not ready, initializing command required" (02/04/02)
 *                  until START STOP UNIT power-condition ACTIVE hits W-LUN 0xd0
 *
 * Memory regions (sysbus mmio): 0 = HCI std + vendor HCI (0x2000, vendor at
 * +0x1100), 1 = UniPro (0x8000, read-as-written), 2 = UFS protector (0x100).
 * Board maps them at 0x11120000, 0x11110000, 0x11130000. One IRQ (SPI 334).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "system/dma.h"
#include "qom/object.h"

static inline uint32_t ld32le(const void *p) { return (uint32_t)ldl_le_p(p); }
static inline uint32_t ld32be(const void *p) { return (uint32_t)ldl_be_p(p); }

#define TYPE_EXYNOS_UFS "exynos-ufs"
OBJECT_DECLARE_SIMPLE_TYPE(ExynosUfsState, EXYNOS_UFS)

#define HCI_SIZE        0x2000
#define VS_BASE         0x1100
#define VS_SIZE         0x200
#define UNIPRO_SIZE     0x8000
#define UFSP_SIZE       0x100

/* UFSHCI registers */
#define R_CAP           0x00
#define R_VER           0x08
#define R_HCDDID        0x10
#define R_HCPMID        0x14
#define R_AHIT          0x18
#define R_IS            0x20
#define R_IE            0x24
#define R_HCS           0x30
#define R_HCE           0x34
#define R_UECPA         0x38
#define R_UECDL         0x3c
#define R_UECN          0x40
#define R_UECT          0x44
#define R_UECDME        0x48
#define R_UTRIACR       0x4c
#define R_UTRLBA        0x50
#define R_UTRLBAU       0x54
#define R_UTRLDBR       0x58
#define R_UTRLCLR       0x5c
#define R_UTRLRSR       0x60
#define R_UTRLCNR       0x64
#define R_UTMRLBA       0x70
#define R_UTMRLBAU      0x74
#define R_UTMRLDBR      0x78
#define R_UTMRLCLR      0x7c
#define R_UTMRLRSR      0x80
#define R_UICCMD        0x90
#define R_UCMDARG1      0x94
#define R_UCMDARG2      0x98
#define R_UCMDARG3      0x9c

/* IS bits */
#define IS_UTRCS        (1u << 0)
#define IS_UPMS         (1u << 4)
#define IS_UHXS         (1u << 5)
#define IS_UHES         (1u << 6)
#define IS_ULSS         (1u << 8)
#define IS_UTMRCS       (1u << 9)
#define IS_UCCS         (1u << 10)
/* HCS bits */
#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define HCS_UTMRLRDY    (1u << 2)
#define HCS_UCRDY       (1u << 3)

/* UIC command opcodes */
#define UIC_GET         0x01
#define UIC_SET         0x02
#define UIC_PEER_GET    0x03
#define UIC_PEER_SET    0x04
#define UIC_ENABLE      0x12
#define UIC_RESET       0x14
#define UIC_END_PT_RST  0x15
#define UIC_LINKSTARTUP 0x16
#define UIC_HIBER_ENTER 0x17
#define UIC_HIBER_EXIT  0x18
#define UIC_TEST_MODE   0x1a

#define MIB_PA_PWRMODE  0x1571

#define UFS_BLOCK       4096u
#define NUM_SLOTS       16
#define NUM_TM_SLOTS    4
#define MAX_XFER        (64u * MiB)

#define WLUN_REPORT     0x81
#define WLUN_DEVICE     0xd0
#define WLUN_BOOT       0xb0
#define WLUN_RPMB       0xc4

struct ExynosUfsState {
    SysBusDevice parent_obj;

    MemoryRegion mr_hci, mr_unipro, mr_ufsp;
    qemu_irq irq;
    AddressSpace *as;

    /* properties */
    BlockBackend *blk;
    uint64_t lun_offset;
    uint64_t lun_size;
    bool byte_utrd;
    bool inverted_clr;
    bool booted;
    bool start_stopped;
    uint32_t cap;

    /* UFSHCI */
    uint32_t is, ie, hcs, hce, ahit, utriacr;
    uint32_t utrlba, utrlbau, utrldbr, utrlrsr, utrlcnr;
    uint32_t utmrlba, utmrlbau, utmrldbr, utmrlrsr;
    uint32_t uiccmd, ucmdarg[3];
    uint32_t uec[5];

    uint8_t vs[VS_SIZE];
    uint8_t unipro[UNIPRO_SIZE];
    uint32_t ufsp[UFSP_SIZE / 4];

    /* UIC MIB (key = attr<<16 | selector) */
    GHashTable *mib;

    /* device */
    uint64_t lun_blocks;
    bool dev_active;            /* UFS device W-LUN power condition ACTIVE */
    uint8_t flags[256];
    uint32_t attrs[256];
    uint8_t sense[18];          /* sense of the last failed command */
};

/* ---------------------------------------------------------------- IRQ */

static void ufs_update_irq(ExynosUfsState *s)
{
    qemu_set_irq(s->irq, !!(s->is & s->ie));
}

static void ufs_raise(ExynosUfsState *s, uint32_t bits)
{
    s->is |= bits;
    ufs_update_irq(s);
}

/* ---------------------------------------------------------------- DMA helpers */

static bool dma_rd(ExynosUfsState *s, uint64_t addr, void *buf, uint32_t len)
{
    MemTxResult r = address_space_read(s->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "exynos-ufs: DMA read of %u bytes at 0x%" PRIx64 " failed\n", len, addr);
    }
    return r == MEMTX_OK;
}

static bool dma_wr(ExynosUfsState *s, uint64_t addr, const void *buf, uint32_t len)
{
    MemTxResult r = address_space_write(s->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "exynos-ufs: DMA write of %u bytes at 0x%" PRIx64 " failed\n", len, addr);
    }
    return r == MEMTX_OK;
}

typedef struct Sg {
    uint64_t addr;
    uint32_t len;
} Sg;

/* PRDT -> array of segments; returns count (g_free *out). */
static unsigned ufs_read_prdt(ExynosUfsState *s, uint64_t ucd, uint32_t dw7, Sg **out)
{
    uint32_t off = dw7 >> 16, cnt = dw7 & 0xffff;
    uint32_t stride = (((s->ufsp[0x10 / 4] >> 19) & 3) == 3) ? 128 : 16;
    unsigned n = s->byte_utrd ? cnt / stride : cnt;
    uint64_t base = ucd + (s->byte_utrd ? off : off * 4);
    Sg *sg = g_new0(Sg, n ? n : 1);

    for (unsigned i = 0; i < n; i++) {
        uint8_t e[16];
        if (!dma_rd(s, base + (uint64_t)i * stride, e, 16)) {
            n = i;
            break;
        }
        sg[i].addr = ld32le(e) | ((uint64_t)ld32le(e + 4) << 32);
        sg[i].len = (ld32le(e + 12) & 0x3ffff) + 1;
    }
    *out = sg;
    return n;
}

static uint64_t sg_total(const Sg *sg, unsigned n)
{
    uint64_t t = 0;
    for (unsigned i = 0; i < n; i++) {
        t += sg[i].len;
    }
    return t;
}

/* copy buf -> guest (to_guest) or guest -> buf, over the segments, len bytes */
static bool sg_copy(ExynosUfsState *s, const Sg *sg, unsigned n, uint8_t *buf,
                    uint64_t len, bool to_guest)
{
    uint64_t done = 0;
    for (unsigned i = 0; i < n && done < len; i++) {
        uint32_t c = MIN(sg[i].len, len - done);
        bool ok = to_guest ? dma_wr(s, sg[i].addr, buf + done, c)
                           : dma_rd(s, sg[i].addr, buf + done, c);
        if (!ok) {
            return false;
        }
        done += c;
    }
    return done == len;
}

/* ---------------------------------------------------------------- SCSI target */

enum { DIR_NONE, DIR_IN, DIR_OUT };

typedef struct ScsiRes {
    uint8_t status;     /* SCSI status */
    uint8_t key, asc, ascq;
    uint32_t in_len;    /* valid bytes in buf when data-in */
} ScsiRes;

static void check(ScsiRes *r, uint8_t key, uint8_t asc, uint8_t ascq)
{
    r->status = 2;
    r->key = key;
    r->asc = asc;
    r->ascq = ascq;
}

static void be16(uint8_t *p, uint16_t v) { stw_be_p(p, v); }
static void be32(uint8_t *p, uint32_t v) { stl_be_p(p, v); }
static void be64(uint8_t *p, uint64_t v) { stq_be_p(p, v); }

static bool is_wlun(uint8_t lun)
{
    return lun == WLUN_REPORT || lun == WLUN_DEVICE || lun == WLUN_BOOT || lun == WLUN_RPMB;
}

static void inquiry_data(ExynosUfsState *s, uint8_t lun, const uint8_t *cdb,
                         uint8_t *buf, uint32_t alloc, ScsiRes *r)
{
    uint8_t tmp[96] = { 0 };
    uint32_t len;

    if (cdb[1] & 1) { /* EVPD */
        tmp[0] = is_wlun(lun) ? 0x1e : 0x00;
        tmp[1] = cdb[2];
        switch (cdb[2]) {
        case 0x00:
            tmp[3] = 3; tmp[4] = 0x00; tmp[5] = 0x80; tmp[6] = 0xb0;
            len = 7;
            break;
        case 0x80:
            tmp[3] = 10; memcpy(tmp + 4, "S8QEMU0001", 10);
            len = 14;
            break;
        case 0xb0: /* block limits */
            tmp[3] = 0x3c;
            be32(tmp + 8, 0x10000);   /* max transfer length (blocks) */
            be32(tmp + 12, 0x10000);  /* optimal transfer length */
            len = 64;
            break;
        default:
            check(r, 5, 0x24, 0);
            return;
        }
    } else {
        tmp[0] = is_wlun(lun) ? 0x1e : 0x00;
        tmp[2] = 0x06;      /* SPC-4 */
        tmp[3] = 0x02;      /* response data format */
        tmp[4] = 31;
        tmp[7] = 0x02;      /* CmdQue */
        memcpy(tmp + 8, "SAMSUNG ", 8);
        memcpy(tmp + 16, "KLUCG4J1ED-B0C1 ", 16);
        memcpy(tmp + 32, "0300", 4);
        len = 36;
    }
    len = MIN(len, alloc);
    memcpy(buf, tmp, len);
    r->in_len = len;
}

static void mode_sense(const uint8_t *cdb, bool ten, uint8_t *buf, uint32_t alloc, ScsiRes *r)
{
    uint8_t page = cdb[2] & 0x3f;
    uint8_t body[64] = { 0 };
    uint32_t n = 0;
    uint32_t hdr = ten ? 8 : 4;

    if (page == 0x08 || page == 0x3f) {
        body[n] = 0x08; body[n + 1] = 0x12; body[n + 2] = 0x04; /* WCE */
        n += 0x14;
    }
    if (page == 0x0a || page == 0x3f) {
        body[n] = 0x0a; body[n + 1] = 0x0a; body[n + 2] = 0x02;
        n += 0x0c;
    }
    if (!n) {
        check(r, 5, 0x24, 0);
        return;
    }
    uint8_t out[80] = { 0 };
    if (ten) {
        be16(out, hdr + n - 2);
    } else {
        out[0] = hdr + n - 1;
    }
    memcpy(out + hdr, body, n);
    uint32_t len = MIN(hdr + n, alloc);
    memcpy(buf, out, len);
    r->in_len = len;
}

/* Execute one CDB. buf has capacity bufcap (data buffer); data-out already in buf. */
static void scsi_exec(ExynosUfsState *s, uint8_t lun, const uint8_t *cdb, uint8_t *buf,
                      uint32_t bufcap, int dir, ScsiRes *r)
{
    uint8_t op = cdb[0];
    uint64_t lba = 0, nblk = 0;
    uint64_t base = s->lun_offset;

    memset(r, 0, sizeof(*r));

    if (lun != 0 && !is_wlun(lun)) {
        if (op == 0x12) { /* INQUIRY for absent LUN: peripheral qualifier 3 */
            memset(buf, 0, MIN(bufcap, 36));
            if (bufcap) buf[0] = 0x7f;
            r->in_len = MIN(bufcap, 36);
            return;
        }
        check(r, 5, 0x25, 0); /* LU not supported */
        return;
    }

    if (lun == 0 && s->start_stopped && !s->dev_active &&
        op != 0x12 && op != 0x03 && op != 0xa0) {
        check(r, 2, 0x04, 0x02); /* NOT READY, initializing command required */
        return;
    }

    if (lun == 0 && !s->lun_blocks && op != 0x12 && op != 0x03 && op != 0xa0 && op != 0x1b) {
        check(r, 2, 0x3a, 0); /* medium not present */
        return;
    }

    switch (op) {
    case 0x00: /* TEST UNIT READY */
        return;
    case 0x03: { /* REQUEST SENSE */
        uint8_t sn[18] = { 0 };
        if (lun == 0 && s->start_stopped && !s->dev_active) {
            sn[0] = 0x70; sn[2] = 2; sn[7] = 10; sn[12] = 4; sn[13] = 2;
        } else {
            sn[0] = 0x70; sn[7] = 10;
        }
        r->in_len = MIN(bufcap, MIN(cdb[4], 18));
        memcpy(buf, sn, r->in_len);
        return;
    }
    case 0x12: /* INQUIRY */
        inquiry_data(s, lun, cdb, buf, MIN(bufcap, (cdb[3] << 8) | cdb[4]), r);
        return;
    case 0x1a: /* MODE SENSE(6) */
        mode_sense(cdb, false, buf, MIN(bufcap, cdb[4]), r);
        return;
    case 0x5a: /* MODE SENSE(10) */
        mode_sense(cdb, true, buf, MIN(bufcap, (cdb[7] << 8) | cdb[8]), r);
        return;
    case 0x15: case 0x55: /* MODE SELECT: accept */
        return;
    case 0x1b: /* START STOP UNIT */
        if (lun == WLUN_DEVICE && (cdb[4] >> 4) == 1) {
            s->dev_active = true;
        }
        return;
    case 0x35: case 0x91: /* SYNCHRONIZE CACHE 10/16 */
        if (s->blk) {
            blk_flush(s->blk);
        }
        return;
    case 0x2f: case 0x8f: /* VERIFY */
    case 0x42: /* UNMAP: accepted, no-op */
    case 0x1e: /* PREVENT/ALLOW MEDIUM REMOVAL */
    case 0x3c: /* READ BUFFER */
        return;
    case 0xa0: { /* REPORT LUNS */
        uint8_t o[24] = { 0 };
        be32(o, 8);
        r->in_len = MIN(bufcap, (uint32_t)16);
        memcpy(buf, o, r->in_len);
        return;
    }
    case 0x25: { /* READ CAPACITY(10) */
        uint64_t last = s->lun_blocks - 1;
        be32(buf, last > 0xffffffffull ? 0xffffffffu : (uint32_t)last);
        be32(buf + 4, UFS_BLOCK);
        r->in_len = MIN(bufcap, 8u);
        return;
    }
    case 0x9e: { /* SERVICE ACTION IN(16) */
        if ((cdb[1] & 0x1f) != 0x10) {
            check(r, 5, 0x24, 0);
            return;
        }
        uint8_t o[32] = { 0 };
        be64(o, s->lun_blocks - 1);
        be32(o + 8, UFS_BLOCK);
        r->in_len = MIN(bufcap, (uint32_t)32);
        memcpy(buf, o, r->in_len);
        return;
    }
    case 0x08: case 0x28: case 0xa8: case 0x88:
    case 0x0a: case 0x2a: case 0xaa: case 0x8a:
        break;
    default:
        check(r, 5, 0x20, 0); /* invalid command operation code */
        return;
    }

    /* READ/WRITE 6/10/12/16 */
    switch (op) {
    case 0x08: case 0x0a:
        lba = ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3];
        nblk = cdb[4] ? cdb[4] : 256;
        break;
    case 0x28: case 0x2a:
        lba = ld32be(cdb + 2); nblk = lduw_be_p(cdb + 7);
        break;
    case 0xa8: case 0xaa:
        lba = ld32be(cdb + 2); nblk = ld32be(cdb + 6);
        break;
    default:
        lba = ldq_be_p(cdb + 2); nblk = ld32be(cdb + 10);
        break;
    }
    bool wr = (op & 0x02) != 0 && (op == 0x0a || op == 0x2a || op == 0xaa || op == 0x8a);
    if (lun != 0) {
        check(r, 5, 0x20, 0);
        return;
    }
    if (lba >= s->lun_blocks || nblk > s->lun_blocks - lba) {
        check(r, 5, 0x21, 0); /* LBA out of range */
        return;
    }
    if ((uint64_t)nblk * UFS_BLOCK > bufcap) {
        check(r, 5, 0x24, 0);
        return;
    }
    if (!nblk) {
        return;
    }
    if (!s->blk) {
        check(r, 3, 0x11, 0);
        return;
    }
    if (wr) {
        if (dir != DIR_OUT) {
            check(r, 5, 0x24, 0);
            return;
        }
        if (blk_pwrite(s->blk, base + lba * UFS_BLOCK, nblk * UFS_BLOCK, buf, 0) < 0) {
            check(r, 3, 0x0c, 0);
        }
    } else {
        if (blk_pread(s->blk, base + lba * UFS_BLOCK, nblk * UFS_BLOCK, buf, 0) < 0) {
            check(r, 3, 0x11, 0);
        } else {
            r->in_len = nblk * UFS_BLOCK;
        }
    }
}

/* ---------------------------------------------------------------- query descriptors */

static const char *const ufs_strings[] = {
    NULL, "SAMSUNG", "KLUCG4J1ED-B0C1", "S8QEMU0001", "", "0300",
};

/* Build descriptor idn/index; returns descriptor length or 0 + *err */
static uint32_t build_desc(ExynosUfsState *s, uint8_t idn, uint8_t index, uint8_t *d,
                           uint8_t *err)
{
    memset(d, 0, 0x100);
    switch (idn) {
    case 0: /* device */
        d[0] = 0x40; d[1] = 0; d[6] = 1; d[7] = 4; d[8] = 1; d[10] = 1; d[11] = 0x7f;
        d[13] = 1; d[14] = 0x10; be16(d + 16, 0x0210);
        d[20] = 1; d[21] = 2; d[22] = 3; d[23] = 4; be16(d + 24, 0x0145);
        d[26] = 0x16; d[27] = 0x1a; d[28] = 2; d[33] = 32; be16(d + 34, 0x0300);
        d[42] = 5;
        return 0x40;
    case 1: /* configuration (header + unit params, zero) */
        d[0] = 0x90; d[1] = 1; d[2] = index;
        return 0x90;
    case 2: /* unit */
        if (index >= 8) {
            *err = 0xfc;
            return 0;
        }
        d[0] = 0x2d; d[1] = 2; d[2] = index;
        if (index == 0) {
            d[3] = 1; d[4] = 0; d[6] = 32; d[10] = 12;
            be64(d + 11, s->lun_blocks);
            be32(d + 19, 0x400);
            be64(d + 24, s->lun_blocks);
        }
        return 0x2d;
    case 4: /* interconnect */
        d[0] = 6; d[1] = 4; be16(d + 2, 0x0180); be16(d + 4, 0x0310);
        return 6;
    case 5: { /* string */
        if (index == 0 || index >= ARRAY_SIZE(ufs_strings)) {
            *err = 0xfc;
            return 0;
        }
        const char *str = ufs_strings[index];
        uint32_t n = strlen(str);
        d[0] = 2 + 2 * n; d[1] = 5;
        for (uint32_t i = 0; i < n; i++) {
            d[3 + 2 * i] = str[i]; /* UTF-16BE */
        }
        return d[0];
    }
    case 7: /* geometry */
        d[0] = 0x48; d[1] = 7;
        be64(d + 4, s->lun_blocks * (UFS_BLOCK / 512));
        be32(d + 13, 0x2000); d[17] = 8; d[18] = 8; d[19] = 8; d[20] = 8; d[21] = 8; d[22] = 8;
        d[23] = 0; d[26] = 5; d[29] = 0; be16(d + 30, 0x0001);
        return 0x48;
    case 8: /* power */
        d[0] = 0x62; d[1] = 8;
        return 0x62;
    case 9: /* health */
        d[0] = 0x25; d[1] = 9;
        return 0x25;
    default:
        *err = 0xfa;
        return 0;
    }
}

/* Handle a query request UPIU; resp (>= 32 bytes + data) built here. Returns resp length. */
static uint32_t ufs_query(ExynosUfsState *s, const uint8_t *req, const uint8_t *reqdata,
                          uint32_t reqdata_len, uint8_t *resp, uint32_t resp_cap)
{
    uint8_t opcode = req[12], idn = req[13], index = req[14];
    uint16_t length = lduw_be_p(req + 18);
    uint32_t value = ld32be(req + 20);
    uint8_t code = 0;
    uint32_t dlen = 0;
    uint8_t desc[0x100];

    memset(resp, 0, resp_cap);
    memcpy(resp, req, 32);
    resp[0] = 0x36;
    resp[1] = req[1] & 0x0f;

    switch (opcode) {
    case 0x01: { /* read descriptor */
        uint32_t dl = build_desc(s, idn, index, desc, &code);
        if (!dl) {
            break;
        }
        dlen = MIN((uint32_t)length, dl);
        if (32 + dlen > resp_cap) {
            code = 0xf9;
            break;
        }
        memcpy(resp + 32, desc, dlen);
        break;
    }
    case 0x02: { /* write descriptor: accepted (configuration descriptor) */
        (void)reqdata; (void)reqdata_len;
        if (idn > 9) {
            code = 0xfa;
        }
        break;
    }
    case 0x03: /* read attribute */
        value = s->attrs[idn];
        break;
    case 0x04: /* write attribute */
        s->attrs[idn] = value;
        break;
    case 0x05: /* read flag */
        value = s->flags[idn] & 1;
        break;
    case 0x06: /* set flag */
        s->flags[idn] = (idn == 1) ? 0 : 1; /* fDeviceInit self-clears: init completes at once */
        value = s->flags[idn];
        break;
    case 0x07: /* clear flag */
        s->flags[idn] = 0;
        value = 0;
        break;
    case 0x08: /* toggle flag */
        s->flags[idn] ^= 1;
        value = s->flags[idn];
        break;
    default:
        code = 0xff;
        break;
    }
    resp[6] = code;
    stw_be_p(resp + 10, dlen);
    if (opcode == 0x01 && !code) {
        stw_be_p(resp + 18, dlen);
    }
    stl_be_p(resp + 20, value);
    return 32 + dlen;
}

/* ---------------------------------------------------------------- UTP transfer request */

#define OCS_SUCCESS 0x0
#define OCS_INVALID_CMD_TABLE_ATTR 0x1
#define OCS_FATAL 0x7
#define OCS_INVALID 0x0f

static void ufs_do_slot(ExynosUfsState *s, unsigned slot)
{
    uint64_t utrl = ((uint64_t)s->utrlbau << 32) | s->utrlba;
    uint64_t utrd_a = utrl + (uint64_t)slot * 32;
    uint8_t utrd[32];
    uint32_t dw0, dw6, dw7;
    uint64_t ucd;
    uint8_t ocs = OCS_SUCCESS;
    uint8_t req[32 + 512];
    uint8_t resp[512];
    uint32_t resp_len = 0;
    Sg *sg = NULL;
    unsigned nsg = 0;
    uint8_t *buf = NULL;

    if (!dma_rd(s, utrd_a, utrd, 32)) {
        ufs_raise(s, 1u << 17); /* system bus fatal error */
        return;
    }
    dw0 = ld32le(utrd);
    dw6 = ld32le(utrd + 24);
    dw7 = ld32le(utrd + 28);
    ucd = ld32le(utrd + 16) | ((uint64_t)ld32le(utrd + 20) << 32);

    uint64_t resp_a = ucd + (s->byte_utrd ? (dw6 >> 16) : (dw6 >> 16) * 4);
    uint32_t resp_max = s->byte_utrd ? (dw6 & 0xffff) : (dw6 & 0xffff) * 4;

    memset(req, 0, sizeof(req));
    memset(resp, 0, sizeof(resp));
    if (!dma_rd(s, ucd, req, 32)) {
        ocs = OCS_FATAL;
        goto done;
    }
    uint32_t seglen = lduw_be_p(req + 10);
    if (seglen && seglen <= 512 && !dma_rd(s, ucd + 32, req + 32, seglen)) {
        ocs = OCS_FATAL;
        goto done;
    }
    nsg = ufs_read_prdt(s, ucd, dw7, &sg);

    uint8_t ttype = req[0] & 0x3f;
    switch (ttype) {
    case 0x00: /* NOP OUT -> NOP IN */
        resp[0] = 0x20;
        resp[3] = req[3];
        resp_len = 32;
        break;
    case 0x16: { /* QUERY REQUEST */
        resp_len = ufs_query(s, req, req + 32, seglen, resp, sizeof(resp));
        break;
    }
    case 0x01: { /* COMMAND */
        uint8_t lun = req[2];
        uint32_t expect = ld32be(req + 12);
        int dir = (req[1] & 0x40) ? DIR_IN : (req[1] & 0x20) ? DIR_OUT : DIR_NONE;
        uint64_t cap = sg_total(sg, nsg);
        ScsiRes r;

        if (cap > MAX_XFER) {
            cap = MAX_XFER;
        }
        buf = g_malloc0(MAX(cap, (uint64_t)256));
        if (dir == DIR_OUT) {
            uint64_t n = MIN(cap, (uint64_t)expect);
            if (!sg_copy(s, sg, nsg, buf, n, false)) {
                ocs = OCS_FATAL;
                break;
            }
        }
        scsi_exec(s, lun, req + 16, buf, MAX(cap, (uint64_t)256), dir, &r);
        if (dir == DIR_IN && r.in_len) {
            uint64_t n = MIN((uint64_t)r.in_len, cap);
            if (!sg_copy(s, sg, nsg, buf, n, true)) {
                ocs = OCS_FATAL;
                break;
            }
            r.in_len = n;
        }
        resp[0] = 0x21;
        resp[1] = req[1] & 0x0f;
        resp[2] = lun;
        resp[3] = req[3];
        resp[4] = req[4];
        resp[6] = 0;                /* response: TARGET SUCCESS */
        resp[7] = r.status;         /* SCSI status */
        if (dir == DIR_IN) {
            be32(resp + 12, expect > r.in_len ? expect - r.in_len : 0);
        }
        if (r.status) {
            memset(s->sense, 0, sizeof(s->sense));
            s->sense[0] = 0x70; s->sense[2] = r.key; s->sense[7] = 10;
            s->sense[12] = r.asc; s->sense[13] = r.ascq;
            be16(resp + 32, 18);
            memcpy(resp + 34, s->sense, 18);
            be16(resp + 10, 20);
            resp_len = 32 + 20;
        } else {
            resp_len = 32;
        }
        break;
    }
    default:
        ocs = OCS_INVALID_CMD_TABLE_ATTR;
        break;
    }

    if (!ocs && resp_len) {
        if (resp_len > resp_max) {
            resp_len = resp_max;
        }
        if (!dma_wr(s, resp_a, resp, resp_len)) {
            ocs = OCS_FATAL;
        }
    }
done:
    g_free(buf);
    g_free(sg);
    /* write OCS back into UTRD dw2 */
    stl_le_p(utrd + 8, (ld32le(utrd + 8) & ~0xffu) | ocs);
    dma_wr(s, utrd_a + 8, utrd + 8, 4);

    s->utrldbr &= ~(1u << slot);
    if (dw0 & (1u << 24)) {
        ufs_raise(s, IS_UTRCS);
    }
    s->utrlcnr |= 1u << slot;
}

static void ufs_run_doorbell(ExynosUfsState *s)
{
    if (!(s->utrlrsr & 1)) {
        return;
    }
    for (unsigned i = 0; i < NUM_SLOTS; i++) {
        if (s->utrldbr & (1u << i)) {
            ufs_do_slot(s, i);
        }
    }
}

static void ufs_do_tm(ExynosUfsState *s, unsigned slot)
{
    uint64_t base = (((uint64_t)s->utmrlbau << 32) | s->utmrlba) + slot * 80ull;
    uint8_t d[80];

    if (!dma_rd(s, base, d, 80)) {
        s->utmrldbr &= ~(1u << slot);
        return;
    }
    memset(d + 48, 0, 32);
    d[48] = 0x24;       /* TASK MANAGEMENT RESPONSE */
    d[48 + 3] = d[16 + 3];
    stl_le_p(d + 8, ld32le(d + 8) & ~0xffu); /* OCS success */
    dma_wr(s, base + 8, d + 8, 4);
    dma_wr(s, base + 48, d + 48, 32);
    s->utmrldbr &= ~(1u << slot);
    ufs_raise(s, IS_UTMRCS);
}

/* ---------------------------------------------------------------- UIC */

static uint32_t mib_default(uint32_t key)
{
    switch (key >> 16) {
    case 0x1520: /* PA_AvailTxDataLanes */
    case 0x1540: /* PA_AvailRxDataLanes */
    case 0x1561: /* PA_ConnectedTxDataLanes */
    case 0x1581: /* PA_ConnectedRxDataLanes */
        return 2;
    case 0x1560: case 0x1580: /* PA_ActiveTx/RxDataLanes */
        return 2;
    case 0x1571: return 0x11;   /* PA_PWRMODE FAST/FAST */
    case 0x15a8: return 1;      /* PA_LOCAL_TX_LCC_ENABLE? */
    case 0x1587: case 0x1568: /* PA_MaxRxHSGear / PA_TxGear */
        return 3;
    case 0x3000: return 0x0180; /* DME... */
    default:
        return 0;
    }
}

static void ufs_uic(ExynosUfsState *s, uint32_t cmd)
{
    uint32_t key = s->ucmdarg[0];
    uint32_t res = 0;
    uint32_t is = IS_UCCS;

    switch (cmd & 0xff) {
    case UIC_GET:
    case UIC_PEER_GET: {
        gpointer v;
        s->ucmdarg[2] = g_hash_table_lookup_extended(s->mib, GUINT_TO_POINTER(key), NULL, &v)
                        ? GPOINTER_TO_UINT(v) : mib_default(key);
        break;
    }
    case UIC_SET:
    case UIC_PEER_SET:
        g_hash_table_insert(s->mib, GUINT_TO_POINTER(key), GUINT_TO_POINTER(s->ucmdarg[2]));
        if ((cmd & 0xff) == UIC_SET && (key >> 16) == MIB_PA_PWRMODE) {
            s->hcs = (s->hcs & ~(7u << 8)) | (1u << 8); /* UPMCRS = PWR_LOCAL */
            is |= IS_UPMS;
        }
        break;
    case UIC_LINKSTARTUP:
        s->hcs |= HCS_DP;
        is |= IS_ULSS;
        break;
    case UIC_HIBER_ENTER:
        is |= IS_UHES;
        break;
    case UIC_HIBER_EXIT:
        is |= IS_UHXS;
        break;
    case UIC_ENABLE: case UIC_RESET: case UIC_END_PT_RST: case UIC_TEST_MODE:
        break;
    default:
        res = 0x0a; /* DME failure */
        break;
    }
    s->ucmdarg[1] = (s->ucmdarg[1] & ~0xffu) | res;
    s->uiccmd = cmd & 0xff;
    ufs_raise(s, is);
}

/* ---------------------------------------------------------------- reset */

static void ufs_reset_regs(ExynosUfsState *s)
{
    s->is = s->ie = s->hce = s->ahit = s->utriacr = 0;
    s->utrlba = s->utrlbau = s->utrldbr = s->utrlrsr = s->utrlcnr = 0;
    s->utmrlba = s->utmrlbau = s->utmrldbr = s->utmrlrsr = 0;
    s->uiccmd = 0;
    memset(s->ucmdarg, 0, sizeof(s->ucmdarg));
    memset(s->uec, 0, sizeof(s->uec));
    s->hcs = 0;
    ufs_update_irq(s);
}

static void ufs_dev_reset(DeviceState *dev)
{
    ExynosUfsState *s = EXYNOS_UFS(dev);

    ufs_reset_regs(s);
    memset(s->vs, 0, sizeof(s->vs));
    stl_le_p(s->vs + 0x00, 0xc);        /* TXPRDT_ENTRY_SIZE (captured on phone) */
    stl_le_p(s->vs + 0x04, 0xc);
    stl_le_p(s->vs + 0x40, 0xffff);     /* UTRL_NEXUS_TYPE (captured) */
    memset(s->unipro, 0, sizeof(s->unipro));
    memset(s->ufsp, 0, sizeof(s->ufsp));
    s->ufsp[0x10 / 4] = 0xefe26492;     /* UFSPRSECURITY (captured; desc type != 3 -> 16B PRDT) */
    g_hash_table_remove_all(s->mib);
    memset(s->flags, 0, sizeof(s->flags));
    memset(s->attrs, 0, sizeof(s->attrs));
    s->attrs[0x00] = 1;         /* bBootLunEn */
    s->attrs[0x02] = 0x11;      /* bCurrentPowerMode: active */
    s->attrs[0x03] = 0;         /* bActiveICCLevel */
    s->attrs[0x07] = 0x20;      /* bMaxDataInSize */
    s->attrs[0x08] = 0x20;
    s->attrs[0x0a] = 0;         /* bRefClkFreq */
    s->attrs[0x0c] = 2;         /* bMaxNumOfRTT */
    s->dev_active = !s->start_stopped;
    memset(s->sense, 0, sizeof(s->sense));

    if (s->booted) {
        /* state left by S-Boot: link up, lists ready, running; HCE reads 0 */
        s->hcs = HCS_DP | HCS_UTRLRDY | HCS_UTMRLRDY | HCS_UCRDY;
        s->utrlrsr = 1;
        s->utmrlrsr = 1;
        s->utrlba = 0xef002000;     /* S-Boot's list address (captured); the driver replaces it */
    }
}

/* ---------------------------------------------------------------- MMIO: HCI + vendor */

static uint64_t hci_read(void *opaque, hwaddr off, unsigned size)
{
    ExynosUfsState *s = opaque;
    uint32_t v = 0;

    if (off >= VS_BASE && off < VS_BASE + VS_SIZE) {
        hwaddr o = off - VS_BASE;
        if (o == 0x50) {
            return 0;   /* HCI_SW_RST self-clears */
        }
        return ld32le(s->vs + (o & ~3));
    }
    switch (off & ~3) {
    case R_CAP:      v = s->cap; break;
    case R_VER:      v = 0x210; break;
    case R_HCDDID:   v = 0; break;
    case R_HCPMID:   v = 0x1; break;
    case R_AHIT:     v = s->ahit; break;
    case R_IS:       v = s->is; break;
    case R_IE:       v = s->ie; break;
    case R_HCS:      v = s->hcs; break;
    case R_HCE:      v = s->hce; break;
    case R_UECPA:    v = s->uec[0]; break;
    case R_UECDL:    v = s->uec[1]; break;
    case R_UECN:     v = s->uec[2]; break;
    case R_UECT:     v = s->uec[3]; break;
    case R_UECDME:   v = s->uec[4]; break;
    case R_UTRIACR:  v = s->utriacr; break;
    case R_UTRLBA:   v = s->utrlba; break;
    case R_UTRLBAU:  v = s->utrlbau; break;
    case R_UTRLDBR:  v = s->utrldbr; break;
    case R_UTRLCLR:  v = 0; break;
    case R_UTRLRSR:  v = s->utrlrsr; break;
    case R_UTRLCNR:  v = s->utrlcnr; break;
    case R_UTMRLBA:  v = s->utmrlba; break;
    case R_UTMRLBAU: v = s->utmrlbau; break;
    case R_UTMRLDBR: v = s->utmrldbr; break;
    case R_UTMRLCLR: v = 0; break;
    case R_UTMRLRSR: v = s->utmrlrsr; break;
    case R_UICCMD:   v = s->uiccmd; break;
    case R_UCMDARG1: v = s->ucmdarg[0]; break;
    case R_UCMDARG2: v = s->ucmdarg[1]; break;
    case R_UCMDARG3: v = s->ucmdarg[2]; break;
    default:
        qemu_log_mask(LOG_UNIMP, "exynos-ufs: read of unknown HCI reg 0x%" HWADDR_PRIx "\n", off);
        break;
    }
    return v;
}

static void hci_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    ExynosUfsState *s = opaque;
    uint32_t v = val;

    if (off >= VS_BASE && off < VS_BASE + VS_SIZE) {
        hwaddr o = off - VS_BASE;
        if (o == 0x38) {            /* HCI_VENDOR_SPECIFIC_IS: W1C */
            stl_le_p(s->vs + o, ld32le(s->vs + o) & ~v);
        } else {
            stl_le_p(s->vs + (o & ~3), v);
        }
        return;
    }
    switch (off & ~3) {
    case R_AHIT:     s->ahit = v; break;
    case R_IS:       s->is &= ~v; ufs_update_irq(s); break;
    case R_IE:       s->ie = v & 0x3ffff; ufs_update_irq(s); break;
    case R_HCE:
        if (v & 1) {
            if (!(s->hce & 1)) {
                ufs_reset_regs(s);
                s->hce = 1;
                s->hcs = HCS_UTRLRDY | HCS_UTMRLRDY | HCS_UCRDY;
            }
        } else {
            ufs_reset_regs(s);
        }
        break;
    case R_UTRIACR:  s->utriacr = v; break;
    case R_UTRLBA:   s->utrlba = v & ~0x3ffu; break;
    case R_UTRLBAU:  s->utrlbau = v; break;
    case R_UTRLDBR:
        s->utrldbr |= v & ((1u << NUM_SLOTS) - 1);
        ufs_run_doorbell(s);
        break;
    case R_UTRLCLR: {
        uint32_t clr = s->inverted_clr ? v : ~v;
        s->utrldbr &= ~clr;
        s->utrlcnr &= ~clr;
        break;
    }
    case R_UTRLRSR:  s->utrlrsr = v & 1; if (v & 1) { ufs_run_doorbell(s); } break;
    case R_UTRLCNR:  s->utrlcnr &= ~v; break;
    case R_UTMRLBA:  s->utmrlba = v & ~0x3ffu; break;
    case R_UTMRLBAU: s->utmrlbau = v; break;
    case R_UTMRLDBR:
        s->utmrldbr |= v & ((1u << NUM_TM_SLOTS) - 1);
        if (s->utmrlrsr & 1) {
            for (unsigned i = 0; i < NUM_TM_SLOTS; i++) {
                if (s->utmrldbr & (1u << i)) {
                    ufs_do_tm(s, i);
                }
            }
        }
        break;
    case R_UTMRLCLR: s->utmrldbr &= v; break;
    case R_UTMRLRSR: s->utmrlrsr = v & 1; break;
    case R_UICCMD:   ufs_uic(s, v); break;
    case R_UCMDARG1: s->ucmdarg[0] = v; break;
    case R_UCMDARG2: s->ucmdarg[1] = v; break;
    case R_UCMDARG3: s->ucmdarg[2] = v; break;
    case R_UECPA: case R_UECDL: case R_UECN: case R_UECT: case R_UECDME:
        break;  /* read-only */
    default:
        qemu_log_mask(LOG_UNIMP, "exynos-ufs: write of unknown HCI reg 0x%" HWADDR_PRIx
                      " = 0x%x\n", off, v);
        break;
    }
}

static const MemoryRegionOps hci_ops = {
    .read = hci_read,
    .write = hci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* plain read-as-written RAM-like register blocks */
static uint64_t unipro_read(void *opaque, hwaddr off, unsigned size)
{
    ExynosUfsState *s = opaque;
    return ld32le(s->unipro + (off & ~3));
}

static void unipro_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    ExynosUfsState *s = opaque;
    stl_le_p(s->unipro + (off & ~3), v);
}

static const MemoryRegionOps unipro_ops = {
    .read = unipro_read,
    .write = unipro_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t ufsp_read(void *opaque, hwaddr off, unsigned size)
{
    ExynosUfsState *s = opaque;
    return s->ufsp[(off & (UFSP_SIZE - 1)) / 4];
}

static void ufsp_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    ExynosUfsState *s = opaque;
    s->ufsp[(off & (UFSP_SIZE - 1)) / 4] = v;
}

static const MemoryRegionOps ufsp_ops = {
    .read = ufsp_read,
    .write = ufsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------- QOM */

static void ufs_realize(DeviceState *dev, Error **errp)
{
    ExynosUfsState *s = EXYNOS_UFS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int64_t len = 0;

    s->as = &address_space_memory;
    s->mib = g_hash_table_new(g_direct_hash, g_direct_equal);

    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ;
        if (blk_supports_write_perm(s->blk)) {
            perm |= BLK_PERM_WRITE;
        }
        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        len = blk_getlength(s->blk);
        if (len < 0) {
            error_setg(errp, "exynos-ufs: cannot get drive size");
            return;
        }
        if (!blk_supports_write_perm(s->blk)) {
            qemu_log_mask(LOG_GUEST_ERROR, "exynos-ufs: drive is read-only\n");
        }
        if ((uint64_t)len <= s->lun_offset) {
            error_setg(errp, "exynos-ufs: lun-offset beyond end of drive");
            return;
        }
        uint64_t avail = len - s->lun_offset;
        if (s->lun_size && s->lun_size < avail) {
            avail = s->lun_size;
        }
        s->lun_blocks = avail / UFS_BLOCK;
    } else {
        s->lun_blocks = 0;
    }
    if (!s->blk) {
        /* machine instantiates the device unconditionally: run with an empty (0 block) LUN */
        warn_report("exynos-ufs: no drive= given, LUN 0 has no medium");
    } else if (!s->lun_blocks) {
        error_setg(errp, "exynos-ufs: drive smaller than 4096 bytes");
        return;
    }
    if (s->lun_offset % UFS_BLOCK) {
        error_setg(errp, "exynos-ufs: lun-offset must be a multiple of 4096");
        return;
    }

    memory_region_init_io(&s->mr_hci, OBJECT(dev), &hci_ops, s, "exynos-ufs-hci", HCI_SIZE);
    memory_region_init_io(&s->mr_unipro, OBJECT(dev), &unipro_ops, s, "exynos-ufs-unipro",
                          UNIPRO_SIZE);
    memory_region_init_io(&s->mr_ufsp, OBJECT(dev), &ufsp_ops, s, "exynos-ufs-ufsp", UFSP_SIZE);
    sysbus_init_mmio(sbd, &s->mr_hci);
    sysbus_init_mmio(sbd, &s->mr_unipro);
    sysbus_init_mmio(sbd, &s->mr_ufsp);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property ufs_props[] = {
    DEFINE_PROP_DRIVE("drive", ExynosUfsState, blk),
    DEFINE_PROP_UINT64("lun-offset", ExynosUfsState, lun_offset, 0),
    DEFINE_PROP_UINT64("lun-size", ExynosUfsState, lun_size, 0),
    DEFINE_PROP_BOOL("byte-utrd", ExynosUfsState, byte_utrd, true),
    DEFINE_PROP_BOOL("inverted-clr", ExynosUfsState, inverted_clr, true),
    DEFINE_PROP_BOOL("booted", ExynosUfsState, booted, true),
    DEFINE_PROP_BOOL("start-stopped", ExynosUfsState, start_stopped, false),
    DEFINE_PROP_UINT32("cap", ExynosUfsState, cap, 0x1303ff0f),
};

static void ufs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ufs_realize;
    device_class_set_legacy_reset(dc, ufs_dev_reset);
    device_class_set_props(dc, ufs_props);
    dc->desc = "Exynos 8895 UFS host controller with one LUN";
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo ufs_info = {
    .name = TYPE_EXYNOS_UFS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ExynosUfsState),
    .class_init = ufs_class_init,
};

static void ufs_register_types(void)
{
    type_register_static(&ufs_info);
}

type_init(ufs_register_types)
