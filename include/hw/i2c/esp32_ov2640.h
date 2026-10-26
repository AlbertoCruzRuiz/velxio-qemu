/*
 * OV2640 image sensor SCCB (I²C) device emulation for QEMU.
 *
 * Models the register-level interface that the upstream
 * espressif/esp32-camera driver writes to during esp_camera_init().
 * Pixel data delivery happens via the separate I²S+DMA peripheral
 * (esp32_i2s_cam.c) — this device only owns the register state.
 *
 * Spec / source-of-truth:
 *   - test/test-esp32-cam/autosearch/07_ov2640_sccb_spec.md
 *   - wokwi-libs/esp32-camera/sensors/ov2640.c
 *   - wokwi-libs/esp32-camera/sensors/private_include/ov2640_regs.h
 *   - OmniVision OV2640 datasheet rev 1.6 (Feb 2006)
 *
 * License: GPL v2+ (matches the rest of the QEMU fork).
 */
#ifndef HW_I2C_ESP32_OV2640_H
#define HW_I2C_ESP32_OV2640_H

#include "hw/i2c/i2c.h"

#define TYPE_ESP32_OV2640 "esp32.ov2640"
#define ESP32_OV2640(obj) OBJECT_CHECK(Esp32Ov2640State, (obj), TYPE_ESP32_OV2640)

/* 7-bit slave address per OV2640 datasheet (0x60 write / 0x61 read). */
#define ESP32_OV2640_SLAVE_ADDR 0x30

/* Bank-select register (0xFF) values. */
#define OV2640_BANK_DSP    0x00
#define OV2640_BANK_SENSOR 0x01

/* Sensor-bank chip identification registers (datasheet §4.2.1). */
#define OV2640_REG_PID   0x0A   /* product ID high  -> 0x26 */
#define OV2640_REG_VER   0x0B   /* product ID low   -> 0x42 */
#define OV2640_REG_MIDH  0x1C   /* manuf. ID high   -> 0xA2 */
#define OV2640_REG_MIDL  0x1D   /* manuf. ID low    -> 0x7F */
#define OV2640_REG_COM7  0x12   /* sensor-bank: bit7 = soft reset */

/* Bank-select register address. Same address in either bank. */
#define OV2640_REG_BANK_SEL 0xFF

/* Expected chip-id values (the four magic bytes the driver reads). */
#define OV2640_CHIP_PID  0x26
#define OV2640_CHIP_VER  0x42
#define OV2640_CHIP_MIDH 0xA2
#define OV2640_CHIP_MIDL 0x7F

typedef struct Esp32Ov2640State {
    I2CSlave parent_obj;

    /* Currently selected register bank (DSP=0x00 or SENSOR=0x01). */
    uint8_t current_bank;

    /* Two 256-byte register pages, indexed by [reg]. The chip-id ROM
     * reads come from sensor_regs[]; everything else stores whatever the
     * driver writes so subsequent reads are stable. */
    uint8_t dsp_regs[256];
    uint8_t sensor_regs[256];

    /* SCCB transaction state machine. The driver issues either:
     *   - one byte (register address) followed by I2C_FINISH (= write
     *     of register pointer for a future read)
     *   - two bytes (address + value) followed by FINISH (write).
     * We track state across i2c_send/recv events. */
    uint8_t pending_reg;     /* register address selected by last write */
    bool    have_pending;    /* true after the address byte was sent   */
} Esp32Ov2640State;

#endif /* HW_I2C_ESP32_OV2640_H */
