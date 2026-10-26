#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_reg.h"


#define TYPE_ESP32_SENS "misc.esp32.sens"
#define ESP32_SENS(obj) OBJECT_CHECK(Esp32SensState, (obj), TYPE_ESP32_SENS)

#define ESP32_SENS_NUM_CHANNELS 32

typedef struct Esp32SensState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int touch_sensor[10];
    short ADC_values[32];
    int channel1;
    int channel2;

    /* Periodic waveform LUTs for AC/transient sources, one per channel.
     * When waveform_samples[ch] is non-NULL, reads interpolate the LUT
     * against qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL). Set via
     * qemu_picsimlab_set_apin_waveform(). Ownership: heap-allocated,
     * freed when replaced or on reset. */
    uint16_t *waveform_samples[ESP32_SENS_NUM_CHANNELS];
    int       waveform_len[ESP32_SENS_NUM_CHANNELS];
    uint64_t  waveform_period_ns[ESP32_SENS_NUM_CHANNELS];
    uint64_t  waveform_epoch_ns[ESP32_SENS_NUM_CHANNELS];
} Esp32SensState;

/* Push a periodic waveform LUT for ADC channel `chn`.
 *   samples:    pointer to uint16 LUT (12-bit values, 0..4095)
 *   n:          number of samples
 *   period_ns:  full period of the LUT in nanoseconds
 * Passing samples=NULL or n<=0 clears the waveform and restores the
 * static ADC_values[] path. */
void esp32_sens_set_waveform(Esp32SensState *s, int chn,
                             const uint16_t *samples, int n,
                             uint64_t period_ns);

//#define ESP32_RNG_BASE (DR_REG_WDEV_BASE + 0x144)

