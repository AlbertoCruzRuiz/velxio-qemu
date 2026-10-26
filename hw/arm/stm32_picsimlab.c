/*
 * STM32 PICSimLab / Velxio host bridge.
 *
 * Provides the qemu_picsimlab_* C API for the arm-softmmu target, the same
 * ABI the Velxio worker (stm32_worker.py) loads from libqemu-arm over
 * ctypes. Connects each STM32 GPIO port's output/direction IRQ lines to the
 * host's picsimlab_write_pin / picsimlab_dir_pin callbacks.
 *
 * Modeled on hw/xtensa/esp32_picsimlab.c. STM32 has no GPIO matrix, LEDC or
 * RMT, so those callbacks are accepted (for ABI compatibility with the
 * shared worker) but never fired here. I2C/SPI/UART hookups land in later
 * patches; the function pointers exist so the worker can register them
 * without a struct-layout mismatch.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/gpio/stm32_gpio.h"
#include "hw/arm/stm32_picsimlab.h"

/* Maximum linear pins we track (11 ports * 16 = 176; round up). */
#define STM32_PICSIMLAB_MAX_PINS 192

/*
 * Callback struct shared with the host. MUST match, field-for-field, the
 * _CallbacksT ctypes.Structure in the worker. The trailing fields
 * (rmt_event, gpio_matrix_cb) are unused on STM32 but kept so the same
 * worker struct binds cleanly.
 */
typedef struct {
    void (*picsimlab_write_pin)(int pin, int value);
    void (*picsimlab_dir_pin)(int pin, int value);
    int (*picsimlab_i2c_event)(const uint8_t id, const uint8_t addr,
                               const uint16_t event);
    uint8_t (*picsimlab_spi_event)(const uint8_t id, const uint16_t event);
    void (*picsimlab_uart_tx_event)(const uint8_t id, const uint8_t value);
    const short int *pinmap;
    void (*picsimlab_rmt_event)(const uint8_t channel, const uint32_t config0,
                                const uint32_t value);
    void (*picsimlab_gpio_matrix_cb)(int gpio, int signal_id);
    /* Appended after the ESP32-compatible prefix: per-pin internal pull
     * (0=none, 1=up, 2=down). Optional — older workers leave it NULL. The
     * STM32 worker's _CallbacksT must append a matching trailing field. */
    void (*picsimlab_pull_pin)(int pin, int value);
} callbacks_t;

/* Exported API (consumed by the worker via ctypes). */
void qemu_picsimlab_register_callbacks(void *arg);
void qemu_picsimlab_set_pin(int pin, int value);
uint32_t *qemu_picsimlab_get_internals(int cfg);

/* Default no-op handlers so an un-registered library never SIGSEGVs. */
static void ph_write(int pin, int value) { (void)pin; (void)value; }
static void ph_dir(int pin, int value) { (void)pin; (void)value; }
static int ph_i2c(const uint8_t id, const uint8_t addr, const uint16_t ev)
{ (void)id; (void)addr; (void)ev; return 0; }
static uint8_t ph_spi(const uint8_t id, const uint16_t ev)
{ (void)id; (void)ev; return 0xFF; }
static void ph_uart(const uint8_t id, const uint8_t value)
{ (void)id; (void)value; }
static void ph_rmt(const uint8_t ch, const uint32_t c0, const uint32_t v)
{ (void)ch; (void)c0; (void)v; }
static void ph_matrix(int gpio, int sig) { (void)gpio; (void)sig; }
static void ph_pull(int pin, int value) { (void)pin; (void)value; }

void (*picsimlab_write_pin)(int pin, int value) = ph_write;
void (*picsimlab_dir_pin)(int pin, int value) = ph_dir;
int (*picsimlab_i2c_event)(const uint8_t, const uint8_t, const uint16_t) = ph_i2c;
uint8_t (*picsimlab_spi_event)(const uint8_t, const uint16_t) = ph_spi;
void (*picsimlab_uart_tx_event)(const uint8_t, const uint8_t) = ph_uart;
void (*picsimlab_rmt_event)(const uint8_t, const uint32_t, const uint32_t) = ph_rmt;
void (*picsimlab_gpio_matrix_cb)(int, int) = ph_matrix;
void (*picsimlab_pull_pin)(int pin, int value) = ph_pull;
static const short int *pinmap = NULL;

