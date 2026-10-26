/*
 * Minimal STM32 RCC for PICSimLab / Velxio. See stm32_rcc_min.h.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/misc/stm32_rcc_min.h"
#include "migration/vmstate.h"

#define R_CR 0x00

/* CR bit layout (common F1/F4). */
#define CR_HSION   (1u << 0)
#define CR_HSIRDY  (1u << 1)
#define CR_HSEON   (1u << 16)
#define CR_HSERDY  (1u << 17)
#define CR_PLLON   (1u << 24)
#define CR_PLLRDY  (1u << 25)
#define CR_PLL2ON  (1u << 26)  /* F1 connectivity / F4 PLLI2S */
#define CR_PLL2RDY (1u << 27)

static uint64_t stm32_rcc_min_read(void *opaque, hwaddr addr, unsigned size)
{
    Stm32RccMinState *s = STM32_RCC_MIN(opaque);
    if (addr + 4 > STM32_RCC_MIN_REGS) {
        return 0;
    }
    uint32_t v = s->regs[addr / 4];

    if (addr == R_CR) {
        /* Mirror each enable bit to its ready bit so HAL wait loops pass. */
        if (v & CR_HSION)  v |= CR_HSIRDY;
        if (v & CR_HSEON)  v |= CR_HSERDY;
        if (v & CR_PLLON)  v |= CR_PLLRDY;
        if (v & CR_PLL2ON) v |= CR_PLL2RDY;
    } else if (addr == s->cfgr_offset) {
        /* Mirror SW[1:0] -> SWS[3:2] so the clock-switch wait completes. */
        v = (v & ~0xCu) | ((v & 0x3u) << 2);
    }
    return v;
}

static void stm32_rcc_min_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Stm32RccMinState *s = STM32_RCC_MIN(opaque);
    if (addr + 4 > STM32_RCC_MIN_REGS) {
        return;
    }
    s->regs[addr / 4] = (uint32_t)val;
}

static const MemoryRegionOps stm32_rcc_min_ops = {
    .read = stm32_rcc_min_read,
    .write = stm32_rcc_min_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void stm32_rcc_min_reset(DeviceState *dev)
{
    Stm32RccMinState *s = STM32_RCC_MIN(dev);
    memset(s->regs, 0, sizeof(s->regs));
    /* Reset: HSI on and ready (matches real silicon CR reset). */
    s->regs[R_CR / 4] = CR_HSION | CR_HSIRDY;
}

static void stm32_rcc_min_init(Object *obj)
{
    Stm32RccMinState *s = STM32_RCC_MIN(obj);
    memory_region_init_io(&s->iomem, obj, &stm32_rcc_min_ops, s,
                          TYPE_STM32_RCC_MIN, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_stm32_rcc_min = {
    .name = TYPE_STM32_RCC_MIN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32RccMinState, STM32_RCC_MIN_REGS / 4),
        VMSTATE_END_OF_LIST()
    }
};

static Property stm32_rcc_min_properties[] = {
    DEFINE_PROP_UINT32("cfgr-offset", Stm32RccMinState, cfgr_offset, 0x04),
    DEFINE_PROP_END_OF_LIST(),
};

static void stm32_rcc_min_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, stm32_rcc_min_reset);
    dc->vmsd = &vmstate_stm32_rcc_min;
    device_class_set_props(dc, stm32_rcc_min_properties);
}

static const TypeInfo stm32_rcc_min_info = {
    .name          = TYPE_STM32_RCC_MIN,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32RccMinState),
    .instance_init = stm32_rcc_min_init,
    .class_init    = stm32_rcc_min_class_init,
};

static void stm32_rcc_min_register_types(void)
{
    type_register_static(&stm32_rcc_min_info);
}

type_init(stm32_rcc_min_register_types)
