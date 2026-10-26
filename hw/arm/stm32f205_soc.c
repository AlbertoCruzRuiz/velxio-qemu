/*
 * STM32F205 SoC
 *
 * Copyright (c) 2014 Alistair Francis <alistair@alistair23.me>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "hw/arm/stm32f205_soc.h"
#include "hw/arm/stm32_picsimlab.h"
#include "hw/misc/stm32_rcc_min.h"
#include "hw/i2c/i2c.h"
#include "hw/ssi/ssi.h"
#include "hw/misc/unimp.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "sysemu/sysemu.h"

#define GPIO_BASE_ADDRESS 0x40020000
#define GPIO_PORT_STRIDE  0x400

/* At the moment only Timer 2 to 5 are modelled */
static const uint32_t timer_addr[STM_NUM_TIMERS] = { 0x40000000, 0x40000400,
    0x40000800, 0x40000C00 };
static const uint32_t usart_addr[STM_NUM_USARTS] = { 0x40011000, 0x40004400,
    0x40004800, 0x40004C00, 0x40005000, 0x40011400 };
static const uint32_t adc_addr[STM_NUM_ADCS] = { 0x40012000, 0x40012100,
    0x40012200 };
static const uint32_t spi_addr[STM_NUM_SPIS] = { 0x40013000, 0x40003800,
    0x40003C00 };
/* I2C1/2/3 share the F4 register map (RM0033). EVT IRQs 31/33/72. */
static const uint32_t i2c_addr[STM_NUM_I2CS] = { 0x40005400, 0x40005800, 0x40005C00 };
static const int i2c_irq[STM_NUM_I2CS] = { 31, 33, 72 };

static const int timer_irq[STM_NUM_TIMERS] = {28, 29, 30, 50};
static const int usart_irq[STM_NUM_USARTS] = {37, 38, 39, 52, 53, 71};
#define ADC_IRQ 18
static const int spi_irq[STM_NUM_SPIS] = {35, 36, 51};

static void stm32f205_soc_initfn(Object *obj)
{
    STM32F205State *s = STM32F205_SOC(obj);
    int i;

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

    object_initialize_child(obj, "syscfg", &s->syscfg, TYPE_STM32F2XX_SYSCFG);

    for (i = 0; i < STM_NUM_USARTS; i++) {
        object_initialize_child(obj, "usart[*]", &s->usart[i],
                                TYPE_STM32F2XX_USART);
    }

    for (i = 0; i < STM_NUM_TIMERS; i++) {
        object_initialize_child(obj, "timer[*]", &s->timer[i],
                                TYPE_STM32F2XX_TIMER);
    }

    s->adc_irqs = OR_IRQ(object_new(TYPE_OR_IRQ));

    for (i = 0; i < STM_NUM_ADCS; i++) {
        object_initialize_child(obj, "adc[*]", &s->adc[i], TYPE_STM32F2XX_ADC);
    }

    for (i = 0; i < STM_NUM_SPIS; i++) {
        object_initialize_child(obj, "spi[*]", &s->spi[i], TYPE_STM32F2XX_SPI);
    }

    for (i = 0; i < STM_NUM_GPIOS; i++) {
        object_initialize_child(obj, "gpio[*]", &s->gpio[i], TYPE_STM32_GPIO);
    }

    for (i = 0; i < STM_NUM_I2CS; i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_STM32_I2C);
    }

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(DEVICE(s), "refclk", NULL, NULL, 0);
}

