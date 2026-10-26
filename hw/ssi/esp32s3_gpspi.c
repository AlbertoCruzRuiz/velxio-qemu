/*
 * ESP32-S3 GPSPI (general-purpose SPI: SPI2 / SPI3, a.k.a. FSPI/HSPI).
 *
 * The plain esp32s3 machine leaves GPSPI2 (0x60024000) unimplemented, so
 * SPI displays (ILI9341/ST7789 over the Arduino SPI library) have no
 * controller to talk to. This models the GPSPI register set enough to drive
 * the Arduino `SPIClass` write path and route the MOSI byte stream to the
 * picsimlab host shim (esp32s3-picsimlab wires a "picsimlab_spi" SSI slave on
 * this bus), exactly like esp32_spi.c does for the original ESP32's SPI2.
 *
 * Register layout differs from the original ESP32 SPI (the "GPSPI" redesign):
 *   CMD 0x00 (USR = bit24), USER 0x10 (MOSI=bit27, MISO=bit28), USER1 0x14,
 *   USER2 0x18, MS_DLEN 0x1C (data bitlen [17:0]), MISC 0x20 (CS*_DIS),
 *   CLOCK 0x0C, DMA_CONF 0x30, W0..W15 0x98..0xD4 (16 words = 64-byte buffer).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"

#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"
#define ESP32S3_GPSPI(obj) OBJECT_CHECK(Esp32s3GpspiState, (obj), TYPE_ESP32S3_GPSPI)

#define ESP32S3_GPSPI_CS_COUNT   3
#define ESP32S3_GPSPI_BUF_WORDS  16          /* W0..W15 */
#define ESP32S3_GPSPI_REG_SIZE   0x1000

REG32(S3SPI_CMD,   0x00)
    FIELD(S3SPI_CMD, USR, 24, 1)             /* start user-defined transaction */
REG32(S3SPI_ADDR,  0x04)
REG32(S3SPI_CTRL,  0x08)
REG32(S3SPI_CLOCK, 0x0C)
REG32(S3SPI_USER,  0x10)
    FIELD(S3SPI_USER, USR_MISO, 28, 1)
    FIELD(S3SPI_USER, USR_MOSI, 27, 1)
REG32(S3SPI_USER1, 0x14)
REG32(S3SPI_USER2, 0x18)
REG32(S3SPI_MS_DLEN, 0x1C)                   /* SPI_MS_DATA_BITLEN [17:0] */
    FIELD(S3SPI_MS_DLEN, BITLEN, 0, 18)
REG32(S3SPI_MISC,  0x20)
    FIELD(S3SPI_MISC, CS0_DIS, 0, 1)
REG32(S3SPI_DMA_CONF, 0x30)
REG32(S3SPI_DMA_INT_RAW, 0x38)
REG32(S3SPI_DMA_INT_ST,  0x40)
REG32(S3SPI_W0,   0x98)
REG32(S3SPI_W15,  0xD4)

typedef struct Esp32s3GpspiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq cs_gpio[ESP32S3_GPSPI_CS_COUNT];
    SSIBus *spi;

    uint32_t addr_reg;
    uint32_t ctrl_reg;
    uint32_t clock_reg;
    uint32_t user_reg;
    uint32_t user1_reg;
    uint32_t user2_reg;
    uint32_t ms_dlen_reg;
    uint32_t misc_reg;
    uint32_t dma_conf_reg;
    uint32_t data_reg[ESP32S3_GPSPI_BUF_WORDS];
} Esp32s3GpspiState;

/* Shared with esp32_spi.c / esp32_picsimlab.c in the same libqemu-xtensa .so. */
extern void (*picsimlab_spi_event_batch)(const uint8_t id, const uint8_t *mosi, const int len);

/* Same TFT fast-path + per-byte fallback as esp32_spi.c: a write-dominant
 * transfer to the picsimlab host shim is delivered in ONE batch call. */
static void esp32s3_gpspi_txrx_buffer(Esp32s3GpspiState *s, void *buf,
                                      int tx_bytes, int rx_bytes)
{
    if (rx_bytes <= 1 && tx_bytes > 1 && picsimlab_spi_event_batch) {
        BusState *b = BUS(s->spi);
        BusChild *ch = b ? QTAILQ_FIRST(&b->children) : NULL;
        if (ch && object_dynamic_cast(OBJECT(ch->child), "picsimlab_spi")) {
            picsimlab_spi_event_batch(0, (const uint8_t *) buf, tx_bytes);
            if (rx_bytes >= 1) {
                ((uint8_t *) buf)[0] = 0xFF;   /* dummy MISO, ignored by displays */
            }
            return;
        }
    }

    int bytes = MAX(tx_bytes, rx_bytes);
    uint8_t *c_buf = (uint8_t *) buf;
    for (int i = 0; i < bytes; ++i) {
        uint8_t byte = 0;
        if (i < tx_bytes) {
            byte = c_buf[i];
        }
        uint32_t res = ssi_transfer(s->spi, byte);
        if (i < rx_bytes) {
            c_buf[i] = (uint8_t) res;
        }
    }
}

