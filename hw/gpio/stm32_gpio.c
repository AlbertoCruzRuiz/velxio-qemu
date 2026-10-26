/*
 * STM32 GPIO controller (PICSimLab / Velxio)
 *
 * One instance per port. Models two register layouts (selected by the
 * `variant` property): STM32F1 (CRL/CRH/IDR/ODR/BSRR/BRR, RM0008) and
 * STM32F2/F4 (MODER/.../ODR/BSRR, RM0090). On every change of an output
 * pin's level it drives a named output IRQ line so hw/arm/stm32_picsimlab.c
 * can forward it to the host (Velxio frontend) via picsimlab_write_pin /
 * picsimlab_dir_pin.
 *
 * Modeled on hw/gpio/esp32_gpio.c, adapted to the STM32 per-port layout.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/stm32_gpio.h"
#include "migration/vmstate.h"

/* F4 (RM0090) register offsets. */
#define F4_MODER   0x00
#define F4_OTYPER  0x04
#define F4_OSPEEDR 0x08
#define F4_PUPDR   0x0C
#define F4_IDR     0x10
#define F4_ODR     0x14
#define F4_BSRR    0x18
#define F4_LCKR    0x1C
#define F4_AFRL    0x20
#define F4_AFRH    0x24

/* F1 (RM0008) register offsets. */
#define F1_CRL     0x00
#define F1_CRH     0x04
#define F1_IDR     0x08
#define F1_ODR     0x0C
#define F1_BSRR    0x10
#define F1_BRR     0x14
#define F1_LCKR    0x18

/* Is `pin` configured as an output? F4: MODER field == 01. F1: the 2-bit
 * MODE sub-field of the pin's 4-bit CRL/CRH nibble is non-zero. */
static int pin_is_output(Stm32GpioState *s, int pin)
{
    if (s->variant == STM32_GPIO_VARIANT_F1) {
        uint32_t cr = (pin < 8) ? s->crl : s->crh;
        uint32_t mode = (cr >> ((pin & 7) * 4)) & 0x3;  /* MODE[1:0] */
        return mode != 0;   /* 00 = input, !=0 = output */
    }
    return ((s->moder >> (pin * 2)) & 0x3) == 0x1;
}

/* Pull resistor on an INPUT `pin`: +1 = pull-up, -1 = pull-down, 0 = floating.
 * F4: PUPDR field (01=up, 10=down). F1: input mode CNF=10 with ODR bit
 * selecting up(1)/down(0). */
static int pin_pull(Stm32GpioState *s, int pin)
{
    if (s->variant == STM32_GPIO_VARIANT_F1) {
        uint32_t cr = (pin < 8) ? s->crl : s->crh;
        uint32_t cnf = (cr >> ((pin & 7) * 4 + 2)) & 0x3;
        if (cnf == 0x2) {  /* input with pull-up/pull-down */
            return ((s->odr >> pin) & 1) ? 1 : -1;
        }
        return 0;
    }
    uint32_t p = (s->pupdr >> (pin * 2)) & 0x3;
    return p == 0x1 ? 1 : (p == 0x2 ? -1 : 0);
}

/* Effective IDR seen by the guest: output pins read their driven ODR level,
 * externally-driven inputs read the wire level, other inputs read their
 * pull resistor (idle). */
static uint32_t stm32_gpio_effective_idr(Stm32GpioState *s)
{
    uint32_t v = 0;
    for (int pin = 0; pin < STM32_GPIO_NPINS; pin++) {
        int bit;
        if (pin_is_output(s, pin)) {
            bit = (s->odr >> pin) & 1;
        } else if (s->ext_driven & (1u << pin)) {
            bit = (s->ext_val >> pin) & 1;
        } else {
            bit = pin_pull(s, pin) > 0 ? 1 : 0;
        }
        v |= (uint32_t)bit << pin;
    }
    return v;
}

