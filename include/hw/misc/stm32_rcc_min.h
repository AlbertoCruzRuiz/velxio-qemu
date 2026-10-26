/*
 * Minimal STM32 RCC (Reset and Clock Control) for PICSimLab / Velxio.
 *
 * Just enough to let the STM32duino / STM32Cube HAL's SystemClock_Config pass:
 * it mirrors each oscillator/PLL enable bit to its "ready" bit in CR, and
 * mirrors the clock-switch field SW -> SWS in CFGR, so the HAL's busy-wait
 * loops complete instead of hanging on an unimplemented RCC. All other RCC
 * registers behave as plain storage (peripheral clock-enable writes are
 * harmless no-ops for emulation).
 *
 * Variant differs only by the CFGR offset (F1: 0x04, F4: 0x08); the CR bit
 * layout (HSION0/HSIRDY1/HSEON16/HSERDY17/PLLON24/PLLRDY25) is common.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#ifndef HW_MISC_STM32_RCC_MIN_H
#define HW_MISC_STM32_RCC_MIN_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32_RCC_MIN "stm32-rcc-min"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32RccMinState, STM32_RCC_MIN)

#define STM32_RCC_MIN_REGS 0x40   /* 64 bytes of register space backed as RAM */

struct Stm32RccMinState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t cfgr_offset;          /* 0x04 on F1, 0x08 on F4 */
    uint32_t regs[STM32_RCC_MIN_REGS / 4];
};

#endif /* HW_MISC_STM32_RCC_MIN_H */
