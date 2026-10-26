/*
 * STM32F1/F2/F4 I2C master controller (PICSimLab / Velxio). See stm32_i2c.h.
 *
 * Drives the QEMU I2C bus from the STM32 HAL's register-level master sequence.
 * A picsimlab_i2c slave on the bus forwards START/SEND/RECV/FINISH/NACK to the
 * Velxio worker. Models the SR1/SR2 flag handshake the HAL polls.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/stm32_i2c.h"
#include "migration/vmstate.h"

#define R_CR1   0x00
#define R_CR2   0x04
#define R_OAR1  0x08
#define R_OAR2  0x0C
#define R_DR    0x10
#define R_SR1   0x14
#define R_SR2   0x18
#define R_CCR   0x1C
#define R_TRISE 0x20

/* CR1 */
#define CR1_PE    (1u << 0)
#define CR1_START (1u << 8)
#define CR1_STOP  (1u << 9)
#define CR1_ACK   (1u << 10)
#define CR1_POS   (1u << 11)  /* ACK position — F1 2-byte receive sequence */
#define CR1_SWRST (1u << 15)

/* SR1 */
#define SR1_SB     (1u << 0)   /* start bit sent */
#define SR1_ADDR   (1u << 1)   /* address sent/matched */
#define SR1_BTF    (1u << 2)   /* byte transfer finished */
#define SR1_STOPF  (1u << 4)
#define SR1_RXNE   (1u << 6)   /* data register not empty (rx) */
#define SR1_TXE    (1u << 7)   /* data register empty (tx) */
#define SR1_AF     (1u << 10)  /* acknowledge failure (NACK) */

/* CR2 interrupt enables */
#define CR2_ITERREN (1u << 8)
#define CR2_ITEVTEN (1u << 9)
#define CR2_ITBUFEN (1u << 10)

/* SR2 */
#define SR2_MSL    (1u << 0)   /* master mode */
#define SR2_BUSY   (1u << 1)
#define SR2_TRA    (1u << 2)   /* transmitter (vs receiver) */

/* STM32duino's Wire drives I2C interrupt-driven (HAL_..._IT), so we must
 * raise the event IRQ whenever a serviceable flag is set and its enable is
 * on. Level-triggered: the ISR clears flags by reading SR1/SR2 or DR. */
static void stm32_i2c_update_irq(Stm32I2CState *s)
{
    int ev = 0;
    if (s->cr2 & CR2_ITEVTEN) {
        if (s->sr1 & (SR1_SB | SR1_ADDR | SR1_STOPF | SR1_BTF)) {
            ev = 1;
        }
        if ((s->cr2 & CR2_ITBUFEN) && (s->sr1 & (SR1_TXE | SR1_RXNE))) {
            ev = 1;
        }
    }
    if ((s->cr2 & CR2_ITERREN) && (s->sr1 & SR1_AF)) {
        ev = 1;
    }
    qemu_set_irq(s->irq, ev);
}

static void stm32_i2c_reset(DeviceState *dev)
{
    Stm32I2CState *s = STM32_I2C(dev);
    if (s->transfer_active) {
        i2c_end_transfer(s->bus);
    }
    s->cr1 = s->cr2 = s->oar1 = s->oar2 = 0;
    s->dr = 0;
    s->sr1 = s->sr2 = 0;
    s->ccr = s->trise = 0;
    s->transfer_active = false;
    s->is_read = false;
    s->addr_phase = false;
    s->rx_shift = 0;
    s->rx_shift_valid = false;
    s->rx_done = false;
}

/* Prime the receive pipeline right after ADDR is cleared in receive mode.
 * Always pulls the first byte into DR (RXNE). For a multi-byte read (ACK or
 * POS set when ADDR clears) it also pulls the second byte into the shift
 * register and raises BTF — the flag the F1 HAL's 2-byte and N>2 receive
 * sequences poll on (the single-byte path never sets POS/ACK, so it stays
 * RXNE-only). */
static void stm32_i2c_rx_prime(Stm32I2CState *s)
{
    if (!s->transfer_active) {
        return;
    }
    s->dr = i2c_recv(s->bus);
    s->sr1 |= SR1_RXNE;
    s->rx_shift_valid = false;
    if (s->cr1 & CR1_POS) {
        /* N==2 sequence (HAL sets POS at ADDR): two bytes are buffered and
         * read out on BTF, then the transfer ends. */
        s->rx_shift = i2c_recv(s->bus);
        s->rx_shift_valid = true;
        s->sr1 |= SR1_BTF;
        s->rx_done = true;
    } else {
        /* N==1 (ACK clear at ADDR) -> one byte, terminal.
         * N>=3 bulk (ACK set)      -> stream one byte at a time via RXNE; the
         * shift/BTF tail is staged when the HAL disables the BUF interrupt. */
        s->rx_done = !(s->cr1 & CR1_ACK);
    }
}

