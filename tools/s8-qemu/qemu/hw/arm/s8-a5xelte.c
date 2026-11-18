/*
 * s8-a5xelte: Samsung Galaxy S8 (Exynos 8895) platform model for booting our
 * patched iOS 16.7 XNU kernel (project s8-qemu).
 *
 * 4x Cortex-A53 (MPIDR 0x100..0x103, like the little cluster on the phone),
 * GICv2, ARM generic timer at 26 MHz, PSCI over smc, PL011 console, a tiny
 * Exynos PMU stub, and a RAZ/WI window over the whole Exynos MMIO range.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/pl011.h"
#include "hw/misc/unimp.h"
#include "hw/misc/s8-devices.h"
#include "hw/intc/arm_gic_common.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"
#include "target/arm/multiprocessing.h"

#define TYPE_S8_A5XELTE_MACHINE MACHINE_TYPE_NAME("s8-a5xelte")
OBJECT_DECLARE_SIMPLE_TYPE(S8MachineState, S8_A5XELTE_MACHINE)

/* ---------------------------------------------------------------------- *
 * THE ADDRESS / IRQ TABLE.  Keep every platform address here.
 * IRQ numbers are GIC SPI numbers (INTID = 32 + SPI) as in the Samsung dts.
 * ---------------------------------------------------------------------- */
#define S8_DRAM_LOW_BASE      0x80000000ULL   /* 2 GiB at most             */
#define S8_DRAM_LOW_MAX       (2 * GiB)
#define S8_DRAM_HIGH_BASE     0x880000000ULL  /* XNU's 512 MiB lives here  */
#define S8_DEFAULT_RAM        (2 * GiB + 512 * MiB)

#define S8_MMIO_BASE          0x10000000ULL   /* RAZ/WI window start       */
#define S8_MMIO_SIZE          0x10000000ULL   /* up to 0x20000000          */

#define S8_GICD_BASE          0x10201000ULL
#define S8_GICC_BASE          0x10202000ULL
/* GICH 0x10204000 / GICV 0x10206000 exist on silicon; not modelled (no EL2) */
#define S8_NUM_SPI            608             /* num-irq = 608 + 32        */

#define S8_PMU_BASE           0x16480000ULL   /* Exynos PMU (stub below)   */
#define S8_PMU_SIZE           0x10000

#define S8_UART_BASE          0x13800000ULL   /* PL011, our console        */
#define S8_UART_SPI           385

/* Agent C: UFS. HCI standard regs 0x11120000 (0x200), vendor HCI 0x11121100
 * (0x200), UniPro 0x11110000, protector 0x11130000. SPI 334.
 * Sysbus device type "exynos-ufs"; instantiated below when the type exists.
 * Attach the image with: -global exynos-ufs.drive=ufs0 */
#define S8_UFS_BASE           0x11120000ULL
#define S8_UFS_SPI            334

/* Agent E: framebuffer (the kernel's panel buffer inside low DRAM). */
#define S8_FB_BASE_DEFAULT    0xcc000000ULL
#define S8_FB_SIZE_DEFAULT    (1440ULL * 2960 * 4)
/* Agent E: I2C touch S6SY761 on hsi2c_23 0x108e0000, PMIC/fuel gauge MAX77865
 * on hsi2c 0x10970000, RTC etc. */
#define S8_I2C_TOUCH_BASE     0x108e0000ULL
#define S8_I2C_PMIC_BASE      0x10970000ULL

#define S8_CPU_COUNT_DEFAULT  4
#define S8_CNTFRQ_HZ          26000000
#define S8_MPIDR_BASE         0x0             /* CPU n has MPIDR n (the ADT uses 0..3) */

struct S8MachineState {
    MachineState parent;
    DeviceState *gic;
    uint64_t fb_base;
    uint64_t fb_size;
    bool crop;             /* Agent E: show only the 1125x2436 UI area */
    /* Boot CPU reset state, set by machine properties (0 = untouched). */
    uint64_t entry;
    uint64_t x[4];
};

