/*
 * ESP32-S3 GPIO emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32s3_gpio.h"


static void esp32s3_gpio_init(Object *obj)
{
    /* Set the default value for the property */
    object_property_set_int(obj, "strap_mode", ESP32S3_STRAP_MODE_FLASH_BOOT, &error_fatal);
}

/* ESP32-S3 (soc/esp32s3/gpio_reg.h): 49 pins, the per-pin and matrix registers
 * further up than the ESP32's, and one pending view per bank that both cores
 * read (gpio_ll_get_intr_status ignores core_id), fed by INT_ENA bit 0
 * (PIN bit 13, GPIO_LL_INTR_ENA). */
static const Esp32GpioLayout esp32s3_gpio_layout = {
    .pin_base = 0x74,
    .pin_end = 0x74 + 49 * 4,
    .in_sel_base = 0x154,
    .out_sel_base = 0x554,
    .out_sel_end = 0x554 + 49 * 4,
    .pro_int = 0x5c,
    .pro_int1 = 0x68,
    .app_int = 0,
    .app_int1 = 0,
    .pro_ena = 1 << 13,
    .app_ena = 0,
};

static void esp32s3_gpio_class_init(ObjectClass *klass, void *data)
{
    ESP32_GPIO_CLASS(klass)->layout = &esp32s3_gpio_layout;
}

static const TypeInfo esp32s3_gpio_info = {
    .name = TYPE_ESP32S3_GPIO,
    .parent = TYPE_ESP32_GPIO,
    .instance_size = sizeof(ESP32S3GPIOState),
    .instance_init = esp32s3_gpio_init,
    .class_init = esp32s3_gpio_class_init,
    .class_size = sizeof(ESP32S3GPIOClass),
};

static void esp32s3_gpio_register_types(void)
{
    type_register_static(&esp32s3_gpio_info);
}

type_init(esp32s3_gpio_register_types)
