/*
 * STM32 PICSimLab / Velxio host bridge.
 *
 * Connects the STM32 GPIO ports' output/direction IRQ lines to the
 * picsimlab_write_pin / picsimlab_dir_pin host callbacks, and exposes the
 * qemu_picsimlab_* C API consumed by the Velxio worker over ctypes
 * (libqemu-arm). Mirrors the role hw/xtensa/esp32_picsimlab.c plays for
 * the ESP32 SoC.
 *
 * Pin numbering is linear: global_pin = port_index * 16 + pin, so PA0 = 0,
 * PA15 = 15, PB0 = 16, ... The host pinmap addresses pins by this number.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#ifndef HW_ARM_STM32_PICSIMLAB_H
#define HW_ARM_STM32_PICSIMLAB_H

#include "qom/object.h"
#include "hw/gpio/stm32_gpio.h"

/*
 * Wire the host bridge to the SoC's GPIO ports. Call once from the SoC
 * realize after every port in `ports[0..nports-1]` has been realized.
 * Allocates the pout/pdir/psync IRQs, connects them to each port's
 * STM32_GPIOS / STM32_GPIOS_DIR / STM32_GPIOS_SYNC lines, and records the
 * per-pin input lines so qemu_picsimlab_set_pin() can drive them.
 */
void stm32_picsimlab_connect(Stm32GpioState *ports[], int nports);

#endif /* HW_ARM_STM32_PICSIMLAB_H */
