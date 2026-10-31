# Corresponding source for the Velxio libqemu binaries

This archive is the complete corresponding source, under GPL-2.0, for the
prebuilt QEMU libraries Velxio distributes. It is offered to everyone who
receives one of those binaries, with no further conditions attached.

- Commit: `985803692859cf8f08a48e4f13bbf95e8a6e6798`
- Commit date: 2026-10-31
- Subject: fix(esp32s3_gpio): the S3 GPIO block uses the S3's register map, and its interrupt reaches the CPU
- Repository: a Velxio fork of https://github.com/lcgamboa/qemu
  (branch `picsimlab-esp32`), which is itself a fork of QEMU.

## Which binaries this corresponds to

- `libqemu-xtensa-amd64` (version 1.2.10)
- `libqemu-xtensa-arm64` (version 1.2.10)
- `libqemu-xtensa-macos-arm64` (version 1.2.10)

Those assets are served from
`https://velxio.dev/api/pro/license/downloads/<asset>?key=<your key>`.
A free personal key at https://velxio.dev/license/signup fetches both the
binaries and this source.

## An upstream clone will not substitute for this

Building https://github.com/lcgamboa/qemu will not produce a drop-in
replacement, so build from this archive rather than from a fresh upstream
checkout. The differences that matter:

- Velxio instantiates `esp32s3-picsimlab`, a machine variant that wires the
  host-side GPIO, SPI, I2C and UART callbacks the Python worker binds to.
  Upstream registers Espressif's plain `-M esp32s3` and has no such machine,
  so a stock build starts and then fails to find the machine Velxio asks for.
- Several device models exist only in this tree, among them
  `hw/ssi/esp32s3_gpspi.c` (the S3's GPSPI2 controller, which the plain
  machine leaves unimplemented, so SPI displays have no controller to talk
  to), `hw/i2c/esp32_ov2640.c`, `hw/misc/esp32_i2s_cam.c` and
  `hw/misc/velxio_camera_export.c` (the ESP32-CAM path).
- The STM32 side adds `hw/i2c/stm32_i2c.c`, `hw/misc/stm32_rcc_min.c`,
  `hw/misc/stm32_dwt.c` and the `stm32_picsimlab` host hooks.

Files this tree changes or adds relative to `lcgamboa/qemu picsimlab-esp32`:

  .github/workflows/build-libqemu.yml
  .github/workflows/build.yml
  .gitignore
  Dockerfile.build
  build_libqemu-arm.sh
  build_libqemu-esp32-win.sh
  build_libqemu-esp32.sh
  ci/upload-release-assets.sh
  doc/wiki/esp32-wifi-emulation-fixes.md
  hw/arm/Kconfig
  hw/arm/armv7m.c
  hw/arm/meson.build
  hw/arm/stm32_picsimlab.c
  hw/arm/stm32f100_soc.c
  hw/arm/stm32f205_soc.c
  hw/arm/stm32f405_soc.c
  hw/char/stm32f2xx_usart.c
  hw/display/rgb_led.c
  hw/gpio/Kconfig
  hw/gpio/esp32_gpio.c
  hw/gpio/esp32c3_gpio.c
  hw/gpio/esp32s3_gpio.c
  hw/gpio/meson.build
  hw/gpio/stm32_gpio.c
  hw/i2c/Kconfig
  hw/i2c/esp32_ov2640.c
  hw/i2c/meson.build
  hw/i2c/picsimlab_i2c.c
  hw/i2c/stm32_i2c.c
  hw/misc/Kconfig
  hw/misc/esp32_ana.c
  hw/misc/esp32_dport.c
  hw/misc/esp32_i2s_cam.c
  hw/misc/esp32_phya.c
  hw/misc/esp32_sens.c
  hw/misc/esp32_wifi.c
  hw/misc/esp32_wifi_ap.c
  hw/misc/esp32_wlan_packet.c
  hw/misc/esp32c3_saradc.c
  hw/misc/esp32c3_wifi.c
  hw/misc/meson.build
  hw/misc/stm32_dwt.c
  hw/misc/stm32_rcc_min.c
  hw/misc/velxio_camera_export.c
  hw/nvram/esp_efuse.c
  hw/riscv/esp32c3_picsimlab.c
  hw/ssi/Kconfig
  hw/ssi/esp32_rmt.c
  hw/ssi/esp32_spi.c
  hw/ssi/esp32s3_gpspi.c
  hw/ssi/meson.build
  hw/timer/esp_systimer.c
  hw/xtensa/esp32_picsimlab.c
  hw/xtensa/esp32s3.c
  include/hw/arm/stm32_picsimlab.h
  include/hw/arm/stm32f100_soc.h
  include/hw/arm/stm32f205_soc.h
  include/hw/arm/stm32f405_soc.h
  include/hw/char/stm32f2xx_usart.h
  include/hw/gpio/esp32_gpio.h
  include/hw/gpio/stm32_gpio.h
  include/hw/i2c/esp32_ov2640.h
  include/hw/i2c/stm32_i2c.h
  include/hw/misc/esp32_i2s_cam.h
  include/hw/misc/esp32_sens.h
  include/hw/misc/esp32c3_saradc.h
  include/hw/misc/stm32_rcc_min.h
  include/hw/timer/esp_systimer.h
  meson.build

## Building

`.github/workflows/build-libqemu.yml` in this archive is the workflow that
produced the distributed binaries, and the scripts under
`.github/workflows/scripts/` are the exact configure and prerequisite steps
for each platform, Windows MSYS2 and macOS included. A prose walkthrough
lives in `docs/BUILD-QEMU.md` in the Velxio repository:
https://github.com/davidmonterocrespo24/velxio/blob/master/docs/BUILD-QEMU.md

The build produces `libqemu-xtensa` (ESP32, ESP32-S3, ESP32-CAM, Arduino
Nano ESP32), `libqemu-riscv32` (ESP32-C3, ESP32-C6, XIAO ESP32-C3,
CH32V003) and `libqemu-arm` (Raspberry Pi and STM32). Drop the result into
`/app/lib/` inside the Velxio container and it is used on the next
simulation start.

Note that the ESP32-P4 and ESP32-C5 do not appear above and are not in this
tree. They have no QEMU machine, ours or upstream. They run on a separate
in-browser engine that is not part of this work and carries no GPL
obligation.

## License

QEMU is GPL-2.0, and so is every line this fork adds to it. See `COPYING`
and `LICENSE` in this archive. You may study, modify and redistribute it
under those terms.

If you received a Velxio binary and cannot reach the download endpoint for
any reason, write to info@velxio.dev. You are entitled to this source and we
will get it to you.
