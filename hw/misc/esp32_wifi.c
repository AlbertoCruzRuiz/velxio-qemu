#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_wifi.h"
#include "exec/address-spaces.h"
#include "esp32_wlan_packet.h"
#include "hw/qdev-properties.h"
#include "esp32_wlan.h"

/* Defined in esp32_wifi_ap.c — used to resolve channel for non-beacon frames */
extern access_point_info access_points[];
extern int nb_aps;

/* DEBUG=1 enables register-level trace to stderr so velxio's worker
 * subprocess forwards it via the stderr pipe (fd 1 is redirected to
 * /dev/null in esp32_worker.py to keep the JSON event pipe clean).
 * Phase 7 of the velxio MicroPython WiFi project uses this trace to
 * map out which registers MP polls that we don't emulate yet. */
#define DEBUG 0

static uint64_t esp32_wifi_read(void *opaque, hwaddr addr, unsigned int size)
{

    Esp32WifiState *s = ESP32_WIFI(opaque);
    uint32_t r = s->mem[addr/4];

    switch(addr) {
        case A_WIFI_DMA_IN_STATUS:
            r=0;
            break;
        case A_WIFI_DMA_INT_STATUS:
        case A_WIFI_DMA_INT_CLR:
            r=s->raw_interrupt;
            break;
        case A_WIFI_STATUS:
        case A_WIFI_DMA_OUT_STATUS:
            r=1;
            break;
    }

    if(DEBUG) fprintf(stderr, "[wifi] R 0x%04lx= 0x%08x\n",(unsigned long) addr,r);

    return r;
}
static void set_interrupt(Esp32WifiState *s,int e) {
    s->raw_interrupt |= e;
    qemu_set_irq(s->irq, 1);
}

void Esp32_WLAN_frame_delivered(Esp32WifiState *s){
    s->raw_interrupt |= 0x80;
    qemu_set_irq(s->irq, 1);
}

static void esp32_wifi_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size) {
    Esp32WifiState *s = ESP32_WIFI(opaque);
    if(DEBUG) fprintf(stderr, "[wifi] W 0x%04lx= 0x%08lx\n",(unsigned long) addr, (unsigned long) value);

    switch (addr) {
        case A_WIFI_DMA_INLINK:
            s->dma_inlink_address = value;
            break;
        case A_WIFI_DMA_INT_CLR:
            s->raw_interrupt &= ~value;
            if(s->raw_interrupt==0)
                qemu_set_irq(s->irq, 0);
            break;
        case A_WIFI_DMA_OUTLINK:
            if (value & 0xc0000000) {
                // do a DMA transfer to the hardware from esp32 memory
                mac80211_frame frame;
                dma_list_item item;
                unsigned memaddr = (0x3ff00000 | (value & 0xfffff));
                address_space_read(&address_space_memory, memaddr,
                            MEMTXATTRS_UNSPECIFIED, &item, 12);
                address_space_read(&address_space_memory, item.address,
                            MEMTXATTRS_UNSPECIFIED, &frame, item.length);
                // frame from esp32 to ap
                frame.frame_length=item.length;
                frame.next_frame=0;
                Esp32_WLAN_handle_frame(s, &frame);
                set_interrupt(s,0x80);
            }
    }
    s->mem[addr/4]=value;
}

