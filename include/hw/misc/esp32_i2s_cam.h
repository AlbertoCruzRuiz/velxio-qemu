/*
 * ESP32 I²S0 in slave-RX (camera/DVP) mode — QEMU peripheral.
 *
 * Replaces the unimp stub that previously sat at DR_REG_I2S_BASE.
 * Models the subset of the ESP32 I²S0 register block that the
 * upstream esp32-camera driver writes to (target/esp32/ll_cam.c).
 *
 * Spec / source-of-truth:
 *   - test/test-esp32-cam/autosearch/08_dvp_i2s_spec.md
 *   - wokwi-libs/esp32-camera/target/esp32/ll_cam.c
 *   - ESP-IDF soc/i2s_struct.h (field offsets)
 *
 * The frame *content* is provided by the host via the picsimlab
 * callback bridge (see esp32_i2s_cam_push_frame() below). When no
 * frame has been pushed, the device fills DMA buffers with the idle
 * pattern 0xAA — that lets the Phase-2 dma_smoke test verify the
 * descriptor walker without a host frame source ready yet.
 *
 * License: GPL v2+ (matches the rest of the QEMU fork).
 */
#ifndef HW_MISC_ESP32_I2S_CAM_H
#define HW_MISC_ESP32_I2S_CAM_H

#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define TYPE_ESP32_I2S_CAM "esp32.i2s_cam"
#define ESP32_I2S_CAM(obj) \
    OBJECT_CHECK(Esp32I2sCamState, (obj), TYPE_ESP32_I2S_CAM)

#define ESP32_I2S_CAM_REGION_SIZE 0x1000

/* Register offsets — match ESP-IDF soc/i2s_struct.h. */
#define I2S_FIFO_RD          0x000
#define I2S_FIFO_WR          0x004
#define I2S_CONF_REG         0x008
#define I2S_INT_RAW_REG      0x00C
#define I2S_INT_ST_REG       0x010
#define I2S_INT_ENA_REG      0x014
#define I2S_INT_CLR_REG      0x018
#define I2S_TIMING_REG       0x01C
#define I2S_FIFO_CONF_REG    0x020
#define I2S_RX_EOF_NUM_REG   0x024
#define I2S_CONF_SINGLE_REG  0x028
#define I2S_CONF_CHAN_REG    0x02C
#define I2S_OUT_LINK_REG     0x030
#define I2S_IN_LINK_REG      0x034
#define I2S_CONF2_REG        0x0A8   /* was 0x038, which is OUT_EOF_DES_ADDR: conf2 writes never landed */
#define I2S_LC_CONF_REG      0x060

/* I2S_CONF_REG bits the driver uses. */
#define I2S_CONF_TX_RESET        (1u << 0)
#define I2S_CONF_RX_RESET        (1u << 1)
#define I2S_CONF_TX_FIFO_RESET   (1u << 2)
#define I2S_CONF_RX_FIFO_RESET   (1u << 3)
#define I2S_CONF_TX_START        (1u << 4)
#define I2S_CONF_RX_START        (1u << 5)

/* I2S_CONF2_REG bits. esp32-camera's ll_cam_config() sets CAMERA_EN (and
 * LCD_EN) before it starts a capture; nothing else on the ESP32 does. */
#define I2S_CONF2_CAMERA_EN      (1u << 0)

/* I2S_INT_*_REG bits the driver uses. */
#define I2S_INT_RX_TAKE_DATA     (1u << 0)
#define I2S_INT_IN_DONE          (1u << 8)
#define I2S_INT_IN_SUC_EOF       (1u << 9)
#define I2S_INT_IN_ERR_EOF       (1u << 10)

/* I2S_LC_CONF_REG bits the driver uses. */
#define I2S_LC_IN_RST            (1u << 0)
#define I2S_LC_OUT_RST           (1u << 1)
#define I2S_LC_AHBM_FIFO_RST     (1u << 2)
#define I2S_LC_AHBM_RST          (1u << 3)

/* I2S_IN_LINK_REG bits. */
#define I2S_IN_LINK_ADDR_MASK    0x000FFFFFu
#define I2S_IN_LINK_STOP         (1u << 20)
#define I2S_IN_LINK_PARK         (1u << 21)
#define I2S_IN_LINK_RESTART      (1u << 28)
#define I2S_IN_LINK_START        (1u << 29)

