/*
 * ESP32 Analog Sensors peripheral
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
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_sens.h"


/* Linearly interpolate the waveform LUT at the current virtual clock.
 * Returns a 12-bit sample (0..4095). Caller guarantees waveform_samples
 * and waveform_len > 0 and waveform_period_ns > 0. */
static uint16_t esp32_sens_sample_waveform(Esp32SensState *s, int ch)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t period = s->waveform_period_ns[ch];
    if (period == 0) {
        return s->ADC_values[ch];
    }
    uint64_t dt = (now - s->waveform_epoch_ns[ch]) % period;
    int n = s->waveform_len[ch];
    if (n <= 0) return s->ADC_values[ch];
    if (n == 1) return s->waveform_samples[ch][0];

    /* Map dt ∈ [0, period) → fractional index ∈ [0, n-1]. */
    double t_frac = (double) dt / (double) period * (double) (n - 1);
    int lo = (int) t_frac;
    if (lo >= n - 1) lo = n - 2;
    int hi = lo + 1;
    double a = t_frac - (double) lo;
    double v = (double) s->waveform_samples[ch][lo] * (1.0 - a)
             + (double) s->waveform_samples[ch][hi] * a;
    int raw = (int) (v + 0.5);
    if (raw < 0) raw = 0;
    if (raw > 0xFFFF) raw = 0xFFFF;
    return (uint16_t) raw;
}

static uint64_t esp32_sens_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);
    uint32_t r = 0;
    int ch;
    uint16_t sample;
    //printf("esp32_sens_read 0x%lx\n",addr);
    switch(addr) {
    case 0x54://SENS_MEAS1_START_SAR
        ch = s->channel1;
        if (ch >= 0 && ch < ESP32_SENS_NUM_CHANNELS && s->waveform_samples[ch]) {
            sample = esp32_sens_sample_waveform(s, ch);
        } else {
            sample = (uint16_t) s->ADC_values[ch];
        }
        return 0x10000 + sample;
    case 0x84://SENS_SAR_TOUCH_CTRL2_REG
	return (1<<10);
    case 0x94://SENS_MEAS2_START_SAR
        ch = s->channel2 + 8;
        if (ch >= 0 && ch < ESP32_SENS_NUM_CHANNELS && s->waveform_samples[ch]) {
            sample = esp32_sens_sample_waveform(s, ch);
        } else {
            sample = (uint16_t) s->ADC_values[ch];
        }
        return 0x10000 + sample;
    }
// 2=gpio2, 3=gpio15, 4=gpio14(13?), 5=gpio12, 7=gpio27, 8=gpio33, 9=gpio32
// land +/- 300: 2=12166,31743 15=13618,31585 13=15277,31743 12=16798,31743 27=18388,3071 33=13791,2993 32=12166,2914
// port	       : 2=1417,12132  15=1339,13791  13=1496,15312  12=1417,16694  27=30010,18388 33=30088,13860 32=30010,12201
    if(addr>=0x70 && addr<0x84) {
	int n1=((addr-0x70)/4)*2;
        return ((1500-s->touch_sensor[n1]+rand()%20)<<16) | (1500-s->touch_sensor[n1+1]+rand()%20);
    }

//    qemu_guest_getrandom_nofail(&r, sizeof(r));
    return r;
}

static int bitpos( int value )
{
    switch( value )
    {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        case 16: return 4;
        case 32: return 5;
        case 64: return 6;
        case 128: return 7;
        case 256: return 8;
        case 512: return 9;
        case 1024: return 10;
        case 2048: return 11;
        case 4096: return 12;
        case 8192: return 13;
        case 16384: return 14;
        case 32768: return 15;
        case 65536: return 16;
       default      : return 0;
    }
}

static void esp32_sens_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size) {
    int cval;
    Esp32SensState *s = ESP32_SENS(opaque);
   // printf("esp32_sens_write 0x%lx 0x%lx\n",addr, value);
    switch(addr) {
    case 0x54://SENS_MEAS1_START_SAR
     cval = (value & 0x7FF80000) >> 19;
     if(cval){
       s->channel1 = bitpos(cval);
     }
    break;
    case 0x94://SENS_MEAS2_START_SAR
     cval = (value & 0x7FF80000) >> 19;
     if(cval){
       s->channel2 = bitpos(cval);
     }
    break;
    }
}

void esp32_sens_set_waveform(Esp32SensState *s, int chn,
                             const uint16_t *samples, int n,
                             uint64_t period_ns)
{
    if (!s || chn < 0 || chn >= ESP32_SENS_NUM_CHANNELS) {
        return;
    }
    /* Free any previous buffer */
    if (s->waveform_samples[chn]) {
        g_free(s->waveform_samples[chn]);
        s->waveform_samples[chn] = NULL;
        s->waveform_len[chn] = 0;
        s->waveform_period_ns[chn] = 0;
    }
    if (samples && n > 0 && period_ns > 0) {
        s->waveform_samples[chn] = g_malloc(sizeof(uint16_t) * n);
        memcpy(s->waveform_samples[chn], samples, sizeof(uint16_t) * n);
        s->waveform_len[chn] = n;
        s->waveform_period_ns[chn] = period_ns;
        s->waveform_epoch_ns[chn] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
}

static const MemoryRegionOps esp32_sens_ops = {
    .read =  esp32_sens_read,
    .write = esp32_sens_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_sens_init(Object *obj)
{
    Esp32SensState *s = ESP32_SENS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_sens_ops, s,
                          TYPE_ESP32_SENS, 0x400);
    sysbus_init_mmio(sbd, &s->iomem);

    memset(s->touch_sensor, 0, sizeof(s->touch_sensor));
    memset(s->ADC_values, 0, sizeof(s->ADC_values));
    s->channel1 = 0;
    s->channel2 = 0;
    memset(s->waveform_samples, 0, sizeof(s->waveform_samples));
    memset(s->waveform_len, 0, sizeof(s->waveform_len));
    memset(s->waveform_period_ns, 0, sizeof(s->waveform_period_ns));
    memset(s->waveform_epoch_ns, 0, sizeof(s->waveform_epoch_ns));
}


static const TypeInfo esp32_sens_info = {
    .name = TYPE_ESP32_SENS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SensState),
    .instance_init = esp32_sens_init,
};

static void esp32_sens_register_types(void)
{
    type_register_static(&esp32_sens_info);
}

type_init(esp32_sens_register_types)
