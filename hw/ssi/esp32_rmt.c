/*
 * ESP32 RMT controller
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/esp32_rmt.h"

#define DEBUG 0

extern void (*picsimlab_rmt_event)(const uint8_t channel, const uint32_t config0, const uint32_t value);

static void restart_timer(Esp32RmtState *s, int channel) {
    // One chunk of txlim items at 1.25 us each. Never zero: the guard in
    // send_data re-arms this timer while it waits, and a limit of 0 would
    // turn that wait into a main-loop spin at the same virtual time.
    int64_t step = 1250 * (int64_t)s->txlim[channel];
    if (step < 2000) {
        step = 2000;
    }
    timer_mod_anticipate_ns(&s->rmt_timer,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+step);
}

// send txlim data values, stop if a value is 0
// set the correct raw int for tx_end or tx_thr_event
static void send_data(Esp32RmtState *s, int channel) {
    if(DEBUG) printf("send ch=%d limit=%d sent=%d \n",channel, s->txlim[channel], s->sent);
    BusState *b = BUS(s->rmt);
    BusChild *ch = QTAILQ_FIRST(&b->children);
    SSIPeripheral *slave = NULL;
    SSIPeripheralClass *ssc = NULL;
    if(ch){
       slave = SSI_PERIPHERAL(ch->child);
       ssc = SSI_PERIPHERAL_GET_CLASS(slave);
    }
    
    // don't send data when interrupt hasn't been handled
    // a real device can't do this but it's necessary because 
    // qemu can't always keep up. 
    //
    // Only an interrupt the guest has ENABLED counts as "not handled". The
    // ISR of the ESP-IDF 5 driver (rmt_tx.c) clears the status it reads,
    // which is raw AND enabled, and once the whole frame is in the memory
    // block it disables tx_thr_event from inside the ISR (rmt_tx_mark_eof)
    // and never touches that bit again. The raw bit then stays set on every
    // later chunk, and a guard on the raw register alone stalled the
    // transmission one chunk short of the end-of-frame symbol: a 2-pixel
    // WS2812 frame stopped after 32 items, a 3-pixel one after 64, a
    // 16-pixel ring after 384, and the sketch waited in rmt_tx_wait_all_done
    // for a tx_end that never came (velxio issue #381). A single pixel fits in
    // one chunk, which is why only that case ever worked.
    uint32_t wait_mask = (1u << (channel + 24)) | (1u << (channel * 3));
    if (s->int_raw & s->int_en & wait_mask) {
        // send data when the interrupt is cleared; poll again meanwhile, so a
        // guest that disables the interrupt instead of clearing it (see
        // above) is not stuck waiting for a write that never comes.
        s->unsent_data=true;
        if(DEBUG) printf("esp32_rmt_send_data waiting interrupt !!!!\n");
        restart_timer(s, channel);
        return;
    } else 
        s->unsent_data=false;
    
    int memsize=((s->conf0[channel]>>24)&0xf)*64;
    

    for (int i = 0; i < s->txlim[channel] ; i++) {    
        int v;
        if(s->apb_conf & R_RMT_APB_CONF_MEM_TX_WRAP_EN_MASK){
            v = s->data[((i+s->sent)%memsize+channel*64)%memsize]; 
        }else{
            v = s->data[((i+s->sent)%memsize+channel*64)%ESP32_RMT_BUF_WORDS];
        }
        if(v==0) { // stop sending when we see a zero
            // Hand the stop symbol to the host too. It is a real item on the
            // wire and the host decoder keys end-of-frame on it: the WS2812
            // decoder in esp32_worker.py flushes its pixel buffer when it sees
            // {0,0}, so dropping this one item meant a frame NEVER completed
            // and no ws2812_update was ever emitted — a NeoPixel under the
            // QEMU engine stayed dark no matter what the sketch did.
            picsimlab_rmt_event(channel, s->conf0[channel], v);
            s->int_raw|=(1<<(channel*3)); //set TX_END
            s->int_raw&=~(1<<(channel+24)); //clearTHR
            s->sent=0;
            s->conf1[channel] &= ~R_RMT_CHnCONF1_TX_START_MASK;
            if(s->int_en & (1<<(channel*3)))
                qemu_irq_raise(s->irq);
            return;
        }
        if(ssc){
           ssc->transfer(slave,v);
        }
        picsimlab_rmt_event(channel, s->conf0[channel], v);
    }
    s->sent+=s->txlim[channel];
    s->int_raw|=(1<<(channel+24)); //set THR
    if(s->int_en & (1<<(channel+24)))
        qemu_irq_raise(s->irq);
    restart_timer(s,channel);
}

static void esp32_rmt_timer_cb(void *opaque) {
    Esp32RmtState *s = ESP32_RMT(opaque);
    // send data for any enabled channels 
    for(int i=0;i<8;i++) {
        if((s->conf1[i] & R_RMT_CHnCONF1_TX_START_MASK)) {
            send_data(s,i);
        }
    }    
}

static uint64_t esp32_rmt_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32RmtState *s = ESP32_RMT(opaque);
    uint32_t r = 0;
    int channel=(addr-32)/8;
    switch (addr) {
    case A_RMT_CH0CONF0 ... (A_RMT_CH0CONF0+8*8)-4:
        if(addr & 0x4) 
            r=s->conf1[channel];
        else 
            r=s->conf0[channel];
        break;
    case A_RMT_INT_RAW:
        r = s->int_raw ;
        break;
    case A_RMT_INT_ST:
        r = s->int_raw & s->int_en;
        break;
    case A_RMT_INT_ENA:
        r = s->int_en;
        break;
    case A_RMT_TX_LIM ... A_RMT_TX_LIM+7*4:
        channel=(addr-A_RMT_TX_LIM)/4;
        r = s->txlim[channel];
        break;
    case A_RMT_DATA ... A_RMT_DATA+(ESP32_RMT_BUF_WORDS-1)* 4:
        r = s->data[(addr-A_RMT_DATA)/4];
        break;
    case A_RMT_APB_CONF:
        r = s->apb_conf;
        break;
    }
    if(DEBUG) printf("esp32_rmt_read  0x%04lx= 0x%08x\n",(unsigned long) addr,r);
    return r;
}


static void esp32_rmt_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32RmtState *s = ESP32_RMT(opaque);

    if(DEBUG) printf("esp32_rmt_write 0x%04lx= 0x%08lx\n",(unsigned long) addr, (unsigned long) value);

//    if(addr<A_RMT_DATA)
//        printf("rmt write %ld %ld\n",addr,value);
    int channel;
    switch (addr) {
    case A_RMT_CH0CONF0 ...  (A_RMT_CH0CONF0+8*8)-4:
        channel=(addr-32)/8;
        if((addr & 0x4)==0) {
            s->conf0[channel]=value;
        } else {
            s->conf1[channel]=value;
            if((value & 0x8 /*R_RMT_CHnCONF1_MEM_RD_RST_MASK*/)) {
                s->sent=0;
            }
            if((value & R_RMT_CHnCONF1_TX_START_MASK)) {
                // start timer to send data
                restart_timer(s,channel);
            }
        }
        break;
    case A_RMT_TX_LIM ... A_RMT_TX_LIM+7*4:
        channel=(addr-A_RMT_TX_LIM)/4;
        s->txlim[channel]=value;
        break;
    case A_RMT_INT_ENA:
        s->int_en=value;
        // The line follows the enable mask, as on the chip: the driver
        // disables tx_thr_event from the ISR while its raw bit is still set,
        // and a line left high with nothing enabled behind it is an interrupt
        // storm.
        if((s->int_raw & s->int_en)==0) {
            qemu_irq_lower(s->irq);
        }
        break;
    case A_RMT_INT_CLR:
        s->int_raw&=(~value);
        if((s->int_raw & s->int_en)==0) {
            qemu_irq_lower(s->irq);
        }
        if(s->unsent_data){ 
            esp32_rmt_timer_cb(s);
        }
        break;
    case A_RMT_DATA ... A_RMT_DATA+(ESP32_RMT_BUF_WORDS-1)*4:
        s->data[(addr-A_RMT_DATA)/4]=value;
        break;
    case A_RMT_APB_CONF:
        s->apb_conf=value;
        break;
    }
}