typedef struct Esp32I2sCamState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t conf;
    uint32_t int_raw;
    uint32_t int_ena;
    uint32_t rx_eof_num;
    uint32_t in_link;
    uint32_t lc_conf;
    uint32_t fifo_conf;
    uint32_t conf2;

    /* Address of the head DMA descriptor, masked to 20 bits. The real
     * SoC OR-ins a fixed prefix to address SRAM, which is the same for
     * every ESP32 build the driver targets. */
    uint32_t in_link_addr;

    /* Frame source provided by the host (Phase 3). When NULL, the
     * descriptor walker fills with the idle pattern. */
    const uint8_t *frame_buf;
    size_t        frame_len;
    size_t        frame_pos;        /* read cursor inside frame_buf  */

    /* One-shot EOF timer. Scheduled by vsync_kick_timer for the mid-cycle
     * point (between two VSYNCs). Fires one rx_eof_num-sized chunk +
     * in_suc_eof IRQ. Bails silently if rx_start=0 (e.g. during the
     * brief ll_cam_stop window between frames). */
    QEMUTimer    *eof_timer;

    /* INDEPENDENT VSYNC pulse timer — runs continuously at ~30 fps
     * regardless of rx_start. Real OV2640 generates VSYNC pulses
     * whenever the chip is powered, not only when the SoC has armed
     * its DMA. cam_hal.c's cam_task waits in IDLE for the FIRST VSYNC
     * before calling ll_cam_start (rx_start=1). So if VSYNC depended
     * on rx_start, we'd have a chicken-and-egg deadlock — that's why
     * this is a separate timer that runs from device init. */
    QEMUTimer    *vsync_kick_timer;

    /* Timer that fires the falling edge of the VSYNC pulse 8 ms after
     * we raised it. A back-to-back raise+lower in the same QEMU tick
     * is coalesced and the firmware misses the edge. */
    QEMUTimer    *vsync_fall_timer;

    /* How many more EOFs to fire in this VSYNC cycle. cam_hal.c's
     * cam_task accumulates one chunk's worth of JPEG bytes per EOF
     * (via ll_cam_memcpy with len=dma_half_buffer_size). Reset to
     * ESP32_I2S_CAM_EOFS_PER_FRAME on each vsync_kick. */
    int           eofs_remaining;

    /* Set by eof_timer_cb for the LAST EOF of the burst. Tells the
     * walker to override the final 2 samples with FF D9 (JPEG EOI)
     * so the firmware's cam_verify_jpeg_eoi accepts the frame even
     * when the source JPEG is too large to fit in the deliverable
     * byte budget (typical with real webcam frames at quality > 0.4). */
    bool          inject_eoi_now;

    /* Lap counter inside a single VSYNC cycle. Incremented each time
     * the walker exhausts owner=1 descriptors and force-resets the
     * ring with reset_descriptor_ring() to keep delivering more bytes.
     * Caps at ESP32_I2S_CAM_MAX_LAPS_PER_BURST so a runaway walker
     * can't loop forever. Reset to 0 on every vsync_kick.
     *
     * Without this, the walker stops after one full lap (8 EOFs at
     * the default 16-descriptor ring), capping per-frame delivery at
     * ~8 KiB. With multi-lap support the cap rises to 24 KiB —
     * enough headroom for HD-webcam JPEGs at quality 0.6+. */
    int           laps_in_burst;
} Esp32I2sCamState;

/*
 * Phase-3 hook: called from the picsimlab callback bridge when the
 * backend pushes a new JPEG/RGB565 frame from the browser webcam.
 * Hands ownership of `buf` to the device (it will be freed on the
 * next push). Pass NULL/0 to detach.
 *
 * Implementation note: the device makes a private copy of `buf` so
 * the caller can free its own copy immediately.
 */
void esp32_i2s_cam_push_frame(Esp32I2sCamState *s,
                              const uint8_t *buf, size_t len);

/*
 * Lookup helper for the picsimlab bridge. `picsimlab.callback` knows
 * the SoC instance pointer; we expose a tiny accessor so the bridge
 * can locate the i2s_cam device without a global.
 */
Esp32I2sCamState *esp32_i2s_cam_from_soc(void *esp32_soc);

#endif /* HW_MISC_ESP32_I2S_CAM_H */