/* Per-pin input lines into the GPIO ports' IDR (indexed by global pin). */
static qemu_irq pin_in_irq[STM32_PICSIMLAB_MAX_PINS];
static qemu_irq *pout_irq;   /* fan-in for all ports' output lines */
static qemu_irq *pdir_irq;   /* fan-in for all ports' direction lines */
static qemu_irq *ppull_irq;  /* fan-in for all ports' pull lines */
static qemu_irq *psync_irq;  /* IDR-read sync */
static int connected_pins;

void qemu_picsimlab_register_callbacks(void *arg)
{
    const callbacks_t *cb = (const callbacks_t *)arg;

    picsimlab_write_pin = cb->picsimlab_write_pin;
    picsimlab_dir_pin = cb->picsimlab_dir_pin;
    picsimlab_i2c_event = cb->picsimlab_i2c_event;
    picsimlab_spi_event = cb->picsimlab_spi_event;
    picsimlab_uart_tx_event = cb->picsimlab_uart_tx_event;
    pinmap = cb->pinmap;
    if (cb->picsimlab_rmt_event) {
        picsimlab_rmt_event = cb->picsimlab_rmt_event;
    }
    if (cb->picsimlab_gpio_matrix_cb) {
        picsimlab_gpio_matrix_cb = cb->picsimlab_gpio_matrix_cb;
    }
    if (cb->picsimlab_pull_pin) {
        picsimlab_pull_pin = cb->picsimlab_pull_pin;
    }
}

/* Drive a host-side input level onto a guest input pin (global numbering). */
void qemu_picsimlab_set_pin(int pin, int value)
{
    if (pin < 0 || pin >= connected_pins) {
        return;
    }
    if (value) {
        qemu_irq_raise(pin_in_irq[pin]);
    } else {
        qemu_irq_lower(pin_in_irq[pin]);
    }
}

/* STM32 exposes no GPIO matrix / LEDC internals; return NULL. The worker's
 * STM32 path must not call this. */
uint32_t *qemu_picsimlab_get_internals(int cfg)
{
    (void)cfg;
    return NULL;
}

/* IRQ fan-in handlers: n is the global pin number. */
static void pout_handler(void *opaque, int n, int level)
{
    (void)opaque;
    picsimlab_write_pin(n, level);
}

static void pdir_handler(void *opaque, int n, int dir)
{
    (void)opaque;
    picsimlab_dir_pin(n, dir);
}

/* n = global pin number, level = pull code (0=none, 1=up, 2=down). */
static void ppull_handler(void *opaque, int n, int level)
{
    (void)opaque;
    picsimlab_pull_pin(n, level);
}

static void psync_handler(void *opaque, int n, int dir)
{
    (void)opaque; (void)n;
    /* dir == -1 means "IDR read sync" (see stm32_gpio_read). */
    picsimlab_dir_pin(-1, dir);
}

void stm32_picsimlab_connect(Stm32GpioState *ports[], int nports)
{
    int total = nports * STM32_GPIO_NPINS;
    if (total > STM32_PICSIMLAB_MAX_PINS) {
        total = STM32_PICSIMLAB_MAX_PINS;
    }
    connected_pins = total;

    pout_irq = qemu_allocate_irqs(pout_handler, NULL, total);
    pdir_irq = qemu_allocate_irqs(pdir_handler, NULL, total);
    ppull_irq = qemu_allocate_irqs(ppull_handler, NULL, total);
    psync_irq = qemu_allocate_irqs(psync_handler, NULL, 1);

    for (int p = 0; p < nports; p++) {
        DeviceState *dev = DEVICE(ports[p]);
        for (int pin = 0; pin < STM32_GPIO_NPINS; pin++) {
            int g = p * STM32_GPIO_NPINS + pin;
            if (g >= total) {
                break;
            }
            qdev_connect_gpio_out_named(dev, STM32_GPIOS, pin, pout_irq[g]);
            qdev_connect_gpio_out_named(dev, STM32_GPIOS_DIR, pin, pdir_irq[g]);
            qdev_connect_gpio_out_named(dev, STM32_GPIOS_PULL, pin, ppull_irq[g]);
            pin_in_irq[g] = qdev_get_gpio_in_named(dev, STM32_GPIOS_IN, pin);
        }
        /* All ports share the single host sync line. */
        qdev_connect_gpio_out_named(dev, STM32_GPIOS_SYNC, 0, psync_irq[0]);
    }
}
