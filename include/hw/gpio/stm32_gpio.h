/*
 * STM32 GPIO controller (PICSimLab / Velxio)
 *
 * One instance per port (GPIOA..GPIOK). Drives named output IRQ lines on
 * every change of an output pin's level so the PICSimLab bridge
 * (hw/arm/stm32_picsimlab.c) can forward them to the host via
 * picsimlab_write_pin().
 *
 * Supports two register layouts selected by the `variant` property:
 *   STM32_GPIO_VARIANT_F4 - STM32F2/F4 (MODER/OTYPER/.../ODR/BSRR, RM0090)
 *   STM32_GPIO_VARIANT_F1 - STM32F1    (CRL/CRH/IDR/ODR/BSRR/BRR, RM0008)
 * The STM32F1 family (incl. the Blue Pill F103 / VL F100) uses a different
 * map and CRL/CRH nibble-based mode config, so both are handled here.
 *
 * Mirrors the role esp32_gpio.c plays for the ESP32 SoC.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#ifndef HW_GPIO_STM32_GPIO_H
#define HW_GPIO_STM32_GPIO_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32_GPIO "stm32-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32GpioState, STM32_GPIO)

#define STM32_GPIO_NPINS 16

#define STM32_GPIO_VARIANT_F4 0
#define STM32_GPIO_VARIANT_F1 1

/* Named output IRQ arrays exported by the device. The PICSimLab bridge
 * connects these to its pout/pdir handlers, exactly like the ESP32 SoC
 * connects ESP32_GPIOS / ESP32_GPIOS_DIR. */
#define STM32_GPIOS      "stm32-gpios"       /* per-pin output level    */
#define STM32_GPIOS_DIR  "stm32-gpios-dir"   /* per-pin direction       */
#define STM32_GPIOS_PULL "stm32-gpios-pull"  /* per-pin internal pull   */
#define STM32_GPIOS_SYNC "stm32-gpios-sync"  /* IDR read sync (1 line)  */
#define STM32_GPIOS_IN   "stm32-gpios-in"    /* external drive into IDR */

struct Stm32GpioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;

    /* 0 = GPIOA, 1 = GPIOB, ... Used to compute the global linear pin
     * number (port * 16 + pin) the host/worker addresses. */
    uint32_t port_index;
    uint32_t variant;   /* STM32_GPIO_VARIANT_F1 / _F4 */

    /* F4 register file (RM0090 8.4). */
    uint32_t moder;     /* 0x00 mode: 2 bits/pin (01 = output)        */
    uint32_t otyper;    /* 0x04                                       */
    uint32_t ospeedr;   /* 0x08                                       */
    uint32_t pupdr;     /* 0x0C                                       */
    uint32_t afrl;      /* 0x20                                       */
    uint32_t afrh;      /* 0x24                                       */

    /* F1 register file (RM0008 9.2). */
    uint32_t crl;       /* 0x00 config low  (pins 0-7, 4 bits/pin)    */
    uint32_t crh;       /* 0x04 config high (pins 8-15)               */

    /* Common. */
    uint32_t idr;       /* input data (read)                         */
    uint32_t odr;       /* output data                               */
    uint32_t lckr;      /* config lock                               */

    /* External-drive tracking for input pins: ext_driven marks pins a
     * wired peer / the host is actively driving; ext_val holds their level.
     * Pins that are inputs, not externally driven, read their pull-up /
     * pull-down (so idle I2C SDA/SCL read HIGH and Wire bus-recovery passes). */
    uint32_t ext_driven;
    uint32_t ext_val;

    /* Reset values vary per port (e.g. GPIOA/B SWD/JTAG defaults).
     * Set as qdev properties by the SoC. Interpreted per variant:
     *   F4: reset_moder / reset_pupdr / reset_ospeedr
     *   F1: reset_crl / reset_crh */
    uint32_t reset_moder;
    uint32_t reset_pupdr;
    uint32_t reset_ospeedr;
    uint32_t reset_crl;
    uint32_t reset_crh;

    qemu_irq gpios[STM32_GPIO_NPINS];      /* output level out lines   */
    qemu_irq gpios_dir[STM32_GPIO_NPINS];  /* direction out lines      */
    qemu_irq gpios_pull[STM32_GPIO_NPINS]; /* internal-pull out lines  */
    qemu_irq gpios_sync[1];                /* IDR-read sync            */

    /* Last pull level emitted per pin (0=none, 1=up, 2=down), so the pull
     * out line only fires on a real change. -1 = nothing emitted yet (forces
     * the first emit after reset). Runtime cache, not migrated. */
    int8_t emitted_pull[STM32_GPIO_NPINS];
};

#endif /* HW_GPIO_STM32_GPIO_H */