/* Stage the second (shift-register) byte so BTF is raised. Called when the
 * HAL disables its BUF interrupt at the 3-bytes-remaining point of an N>2
 * receive, switching to BTF-driven reads for the final bytes. */
static void stm32_i2c_rx_stage_tail(Stm32I2CState *s)
{
    if (s->is_read && s->transfer_active && (s->sr1 & SR1_RXNE) &&
        !s->rx_shift_valid && !s->rx_done) {
        s->rx_shift = i2c_recv(s->bus);
        s->rx_shift_valid = true;
        s->sr1 |= SR1_BTF;
    }
}

/* Advance the pipeline after the guest reads DR. With a shift byte staged
 * (N==2 or the N>2 tail) it promotes that byte into DR and, until CR1.ACK is
 * cleared, pulls the next byte behind it for the following BTF. In the N>2
 * bulk (single buffer) it just streams the next byte through DR on RXNE. */
static void stm32_i2c_rx_pop(Stm32I2CState *s)
{
    s->sr1 &= ~SR1_BTF;          /* reading DR always clears BTF */
    if (!(s->sr1 & SR1_RXNE)) {
        return;
    }
    s->sr1 &= ~SR1_RXNE;
    if (s->rx_shift_valid) {
        s->dr = s->rx_shift;
        s->rx_shift_valid = false;
        s->sr1 |= SR1_RXNE;
        if (!s->rx_done) {
            s->rx_shift = i2c_recv(s->bus);
            s->rx_shift_valid = true;
            s->sr1 |= SR1_BTF;
            if (!(s->cr1 & CR1_ACK)) {
                s->rx_done = true;  /* freshly-pulled byte is the NACKed last */
            }
        }
    } else if (!s->rx_done && (s->cr1 & CR1_ACK)) {
        s->dr = i2c_recv(s->bus);   /* N>=3 bulk: next byte, RXNE only */
        s->sr1 |= SR1_RXNE;
    }
}