static const MemoryRegionOps esp32_rmt_ops = {
    .read =  esp32_rmt_read,
    .write = esp32_rmt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_rmt_reset_enter(Object *obj, ResetType type)
{
    Esp32RmtState *s = ESP32_RMT(obj);
    s->int_raw=0;
    s->sent=0;
    s->int_en=0;
    qemu_irq_lower(s->irq);
    timer_del(&s->rmt_timer);
    for(int i=0;i<8;i++) {
        s->conf0[i]=0;
        s->conf1[i]=0;
    }
}

static void esp32_rmt_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_rmt_init(Object *obj)
{
    Esp32RmtState *s = ESP32_RMT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_rmt_ops, s,
                          TYPE_ESP32_RMT, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    timer_init_ns(&s->rmt_timer, QEMU_CLOCK_VIRTUAL, esp32_rmt_timer_cb, s);
    s->rmt = ssi_create_bus(DEVICE(s), "rmt");
}

static Property esp32_rmt_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_rmt_class_init(ObjectClass *klass, void *data)
{
    ResettablePhases rp;
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = esp32_rmt_realize;
    device_class_set_props(dc, esp32_rmt_properties);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    resettable_class_set_parent_phases(rc, esp32_rmt_reset_enter, NULL, NULL, &rp);
}

static const TypeInfo esp32_rmt_info = {
    .name = TYPE_ESP32_RMT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32RmtState),
    .instance_init = esp32_rmt_init,
    .class_init = esp32_rmt_class_init
};

static void esp32_rmt_register_types(void)
{
    type_register_static(&esp32_rmt_info);
}

type_init(esp32_rmt_register_types)
