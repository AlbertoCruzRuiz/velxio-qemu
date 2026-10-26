#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"

#define TYPE_ESP32C3_SARADC "misc.esp32c3.saradc"
#define ESP32C3_SARADC(obj) OBJECT_CHECK(Esp32c3SarAdcState, (obj), TYPE_ESP32C3_SARADC)

#define ESP32C3_SARADC_NUM_CHANNELS 32

typedef struct Esp32c3SarAdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t mem[1024];
    unsigned short ADC_values[32];
    uint32_t int_ena_reg;
    uint32_t int_raw_reg;
    int channel1;
    int channel2;

    /* Periodic waveform LUTs for AC/transient sources. When waveform_samples[ch]
     * is non-NULL, reads interpolate the LUT against QEMU_CLOCK_VIRTUAL. */
    uint16_t *waveform_samples[ESP32C3_SARADC_NUM_CHANNELS];
    int       waveform_len[ESP32C3_SARADC_NUM_CHANNELS];
    uint64_t  waveform_period_ns[ESP32C3_SARADC_NUM_CHANNELS];
    uint64_t  waveform_epoch_ns[ESP32C3_SARADC_NUM_CHANNELS];
} Esp32c3SarAdcState;

/* Push a periodic waveform LUT for ADC channel `chn`. Passing samples=NULL or
 * n<=0 clears the waveform and restores the static ADC_values[] path. */
void esp32c3_saradc_set_waveform(Esp32c3SarAdcState *s, int chn,
                                 const uint16_t *samples, int n,
                                 uint64_t period_ns);


REG32(APB_SARADC_CTRL_REG, 0x000);

REG32(APB_SARADC_ONETIME_SAMPLE_REG, 0x20);
  FIELD(APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC1_ONETIME_SAMPLE, 31, 1);
  FIELD(APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC2_ONETIME_SAMPLE, 30, 1);
  FIELD(APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC_ONETIME_START, 29, 1);
  FIELD(APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC_ONETIME_CHANNEL,25,4);
  FIELD(APB_SARADC_ONETIME_SAMPLE_REG,APB_SARADC_ONETIME_ATTEN, 23, 2);


REG32(APB_SARADC_APB_ADC_ARB_CTRL_REG , 0x024);


REG32(APB_SARADC_1_DATA_STATUS_REG, 0x2C);
  FIELD(APB_SARADC_1_DATA_STATUS_REG, APB_SARADC_ADC1_DATA,0, 17);

REG32(APB_SARADC_2_DATA_STATUS_REG, 0x3C);
  FIELD(APB_SARADC_2_DATA_STATUS_REG, APB_SARADC_ADC2_DATA,0, 17);


REG32(APB_SARADC_INT_ENA_REG  , 0x040);
  FIELD(APB_SARADC_INT_ENA_REG, APB_SARADC_ADC1_DONE_INT_ENA, 31, 1);
  FIELD(APB_SARADC_INT_ENA_REG, APB_SARADC_ADC2_DONE_INT_ENA, 30, 1);

REG32(APB_SARADC_INT_RAW_REG , 0x044);
  FIELD(APB_SARADC_INT_RAW_REG, APB_SARADC_ADC1_DONE_INT_RAW, 31, 1);
  FIELD(APB_SARADC_INT_RAW_REG, APB_SARADC_ADC2_DONE_INT_RAW, 30, 1);

REG32(APB_SARADC_INT_ST_REG  , 0x048);
  FIELD(APB_SARADC_INT_ST_REG, APB_SARADC_ADC1_DONE_INT_ST, 31, 1);
  FIELD(APB_SARADC_INT_ST_REG, APB_SARADC_ADC2_DONE_INT_ST, 30, 1);

REG32(APB_SARADC_INT_CLR_REG   , 0x04C);
  FIELD(APB_SARADC_INT_CLR_REG, APB_SARADC_ADC1_DONE_INT_CLR, 31, 1);
  FIELD(APB_SARADC_INT_CLR_REG, APB_SARADC_ADC2_DONE_INT_CLR, 30, 1);


REG32(APB_SARADC_APB_ADC_CLKM_CONF_REG   , 0x054);

