#!/bin/sh
# Build libqemu-arm.{so,dll} for the STM32 / Velxio worker.
#
# Mirror of build_libqemu-esp32.sh but for the arm-softmmu target. Produces
# a shared library (same relink-the-ninja-.rsp trick, dropping softmmu_main.o)
# that stm32_worker.py loads via ctypes — exactly like libqemu-xtensa.dll for
# ESP32.
#
# Run from the fork root inside the same msys2/mingw64 (Windows) or Linux
# toolchain used to build the ESP32 libs.

set -e

target="arm-softmmu"

case "$(uname)" in
  Linux)  ncpu="$(nproc)" ;;
  Darwin) ncpu="$(sysctl -n hw.physicalcpu)" ;;
  *)      ncpu=4 ;;
esac

flags="$(./configure --help | perl -ne 'print if s/^  ([a-z][\w-]*) .*/\1/' | tail -n +2 | awk '{print "--disable-"$1}' ORS=' ')"

${2:-.}/configure --target-list=$target --extra-cflags="-fPIC" --disable-slirp $flags --enable-tcg \
	--enable-system --disable-werror --disable-alsa \
	--enable-debug --enable-debug-info \
	--enable-gcrypt --enable-slirp

make clean >/dev/null

make "-j$ncpu"

cd build

relink_as_so() {
    local arch="$1"
    local out="libqemu-${arch}.so"

    rm -f "qemu-system-${arch}" "qemu-system-${arch}-unsigned" \
          "qemu-system-${arch}.exe" \
          "qemu-system-${arch}.rsp" "qemu-system-${arch}-unsigned.rsp" \
          "qemu-system-${arch}.exe.rsp"

    local logfile="ninja-link-${arch}.log"
    ninja -v -d keeprsp "qemu-system-${arch}-unsigned" > "$logfile" 2>&1 \
      || ninja -v -d keeprsp "qemu-system-${arch}"     > "$logfile" 2>&1 \
      || ninja -v -d keeprsp "qemu-system-${arch}.exe" > "$logfile" 2>&1

    local link_cmd
    link_cmd=$(grep -E "^\\[[0-9]+/[0-9]+\\] .*-o qemu-system-${arch}.*softmmu_main" "$logfile" | tail -1)
    if [ -z "$link_cmd" ]; then
        link_cmd=$(grep -E -- "-o qemu-system-${arch}.*softmmu_main" "$logfile" | tail -1)
    fi
    if [ -z "$link_cmd" ]; then
        echo "  ERROR: could not find link command for ${arch} in ninja output" >&2
        tail -30 "$logfile" >&2
        return 1
    fi
    link_cmd=$(echo "$link_cmd" | sed -E 's|^\[[0-9]+/[0-9]+\] *||')
    link_cmd=$(echo "$link_cmd" | sed -E "s| qemu-system-${arch}(-unsigned)?\\.p/softmmu_main\\.c\\.o||g")
    link_cmd=$(echo "$link_cmd" | sed -E "s|-o qemu-system-${arch}-unsigned|-shared -o ${out}|g")
    link_cmd=$(echo "$link_cmd" | sed -E "s|-o qemu-system-${arch}([[:space:]])|-shared -o ${out}\\1|g")

    echo "  Re-linking ${out}..."
    sh -c "$link_cmd -ggdb"

    if [ ! -f "$out" ]; then
        echo "  ERROR: ${out} not produced" >&2
        return 1
    fi
    echo "  => build/${out} created"
}

relink_as_so arm
