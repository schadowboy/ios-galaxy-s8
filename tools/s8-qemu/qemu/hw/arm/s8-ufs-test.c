/*
 * s8-ufs-test: minimal aarch64 test machine for the exynos-ufs device (Agent C).
 * cortex-a53, 256 MiB RAM at 0x80000000, PL011 at 0x09000000, exynos-ufs mapped
 * exactly as on the S8 (HCI 0x11120000, UniPro 0x11110000, UFSP 0x11130000).
 * Run bare-metal code with -device loader,file=x.bin,addr=0x80000000,cpu-num=0 -semihosting.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/char/pl011.h"
#include "hw/arm/machines-qom.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"

static void s8_ufs_test_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    DeviceState *ufs;
    SysBusDevice *sbd;

    qdev_realize_and_unref(DEVICE(cpuobj), NULL, &error_fatal);
    memory_region_add_subregion(sysmem, 0x80000000, machine->ram);
    /* 64 MiB high bank at 0x880000000 like the S8's second DRAM bank (UFS DMA above 4 GiB) */
    MemoryRegion *hi = g_new0(MemoryRegion, 1);
    memory_region_init_ram(hi, NULL, "highbank", 64 * MiB, &error_fatal);
    memory_region_add_subregion(sysmem, 0x880000000ULL, hi);
    pl011_create(0x09000000, NULL, serial_hd(0));

    ufs = qdev_new("exynos-ufs");
    /* the drive property is set from the command line via -global exynos-ufs.drive=ufs0 */
    sbd = SYS_BUS_DEVICE(ufs);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x11120000);
    sysbus_mmio_map(sbd, 1, 0x11110000);
    sysbus_mmio_map(sbd, 2, 0x11130000);
}

static void s8_ufs_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "exynos-ufs test machine (bare-metal)";
    mc->init = s8_ufs_test_init;
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "ram";
}

static const TypeInfo s8_ufs_test_info = {
    .name = MACHINE_TYPE_NAME("s8-ufs-test"),
    .parent = TYPE_MACHINE,
    .interfaces = aarch64_machine_interfaces,
    .class_init = s8_ufs_test_class_init,
};

static void s8_ufs_test_register(void)
{
    type_register_static(&s8_ufs_test_info);
}

type_init(s8_ufs_test_register)
