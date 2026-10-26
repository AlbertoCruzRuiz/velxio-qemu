/*
 * STM32F1/F2/F4 I2C master controller (PICSimLab / Velxio).
 *
 * Models enough of the legacy STM32 I2C peripheral (CR1/CR2/DR/SR1/SR2) for
 * the STM32duino / STM32Cube HAL master polling loops to drive the QEMU I2C
 * bus. A picsimlab_i2c slave attached to this bus forwards transactions to
 * the Velxio worker (the host-side I2C device models).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#ifndef HW_I2C_STM32_I2C_H
#define HW_I2C_STM32_I2C_H

#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_STM32_I2C "stm32-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32I2CState, STM32_I2C)

struct Stm32I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;            /* picsimlab_i2c slave attaches here (set by SoC) */
    uint32_t id;            /* 0 = I2C1, 1 = I2C2, ... (for the host) */

    uint32_t cr1;
    uint32_t cr2;
    uint32_t oar1;
    uint32_t oar2;
    uint32_t dr;           /* last byte read (RX) or staged for read */
    uint32_t sr1;
    uint32_t sr2;
    uint32_t ccr;
    uint32_t trise;

    bool transfer_active;  /* a bus transfer is in progress */
    bool is_read;          /* current transfer direction */
    bool addr_phase;       /* next DR write is the address byte */

    /* Two-stage receive pipeline modelling DR + the internal shift register,
     * so the F1 HAL's BTF-based 2-byte / N>2 read sequences line up. `dr`
     * holds the front byte (read out as RXNE); `rx_shift` is the byte queued
     * behind it (both full => BTF). `rx_done` latches once the NACKed final
     * byte has been pulled from the bus so we stop fetching. */
    uint8_t rx_shift;
    bool rx_shift_valid;
    bool rx_done;
};

#endif /* HW_I2C_STM32_I2C_H */