/* ---------------------------------------------------------------------- *
 * Exynos PMU stub: plain register file, except that the per-CPU
 * CPUn_STATUS (CONFIGURATION + 4) mirrors the low 4 bits of
 * CPUn_CONFIGURATION (0x2000 + n*0x80), which the kernel polls.
 * ---------------------------------------------------------------------- */
#define PMU_CPU_CFG_BASE 0x2000
#define PMU_CPU_CFG_STEP 0x80
#define PMU_CPU_CFG_NUM  8

#define TYPE_S8_PMU "s8-exynos-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(S8PmuState, S8_PMU)

struct S8PmuState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[S8_PMU_SIZE / 4];
};

static uint64_t s8_pmu_read(void *opaque, hwaddr off, unsigned size)
{
    S8PmuState *s = opaque;
    uint32_t v = s->regs[off / 4];

    if (off >= PMU_CPU_CFG_BASE &&
        off < PMU_CPU_CFG_BASE + PMU_CPU_CFG_NUM * PMU_CPU_CFG_STEP &&
        ((off - PMU_CPU_CFG_BASE) % PMU_CPU_CFG_STEP) == 4) {
        v = s->regs[(off - 4) / 4] & 0xf;
    }
    return v;
}

static void s8_pmu_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S8PmuState *s = opaque;

    s->regs[off / 4] = val;
    if (off == 0x400) {                      /* SWRESET */
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else if (off == 0x330c && !(val & (1 << 8))) {  /* PS_HOLD released */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

/* ---- generic register-file stub with forced read values (keys, DECON) ---- */
typedef struct { hwaddr off; uint32_t set; uint32_t force; } S8StubInit;
/* read = (stored | set) , and bits in `force` are always 1 */
#define TYPE_S8_STUB "s8-reg-stub"
OBJECT_DECLARE_SIMPLE_TYPE(S8StubState, S8_STUB)
struct S8StubState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    const S8StubInit *init;
    uint32_t regs[0x1000 / 4];
};

static uint64_t s8_stub_read(void *opaque, hwaddr off, unsigned size)
{
    S8StubState *s = opaque;
    uint32_t v = s->regs[off / 4];

    for (const S8StubInit *i = s->init; i && i->off != (hwaddr)-1; i++) {
        if (i->off == (off & ~3)) {
            v = (v & ~i->set) | i->set | i->force;
        }
    }
    return v;
}

static void s8_stub_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S8StubState *s = opaque;

    s->regs[off / 4] = val;
}

