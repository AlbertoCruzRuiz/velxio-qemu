/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "hw/hw.h"
#include "ui/input.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"

/* Picsimlab GPIO Matrix routing callback — defined in
 * hw/xtensa/esp32_picsimlab.c. Must be declared here because we
 * call it from the GPIO_FUNCx_OUT_SEL_CFG_REG write site. Without
 * this declaration the compiler implicit-declares it as a regular
 * function and the call jumps to the address of the global pointer
 * variable (the storage), not the function the pointer references —
 * causing SIGSEGV at the first write. */
extern void (*picsimlab_gpio_matrix_cb)(int gpio, int signal_id);
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"
#include "sysemu/runstate.h"

/* ESP32 (soc/esp32/gpio_reg.h). INT_ENA bit 2 (PIN bit 15) routes a pin's
 * interrupt to the PRO CPU, bit 0 (PIN bit 13) to the APP CPU
 * (hal/esp32/gpio_ll.h GPIO_LL_*_CPU_INTR_ENA). */
static const Esp32GpioLayout esp32_gpio_layout = {
    .pin_base = 0x88,
    .pin_end = 0x130,
    .in_sel_base = 0x130,
    .out_sel_base = 0x530,
    .out_sel_end = 0x5d0,
    .pro_int = 0x68,
    .pro_int1 = 0x7c,
    .app_int = 0x60,
    .app_int1 = 0x74,
    .pro_ena = 1 << 15,
    .app_ena = 1 << 13,
};

static const Esp32GpioLayout *esp32_gpio_layout_of(Esp32GpioState *s)
{
    const Esp32GpioLayout *l = ESP32_GPIO_GET_CLASS(s)->layout;
    return l ? l : &esp32_gpio_layout;
}


static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;
    switch (addr) {
        case 0x04: //GPIO_OUT_REG
            r = s->gpio_out;
            break;
        case 0x10: //GPIO_OUT1_REG
            r = s->gpio_out1;
            break;
        case 0x20: //GPIO_ENABLE_REG
            r = s->gpio_enable;
            break;
        case 0x2C: //GPIO_ENABLE1_REG
            r = s->gpio_enable1;
            break;
        case 0x38: //A_GPIO_STRAP:
            r = s->strap_mode;
            break;
        case 0x3C: //GPIO_IN_REG
            qemu_set_irq(s->gpios_sync[0], -1); //request sync
            r = s->gpio_in;
            break;
        case 0x40: //GPIO_IN1_REG 
            qemu_set_irq(s->gpios_sync[0], -1); //request sync
            r = s->gpio_in1;
            break;
        case 0x44: //GPIO_STATUS_REG
            r = s->gpio_status;
            break;
        case 0x50: //GPIO_STATUS1_REG
            r = s->gpio_status1;
            break;
        default:
            break;
    }
    const Esp32GpioLayout *l = esp32_gpio_layout_of(s);
    if (l->pro_int && addr == l->pro_int) {
        r = s->gpio_pcpu_int;
    } else if (l->pro_int1 && addr == l->pro_int1) {
        r = s->gpio_pcpu_int1;
    } else if (l->app_int && addr == l->app_int) {
        r = s->gpio_acpu_int;
    } else if (l->app_int1 && addr == l->app_int1) {
        r = s->gpio_acpu_int1;
    } else if (addr >= l->pin_base && addr < l->pin_end) { //GPIO_PINXX_REG
        int n = (addr - l->pin_base) / 4;
        r = s->gpio_pin[n];
    }
    else if (addr >= l->in_sel_base && addr < l->out_sel_base) { //GPIO_FUNCY_IN_SEL_CFG_REG
        int n = (addr - l->in_sel_base) / 4;
        r = s->gpio_in_sel[n];
    }
    else if (addr >= l->out_sel_base && addr < l->out_sel_end) { //GPIO_FUNCX_OUT_SEL_CFG_REG
        int n = (addr - l->out_sel_base) / 4;
        r = s->gpio_out_sel[n];
    }
    return r;
}

//Interrupt type selection
static int get_triggering(int int_type, int oldval, int val) {
    switch (int_type) {
        case 1: //rising edge trigger
            return (val > oldval);
        case 2: //falling edge trigger
            return (val < oldval);
        case 3: //any edge trigger
            return (val != oldval);
        case 4: //low level trigger
            return (val == 0);
        case 5: //high level trigger
            return (val == 1);
    }
    return 0; //GPIO interrupt disable
}

