/*
 * OV2640 SCCB device emulation.
 *
 * SCCB is a single-master, single-slave bus that's electrically and
 * timing-compatible with I²C. From QEMU's POV we hang off an I2CBus
 * just like any other I2CSlave. The protocol differences are at the
 * physical layer (the real chip ignores ACK/NACK in some phases) and
 * don't matter to us — the upstream esp32-camera driver always treats
 * it as plain hardware I²C.
 *
 * Implementation strategy:
 *   1. Maintain two 256-byte register tables (DSP and SENSOR banks).
 *   2. On TX of the first byte: latch it as the register pointer.
 *   3. On TX of subsequent bytes (still in the same I2C_START..STOP
 *      transaction): write to the current bank's pointer slot, with
 *      auto-increment, AND apply side effects (bank switch, soft reset).
 *   4. On RX: return whatever's in the current bank at the pointer,
 *      then auto-increment.
 *
 * Side effects we model:
 *   - BANK_SEL (0xFF) writes update current_bank.
 *   - COM7 (0x12) bit-7 in sensor bank triggers soft reset: zero both
 *     banks, restore chip-id ROM, set bank=SENSOR.
 *
 * What we DON'T model:
 *   - Read-modify-write semantics on AGC/AEC autobright registers.
 *     The driver always writes specific values and never reads back
 *     the same regs, so storing-as-written is faithful.
 *   - The 10-ms post-reset settling time. QEMU's I²C bus is instant.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/esp32_ov2640.h"
#include "migration/vmstate.h"

/* Apply the chip-id ROM into the sensor bank table. Called on cold
 * reset and on soft reset (COM7 bit-7). */
static void esp32_ov2640_apply_chipid(Esp32Ov2640State *s)
{
    s->sensor_regs[OV2640_REG_PID]  = OV2640_CHIP_PID;
    s->sensor_regs[OV2640_REG_VER]  = OV2640_CHIP_VER;
    s->sensor_regs[OV2640_REG_MIDH] = OV2640_CHIP_MIDH;
    s->sensor_regs[OV2640_REG_MIDL] = OV2640_CHIP_MIDL;
}

static void esp32_ov2640_soft_reset(Esp32Ov2640State *s)
{
    memset(s->dsp_regs, 0, sizeof(s->dsp_regs));
    memset(s->sensor_regs, 0, sizeof(s->sensor_regs));
    esp32_ov2640_apply_chipid(s);
    s->current_bank   = OV2640_BANK_SENSOR;
    s->pending_reg    = 0;
    s->have_pending   = false;
}

static void esp32_ov2640_handle_write(Esp32Ov2640State *s,
                                      uint8_t reg, uint8_t val)
{
    /* Bank-select register lives in BOTH banks at the same offset. */
    if (reg == OV2640_REG_BANK_SEL) {
        s->current_bank = val & 0x01;
        return;
    }

    if (s->current_bank == OV2640_BANK_SENSOR) {
        /* Soft reset trigger: COM7 bit-7. */
        if (reg == OV2640_REG_COM7 && (val & 0x80)) {
            esp32_ov2640_soft_reset(s);
            return;
        }
        s->sensor_regs[reg] = val;
    } else {
        s->dsp_regs[reg] = val;
    }
}

static uint8_t esp32_ov2640_handle_read(Esp32Ov2640State *s, uint8_t reg)
{
    if (reg == OV2640_REG_BANK_SEL) {
        /* The datasheet says bank-sel reads the same bit7 we wrote;
         * being faithful here costs nothing. */
        return s->current_bank;
    }
    return (s->current_bank == OV2640_BANK_SENSOR)
        ? s->sensor_regs[reg]
        : s->dsp_regs[reg];
}

/* ── I²C slave callbacks ───────────────────────────────────────────── */

static int esp32_ov2640_event(I2CSlave *i2c, enum i2c_event event)
{
    Esp32Ov2640State *s = ESP32_OV2640(i2c);
    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        /* New transaction. The pending_reg from a prior "write of
         * register pointer" survives across the START_RECV — that's
         * the standard I²C "set pointer, then read" pattern. */
        if (event == I2C_START_SEND) {
            /* The driver may issue a fresh write — clear the pending
             * flag so the next byte is treated as the register
             * pointer again. The pointer value itself is reset by
             * the first SEND below. */
            s->have_pending = false;
        }
        break;
    case I2C_FINISH:
    case I2C_NACK:
        /* End of transaction; nothing to do. */
        break;
    default:
        break;
    }
    return 0;
}

static int esp32_ov2640_send(I2CSlave *i2c, uint8_t data)
{
    Esp32Ov2640State *s = ESP32_OV2640(i2c);

    if (!s->have_pending) {
        /* First byte after START → register pointer. */
        s->pending_reg  = data;
        s->have_pending = true;
        return 0;
    }
    /* Subsequent bytes → write data with auto-increment. The auto-
     * increment behaviour is what the OV2640 manual calls "burst
     * write"; ll_cam.c relies on it for the long init sequences. */
    esp32_ov2640_handle_write(s, s->pending_reg, data);
    s->pending_reg++;
    return 0;
}

static uint8_t esp32_ov2640_recv(I2CSlave *i2c)
{
    Esp32Ov2640State *s = ESP32_OV2640(i2c);
    uint8_t value = esp32_ov2640_handle_read(s, s->pending_reg);
    s->pending_reg++;
    return value;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static void esp32_ov2640_realize(DeviceState *dev, Error **errp)
{
    Esp32Ov2640State *s = ESP32_OV2640(dev);
    esp32_ov2640_soft_reset(s);
}

static void esp32_ov2640_reset(DeviceState *dev)
{
    Esp32Ov2640State *s = ESP32_OV2640(dev);
    esp32_ov2640_soft_reset(s);
}

static const VMStateDescription esp32_ov2640_vmstate = {
    .name = TYPE_ESP32_OV2640,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Esp32Ov2640State),
        VMSTATE_UINT8(current_bank, Esp32Ov2640State),
        VMSTATE_UINT8_ARRAY(dsp_regs, Esp32Ov2640State, 256),
        VMSTATE_UINT8_ARRAY(sensor_regs, Esp32Ov2640State, 256),
        VMSTATE_UINT8(pending_reg, Esp32Ov2640State),
        VMSTATE_BOOL(have_pending, Esp32Ov2640State),
        VMSTATE_END_OF_LIST()
    }
};

static void esp32_ov2640_class_init(ObjectClass *klass, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    I2CSlaveClass  *sc = I2C_SLAVE_CLASS(klass);

    dc->realize  = esp32_ov2640_realize;
    device_class_set_legacy_reset(dc, esp32_ov2640_reset);
    dc->vmsd     = &esp32_ov2640_vmstate;
    dc->desc     = "OmniVision OV2640 image sensor (SCCB only)";

    sc->event = esp32_ov2640_event;
    sc->send  = esp32_ov2640_send;
    sc->recv  = esp32_ov2640_recv;
}

static const TypeInfo esp32_ov2640_info = {
    .name          = TYPE_ESP32_OV2640,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Esp32Ov2640State),
    .class_init    = esp32_ov2640_class_init,
};

static void esp32_ov2640_register_types(void)
{
    type_register_static(&esp32_ov2640_info);
}

type_init(esp32_ov2640_register_types)