static const MemoryRegionOps s8_stub_ops = {
    .read = s8_stub_read,
    .write = s8_stub_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void s8_stub_init(Object *obj)
{
    S8StubState *s = S8_STUB(obj);

    memory_region_init_io(&s->iomem, obj, &s8_stub_ops, s, "s8-regstub", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const TypeInfo s8_stub_info = {
    .name = TYPE_S8_STUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S8StubState),
    .instance_init = s8_stub_init,
};

static void s8_stub_create(hwaddr base, const S8StubInit *init)
{
    DeviceState *d = qdev_new(TYPE_S8_STUB);

    S8_STUB(d)->init = init;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, base);
}

/* 0x164b0000 keys: +0x24,+0x64 read 0xff; +0x44 bit0 always high */
static const S8StubInit s8_keys_init[] = {
    { 0x24, 0xff, 0 }, { 0x64, 0xff, 0 }, { 0x44, 0, 1 }, { -1, 0, 0 },
};
/* DECON 0x12860000: +0x4c bit 13 reads 1 (Agent E may replace with a real
 * display device that provides this bit) */
static const S8StubInit s8_decon_init[] = {
    { 0x4c, 0, 1 << 13 }, { -1, 0, 0 },
};

static const MemoryRegionOps s8_pmu_ops = {
    .read = s8_pmu_read,
    .write = s8_pmu_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void s8_pmu_init(Object *obj)
{
    S8PmuState *s = S8_PMU(obj);

    memory_region_init_io(&s->iomem, obj, &s8_pmu_ops, s, "s8-pmu",
                          S8_PMU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const TypeInfo s8_pmu_info = {
    .name = TYPE_S8_PMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S8PmuState),
    .instance_init = s8_pmu_init,
};

/* ---------------------------------------------------------------------- */

/* Applied after the CPUs reset (registered later, so it runs later). */
static void s8_boot_cpu_reset(void *opaque)
{
    S8MachineState *s = opaque;
    ARMCPU *cpu = ARM_CPU(first_cpu);
    CPUARMState *env = &cpu->env;
    int i;

    if (s->entry) {
        cpu_set_pc(first_cpu, s->entry);
    }
    for (i = 0; i < 4; i++) {
        if (s->x[i]) {
            env->xregs[i] = s->x[i];
        }
    }
}

static void s8_init(MachineState *machine)
{
    S8MachineState *s = S8_A5XELTE_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    unsigned smp_cpus = machine->smp.cpus;
    SysBusDevice *gicbusdev;
    qemu_irq pic[S8_NUM_SPI];
    unsigned i;

    if (smp_cpus > 4) {
        error_report("s8-a5xelte: at most 4 CPUs (little cluster) supported");
        exit(1);
    }

    /* ---- CPUs ---- */
    for (i = 0; i < smp_cpus; i++) {
        Object *cpuobj = object_new(machine->cpu_type);

        object_property_set_int(cpuobj, "mp-affinity", S8_MPIDR_BASE + i,
                                &error_fatal);
        object_property_set_int(cpuobj, "cntfrq", S8_CNTFRQ_HZ, &error_fatal);
        object_property_set_bool(cpuobj, "has_el3", false, NULL);
        if (object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", false, NULL);
        }
        object_property_set_int(cpuobj, "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_fatal);
        if (i > 0) {
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_fatal);
        }
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);
        qdev_realize_and_unref(DEVICE(cpuobj), NULL, &error_fatal);
    }

    /* ---- RAM: low bank (<= 2 GiB) + high bank at 0x880000000 ---- */
    {
        uint64_t total = machine->ram_size;
        uint64_t low = MIN(total, S8_DRAM_LOW_MAX);
        uint64_t high = total - low;
        MemoryRegion *lo = g_new(MemoryRegion, 1);

        memory_region_init_alias(lo, NULL, "s8.dram.low", machine->ram, 0, low);
        memory_region_add_subregion(sysmem, S8_DRAM_LOW_BASE, lo);
        if (high) {
            MemoryRegion *hi = g_new(MemoryRegion, 1);

            memory_region_init_alias(hi, NULL, "s8.dram.high", machine->ram,
                                     low, high);
            memory_region_add_subregion(sysmem, S8_DRAM_HIGH_BASE, hi);
        }
    }

    /* ---- RAZ/WI Exynos MMIO window (lowest priority; real devices win) ---- */
    create_unimplemented_device("exynos-mmio", S8_MMIO_BASE, S8_MMIO_SIZE);

    /* ---- GICv2 ---- */
    s->gic = qdev_new("arm_gic");
    qdev_prop_set_uint32(s->gic, "revision", 2);
    /* QEMU makes the GICD target registers RAZ/WI on a uniprocessor GIC, but the kernel reads a real target mask from ITARGETSR0 (pe_fiq.c:313),
     * and the real S8 GIC has 8 CPU interfaces: always model at least 4 (unused interfaces stay unconnected). */
    unsigned gic_cpus = smp_cpus < 4 ? 4 : smp_cpus;
    qdev_prop_set_uint32(s->gic, "num-cpu", gic_cpus);
    qdev_prop_set_uint32(s->gic, "num-irq", S8_NUM_SPI + 32);
    qdev_prop_set_bit(s->gic, "has-security-extensions", false);
    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, S8_GICD_BASE);
    sysbus_mmio_map(gicbusdev, 1, S8_GICC_BASE);

    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int intidbase = S8_NUM_SPI + i * GIC_INTERNAL;
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,   /* PPI 14, INTID 30 */
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,     /* PPI 11, INTID 27 */
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
            [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
            [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
        };

        for (unsigned irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(s->gic,
                                                   intidbase + timer_irq[irq]));
        }
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(s->gic, intidbase +
                                                     VIRTUAL_PMU_IRQ));
        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + gic_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * gic_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * gic_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
    for (i = 0; i < S8_NUM_SPI; i++) {
        pic[i] = qdev_get_gpio_in(s->gic, i);
    }

    /* ---- PMU stub ---- */
    {
        DeviceState *d = qdev_new(TYPE_S8_PMU);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, S8_PMU_BASE);
    }

    /* ---- required register stubs (platform.md) ---- */
    s8_stub_create(0x164b0000ULL, s8_keys_init);
    s8_stub_create(0x12860000ULL, s8_decon_init);

    /* ---- Spare PL011 (the kernel has no UART; console = fb + RAM ring) ---- */
    pl011_create(S8_UART_BASE, pic[S8_UART_SPI], serial_hd(0));

    /* ==== EXTENSION HOOK (Agent C): UFS ==== */
    if (object_class_by_name("exynos-ufs")) {
        DeviceState *d = qdev_new("exynos-ufs");

        sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, S8_UFS_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(d), 0, pic[S8_UFS_SPI]);
    }

    /* ==== EXTENSION HOOK (Agent E): framebuffer, I2C touch/PMIC stubs ====
     * s->fb_base / s->fb_size are the "fb-base"/"fb-size" properties; the
     * devices go here (S8_I2C_TOUCH_BASE, S8_I2C_PMIC_BASE above).
     */
    s8_e_devices_create(sysmem, s->fb_base, s->fb_size, s->crop);

    qemu_register_reset(s8_boot_cpu_reset, s);
}

