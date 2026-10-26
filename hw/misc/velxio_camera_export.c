/*
 * velxio_camera_export.c — public C symbol for host-side frame
 * injection into the ESP32-CAM I²S peripheral.
 *
 * The Velxio Python worker (backend/app/services/esp32_worker.py)
 * loads libqemu-xtensa via ctypes and calls
 *   velxio_push_camera_frame(buf, len)
 * to deliver a JPEG/RGB565 frame from the browser webcam. We forward
 * the call to the registered Esp32I2sCamState instance.
 *
 * This file lives outside esp32_i2s_cam.c so that:
 *   1. The symbol is unconditionally exported (not behind a CONFIG_ ifdef).
 *   2. It can locate the device through the QEMU object tree without
 *      polluting the device's own header with a global.
 *
 * License: GPL v2+ (matches the rest of the QEMU fork).
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "qemu/main-loop.h"
#include "hw/misc/esp32_i2s_cam.h"

/* Single global pointer: the ESP32 SoC creates exactly one I²S camera
 * device per machine, so a global is sufficient. Set during machine
 * init in esp32_picsimlab.c just after the device is instantiated. */
static Esp32I2sCamState *g_velxio_i2s_cam = NULL;

void velxio_register_i2s_cam(Esp32I2sCamState *s);
void velxio_push_camera_frame(const uint8_t *buf, size_t len);

void velxio_register_i2s_cam(Esp32I2sCamState *s)
{
    g_velxio_i2s_cam = s;
}

/* Exported. The Python worker holds the iothread lock around the call. */
void velxio_push_camera_frame(const uint8_t *buf, size_t len)
{
    if (!g_velxio_i2s_cam) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velxio_push_camera_frame: i2s_cam not registered "
                      "(machine has no esp32-cam I²S device)\n");
        return;
    }
    esp32_i2s_cam_push_frame(g_velxio_i2s_cam, buf, len);
}
