/*
 * ESP32 I²S0 in slave-RX (camera/DVP) mode — peripheral emulation.
 *
 * Big picture (see autosearch/08_dvp_i2s_spec.md for the full spec):
 *
 *   The upstream esp32-camera driver pokes I²S0 in a very specific
 *   sequence to start a frame capture. We model that sequence with
 *   just enough fidelity that the driver doesn't notice it's emulated:
 *
 *     1. driver writes bunch of resets to conf / lc_conf            -- we ack
 *     2. driver sets rx_eof_num = bytes-per-half-buffer / 4         -- we store
 *     3. driver sets in_link.addr = &lldesc[0]                      -- we store
 *     4. driver writes in_link.start = 1                            -- we WALK
 *     5. driver writes conf.rx_start = 1                            -- we ARM
 *     6. on (5) we synthesise frame data into the descriptor chain,
 *        eventually setting in_suc_eof in int_raw and (if int_ena
 *        permits) raising IRQ ETS_I2S0_INTR_SOURCE.
 *     7. driver IRQ handler reads int_st, clears in_suc_eof in
 *        int_clr, processes the buffer, and re-arms.
 *
 *   For the MVP we implement strategy "(A)" from the spec: drop the
 *   entire frame in one go when rx_start is asserted, fire EOF, done.
 *   Strategy "(B)" — pacing via timer to mimic real PCLK timing —
 *   is left as a follow-up; no real sketch needs it.
 *
 *   Pixel-byte padding ("dma_elem_t"):
 *     The I²S bus is 16-bit wide minimum; our 8-bit DVP gets one
 *     pixel-byte per sample with 8 zero-bits of padding. The driver
 *     unpacks via ll_cam_dma_filter_jpeg/yuv422/grayscale callbacks.
 *     For us this means: for every pixel byte we want to deliver,
 *     write 4 bytes to RAM:
 *         byte0 = padding (0x00)
 *         byte1 = pixel byte
 *         byte2 = padding (0x00)
 *         byte3 = pixel byte (the next one)
 *     i.e. each 32-bit word holds two pixel bytes in the high half of
 *     each 16-bit slot. Layout matches dma_elem_t in
 *     wokwi-libs/esp32-camera/target/esp32/ll_cam.c:
 *         { sample2:8, unused2:8, sample1:8, unused1:8 }
 *     so we pack:
 *         word = (pixel1 << 8) | (pixel2 << 24)
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/misc/esp32_i2s_cam.h"
#include "exec/cpu-common.h"
#include "exec/memory.h"
#include "sysemu/dma.h"
#include "migration/vmstate.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ── Debug logger ─────────────────────────────────────────────────── */

/* Optional file logger for I²S device events. fprintf(stderr) from
 * inside a DLL loaded via Python ctypes does NOT reach the parent
 * process's stderr on Windows, so we write to a file when the env
 * var VELXIO_I2S_CAM_LOG is set to a file path.
 *
 * Disabled by default (zero overhead) — set the env var when
 * debugging walker/timer/IRQ behaviour. Was extensively used during
 * the initial bring-up of the OV2640 + I²S DVP path. */
static FILE *_i2s_cam_logf = (FILE *)-1;  /* -1 = uninitialised */
static void i2s_cam_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void i2s_cam_log(const char *fmt, ...)
{
    if (_i2s_cam_logf == (FILE *)-1) {
        const char *path = getenv("VELXIO_I2S_CAM_LOG");
        _i2s_cam_logf = path && *path ? fopen(path, "a") : NULL;
    }
    if (!_i2s_cam_logf) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(_i2s_cam_logf, fmt, ap);
    va_end(ap);
    fflush(_i2s_cam_logf);
}

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Real ESP32 SRAM is at 0x3FFAE000; in_link.addr stores only bits
 * [19:0] and the SoC OR-ins the high bits internally. We mirror that
 * here so descriptor walks land in the correct address range. */
#define ESP32_SRAM_PREFIX 0x3FF00000u

static hwaddr resolve_dma_addr(uint32_t in_link_addr_field)
{
    uint32_t low = in_link_addr_field & I2S_IN_LINK_ADDR_MASK;
    return (hwaddr)(ESP32_SRAM_PREFIX | low);
}

/* lldesc_t layout (12 bytes, packed bitfield in the first word):
 *
 *   word[0] = size:12 | length:12 | offset:5 | sosf:1 | eof:1 | owner:1
 *   word[1] = buf_ptr (32-bit physical addr)
 *   word[2] = next    (32-bit, 0 = end)
 */
typedef struct {
    uint32_t ctrl;
    uint32_t buf_ptr;
    uint32_t next;
} lldesc_words_t;

static void lldesc_unpack_ctrl(uint32_t ctrl,
                               uint32_t *size, uint32_t *length,
                               uint32_t *eof, uint32_t *owner)
{
    *size   = (ctrl >>  0) & 0xFFF;
    *length = (ctrl >> 12) & 0xFFF;
    *eof    = (ctrl >> 30) & 0x1;
    *owner  = (ctrl >> 31) & 0x1;
}

