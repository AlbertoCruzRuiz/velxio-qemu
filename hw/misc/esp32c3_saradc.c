/* ESP32C3 saradc peripheral handler
*/

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/boards.h"
#include "hw/misc/esp32c3_saradc.h"
#include "hw/misc/esp32c3_reg.h"
#include "hw/irq.h"


#define DEBUG 0


static void esp32c3_saradc_update_irq(Esp32c3SarAdcState * s)
{
    int irq_state = !!(s->int_raw_reg & s->int_ena_reg);
    qemu_set_irq(s->irq, irq_state);
}

/* Linearly interpolate the waveform LUT at the current virtual clock. */
static uint16_t esp32c3_saradc_sample_waveform(Esp32c3SarAdcState *s, int ch)
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

static uint64_t esp32c3_saradc_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32c3SarAdcState *s = ESP32C3_SARADC(opaque);

    uint32_t r = s->mem[addr/4];
    int ch;

    switch(addr) {
        case A_APB_SARADC_INT_ENA_REG:
            r= s->int_ena_reg;
            break;
        case A_APB_SARADC_INT_RAW_REG:
            r= s->int_raw_reg;
            break;
        case A_APB_SARADC_INT_ST_REG:
             r= s->int_raw_reg & s->int_ena_reg;
             break;
        case A_APB_SARADC_1_DATA_STATUS_REG:
            ch = s->channel1;
            if (ch >= 0 && ch < ESP32C3_SARADC_NUM_CHANNELS && s->waveform_samples[ch]) {
                r = esp32c3_saradc_sample_waveform(s, ch);
            } else {
                r = s->ADC_values[ch];
            }
            if(DEBUG) printf("Read ADC 1 channel %i = %i\n",s->channel1, r);
            break;
        case A_APB_SARADC_2_DATA_STATUS_REG:
            ch = s->channel2;
            if (ch >= 0 && ch < ESP32C3_SARADC_NUM_CHANNELS && s->waveform_samples[ch]) {
                r = esp32c3_saradc_sample_waveform(s, ch);
            } else {
                r = s->ADC_values[ch];
            }
            if(DEBUG) printf("Read ADC 2 channel %i = %i\n",s->channel1, r);
            break;
    }

    if(DEBUG) printf("esp32c3_saradc_read  0x%04lx= 0x%08x\n",(unsigned long) addr,r);

    return r;
}

static void esp32c3_saradc_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32c3SarAdcState *s = ESP32C3_SARADC(opaque);

    if(DEBUG) printf("esp32c3_saradc_write 0x%04lx= 0x%08lx\n",(unsigned long) addr, (unsigned long) value);

        switch(addr) {
        case A_APB_SARADC_INT_ENA_REG:
            s->int_ena_reg = value;
            esp32c3_saradc_update_irq(s);
            break;
        case A_APB_SARADC_INT_CLR_REG:
            s->int_raw_reg &= ~value;
            esp32c3_saradc_update_irq(s);
            break;
        case A_APB_SARADC_ONETIME_SAMPLE_REG:
            if(FIELD_EX32(value, APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC_ONETIME_START))
            {
               if(FIELD_EX32(value, APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC1_ONETIME_SAMPLE))
               {
                    s->channel1 = FIELD_EX32(value, APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC_ONETIME_CHANNEL);
                    s->int_raw_reg = FIELD_DP32(s->int_raw_reg, APB_SARADC_INT_RAW_REG, APB_SARADC_ADC1_DONE_INT_RAW, 1);
                    esp32c3_saradc_update_irq(s);
               }
               if(FIELD_EX32(value, APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC2_ONETIME_SAMPLE))
               {
                    s->channel2 = FIELD_EX32(value, APB_SARADC_ONETIME_SAMPLE_REG, APB_SARADC_ONETIME_CHANNEL);
                    s->int_raw_reg = FIELD_DP32(s->int_raw_reg, APB_SARADC_INT_RAW_REG, APB_SARADC_ADC2_DONE_INT_RAW, 1);
                    esp32c3_saradc_update_irq(s);
               }
            }
            break;
    }

    s->mem[addr/4]=value;
}


void esp32c3_saradc_set_waveform(Esp32c3SarAdcState *s, int chn,
                                 const uint16_t *samples, int n,
                                 uint64_t period_ns)
{
    if (!s || chn < 0 || chn >= ESP32C3_SARADC_NUM_CHANNELS) {
        return;
    }
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


static const MemoryRegionOps esp32c3_saradc_ops = {
    .read =  esp32c3_saradc_read,
    .write = esp32c3_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32c3_saradc_init(Object *obj)
{
    Esp32c3SarAdcState *s = ESP32C3_SARADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_saradc_ops, s, TYPE_ESP32C3_SARADC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    memset(s->ADC_values, 0, sizeof(s->ADC_values));
    memset(s->waveform_samples, 0, sizeof(s->waveform_samples));
    memset(s->waveform_len, 0, sizeof(s->waveform_len));
    memset(s->waveform_period_ns, 0, sizeof(s->waveform_period_ns));
    memset(s->waveform_epoch_ns, 0, sizeof(s->waveform_epoch_ns));
}

static void esp32c3_saradc_reset_enter(Object *obj, ResetType type)
{
    Esp32c3SarAdcState * s = ESP32C3_SARADC(obj);

    s->int_ena_reg = 0;
    s->int_raw_reg = 0;
    s->channel1 = 0;
    s->channel2 = 0;
    memset(s->mem, 0, sizeof(s->mem));
    /* Free any waveform LUTs on reset to avoid leaks across reboots. */
    for (int i = 0; i < ESP32C3_SARADC_NUM_CHANNELS; i++) {
        if (s->waveform_samples[i]) {
            g_free(s->waveform_samples[i]);
            s->waveform_samples[i] = NULL;
            s->waveform_len[i] = 0;
            s->waveform_period_ns[i] = 0;
        }
    }
}

static void esp32c3_saradc_class_init(ObjectClass * klass, void * data)
{
    ResettablePhases rp;
    //DeviceClass * dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    resettable_class_set_parent_phases(rc, esp32c3_saradc_reset_enter, NULL, NULL, &rp);
}

static const TypeInfo esp32c3_saradc_info = {
    .name = TYPE_ESP32C3_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32c3SarAdcState),
    .instance_init = esp32c3_saradc_init,
    .class_init = esp32c3_saradc_class_init,
};

static void esp32c3_saradc_register_types(void)
{
    type_register_static(&esp32c3_saradc_info);
}

type_init(esp32c3_saradc_register_types)