static void stm32f205_soc_realize(DeviceState *dev_soc, Error **errp)
{
    STM32F205State *s = STM32F205_SOC(dev_soc);
    DeviceState *dev, *armv7m;
    SysBusDevice *busdev;
    int i;

    MemoryRegion *system_memory = get_system_memory();

    /*
     * We use s->refclk internally and only define it with qdev_init_clock_in()
     * so it is correctly parented and not leaked on an init/deinit; it is not
     * intended as an externally exposed clock.
     */
    if (clock_has_source(s->refclk)) {
        error_setg(errp, "refclk clock must not be wired up by the board code");
        return;
    }

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }

    /*
     * TODO: ideally we should model the SoC RCC and its ability to
     * change the sysclk frequency and define different sysclk sources.
     */

    /* The refclk always runs at frequency HCLK / 8 */
    clock_set_mul_div(s->refclk, 8, 1);
    clock_set_source(s->refclk, s->sysclk);

    memory_region_init_rom(&s->flash, OBJECT(dev_soc), "STM32F205.flash",
                           FLASH_SIZE, &error_fatal);
    memory_region_init_alias(&s->flash_alias, OBJECT(dev_soc),
                             "STM32F205.flash.alias", &s->flash, 0, FLASH_SIZE);

    memory_region_add_subregion(system_memory, FLASH_BASE_ADDRESS, &s->flash);
    memory_region_add_subregion(system_memory, 0, &s->flash_alias);

    memory_region_init_ram(&s->sram, NULL, "STM32F205.sram", SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(system_memory, SRAM_BASE_ADDRESS, &s->sram);

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 96);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 4);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m3"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    /* System configuration controller */
    dev = DEVICE(&s->syscfg);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscfg), errp)) {
        return;
    }
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, 0x40013800);

    /* Attach UART (uses USART registers) and USART controllers */
    for (i = 0; i < STM_NUM_USARTS; i++) {
        dev = DEVICE(&(s->usart[i]));
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->usart[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, usart_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, usart_irq[i]));
    }

    /* Timer 2 to 5 */
    for (i = 0; i < STM_NUM_TIMERS; i++) {
        dev = DEVICE(&(s->timer[i]));
        qdev_prop_set_uint64(dev, "clock-frequency", 1000000000);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->timer[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, timer_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, timer_irq[i]));
    }

    /* ADC 1 to 3 */
    object_property_set_int(OBJECT(s->adc_irqs), "num-lines", STM_NUM_ADCS,
                            &error_abort);
    if (!qdev_realize(DEVICE(s->adc_irqs), NULL, errp)) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(s->adc_irqs), 0,
                          qdev_get_gpio_in(armv7m, ADC_IRQ));

    for (i = 0; i < STM_NUM_ADCS; i++) {
        dev = DEVICE(&(s->adc[i]));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, adc_addr[i]);
        sysbus_connect_irq(busdev, 0,
                           qdev_get_gpio_in(DEVICE(s->adc_irqs), i));
    }

    /* SPI 1 to 3 */
    for (i = 0; i < STM_NUM_SPIS; i++) {
        dev = DEVICE(&(s->spi[i]));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, spi_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, spi_irq[i]));
    }
    /* Attach the PICSimLab SPI forwarder to SPI1/SPI2 so the host sees byte
     * transfers (SPI displays: ILI9341, SSD1306-SPI, ePaper). */
    ssi_create_peripheral(s->spi[0].ssi, "picsimlab_spi");
    ssi_create_peripheral(s->spi[1].ssi, "picsimlab_spi");

    /* I2C controllers + PICSimLab forwarder slave on each bus (RM0033). */
    for (i = 0; i < STM_NUM_I2CS; i++) {
        s->i2c[i].id = i;
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[i]), 0, i2c_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c[i]), 0,
                           qdev_get_gpio_in(armv7m, i2c_irq[i]));
        i2c_slave_create_simple(s->i2c[i].bus, "picsimlab_i2c", 0x00);
    }

    /* GPIO ports A..I. Real controllers (hw/gpio/stm32_gpio.c) so the
     * PICSimLab bridge can forward pin writes to the host. Reset values for
     * the MODER/PUPDR registers of ports A and B carry the SWD/JTAG
     * alternate-function defaults (RM0033 section 6.4, same as F4). */
    {
        Stm32GpioState *gpio_ports[STM_NUM_GPIOS];
        for (i = 0; i < STM_NUM_GPIOS; i++) {
            dev = DEVICE(&s->gpio[i]);
            qdev_prop_set_uint32(dev, "port-index", i);
            if (i == 0) {            /* GPIOA */
                qdev_prop_set_uint32(dev, "reset-moder", 0xA8000000);
                qdev_prop_set_uint32(dev, "reset-pupdr", 0x64000000);
            } else if (i == 1) {     /* GPIOB */
                qdev_prop_set_uint32(dev, "reset-moder",   0x00000280);
                qdev_prop_set_uint32(dev, "reset-pupdr",   0x00000100);
                qdev_prop_set_uint32(dev, "reset-ospeedr", 0x000000C0);
            }
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio[i]), 0,
                            GPIO_BASE_ADDRESS + i * GPIO_PORT_STRIDE);
            gpio_ports[i] = &s->gpio[i];
        }
        stm32_picsimlab_connect(gpio_ports, STM_NUM_GPIOS);
    }

    /* DWT cycle counter (CYCCNT) so delayMicroseconds() / Wire recoverBus()
     * don't hang. Mapped at the architectural DWT base in the PPB. */
    {
        DeviceState *dwt = qdev_new("stm32-dwt");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dwt), &error_fatal);
        memory_region_add_subregion_overlap(system_memory, 0xE0001000,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(dwt), 0), 1);
    }

    /* Minimal RCC: ACKs HSE/PLL ready bits so HAL clock init doesn't hang.
     * F2 CFGR is at offset 0x08 (same as F4). */
    {
        DeviceState *rcc = qdev_new(TYPE_STM32_RCC_MIN);
        qdev_prop_set_uint32(rcc, "cfgr-offset", 0x08);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(rcc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(rcc), 0, 0x40023800);
    }

    /* Flash interface as RAM so FLASH_ACR latency readback matches. */
    {
        MemoryRegion *flashif = g_new(MemoryRegion, 1);
        memory_region_init_ram(flashif, OBJECT(dev_soc), "STM32F205.flashif",
                               0x400, &error_fatal);
        memory_region_add_subregion(system_memory, 0x40023C00, flashif);
    }
}

static void stm32f205_soc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = stm32f205_soc_realize;
    /* No vmstate or reset required: device has no internal state */
}

static const TypeInfo stm32f205_soc_info = {
    .name          = TYPE_STM32F205_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STM32F205State),
    .instance_init = stm32f205_soc_initfn,
    .class_init    = stm32f205_soc_class_init,
};

static void stm32f205_soc_types(void)
{
    type_register_static(&stm32f205_soc_info);
}

type_init(stm32f205_soc_types)