static uint32_t lldesc_pack_ctrl(uint32_t size, uint32_t length,
                                 uint32_t eof, uint32_t owner)
{
    return ((size   & 0xFFF) <<  0)
         | ((length & 0xFFF) << 12)
         | ((eof    & 0x1)   << 30)
         | ((owner  & 0x1)   << 31);
}

/* Compose a 32-bit "dma_elem_t" word containing one JPEG byte at the
 * `sample1` field. The upstream filter
 *   wokwi-libs/esp32-camera/target/esp32/ll_cam.c::ll_cam_dma_filter_jpeg
 * reads only `sample1`, so each 4-byte dma_elem in DMA memory yields
 * exactly ONE real byte to the firmware's frame buffer. Therefore the
 * walker must call next_pixel_byte() ONCE per sample (not twice). The
 * earlier two-arg signature `pack_two_pixels(p1, p2)` discarded p2 but
 * still consumed it from the frame source — silently dropping every
 * other JPEG byte. That made the firmware see garbage even though the
 * bit-packing was right.
 *
 * Layout of dma_elem_t (little-endian uint32):
 *     bits  0-7  : sample2  (unused by JPEG filter)
 *     bits  8-15 : unused2
 *     bits 16-23 : sample1  ← real byte goes here
 *     bits 24-31 : unused1
 */
static inline uint32_t pack_one_pixel(uint8_t p)
{
    return ((uint32_t)p << 16);
}

/* ── Frame source ─────────────────────────────────────────────────── */

void esp32_i2s_cam_push_frame(Esp32I2sCamState *s,
                              const uint8_t *buf, size_t len)
{
    if (s->frame_buf) {
        g_free((void *)s->frame_buf);
        s->frame_buf = NULL;
        s->frame_len = 0;
        s->frame_pos = 0;
    }
    if (buf && len > 0) {
        uint8_t *copy = g_malloc(len);
        memcpy(copy, buf, len);
        s->frame_buf = copy;
        s->frame_len = len;
        s->frame_pos = 0;
    }
}

/* Pop the next pixel byte from the frame source, or 0xAA if no frame
 * is queued (the dma_smoke test idle-pattern). When a real frame ends
 * mid-buffer, we pad with the JPEG End-Of-Image marker (0xFF 0xD9
 * repeating) so the upstream `cam_verify_jpeg_eoi` check in cam_hal.c
 * finds an EOI near the buffer end and accepts the frame. Without
 * this, fb_get() blocks forever because every frame fails validation. */
/* Forward declaration — reset_descriptor_ring is defined below in
 * the lifecycle section (after walk_dma_chain) but walk_dma_chain
 * needs to call it for the multi-lap path. Keeping the definition
 * order to minimise diff against the original layout. */
static void reset_descriptor_ring(Esp32I2sCamState *s);

/* How many EOFs the eof_timer fires per VSYNC cycle. With cam_hal's
 * default 16-descriptor ring (each holding 512 samples = 2048 bytes),
 * one full lap delivers 8 EOFs × 1024 samples = 8192 bytes. Above 8
 * the walker hits owner=0 descriptors from its own previous lap; the
 * multi-lap path force-resets the ring (up to MAX_LAPS_PER_BURST
 * times) so we can deliver up to ~32 KiB per frame. */
#define ESP32_I2S_CAM_EOFS_PER_FRAME     24

/* Maximum number of laps the walker is allowed to make through the
 * descriptor ring within a single VSYNC cycle. 4 laps caps the
 * deliverable budget at ~32 KiB per frame — enough for HD-webcam
 * JPEGs at quality 0.6+. Also prevents a runaway walker from looping
 * indefinitely if the firmware never advances `cnt`. */
#define ESP32_I2S_CAM_MAX_LAPS_PER_BURST 4

static uint8_t next_pixel_byte(Esp32I2sCamState *s)
{
    if (s->frame_buf && s->frame_pos < s->frame_len) {
        return s->frame_buf[s->frame_pos++];
    }
    if (s->frame_buf) {
        /* JPEG queued but exhausted — fill the rest of the buffer with
         * a repeating EOI pattern. cam_verify_jpeg_eoi scans backwards
         * from buffer-end looking for 0xFF 0xD9; this guarantees a hit. */
        uint8_t pad = (s->frame_pos & 1) ? 0xD9u : 0xFFu;
        s->frame_pos++;
        return pad;
    }
    return 0xAAu;
}

/* ── Descriptor walker ────────────────────────────────────────────── */