static void esp32s3_gpspi_cs_set(Esp32s3GpspiState *s, int level)
{
    /* CS0 active unless disabled; drive it through the GPIO-matrix path so the
     * picsimlab host shim / SD / ePaper get CS toggles (mirror esp32_spi.c). */
    if (!FIELD_EX32(s->misc_reg, S3SPI_MISC, CS0_DIS)) {
        qemu_set_irq(s->cs_gpio[0], level);
    }
}

/* SPI_CMD.USR = 1 -> run one user transaction using the W0.. buffer. */
static void esp32s3_gpspi_do_command(Esp32s3GpspiState *s, uint32_t cmd)
{
    if (!FIELD_EX32(cmd, S3SPI_CMD, USR)) {
        return;
    }

    /* SPI_MS_DATA_BITLEN holds (nbits - 1). Round up to whole bytes. */
    int nbits  = FIELD_EX32(s->ms_dlen_reg, S3SPI_MS_DLEN, BITLEN) + 1;
    int nbytes = (nbits + 7) / 8;
    if (nbytes <= 0) {
        return;
    }
    if (nbytes > ESP32S3_GPSPI_BUF_WORDS * 4) {
        nbytes = ESP32S3_GPSPI_BUF_WORDS * 4;
    }

    bool has_mosi = FIELD_EX32(s->user_reg, S3SPI_USER, USR_MOSI);
    bool has_miso = FIELD_EX32(s->user_reg, S3SPI_USER, USR_MISO);

    uint8_t buf[ESP32S3_GPSPI_BUF_WORDS * 4];
    memcpy(buf, s->data_reg, nbytes);

    esp32s3_gpspi_cs_set(s, 0);
    esp32s3_gpspi_txrx_buffer(s, buf,
                              has_mosi ? nbytes : 0,
                              has_miso ? nbytes : 0);
    esp32s3_gpspi_cs_set(s, 1);

    if (has_miso) {
        memcpy(s->data_reg, buf, nbytes);
    }

    /* USR is self-clearing; signal transaction done. */
    if (s->irq) {
        qemu_irq_pulse(s->irq);
    }
}

static uint64_t esp32s3_gpspi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3GpspiState *s = ESP32S3_GPSPI(opaque);
    switch (addr) {
    case A_S3SPI_W0 ... A_S3SPI_W0 + (ESP32S3_GPSPI_BUF_WORDS - 1) * 4:
        return s->data_reg[(addr - A_S3SPI_W0) / 4];
    case A_S3SPI_CMD:      return 0;               /* USR reads back cleared */
    case A_S3SPI_ADDR:     return s->addr_reg;
    case A_S3SPI_CTRL:     return s->ctrl_reg;
    case A_S3SPI_CLOCK:    return s->clock_reg;
    case A_S3SPI_USER:     return s->user_reg;
    case A_S3SPI_USER1:    return s->user1_reg;
    case A_S3SPI_USER2:    return s->user2_reg;
    case A_S3SPI_MS_DLEN:  return s->ms_dlen_reg;
    case A_S3SPI_MISC:     return s->misc_reg;
    case A_S3SPI_DMA_CONF: return s->dma_conf_reg;
    case A_S3SPI_DMA_INT_RAW:
    case A_S3SPI_DMA_INT_ST:
        return 0;
    default:
        return 0;
    }
}

static void esp32s3_gpspi_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    Esp32s3GpspiState *s = ESP32S3_GPSPI(opaque);
    switch (addr) {
    case A_S3SPI_W0 ... A_S3SPI_W0 + (ESP32S3_GPSPI_BUF_WORDS - 1) * 4:
        s->data_reg[(addr - A_S3SPI_W0) / 4] = value;
        break;
    case A_S3SPI_ADDR:     s->addr_reg = value; break;
    case A_S3SPI_CTRL:     s->ctrl_reg = value; break;
    case A_S3SPI_CLOCK:    s->clock_reg = value; break;
    case A_S3SPI_USER:     s->user_reg = value; break;
    case A_S3SPI_USER1:    s->user1_reg = value; break;
    case A_S3SPI_USER2:    s->user2_reg = value; break;
    case A_S3SPI_MS_DLEN:  s->ms_dlen_reg = value; break;
    case A_S3SPI_MISC:     s->misc_reg = value; break;
    case A_S3SPI_DMA_CONF: s->dma_conf_reg = value; break;
    case A_S3SPI_CMD:      esp32s3_gpspi_do_command(s, value); break;
    default:
        break;
    }
}

static const MemoryRegionOps esp32s3_gpspi_ops = {
    .read =  esp32s3_gpspi_read,
    .write = esp32s3_gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_gpspi_realize(DeviceState *dev, Error **errp)
{
    Esp32s3GpspiState *s = ESP32S3_GPSPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &esp32s3_gpspi_ops, s,
                          TYPE_ESP32S3_GPSPI, ESP32S3_GPSPI_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->spi = ssi_create_bus(dev, "spi");
    qdev_init_gpio_out_named(dev, &s->cs_gpio[0], SSI_GPIO_CS, ESP32S3_GPSPI_CS_COUNT);
}

static void esp32s3_gpspi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = esp32s3_gpspi_realize;
}

static const TypeInfo esp32s3_gpspi_info = {
    .name = TYPE_ESP32S3_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3GpspiState),
    .class_init = esp32s3_gpspi_class_init,
};

static void esp32s3_gpspi_register_types(void)
{
    type_register_static(&esp32s3_gpspi_info);
}

type_init(esp32s3_gpspi_register_types)