/* Did `pin`'s direction config change between old and new config words? */
static int pin_dir_changed(Stm32GpioState *s, int pin,
                           uint32_t old_a, uint32_t old_b)
{
    if (s->variant == STM32_GPIO_VARIANT_F1) {
        uint32_t cr_new = (pin < 8) ? s->crl : s->crh;
        uint32_t cr_old = (pin < 8) ? old_a : old_b;
        uint32_t shift = (pin & 7) * 4;
        return ((cr_new >> shift) & 0xF) != ((cr_old >> shift) & 0xF);
    }
    return ((s->moder >> (pin * 2)) & 0x3) != ((old_a >> (pin * 2)) & 0x3);
}

/* Propagate ODR -> output pins. `mode_dirty` flags that a config register
 * changed too (so newly-output pins get emitted even if ODR is unchanged). */
static void stm32_gpio_update_outputs(Stm32GpioState *s, uint32_t old_odr,
                                      bool mode_dirty)
{
    uint32_t diff = s->odr ^ old_odr;

    for (int pin = 0; pin < STM32_GPIO_NPINS; pin++) {
        if (!mode_dirty && !(diff & (1u << pin))) {
            continue;
        }
        if (pin_is_output(s, pin)) {
            int level = (s->odr >> pin) & 1;
            qemu_set_irq(s->gpios[pin], level);
            if (level) {
                s->idr |= (1u << pin);
            } else {
                s->idr &= ~(1u << pin);
            }
        }
    }
}

static void stm32_gpio_update_dirs(Stm32GpioState *s, uint32_t old_a,
                                   uint32_t old_b)
{
    for (int pin = 0; pin < STM32_GPIO_NPINS; pin++) {
        if (pin_dir_changed(s, pin, old_a, old_b)) {
            qemu_set_irq(s->gpios_dir[pin], pin_is_output(s, pin) ? 1 : 0);
        }
    }
}

/* Emit the internal pull (0=none, 1=up, 2=down) on the pull-out line for any
 * INPUT pin whose pull changed since the last emit, so the host can stamp the
 * matching weak resistor in its circuit netlist. Output pins always report 0
 * (their level is already driven via the gpios output lines). Cheap to run on
 * every register write — only fires the IRQ on an actual change. The pull on
 * F1 depends on CRL/CRH (CNF) AND ODR (the up/down selector), and on F4 on
 * MODER + PUPDR, so the simplest correct trigger is "after every write". */
static void stm32_gpio_update_pulls(Stm32GpioState *s)
{
    for (int pin = 0; pin < STM32_GPIO_NPINS; pin++) {
        int enc;
        if (pin_is_output(s, pin)) {
            enc = 0;
        } else {
            int p = pin_pull(s, pin);
            enc = p > 0 ? 1 : (p < 0 ? 2 : 0);
        }
        if (s->emitted_pull[pin] != enc) {
            s->emitted_pull[pin] = (int8_t)enc;
            qemu_set_irq(s->gpios_pull[pin], enc);
        }
    }
}