/*
 * Walk the descriptor ring starting at `head_addr` and deliver
 * `target_samples` samples total across however many owner=1
 * descriptors are needed. Real ESP32 I²S DMA fires `in_suc_eof` only
 * after `rx_eof_num` samples have been written, which can span
 * multiple descriptors when each descriptor's size is smaller than
 * the half-buffer.
 *
 * Reference: `wokwi-libs/esp32-camera/target/esp32/ll_cam.c:287`
 *   `I2S0.rx_eof_num = cam->dma_half_buffer_size / sizeof(dma_elem_t);`
 *
 * Concretely, with cam_hal's defaults (dma_node_buffer_size=2048,
 * dma_half_buffer_size=4096, sizeof(dma_elem_t)=4) we have:
 *   - Each descriptor: size=2048 bytes → capacity 512 samples
 *   - rx_eof_num = 4096/4 = 1024 samples per EOF
 *   - So one EOF spans TWO descriptors.
 *
 * Algorithm:
 *   1. Walk the ring until we find the first owner=1 descriptor.
 *   2. Fill descriptors in order, accumulating samples_written.
 *      Each descriptor we touch becomes owner=0, length=bytes_written.
 *   3. When samples_written >= target_samples, set eof=1 on the LAST
 *      descriptor we filled, fire in_suc_eof IRQ, return.
 *   4. If we run out of owner=1 descriptors before reaching target,
 *      don't fire EOF — try again on the next tick. The descriptors
 *      we already filled retain their data so the next walk continues
 *      from the next owner=1 slot in the ring.
 *
 * If the entire ring is owner=0 (driver still processing), we
 * silently return — try again on the next tick.
 */
