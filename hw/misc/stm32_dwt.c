/*
 * Minimal ARM DWT (Data Watchpoint & Trace) cycle counter for PICSimLab /
 * Velxio. STM32duino enables DWT->CYCCNT and busy-waits on it for
 * delayMicroseconds() and TwoWire::recoverBus(). QEMU's M-profile core does
 * not implement CYCCNT, so it reads a constant and those loops hang forever.
 *
 * We model just CTRL (0x00) and CYCCNT (0x04). CYCCNT advances by a fixed
 * step on every read, which guarantees any "wait until elapsed >= N cycles"
 * loop terminates quickly. Timing is not cycle-accurate (emulation doesn't
 * need it) but always makes forward progress.
 *
 * Mapped at the architectural DWT base 0xE0001000 by the STM32 SoC.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "exec/memory.h"

#define TYPE_STM32_DWT "stm32-dwt"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32DwtState, STM32_DWT)

#define R_DWT_CTRL   0x00
#define R_DWT_CYCCNT 0x04

#define CYCCNT_STEP  0x4000   /* cycles added per read -> delays converge fast */

struct Stm32DwtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t ctrl;
    uint32_t cyccnt;
};

static uint64_t stm32_dwt_read(void *opaque, hwaddr addr, unsigned size)
{
    Stm32DwtState *s = STM32_DWT(opaque);
    switch (addr) {
    case R_DWT_CTRL:
        return s->ctrl;
    case R_DWT_CYCCNT:
        s->cyccnt += CYCCNT_STEP;   /* monotonic forward progress */
        return s->cyccnt;
    default:
        return 0;
    }
}

static void stm32_dwt_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Stm32DwtState *s = STM32_DWT(opaque);
    switch (addr) {
    case R_DWT_CTRL:
        s->ctrl = (uint32_t)val;
        break;
    case R_DWT_CYCCNT:
        s->cyccnt = (uint32_t)val;  /* firmware writes 0 to reset */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps stm32_dwt_ops = {
    .read = stm32_dwt_read,
    .write = stm32_dwt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void stm32_dwt_init(Object *obj)
{
    Stm32DwtState *s = STM32_DWT(obj);
    memory_region_init_io(&s->iomem, obj, &stm32_dwt_ops, s, TYPE_STM32_DWT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const TypeInfo stm32_dwt_info = {
    .name          = TYPE_STM32_DWT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32DwtState),
    .instance_init = stm32_dwt_init,
};

static void stm32_dwt_register_types(void)
{
    type_register_static(&stm32_dwt_info);
}

type_init(stm32_dwt_register_types)