static void set_gpio(void *opaque, int n, int val) {
    Esp32GpioState *s = ESP32_GPIO(opaque);
    const Esp32GpioLayout *l = esp32_gpio_layout_of(s);
    if (n < 32) {
        int oldval = (s->gpio_in >> n) & 1;
        int int_type = (s->gpio_pin[n] >> 7) & 7;
        s->gpio_in &= ~(1 << n);
        s->gpio_in |= (val << n);
        int irq = get_triggering(int_type, oldval, val);
        if (irq && (s->gpio_pin[n] & l->pro_ena)) {  // pro cpu int enable
            qemu_set_irq(s->irq, 1);
            s->gpio_pcpu_int |= (1 << n);
        }
        if (irq && (s->gpio_pin[n] & l->app_ena)) {  // app cpu int enable
            qemu_set_irq(s->irq, 1);
            s->gpio_acpu_int |= (1 << n);
        }
    } else {
        int n1 = n - 32;
        int oldval = (s->gpio_in1 >> n1) & 1;
        int int_type = (s->gpio_pin[n] >> 7) & 7;
        s->gpio_in1 &= ~(1 << n1);
        s->gpio_in1 |= (val << n1);
        int irq = get_triggering(int_type, oldval, val);
        if (irq && (s->gpio_pin[n] & l->pro_ena)) {  // pro cpu int enable
            qemu_set_irq(s->irq, 1);
            s->gpio_pcpu_int1 |= (1 << n1);
        }
        if (irq && (s->gpio_pin[n] & l->app_ena)) {  // app cpu int enable
            qemu_set_irq(s->irq, 1);
            s->gpio_acpu_int1 |= (1 << n1);
        }
    }
}


