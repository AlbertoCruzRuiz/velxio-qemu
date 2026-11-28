#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"

#define TYPE_ESP32_GPIO "esp32.gpio"
#define ESP32_GPIO(obj)             OBJECT_CHECK(Esp32GpioState, (obj), TYPE_ESP32_GPIO)
#define ESP32_GPIO_GET_CLASS(obj)   OBJECT_GET_CLASS(Esp32GpioClass, obj, TYPE_ESP32_GPIO)
#define ESP32_GPIO_CLASS(klass)     OBJECT_CLASS_CHECK(Esp32GpioClass, klass, TYPE_ESP32_GPIO)

REG32(GPIO_STRAP, 0x0038)

#define ESP32_STRAP_MODE_FLASH_BOOT 0x12
#define ESP32_STRAP_MODE_UART_BOOT  0x0f

typedef struct Esp32GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t gpio_out;
    uint32_t gpio_out1;
    uint32_t strap_mode;
    uint32_t gpio_in;
    uint32_t gpio_in1;
    uint32_t gpio_status;
    uint32_t gpio_status1;
    uint32_t gpio_pcpu_int;
    uint32_t gpio_pcpu_int1;
    uint32_t gpio_acpu_int;
    uint32_t gpio_acpu_int1;
    uint32_t gpio_enable;
    uint32_t gpio_enable1;
    /* Sized for the widest chip on this device (ESP32-S3: GPIO0..48, 49 pins).
     * The plain ESP32 (GPIO0..39) only ever touches the low 40 entries / low 8
     * bank-1 lines, so the extra slots are inert for it — this just prevents an
     * out-of-bounds index when S3 firmware configures GPIO40..48 (gpio_pin[n] /
     * gpio_out_sel[n] are indexed by pin number). */
    uint32_t gpio_pin[49];
    uint32_t gpio_in_sel[256];
    uint32_t gpio_out_sel[49];
    qemu_irq gpios[32];
    qemu_irq gpios_dir[32];
    qemu_irq gpios_sync[1];
    qemu_irq gpios1[17];
    qemu_irq gpios_dir1[17];
} Esp32GpioState;

#define ESP32_GPIOS      "esp32_gpios"
#define ESP32_GPIOS_IN   "esp32_gpios_in"
#define ESP32_GPIOS_DIR  "esp32_gpios_dir"
#define ESP32_GPIOS_SYNC "esp32_gpios_sync"
#define ESP32_GPIOS1     "esp32_gpios1"
#define ESP32_GPIOS_DIR1 "esp32_gpios_dir1"

/* Where a chip keeps the GPIO registers that differ between the ESP32 and the
 * ESP32-S3. The banks (OUT/ENABLE/IN/STATUS and their *1 twins) sit at the same
 * offsets on both; the per-pin, matrix and pending-interrupt registers do not.
 * The S3 device inherited the ESP32 layout until 2026-10-31, so every S3 write
 * to GPIO_FUNCn_OUT_SEL_CFG landed on pin n+9, every GPIO_PINn_REG on pin n-5,
 * and GPIO_PCPU_INT read nothing: no I2C device answered under the S3 machine
 * (velxio#405) and no GPIO interrupt reached an ISR. */
typedef struct Esp32GpioLayout {
    hwaddr pin_base;        /* GPIO_PIN0_REG */
    hwaddr pin_end;         /* one past the last GPIO_PINn_REG */
    hwaddr in_sel_base;     /* GPIO_FUNC0_IN_SEL_CFG_REG */
    hwaddr out_sel_base;    /* GPIO_FUNC0_OUT_SEL_CFG_REG */
    hwaddr out_sel_end;     /* one past the last GPIO_FUNCn_OUT_SEL_CFG_REG */
    /* Pending-interrupt views (0 = the chip has no such register) and the
     * GPIO_PINn_REG bit that routes a pin's interrupt to each. */
    hwaddr pro_int;         /* GPIO_PCPU_INT_REG */
    hwaddr pro_int1;        /* GPIO_PCPU_INT1_REG */
    hwaddr app_int;         /* GPIO_ACPU_INT_REG */
    hwaddr app_int1;        /* GPIO_ACPU_INT1_REG */
    uint32_t pro_ena;
    uint32_t app_ena;
} Esp32GpioLayout;

typedef struct Esp32GpioClass {
    SysBusDeviceClass parent_class;
    const Esp32GpioLayout *layout;
} Esp32GpioClass;