static uint64_t stm32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Stm32GpioState *s = STM32_GPIO(opaque);

    if (s->variant == STM32_GPIO_VARIANT_F1) {
        switch (addr) {
        case F1_CRL:  return s->crl;
        case F1_CRH:  return s->crh;
        case F1_IDR:
            qemu_set_irq(s->gpios_sync[0], -1);
            return s->idr & 0xFFFF;
        case F1_ODR:  return s->odr & 0xFFFF;
        case F1_LCKR: return s->lckr;
        case F1_BSRR: return 0;   /* write-only */
        case F1_BRR:  return 0;   /* write-only */
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "stm32_gpio(f1): bad read 0x%" HWADDR_PRIx "\n", addr);
            return 0;
        }
    }

    switch (addr) {
    case F4_MODER:   return s->moder;
    case F4_OTYPER:  return s->otyper;
    case F4_OSPEEDR: return s->ospeedr;
    case F4_PUPDR:   return s->pupdr;
    case F4_IDR:
        qemu_set_irq(s->gpios_sync[0], -1);
        return stm32_gpio_effective_idr(s) & 0xFFFF;
    case F4_ODR:     return s->odr & 0xFFFF;
    case F4_LCKR:    return s->lckr;
    case F4_AFRL:    return s->afrl;
    case F4_AFRH:    return s->afrh;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stm32_gpio(f4): bad read 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void stm32_gpio_write_f1(Stm32GpioState *s, hwaddr addr, uint32_t value)
{
    uint32_t old_odr = s->odr;
    uint32_t old_crl = s->crl, old_crh = s->crh;

    switch (addr) {
    case F1_CRL:
        s->crl = value;
        stm32_gpio_update_dirs(s, old_crl, old_crh);
        stm32_gpio_update_outputs(s, old_odr, true);
        return;
    case F1_CRH:
        s->crh = value;
        stm32_gpio_update_dirs(s, old_crl, old_crh);
        stm32_gpio_update_outputs(s, old_odr, true);
        return;
    case F1_ODR:
        s->odr = value & 0xFFFF;
        stm32_gpio_update_outputs(s, old_odr, false);
        return;
    case F1_BSRR:
        /* Low 16 set, high 16 reset. BSx has priority over BRx. */
        s->odr &= ~((value >> 16) & 0xFFFF);
        s->odr |= (value & 0xFFFF);
        stm32_gpio_update_outputs(s, old_odr, false);
        return;
    case F1_BRR:
        s->odr &= ~(value & 0xFFFF);
        stm32_gpio_update_outputs(s, old_odr, false);
        return;
    case F1_LCKR:
        s->lckr = value;
        return;
    case F1_IDR:
        return;   /* read-only */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stm32_gpio(f1): bad write 0x%" HWADDR_PRIx "\n", addr);
        return;
    }
}

static void stm32_gpio_write_f4(Stm32GpioState *s, hwaddr addr, uint32_t value)
{
    uint32_t old_odr = s->odr;
    uint32_t old_moder = s->moder;

    switch (addr) {
    case F4_MODER:
        s->moder = value;
        stm32_gpio_update_dirs(s, old_moder, old_moder);
        stm32_gpio_update_outputs(s, old_odr, true);
        return;
    case F4_OTYPER:  s->otyper = value;  return;
    case F4_OSPEEDR: s->ospeedr = value; return;
    case F4_PUPDR:   s->pupdr = value;   return;
    case F4_ODR:
        s->odr = value & 0xFFFF;
        stm32_gpio_update_outputs(s, old_odr, false);
        return;
    case F4_BSRR:
        s->odr &= ~((value >> 16) & 0xFFFF);
        s->odr |= (value & 0xFFFF);
        stm32_gpio_update_outputs(s, old_odr, false);
        return;
    case F4_LCKR:    s->lckr = value;    return;
    case F4_AFRL:    s->afrl = value;    return;
    case F4_AFRH:    s->afrh = value;    return;
    case F4_IDR:     return;             /* read-only */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stm32_gpio(f4): bad write 0x%" HWADDR_PRIx "\n", addr);
        return;
    }
}

static void stm32_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned int size)
{
    Stm32GpioState *s = STM32_GPIO(opaque);
    if (s->variant == STM32_GPIO_VARIANT_F1) {
        stm32_gpio_write_f1(s, addr, (uint32_t)value);
    } else {
        stm32_gpio_write_f4(s, addr, (uint32_t)value);
    }
    /* Any register write can change a pin's input pull (PUPDR/MODER on F4;
     * CRL/CRH/ODR on F1); re-evaluate and emit changes to the host. */
    stm32_gpio_update_pulls(s);
}

/* External drive into an input pin (from the PICSimLab bridge / a wired
 * peripheral). `pin` 0..15, level 0/1. Updates IDR. */
static void stm32_gpio_set_input(void *opaque, int pin, int level)
{
    Stm32GpioState *s = STM32_GPIO(opaque);
    if (pin < 0 || pin >= STM32_GPIO_NPINS) {
        return;
    }
    s->ext_driven |= (1u << pin);
    if (level) {
        s->ext_val |= (1u << pin);
        s->idr |= (1u << pin);
    } else {
        s->ext_val &= ~(1u << pin);
        s->idr &= ~(1u << pin);
    }
}