static void esp32_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned int size) {
    Esp32GpioState *s = ESP32_GPIO(opaque);
    int clearirq;
    uint32_t oldvalue;
    uint32_t oldenable;
    uint32_t oldvalue1;
    uint32_t oldenable1;
    oldvalue   = s->gpio_out;
    oldenable  = s->gpio_enable;
    oldvalue1  = s->gpio_out1;
    oldenable1 = s->gpio_enable1;
    switch (addr) {
        case 0x04: //GPIO_OUT_REG
            s->gpio_out = value;
            break;
        case 0x08: //GPIO_OUT_W1TS_REG
            s->gpio_out |= value;
            break;
        case 0x0C: //GPIO_OUT_W1TC_REG
            s->gpio_out &= ~value;
            break;
        case 0x10: //GPIO_OUT1_REG
            s->gpio_out1 = value;
            break;
        case 0x14: //GPIO_OUT1_W1TS_REG
            s->gpio_out1 |= value;
            break;
        case 0x18: //GPIO_OUT1_W1TC_REG
            s->gpio_out1 &= ~value;
            break;
        case 0x20: //GPIO_ENABLE_REG
            s->gpio_enable = value;
            break;
        case 0x24: //GPIO_ENABLE_W1TS_REG
            s->gpio_enable |= value;
            break;
        case 0x28: //GPIO_ENABLE_W1TC_REG
            s->gpio_enable &= ~value;
            break;
        case 0x2C: //GPIO_ENABLE1_REG
            s->gpio_enable1 = value;
            break;
        case 0x30: //GPIO_ENABLE1_W1TS_REG
            s->gpio_enable1 |= value;
            break;
        case 0x34: //GPIO_ENABLE1_W1TC_REG
            s->gpio_enable1 &= ~value;
            break;
        case 0x38: //A_GPIO_STRAP
            s->strap_mode = value;
            break;
        case 0x44: //GPIO_STATUS_REG
            s->gpio_status = value;
            break;
        case 0x48: //GPIO_STATUS_W1TS_REG 
            s->gpio_status |= value;
            break;
        case 0x4c: //GPIO_STATUS_W1TC_REG 
            clearirq = 1;
            for (int i = 0; i < 32; i++) {
                if ((1 << i) & value) {
                    int int_type = (s->gpio_pin[i] >> 7) & 7;
                    if ((int_type == 4 && !(s->gpio_in & (1 << i))) ||
                        (int_type == 5 && (s->gpio_in & (1 << i))))
                        clearirq = 0;
                }
            }
            if (clearirq) {
                s->gpio_status &= ~value;
                s->gpio_pcpu_int &= ~value;
                s->gpio_acpu_int &= ~value;
                qemu_set_irq(s->irq, 0);
            }
            break;
        case 0x50: //GPIO_STATUS1_REG 
            s->gpio_status1 = value;
            break;
        case 0x54: //GPIO_STATUS1_W1TS_REG 
            s->gpio_status1 |= value;
            break;
        case 0x58: //GPIO_STATUS1_W1TC_REG 
            clearirq = 1;
            for (int i = 0; i < 32 && i + 32 < ARRAY_SIZE(s->gpio_pin); i++) {
                if ((1 << i) & value) {
                    int int_type = (s->gpio_pin[i + 32] >> 7) & 7;
                    if ((int_type == 4 && !(s->gpio_in1 & (1 << i))) ||
                        (int_type == 5 && (s->gpio_in1 & (1 << i))))
                        clearirq = 0;
                }
            }
            if (clearirq) {
                s->gpio_status1 &= ~value;
                s->gpio_pcpu_int1 &= ~value;
                s->gpio_acpu_int1 &= ~value;
                qemu_set_irq(s->irq, 0);
            }
            break;
    }
    const Esp32GpioLayout *l = esp32_gpio_layout_of(s);
    if (addr >= l->pin_base && addr < l->pin_end) { //GPIO_PINXX_REG
        int n = (addr - l->pin_base) / 4;
        s->gpio_pin[n] = value;
    }
    else if (addr >= l->in_sel_base && addr < l->out_sel_base) { //GPIO_FUNCY_IN_SEL_CFG_REG
        int n = (addr - l->in_sel_base) / 4;
        s->gpio_in_sel[n] = value;
        qemu_set_irq(s->gpios_sync[0], (0x1000 | n)); //report in sel cfg change
    }
    else if (addr >= l->out_sel_base && addr < l->out_sel_end) { //GPIO_FUNCX_OUT_SEL_CFG_REG
        int n = (addr - l->out_sel_base) / 4;
        s->gpio_out_sel[n] = value;
        /* Legacy gpios_sync IRQ encodes signal_id in 8 bits; signal
         * 0x100 (clear) is indistinguishable from signal 0. Emit it
         * for back-compat and ALSO call the typed picsimlab callback
         * with the full 9-bit value. */
        qemu_set_irq(s->gpios_sync[0], (0x2000 | ((value & 0xFF) << 8) | (n & 0xFF)));
        picsimlab_gpio_matrix_cb(n, value & 0x1FF);
    }

    //printf("out 0x%04X in 0x%04X enable 0x%04X \n"  ,s->gpio_out, s->gpio_in, s->gpio_enable);

    if ((s->gpio_out != oldvalue)||(s->gpio_enable != oldenable)) {
        uint32_t diff = (s->gpio_out ^ oldvalue) | (s->gpio_enable ^ oldenable);
        for (int i = 0; i < 32; i++) {
            if (((1 << i) & diff)&&((1 << i) & s->gpio_enable)){
                qemu_set_irq(s->gpios[i], (s->gpio_out & (1 << i)) ? 1 : 0);
                if(s->gpio_out & (1 << i)){
                   s->gpio_in |= (1 << i);
                }else{
                   s->gpio_in &= ~(1 << i); 
                } 
            }
        }
    }
    if (s->gpio_enable != oldenable) {
        uint32_t diff =(s->gpio_enable ^ oldenable);
        for (int i = 0; i < 32; i++) {
            if ((1 << i) & diff){
                qemu_set_irq(s->gpios_dir[i], (s->gpio_enable & (1 << i)) ? 1 : 0);
            }
        }
    }

    // GPIO 32-48 (gpio_out1 / gpio_enable1). 8 bits on the ESP32 (32-39),
    // 17 bits on the ESP32-S3 (32-48). Iterating 17 is a no-op for the ESP32
    // because its high bank-1 bits are never enabled.
    if ((s->gpio_out1 != oldvalue1) || (s->gpio_enable1 != oldenable1)) {
        uint32_t diff = (s->gpio_out1 ^ oldvalue1) | (s->gpio_enable1 ^ oldenable1);
        for (int i = 0; i < 17; i++) {
            if (((1 << i) & diff) && ((1 << i) & s->gpio_enable1)) {
                qemu_set_irq(s->gpios1[i], (s->gpio_out1 & (1 << i)) ? 1 : 0);
                if (s->gpio_out1 & (1 << i)) {
                    s->gpio_in1 |= (1 << i);
                } else {
                    s->gpio_in1 &= ~(1 << i);
                }
            }
        }
    }
    if (s->gpio_enable1 != oldenable1) {
        uint32_t diff = (s->gpio_enable1 ^ oldenable1);
        for (int i = 0; i < 17; i++) {
            if ((1 << i) & diff) {
                qemu_set_irq(s->gpios_dir1[i], (s->gpio_enable1 & (1 << i)) ? 1 : 0);
            }
        }
    }
}

static const MemoryRegionOps gpio_ops = {
    .read = esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
}

static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, SYSBUS_DEVICE_GPIO_IRQ, 1);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios,      ESP32_GPIOS,      32);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios_dir,  ESP32_GPIOS_DIR,  32);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios_sync, ESP32_GPIOS_SYNC, 1);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios1,     ESP32_GPIOS1,     17);
    qdev_init_gpio_out_named(DEVICE(s), s->gpios_dir1, ESP32_GPIOS_DIR1, 17);
    qdev_init_gpio_in_named(DEVICE(s), set_gpio, ESP32_GPIOS_IN, 49);
    s->gpio_in = 0x1;
    s->gpio_in1 = 0x8;
}

static Property esp32_gpio_properties[] = {
    /* The strap_mode needs to be explicitly set in the instance init, thus, set
     * the default value to 0. */
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_gpio_reset_hold;
    dc->realize = esp32_gpio_realize;
    device_class_set_props(dc, esp32_gpio_properties);
    ESP32_GPIO_CLASS(klass)->layout = &esp32_gpio_layout;
}

static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