static int match_mac_address(uint8_t *a1,uint8_t *a2) {
    if(!memcmp(a1,a2,6)) return 1;
    if(!memcmp(a1,BROADCAST,6)) return 1;
    return 0;
}
// frame from ap to esp32
void Esp32_sendFrame(Esp32WifiState *s, mac80211_frame *frame,int length, int signal_strength) {
    if(s->dma_inlink_address==0) return;
    uint8_t *header=malloc(sizeof(wifi_pkt_rx_ctrl_t)+length);
    memset(header,0,sizeof(wifi_pkt_rx_ctrl_t)+length);
    wifi_pkt_rx_ctrl_t *pkt=(wifi_pkt_rx_ctrl_t *)header;

    // Determine the channel to report in wifi_pkt_rx_ctrl_t.
    // esp32_wifi_channel is updated via a PHY register write formula; on a VPS it
    // often stays 0, causing the firmware to discard every received frame.
    //
    // Strategy (in priority order):
    //  1. Beacon frames: parse the DS Parameter Set IE (tag 0x03) for the true channel.
    //  2. Other management frames: look up the AP channel by source_address MAC.
    //  3. Data frames: look up by bssid_address MAC.
    //  4. Fallback: use esp32_wifi_channel (may be 0 on VPS).
    int pkt_channel = esp32_wifi_channel;

    if (frame->frame_control.type == IEEE80211_TYPE_MGT) {
        if (frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON) {
            // Beacon: extract channel from DS Parameter Set IE
            // Fixed beacon header = 12 bytes (timestamp 8 + interval 2 + capability 2)
            int data_len = length - IEEE80211_HEADER_SIZE;
            int pos = 12;
            while (pos + 2 <= data_len) {
                uint8_t tag = frame->data_and_fcs[pos];
                uint8_t ie_len = frame->data_and_fcs[pos + 1];
                if (tag == 0x03 && ie_len >= 1) { /* DS Parameter Set = channel */
                    pkt_channel = frame->data_and_fcs[pos + 2];
                    break;
                }
                pos += 2 + ie_len;
            }
        }
        // For all management frames (including beacons): if channel still 0,
        // try looking up the AP channel by source address (the AP's MAC).
        if (pkt_channel == 0) {
            for (int i = 0; i < nb_aps; i++) {
                if (memcmp(access_points[i].mac_address, frame->source_address, 6) == 0) {
                    pkt_channel = access_points[i].channel;
                    break;
                }
            }
        }
    } else if (frame->frame_control.type == IEEE80211_TYPE_DATA) {
        // Data frames: look up channel by BSSID (the AP's MAC)
        if (pkt_channel == 0) {
            for (int i = 0; i < nb_aps; i++) {
                if (memcmp(access_points[i].mac_address, frame->bssid_address, 6) == 0) {
                    pkt_channel = access_points[i].channel;
                    break;
                }
            }
        }
    }

    *pkt=(wifi_pkt_rx_ctrl_t){
        .rssi=(signal_strength+(rand()%10)+96),
        .rate=11,
        .sig_len=length,
        .sig_len_copy=length,
        .legacy_length=length,
        .noise_floor=-97,
        .channel=pkt_channel,
        .timestamp=(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)/1000),
    };
    // These 4 bits are set if the mac addresses previously stored at 0x40 and 0x48
    // match the destination or bssid addresses in the frame
    if(match_mac_address(frame->destination_address,(uint8_t *)s->mem+0x40)) 
        pkt->damatch0=1;
    if(match_mac_address(frame->destination_address,(uint8_t *)s->mem+0x48)) 
        pkt->damatch1=1;
    if(match_mac_address(frame->bssid_address,(uint8_t *)s->mem+0x40)) 
        pkt->bssidmatch0=1;
    if(match_mac_address(frame->bssid_address,(uint8_t *)s->mem+0x48)) 
        pkt->bssidmatch1=1;
    //printf("...%x %x\n",header[3],frame->destination_address[0]);

    memcpy(header+sizeof(wifi_pkt_rx_ctrl_t),frame,length);
    length+=sizeof(wifi_pkt_rx_ctrl_t);
    // do a DMA transfer from the hardware to esp32 memory
    dma_list_item item;
    address_space_read(&address_space_memory, s->dma_inlink_address, MEMTXATTRS_UNSPECIFIED, &item, 12);
    address_space_write(&address_space_memory, item.address, MEMTXATTRS_UNSPECIFIED, header, length);
    item.length=length;
    item.eof=1;
    address_space_write(&address_space_memory, s->dma_inlink_address, MEMTXATTRS_UNSPECIFIED,&item,4);
    if (item.next != 0) {
        s->dma_inlink_address = item.next;
    } else {
        /* The firmware cleared the next pointer while recycling the descriptor,
         * breaking the circular ring.  Reset to the start of the ring (the last
         * value the firmware wrote to A_WIFI_DMA_INLINK) so probe/auth/assoc
         * responses are not permanently dropped. */
        s->dma_inlink_address = s->mem[A_WIFI_DMA_INLINK/4];
    }
    set_interrupt(s,0x1000024);
    free(header);
}

static const MemoryRegionOps esp32_wifi_ops = {
    .read =  esp32_wifi_read,
    .write = esp32_wifi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_wifi_realize(DeviceState *dev, Error **errp)
{
    Esp32WifiState *s = ESP32_WIFI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    s->dma_inlink_address=0;

    memory_region_init_io(&s->iomem, OBJECT(dev), &esp32_wifi_ops, s,
                          TYPE_ESP32_WIFI, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    memset(s->mem,0,sizeof(s->mem));
    Esp32_WLAN_setup_ap(dev, s);
}
static Property esp32_wifi_properties[] = {
    DEFINE_NIC_PROPERTIES(Esp32WifiState, conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_wifi_reset_enter(Object *obj, ResetType type)
{
    Esp32WifiState *s = ESP32_WIFI(obj);
    if(s)
	    s->ap_state=0;
}

static void esp32_wifi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettablePhases rp;
    dc->realize = esp32_wifi_realize;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "Esp32 WiFi";
    device_class_set_props(dc, esp32_wifi_properties);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    resettable_class_set_parent_phases(rc, esp32_wifi_reset_enter, NULL, NULL,
                                   &rp);
}

static const TypeInfo esp32_wifi_info = {
    .name = TYPE_ESP32_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32WifiState),
    .class_init    = esp32_wifi_class_init,
};

static void esp32_wifi_register_types(void)
{
    type_register_static(&esp32_wifi_info);
}

type_init(esp32_wifi_register_types)