static bool s8_get_crop(Object *obj, Error **errp)
{
    return S8_A5XELTE_MACHINE(obj)->crop;
}

static void s8_set_crop(Object *obj, bool v, Error **errp)
{
    S8_A5XELTE_MACHINE(obj)->crop = v;
}

static void s8_instance_init(Object *obj)
{
    S8MachineState *s = S8_A5XELTE_MACHINE(obj);

    object_property_add_bool(obj, "crop", s8_get_crop, s8_set_crop);
    s->fb_base = S8_FB_BASE_DEFAULT;
    s->fb_size = S8_FB_SIZE_DEFAULT;
    object_property_add_uint64_ptr(obj, "fb-base", &s->fb_base,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "fb-size", &s->fb_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "entry", &s->entry,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "x0", &s->x[0], OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "x1", &s->x[1], OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "x2", &s->x[2], OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "x3", &s->x[3], OBJ_PROP_FLAG_READWRITE);
}

static void s8_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Samsung Galaxy S8 (Exynos 8895 little cluster) for s8-qemu";
    mc->init = s8_init;
    mc->max_cpus = 4;
    mc->default_cpus = 4;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = S8_DEFAULT_RAM;
    mc->default_ram_id = "s8.ram";
    mc->block_default_type = IF_NONE;
    mc->no_cdrom = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;
}

static const TypeInfo s8_machine_info = {
    .name = TYPE_S8_A5XELTE_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(S8MachineState),
    .instance_init = s8_instance_init,
    .class_init = s8_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_TARGET_AARCH64_MACHINE },
        { }
    },
};

static void s8_machine_register(void)
{
    type_register_static(&s8_machine_info);
    type_register_static(&s8_pmu_info);
    type_register_static(&s8_stub_info);
}

type_init(s8_machine_register)