static uint64_t stm32_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Stm32I2CState *s = STM32_I2C(opaque);
    uint32_t r = 0;

    switch (addr) {
    case R_CR1:   return s->cr1;
    case R_CR2:   return s->cr2;
    case R_OAR1:  return s->oar1;
    case R_OAR2:  return s->oar2;
    case R_DR:
        r = s->dr & 0xFF;
        /* Return the front byte, then advance the two-stage RX pipeline. */
        if (s->is_read) {
            stm32_i2c_rx_pop(s);
        } else {
            s->sr1 &= ~SR1_BTF;   /* tx side: reading DR clears BTF */
        }
        stm32_i2c_update_irq(s);
        return r;
    case R_SR1:   return s->sr1;
    case R_SR2:
        /* Reading SR1 then SR2 is the HAL's ADDR-clear sequence. After ADDR
         * clears in receive mode the peripheral begins clocking in data, so
         * prime the first RX byte here. */
        r = s->sr2;
        if (s->sr1 & SR1_ADDR) {
            s->sr1 &= ~SR1_ADDR;
            if (s->is_read) {
                stm32_i2c_rx_prime(s);
            }
        }
        stm32_i2c_update_irq(s);
        return r;
    case R_CCR:   return s->ccr;
    case R_TRISE: return s->trise;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stm32_i2c: bad read 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void stm32_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Stm32I2CState *s = STM32_I2C(opaque);
    uint32_t v = (uint32_t)val;

    switch (addr) {
    case R_CR1:
        s->cr1 = v;
        if (!(v & CR1_PE)) {
            /* Disabled: drop any active transfer. */
            if (s->transfer_active) {
                i2c_end_transfer(s->bus);
                s->transfer_active = false;
            }
            s->sr1 = s->sr2 = 0;
            return;
        }
        if (v & CR1_START) {
            /* Generate (re)START: HAL then writes the address to DR.
             * START is auto-cleared by hardware once SB is set. */
            s->sr1 |= SR1_SB;
            s->sr2 |= SR2_MSL | SR2_BUSY;
            s->addr_phase = true;
            s->cr1 &= ~CR1_START;
        }
        if (v & CR1_STOP) {
            if (s->transfer_active) {
                i2c_end_transfer(s->bus);
                s->transfer_active = false;
            }
            s->sr1 &= ~(SR1_SB | SR1_ADDR);
            s->sr2 &= ~(SR2_MSL | SR2_BUSY | SR2_TRA);
            if (s->is_read) {
                /* RX side: the F1 N=1/N=2 receive sequences set STOP and THEN
                 * read the already-clocked-in bytes from DR (and the shift
                 * register). Keep RXNE/BTF + the staged shift byte intact so
                 * those trailing reads return real data — the next transfer's
                 * address phase clears the pipeline. Clearing them here was a
                 * hang: the HAL's post-STOP `wait RXNE` never completed. */
            } else {
                s->sr1 &= ~(SR1_TXE | SR1_BTF);  /* TX done — nothing to read */
            }
            s->cr1 &= ~CR1_STOP;   /* auto-cleared by hardware */
        }
        stm32_i2c_update_irq(s);
        return;

    case R_CR2: {
        uint32_t old_cr2 = s->cr2;
        s->cr2 = v;
        /* Falling edge of ITBUFEN during a receive == the HAL switching to the
         * BTF tail of an N>2 read. Stage the shift byte so BTF can fire. */
        if ((old_cr2 & CR2_ITBUFEN) && !(v & CR2_ITBUFEN)) {
            stm32_i2c_rx_stage_tail(s);
        }
        stm32_i2c_update_irq(s);
        return;
    }
    case R_OAR1:  s->oar1 = v;  return;
    case R_OAR2:  s->oar2 = v;  return;
    case R_CCR:   s->ccr = v;   return;
    case R_TRISE: s->trise = v; return;

    case R_DR:
        if (s->addr_phase) {
            /* Address byte: bit0 = R/W, bits 7:1 = 7-bit address. */
            uint8_t a7 = (v >> 1) & 0x7F;
            s->is_read = (v & 1) != 0;
            s->addr_phase = false;
            s->sr1 &= ~SR1_SB;
            if (i2c_start_transfer(s->bus, a7, s->is_read) == 0) {
                s->transfer_active = true;
                s->sr1 |= SR1_ADDR;
                /* Clear stale flags + RX pipeline from any prior phase. */
                s->sr1 &= ~(SR1_TXE | SR1_BTF | SR1_RXNE);
                s->rx_shift_valid = false;
                s->rx_done = false;
                if (s->is_read) {
                    s->sr2 &= ~SR2_TRA;
                } else {
                    s->sr2 |= SR2_TRA;
                    s->sr1 |= SR1_TXE;
                }
            } else {
                /* NACK on address. */
                s->sr1 |= SR1_AF;
                s->transfer_active = false;
            }
        } else if (s->transfer_active && !s->is_read) {
            /* Data byte (write). */
            if (i2c_send(s->bus, v & 0xFF) != 0) {
                s->sr1 |= SR1_AF;
            }
            s->sr1 |= SR1_TXE | SR1_BTF;
        }
        stm32_i2c_update_irq(s);
        return;

    case R_SR1:
        /* SR1 is mostly read-only; HAL writes 0 to clear error flags. */
        s->sr1 &= v;
        stm32_i2c_update_irq(s);
        return;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stm32_i2c: bad write 0x%" HWADDR_PRIx "\n", addr);
        return;
    }
}

static const MemoryRegionOps stm32_i2c_ops = {
    .read = stm32_i2c_read,
    .write = stm32_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void stm32_i2c_init(Object *obj)
{
    Stm32I2CState *s = STM32_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &stm32_i2c_ops, s,
                          TYPE_STM32_I2C, 0x400);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(s), "i2c");
}

static const VMStateDescription vmstate_stm32_i2c = {
    .name = TYPE_STM32_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cr1, Stm32I2CState),
        VMSTATE_UINT32(cr2, Stm32I2CState),
        VMSTATE_UINT32(dr, Stm32I2CState),
        VMSTATE_UINT32(sr1, Stm32I2CState),
        VMSTATE_UINT32(sr2, Stm32I2CState),
        VMSTATE_END_OF_LIST()
    }
};

static Property stm32_i2c_properties[] = {
    DEFINE_PROP_UINT32("id", Stm32I2CState, id, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void stm32_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, stm32_i2c_reset);
    dc->vmsd = &vmstate_stm32_i2c;
    device_class_set_props(dc, stm32_i2c_properties);
}

static const TypeInfo stm32_i2c_info = {
    .name          = TYPE_STM32_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32I2CState),
    .instance_init = stm32_i2c_init,
    .class_init    = stm32_i2c_class_init,
};

static void stm32_i2c_register_types(void)
{
    type_register_static(&stm32_i2c_info);
}

type_init(stm32_i2c_register_types)