static const MemoryRegionOps stm32_gpio_ops = {
    .read = stm32_gpio_read,
    .write = stm32_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void stm32_gpio_reset(DeviceState *dev)
{
    Stm32GpioState *s = STM32_GPIO(dev);

    s->idr  = 0;
    s->odr  = 0;
    s->lckr = 0;
    s->ext_driven = 0;
    s->ext_val = 0;

    /* Force the pull-out lines to re-emit on the firmware's first config
     * write after reset (the guest re-programs PUPDR/CRL/CRH in init). */
    for (int pin = 0; pin < STM32_GPIO_NPINS; pin++) {
        s->emitted_pull[pin] = -1;
    }

    if (s->variant == STM32_GPIO_VARIANT_F1) {
        /* RM0008: CRL/CRH reset = 0x44444444 (all inputs, floating). */
        s->crl = s->reset_crl ? s->reset_crl : 0x44444444;
        s->crh = s->reset_crh ? s->reset_crh : 0x44444444;
    } else {
        s->moder   = s->reset_moder;
        s->otyper  = 0;
        s->ospeedr = s->reset_ospeedr;
        s->pupdr   = s->reset_pupdr;
        s->afrl    = 0;
        s->afrh    = 0;
    }
}

static void stm32_gpio_init(Object *obj)
{
    Stm32GpioState *s = STM32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &stm32_gpio_ops, s,
                          TYPE_STM32_GPIO, 0x400);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_out_named(DEVICE(s), s->gpios,      STM32_GPIOS,      STM32_GPIO_NPINS);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios_dir,  STM32_GPIOS_DIR,  STM32_GPIO_NPINS);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios_pull, STM32_GPIOS_PULL, STM32_GPIO_NPINS);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios_sync, STM32_GPIOS_SYNC, 1);
    qdev_init_gpio_in_named(DEVICE(s), stm32_gpio_set_input, STM32_GPIOS_IN,
                            STM32_GPIO_NPINS);
}

static const VMStateDescription vmstate_stm32_gpio = {
    .name = TYPE_STM32_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(moder,   Stm32GpioState),
        VMSTATE_UINT32(otyper,  Stm32GpioState),
        VMSTATE_UINT32(ospeedr, Stm32GpioState),
        VMSTATE_UINT32(pupdr,   Stm32GpioState),
        VMSTATE_UINT32(afrl,    Stm32GpioState),
        VMSTATE_UINT32(afrh,    Stm32GpioState),
        VMSTATE_UINT32(crl,     Stm32GpioState),
        VMSTATE_UINT32(crh,     Stm32GpioState),
        VMSTATE_UINT32(idr,     Stm32GpioState),
        VMSTATE_UINT32(odr,     Stm32GpioState),
        VMSTATE_UINT32(lckr,    Stm32GpioState),
        VMSTATE_END_OF_LIST()
    }
};

static Property stm32_gpio_properties[] = {
    DEFINE_PROP_UINT32("port-index",    Stm32GpioState, port_index,    0),
    DEFINE_PROP_UINT32("variant",       Stm32GpioState, variant,       STM32_GPIO_VARIANT_F4),
    DEFINE_PROP_UINT32("reset-moder",   Stm32GpioState, reset_moder,   0),
    DEFINE_PROP_UINT32("reset-pupdr",   Stm32GpioState, reset_pupdr,   0),
    DEFINE_PROP_UINT32("reset-ospeedr", Stm32GpioState, reset_ospeedr, 0),
    DEFINE_PROP_UINT32("reset-crl",     Stm32GpioState, reset_crl,     0),
    DEFINE_PROP_UINT32("reset-crh",     Stm32GpioState, reset_crh,     0),
    DEFINE_PROP_END_OF_LIST(),
};

static void stm32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, stm32_gpio_reset);
    dc->vmsd = &vmstate_stm32_gpio;
    device_class_set_props(dc, stm32_gpio_properties);
}

static const TypeInfo stm32_gpio_info = {
    .name          = TYPE_STM32_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32GpioState),
    .instance_init = stm32_gpio_init,
    .class_init    = stm32_gpio_class_init,
};

static void stm32_gpio_register_types(void)
{
    type_register_static(&stm32_gpio_info);
}

type_init(stm32_gpio_register_types)