static void walk_dma_chain(Esp32I2sCamState *s,
                           hwaddr head_addr,
                           uint32_t target_samples)
{
    if (target_samples == 0) {
        /* Driver hasn't set rx_eof_num yet — nothing to do. */
        return;
    }

    /* ── Step 1: scan the ring for the first owner=1 descriptor. ─────
     * If we don't find one (every descriptor is owner=0 from a
     * previous lap that fully covered the ring) AND we still have lap
     * budget for this VSYNC cycle, force-reset the ring back to all
     * owner=1 and retry. This lets one frame deliver more bytes than
     * the ring physically holds (16 descs × 512 samples = 8192 bytes
     * single-lap; multi-lap raises the cap to 32 KiB at MAX_LAPS=4). */
    hwaddr desc_addr;
    lldesc_words_t d;
    bool   found = false;

    while (!found) {
        desc_addr = head_addr;
        int hop_guard = 32;
        bool wrapped_without_owner = false;
        while (hop_guard-- > 0) {
            if (dma_memory_read(&address_space_memory, desc_addr,
                                &d, sizeof(d),
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                return;
            }
            uint32_t size, length, eof, owner;
            lldesc_unpack_ctrl(d.ctrl, &size, &length, &eof, &owner);
            if (owner) {
                found = true;
                break;
            }
            if (d.next == 0 || d.next == head_addr) {
                wrapped_without_owner = true;
                break;
            }
            desc_addr = d.next;
        }
        if (found) break;
        if (!wrapped_without_owner) return;  /* hop_guard exhausted */

        /* Whole ring is owner=0. Try a force-reset if we still have
         * lap budget for this VSYNC. */
        if (s->laps_in_burst >= ESP32_I2S_CAM_MAX_LAPS_PER_BURST) {
            return;  /* hard cap — wait for next vsync_kick */
        }
        s->laps_in_burst++;
        reset_descriptor_ring(s);
        /* Loop reruns the scan from head_addr, which now finds
         * descriptor 0 with owner=1. */
    }

    /* ── Step 2: fill descriptors until we've delivered target_samples ─ */
    uint32_t samples_written = 0;
    hwaddr last_filled_addr = 0;
    lldesc_words_t last_filled_desc;
    uint32_t last_filled_bytes = 0;
    int fill_guard = 32;

    while (samples_written < target_samples && fill_guard-- > 0) {
        uint32_t size = (d.ctrl >> 0) & 0xFFF;
        uint32_t cap_samples = size / 4;
        uint32_t remaining = target_samples - samples_written;
        uint32_t will_fill = (cap_samples < remaining) ? cap_samples : remaining;

        for (uint32_t i = 0; i < will_fill; i++) {
            /* ONE byte per sample — see pack_one_pixel comment above. */
            uint8_t p = next_pixel_byte(s);
            /* On the LAST EOF of the VSYNC burst, override the final
             * 2 samples with the JPEG EOI marker FF D9. Guarantees
             * cam_verify_jpeg_eoi accepts the frame even if the source
             * JPEG was larger than the deliverable byte budget. The
             * walker only knows it's on the final descriptor when
             * samples_written + i covers the last 2 samples of this
             * EOF AND inject_eoi_now is set AND we're on the last
             * descriptor we'll fill (i.e. the threshold sample). */
            if (s->inject_eoi_now && (samples_written + i + 2 >= target_samples)) {
                p = ((samples_written + i + 1 == target_samples) ? 0xD9u : 0xFFu);
            }
            uint32_t word = pack_one_pixel(p);
            hwaddr off = (hwaddr)i * 4;
            if (dma_memory_write(&address_space_memory,
                                 d.buf_ptr + off,
                                 &word, sizeof(word),
                                 MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                return;
            }
        }

        /* Hand THIS descriptor back to CPU. EOF flag goes only on the
         * one where we cross the target_samples threshold (set below). */
        last_filled_addr = desc_addr;
        last_filled_desc = d;
        last_filled_bytes = will_fill * 4;
        d.ctrl = lldesc_pack_ctrl(size,
                                  will_fill * 4,
                                  0 /* eof later, only on final desc */,
                                  0 /* CPU owns it now */);
        if (dma_memory_write(&address_space_memory, desc_addr,
                             &d, sizeof(d.ctrl),
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return;
        }
        samples_written += will_fill;

        if (samples_written >= target_samples) {
            break;
        }

        /* Advance to next descriptor in ring. */
        if (d.next == 0) return;  /* end of chain — shouldn't happen */
        desc_addr = d.next;
        if (dma_memory_read(&address_space_memory, desc_addr,
                            &d, sizeof(d),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return;
        }
        uint32_t nsize, nlength, neof, nowner;
        lldesc_unpack_ctrl(d.ctrl, &nsize, &nlength, &neof, &nowner);
        if (!nowner) {
            /* Hit a CPU-owned descriptor mid-fill. If we still have
             * lap budget, reset the ring (overwriting the previous
             * lap's bytes that the firmware has already read), reload
             * descriptor at head_addr, and continue the burst. */
            if (s->laps_in_burst >= ESP32_I2S_CAM_MAX_LAPS_PER_BURST) {
                return;  /* hard cap reached — emit partial EOF below */
            }
            s->laps_in_burst++;
            reset_descriptor_ring(s);
            desc_addr = head_addr;
            if (dma_memory_read(&address_space_memory, desc_addr,
                                &d, sizeof(d),
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                return;
            }
        }
    }

    if (samples_written < target_samples || last_filled_addr == 0) {
        /* Didn't reach target — already returned above without IRQ. */
        static unsigned partial_cnt = 0;
        if (partial_cnt++ < 10) {
            i2s_cam_log( "[i2s_cam] walker partial: written=%u target=%u\n",
                    samples_written, target_samples);
        }
        return;
    }

    {
        static unsigned eof_irq_cnt = 0;
        if (eof_irq_cnt < 10 || (eof_irq_cnt % 30) == 0) {
            i2s_cam_log( "[i2s_cam] walker fired EOF IRQ #%u "
                    "(samples=%u, last_addr=0x%llx)\n",
                    eof_irq_cnt, samples_written,
                    (unsigned long long)last_filled_addr);
        }
        eof_irq_cnt++;
    }

    /* ── Step 3: re-mark the LAST descriptor with EOF=1 + raise IRQ. ─ */
    uint32_t lsize = (last_filled_desc.ctrl >> 0) & 0xFFF;
    last_filled_desc.ctrl = lldesc_pack_ctrl(lsize,
                                             last_filled_bytes,
                                             1 /* eof */,
                                             0 /* CPU owns */);
    if (dma_memory_write(&address_space_memory, last_filled_addr,
                         &last_filled_desc, sizeof(last_filled_desc.ctrl),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }

    s->int_raw |= I2S_INT_IN_SUC_EOF;
    qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
}

/* Frame cadence: one VSYNC every 100 ms (10 fps). Between VSYNCs we
 * fire ESP32_I2S_CAM_EOFS_PER_FRAME EOFs spaced ESP32_I2S_CAM_EOF_INTERVAL
 * apart, starting ESP32_I2S_CAM_EOF_DELAY_NS after the VSYNC.
 *
 * Each EOF delivers `rx_eof_num` samples = 1 byte/sample = 1024 real
 * bytes (with default cam_hal config). For a typical QVGA JPEG of
 * 4-6 KiB, we need 5-6 EOFs to fit SOI...EOI in the framebuffer
 * (cam_verify_jpeg_eoi scans BACKWARD from buffer end for FF D9,
 * so the EOI must land in the buffer for cam_take to return the
 * frame). 6 gives us comfortable margin. */
#define ESP32_I2S_CAM_VSYNC_PERIOD_NS    (100 * 1000 * 1000)
#define ESP32_I2S_CAM_EOF_DELAY_NS       (  4 * 1000 * 1000)
#define ESP32_I2S_CAM_EOF_INTERVAL_NS    (  4 * 1000 * 1000)
#define ESP32_I2S_CAM_VSYNC_LOW_NS       (  8 * 1000 * 1000)
/* 8 EOFs × 1024 samples = 8192 bytes per frame (the upstream cam_hal
 * default ring of 16 descriptors at 512 samples each tops out at 8
 * EOFs before the walker bails on owner=0 from its own previous
 * lap). Plus 1 final memcpy on VSYNC = 9216 bytes effective.
 *
 * Real webcam JPEGs at QVGA tend to be 5-15 KiB depending on quality.
 * Anything above 9 KiB gets truncated, but truncation is fine because
 * we INJECT FF D9 in the last samples of the last EOF — see the
 * `inject_eoi_now` flag handling in walk_dma_chain. The firmware's
 * cam_verify_jpeg_eoi finds the marker, accepts the frame, and the
 * user-side JPEG decoder either renders the partial image or fails
 * gracefully (most decoders are tolerant).
 *
 * Note: ESP32_I2S_CAM_EOFS_PER_FRAME and MAX_LAPS_PER_BURST were
 * moved up near the top of the file (next to the forward decl of
 * reset_descriptor_ring) so walk_dma_chain — which appears earlier
 * in the source — can reference them. */

/* AI-Thinker ESP32-CAM VSYNC pin is GPIO 25. cam_hal.c's framectrl
 * task waits for a VSYNC GPIO interrupt before transitioning from
 * IDLE → READ_BUF state, so without VSYNC pulses the driver ignores
 * every in_suc_eof we fire. Default driver config uses
 * vsync_invert=true which means NEGEDGE triggers CAM_VSYNC_EVENT.
 *
 * picsimlab indexes pin_irq[] by SLOT, where slot = gpio + 1 (the
 * worker's identity pinmap [_PINMAP[i+1] = i]). So GPIO 25 lives at
 * slot 26. */
#define ESP32_I2S_CAM_VSYNC_GPIO        25
#define ESP32_I2S_CAM_VSYNC_PIN_SLOT    (ESP32_I2S_CAM_VSYNC_GPIO + 1)
extern void qemu_picsimlab_set_pin(int pin, int value);

/* The camera sensor is a part wired to the board, not a block inside the
 * SoC. This device is created on EVERY esp32-picsimlab machine (a DevKit,
 * a WROVER, an ESP32-CAM alike), so it must only drive the VSYNC pad once
 * the firmware has actually brought up a camera, which on the ESP32 means
 * esp32-camera's ll_cam_config() setting I2S0.conf2.camera_en.
 *
 * Before this gate the kick timer pulsed GPIO25 LOW for 8 ms every 100 ms
 * from 500 ms after power-on, forever, on every ESP32. qemu_picsimlab_set_pin
 * writes the same GPIO_IN bit the host's own injection writes, so a button,
 * switch or sensor on GPIO25 read a phantom LOW about one read in twelve,
 * and each pulse ended by forcing the pad HIGH over whatever the circuit
 * held it at (velxio issue #333: "Conflicting inputs" with no input). */
static bool camera_enabled(const Esp32I2sCamState *s)
{
    return (s->conf2 & I2S_CONF2_CAMERA_EN) != 0;
}

/* Walk one chunk's worth of samples and fire EOF. If rx_start=0
 * (driver isn't listening) or no descriptor is owner=1 (driver still
 * processing), the walker is a no-op for this tick — next tick tries
 * again. */
static void produce_one_chunk(Esp32I2sCamState *s)
{
    if (!(s->conf & I2S_CONF_RX_START)) return;
    if (!(s->in_link & I2S_IN_LINK_START)) return;

    walk_dma_chain(s, resolve_dma_addr(s->in_link), s->rx_eof_num);
}

/* eof_timer fires multiple times per VSYNC cycle to deliver enough
 * JPEG bytes for cam_verify_jpeg_eoi to find FF D9. Each tick:
 *   1. Walks one descriptor-half-buffer of JPEG bytes into DMA memory.
 *   2. Raises in_suc_eof (cam_task copies to fb + accumulates len).
 *   3. Re-arms itself if eofs_remaining > 0.
 * Once eofs_remaining hits 0, the timer stops; the next vsync_kick
 * resets it to ESP32_I2S_CAM_EOFS_PER_FRAME. */
static void eof_timer_cb(void *opaque)
{
    Esp32I2sCamState *s = ESP32_I2S_CAM(opaque);
    static unsigned eof_count = 0;
    bool armed = !!(s->conf & I2S_CONF_RX_START);
    if (eof_count < 40 || (eof_count % 60) == 0) {
        i2s_cam_log("[i2s_cam] eof_tick #%u armed=%d remaining=%d "
                "rx_eof_num=%u frame_pos=%zu\n",
                eof_count, armed, s->eofs_remaining,
                s->rx_eof_num, s->frame_pos);
    }
    eof_count++;
    /* Inject EOI on the FINAL EOF of this VSYNC burst. This guarantees
     * cam_verify_jpeg_eoi finds FF D9 in the framebuffer regardless of
     * how large the source JPEG was. */
    s->inject_eoi_now = (s->eofs_remaining == 1);
    produce_one_chunk(s);
    s->inject_eoi_now = false;

    /* Re-arm for next EOF in this VSYNC's burst. */
    if (s->eofs_remaining > 0) {
        s->eofs_remaining--;
        if (s->eofs_remaining > 0) {
            timer_mod(s->eof_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                      + ESP32_I2S_CAM_EOF_INTERVAL_NS);
        }
    }
}

/* Re-raise VSYNC to HIGH (idle level) ~8 ms after we dropped it.
 * Lets the line go back to its resting state so the next NEGEDGE has
 * something to fall from. */
static void vsync_fall_cb(void *opaque)
{
    (void)opaque;
    qemu_picsimlab_set_pin(ESP32_I2S_CAM_VSYNC_PIN_SLOT, 1);
}

/* Pulse VSYNC at the start of a new frame. Real OV2640 holds VSYNC
 * HIGH between frames and pulses LOW at frame boundaries (when
 * vsync_invert=true, which is the cam_hal default). The driver's
 * GPIO ISR fires on NEGEDGE, then ll_cam_vsync_isr reads the level
 * and pushes CAM_VSYNC_EVENT iff level == LOW.
 *
 * Sequence: drop to LOW (fires CAM_VSYNC_EVENT), schedule raise back
 * to HIGH ~8 ms later so the next pulse has a clean falling edge. */
static void pulse_vsync_low(Esp32I2sCamState *s)
{
    qemu_picsimlab_set_pin(ESP32_I2S_CAM_VSYNC_PIN_SLOT, 0);
    if (s->vsync_fall_timer) {
        timer_mod(s->vsync_fall_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + ESP32_I2S_CAM_VSYNC_LOW_NS);
    }
}

/* INDEPENDENT VSYNC kick — runs continuously at ~30 fps regardless of
 * rx_start. Each tick:
 *   1. Resets frame_pos to 0 so the next EOF chunk starts at JPEG SOI.
 *   2. Pulses VSYNC LOW (NEGEDGE) so cam_task advances its state machine.
 *   3. Schedules the eof_timer for mid-cycle (16 ms later).
 *   4. Reschedules itself for the next VSYNC (32 ms from now).
 *
 * Why a separate timer (instead of folding VSYNC into eof_timer):
 * the firmware's cam_task is initially in IDLE waiting for the FIRST
 * VSYNC. ll_cam_start (which writes rx_start=1) only happens when
 * cam_task receives that first VSYNC. So the FIRST VSYNC must fire
 * *before* rx_start can possibly be 1 — chicken-and-egg if the timer
 * is gated on rx_start. Decoupling them is the only way out. */
static void vsync_kick_cb(void *opaque)
{
    Esp32I2sCamState *s = ESP32_I2S_CAM(opaque);
    static unsigned tick_count = 0;
    if (tick_count < 20 || (tick_count % 30) == 0) {
        i2s_cam_log( "[i2s_cam] vsync_kick #%u "
                "rx_start=%d in_link_start=%d rx_eof_num=%u frame_buf=%p frame_pos=%zu\n",
                tick_count,
                !!(s->conf & I2S_CONF_RX_START),
                !!(s->in_link & I2S_IN_LINK_START),
                s->rx_eof_num,
                (void*)s->frame_buf,
                s->frame_pos);
        fflush(stderr);
    }
    /* No camera brought up: keep ticking so the first VSYNC after
     * ll_cam_config() still comes within one period, but leave the pad
     * and the DMA alone. */
    if (!camera_enabled(s)) {
        timer_mod(s->vsync_kick_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + ESP32_I2S_CAM_VSYNC_PERIOD_NS);
        return;
    }
    tick_count++;
    s->frame_pos = 0;
    s->eofs_remaining = ESP32_I2S_CAM_EOFS_PER_FRAME;
    /* Reset the lap counter so the walker can re-walk the descriptor
     * ring up to MAX_LAPS_PER_BURST times within this VSYNC cycle.
     * See walk_dma_chain for the multi-lap logic. */
    s->laps_in_burst = 0;
    pulse_vsync_low(s);
    /* Schedule the first EOF of the burst. eof_timer self-rearms
     * EOFS_PER_FRAME times then stops until the next vsync_kick. */
    if (s->eof_timer) {
        timer_mod(s->eof_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + ESP32_I2S_CAM_EOF_DELAY_NS);
    }
    /* Self-rearm. */
    timer_mod(s->vsync_kick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + ESP32_I2S_CAM_VSYNC_PERIOD_NS);
}

/* Stop all timers (device reset / deinit). The independent VSYNC kick
 * timer is normally NOT stopped — it runs from device init to forever
 * because the firmware's cam_task could enable VSYNC interrupts at any
 * time after boot. */
static void cancel_eof(Esp32I2sCamState *s)
{
    if (s->eof_timer) timer_del(s->eof_timer);
    if (s->vsync_fall_timer) timer_del(s->vsync_fall_timer);
}

/* Reset every descriptor in the ring to owner=1, length=0, eof=0.
 * Real ESP32 I²S DMA can write to descriptors regardless of owner
 * state — it checks owner at "buffer-empty" boundaries. The cam_hal
 * driver therefore never writes owner=1 back after consuming a
 * descriptor; it relies on the hardware's wraparound semantics.
 *
 * In our emulation we explicitly check owner=1 (so the walker won't
 * clobber unread CPU data on first lap), but we never advance the
 * ring back to owner=1, so after one lap every descriptor is owner=0
 * forever and the walker bails. Calling this on every ll_cam_start
 * (rx_start 0→1 edge) restores the "fresh capture" state.
 *
 * cam_hal calls ll_cam_start on every frame boundary (cam_start_frame
 * → ll_cam_start), so this resets every frame — matching what real
 * hardware does when the I²S engine restarts a capture. */
static void reset_descriptor_ring(Esp32I2sCamState *s)
{
    if (!(s->in_link & I2S_IN_LINK_START)) return;
    hwaddr head = resolve_dma_addr(s->in_link);
    hwaddr cur = head;
    int hop_guard = 32;
    while (hop_guard-- > 0) {
        lldesc_words_t d;
        if (dma_memory_read(&address_space_memory, cur, &d, sizeof(d),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return;
        }
        uint32_t size = (d.ctrl >> 0) & 0xFFF;
        d.ctrl = lldesc_pack_ctrl(size, 0 /* length */,
                                   0 /* eof */, 1 /* device owns */);
        if (dma_memory_write(&address_space_memory, cur, &d.ctrl,
                             sizeof(d.ctrl),
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return;
        }
        if (d.next == 0 || d.next == head) return;
        cur = d.next;
    }
}

/* Called when rx_start transitions 0→1. Resets the descriptor ring
 * so the walker has fresh owner=1 descriptors to fill. */
static void maybe_arm_capture(Esp32I2sCamState *s)
{
    if (!(s->conf & I2S_CONF_RX_START)) return;
    if (!(s->in_link & I2S_IN_LINK_START)) return;
    reset_descriptor_ring(s);
    i2s_cam_log("[i2s_cam] reset descriptor ring (rx_start armed)\n");
}

/* ── MMIO read/write ──────────────────────────────────────────────── */

static uint64_t esp32_i2s_cam_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32I2sCamState *s = ESP32_I2S_CAM(opaque);

    switch (addr) {
    case I2S_CONF_REG:        return s->conf;
    case I2S_INT_RAW_REG:     return s->int_raw;
    case I2S_INT_ST_REG:      return s->int_raw & s->int_ena;
    case I2S_INT_ENA_REG:     return s->int_ena;
    case I2S_INT_CLR_REG:     return 0;       /* WO */
    case I2S_RX_EOF_NUM_REG:  return s->rx_eof_num;
    case I2S_IN_LINK_REG:     return s->in_link;
    case I2S_LC_CONF_REG:     return s->lc_conf;
    case I2S_FIFO_CONF_REG:   return s->fifo_conf;
    case I2S_CONF2_REG:       return s->conf2;
    default:
        return 0;
    }
}

static void esp32_i2s_cam_write(void *opaque, hwaddr addr,
                                uint64_t val, unsigned size)
{
    Esp32I2sCamState *s = ESP32_I2S_CAM(opaque);
    uint32_t v = (uint32_t)val;

    switch (addr) {
    case I2S_CONF_REG: {
        bool was_rx_start = !!(s->conf & I2S_CONF_RX_START);
        bool now_rx_start = !!(v & I2S_CONF_RX_START);
        s->conf = v;
        /* Resets are self-clearing-ish in real HW; the driver writes
         * 1 then 0. We honour both by just storing the value — the
         * `0` write afterwards drops the bit. The reset itself is a
         * no-op for our model since there's no internal FIFO state
         * the driver could observe. */
        if (now_rx_start && !was_rx_start) {
            i2s_cam_log( "[i2s_cam] MMIO conf rx_start: 0->1\n");
            maybe_arm_capture(s);
        } else if (!now_rx_start && was_rx_start) {
            i2s_cam_log( "[i2s_cam] MMIO conf rx_start: 1->0\n");
            cancel_eof(s);
        }
        break;
    }
    case I2S_INT_RAW_REG:
        s->int_raw = v;        /* not normally writable; harmless */
        qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
        break;
    case I2S_INT_ENA_REG:
        s->int_ena = v;
        qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
        break;
    case I2S_INT_CLR_REG:
        s->int_raw &= ~v;      /* RW1C */
        qemu_set_irq(s->irq, !!(s->int_raw & s->int_ena));
        break;
    case I2S_RX_EOF_NUM_REG:
        s->rx_eof_num = v;
        i2s_cam_log( "[i2s_cam] MMIO write rx_eof_num=%u\n", v);
        break;
    case I2S_IN_LINK_REG: {
        bool starting = (v & I2S_IN_LINK_START) && !(s->in_link & I2S_IN_LINK_START);
        s->in_link = v;
        /* If the driver toggled start AND rx_start is already on,
         * the capture path can begin immediately. The usual driver
         * order (start then rx_start) is handled by the rx_start
         * branch above; this path covers the unusual order. */
        if (starting) {
            maybe_arm_capture(s);
        }
        break;
    }
    case I2S_LC_CONF_REG:
        s->lc_conf = v;
        /* Reset bits don't persist long enough to matter to us. */
        break;
    case I2S_FIFO_CONF_REG:
        s->fifo_conf = v;
        break;
    case I2S_CONF2_REG: {
        bool was_cam = camera_enabled(s);
        s->conf2 = v;
        i2s_cam_log("[i2s_cam] MMIO conf2=0x%x (camera_en %d->%d)\n", v, was_cam, camera_enabled(s));
        if (camera_enabled(s) && !was_cam) {
            /* The sensor is up: its VSYNC idles HIGH (vsync_invert=true
             * means LOW = active), which is where the first NEGEDGE
             * falls from. */
            qemu_picsimlab_set_pin(ESP32_I2S_CAM_VSYNC_PIN_SLOT, 1);
        } else if (!camera_enabled(s) && was_cam) {
            cancel_eof(s);
        }
        break;
    }
    default:
        /* Silently accept unknown writes — many of them are timing
         * configuration that doesn't affect our model. The driver
         * writes ~30 different registers in init and most are
         * irrelevant here. */
        break;
    }
}

static const MemoryRegionOps esp32_i2s_cam_ops = {
    .read = esp32_i2s_cam_read,
    .write = esp32_i2s_cam_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static void esp32_i2s_cam_reset(DeviceState *dev)
{
    Esp32I2sCamState *s = ESP32_I2S_CAM(dev);
    s->conf       = 0;
    s->int_raw    = 0;
    s->int_ena    = 0;
    s->rx_eof_num = 0;
    s->in_link    = 0;
    s->lc_conf    = 0;
    s->fifo_conf  = 0;
    s->conf2      = 0;
    if (s->eof_timer) timer_del(s->eof_timer);
    if (s->vsync_fall_timer) timer_del(s->vsync_fall_timer);
    /* Don't free frame_buf on reset — the host frame source is
     * external state and persists across guest resets. */
    qemu_set_irq(s->irq, 0);
}

static void esp32_i2s_cam_realize(DeviceState *dev, Error **errp)
{
    Esp32I2sCamState *s = ESP32_I2S_CAM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    i2s_cam_log( "[i2s_cam] realize: instantiating device\n");

    memory_region_init_io(&s->iomem, OBJECT(s), &esp32_i2s_cam_ops, s,
                          TYPE_ESP32_I2S_CAM, ESP32_I2S_CAM_REGION_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->eof_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, eof_timer_cb, s);
    s->vsync_fall_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vsync_fall_cb, s);
    s->vsync_kick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vsync_kick_cb, s);

    /* Start the VSYNC kick timer immediately. The first pulse fires
     * 500 ms after device init — enough time for the firmware to boot
     * and reach esp_camera_init(), but not so long that fb_get() in
     * the user's setup() blocks past the typical 5 s timeout. Stray
     * pulses before cam_hal registers its GPIO ISR are dropped at the
     * peripheral level (no ISR registered → edges go nowhere). */
    timer_mod(s->vsync_kick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500000000ULL);

    /* The VSYNC pad is not touched here: see camera_enabled(). It is set
     * to its idle HIGH when the firmware sets conf2.camera_en. */

    i2s_cam_log( "[i2s_cam] realize: timers armed, vsync_kick at +500ms\n");
}

static const VMStateDescription esp32_i2s_cam_vmstate = {
    .name = TYPE_ESP32_I2S_CAM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(conf,       Esp32I2sCamState),
        VMSTATE_UINT32(int_raw,    Esp32I2sCamState),
        VMSTATE_UINT32(int_ena,    Esp32I2sCamState),
        VMSTATE_UINT32(rx_eof_num, Esp32I2sCamState),
        VMSTATE_UINT32(in_link,    Esp32I2sCamState),
        VMSTATE_UINT32(lc_conf,    Esp32I2sCamState),
        VMSTATE_UINT32(fifo_conf,  Esp32I2sCamState),
        VMSTATE_UINT32(conf2,      Esp32I2sCamState),
        /* frame_buf is host-owned and not migrated. */
        VMSTATE_END_OF_LIST()
    }
};

static void esp32_i2s_cam_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = esp32_i2s_cam_realize;
    device_class_set_legacy_reset(dc, esp32_i2s_cam_reset);
    dc->vmsd    = &esp32_i2s_cam_vmstate;
    dc->desc    = "ESP32 I²S0 in slave-RX (camera/DVP) mode";
}

static const TypeInfo esp32_i2s_cam_info = {
    .name          = TYPE_ESP32_I2S_CAM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32I2sCamState),
    .class_init    = esp32_i2s_cam_class_init,
};

static void esp32_i2s_cam_register_types(void)
{
    type_register_static(&esp32_i2s_cam_info);
}

type_init(esp32_i2s_cam_register_types)
