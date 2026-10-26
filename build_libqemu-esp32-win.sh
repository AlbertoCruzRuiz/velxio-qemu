#!/usr/bin/env bash
# Build libqemu-xtensa.dll + libqemu-riscv32.dll from lcgamboa QEMU fork
# on Windows (MSYS2 UCRT64).
#
# Both DLLs include slirp (user-mode networking) for WiFi emulation.
#
# Prerequisites (MSYS2 UCRT64 shell):
#   pacman -Syu
#   pacman -S diffutils git \
#     mingw-w64-ucrt-x86_64-gcc \
#     mingw-w64-ucrt-x86_64-glib2 \
#     mingw-w64-ucrt-x86_64-libgcrypt \
#     mingw-w64-ucrt-x86_64-libiconv \
#     mingw-w64-ucrt-x86_64-libslirp \
#     mingw-w64-ucrt-x86_64-ninja \
#     mingw-w64-ucrt-x86_64-pixman \
#     mingw-w64-ucrt-x86_64-pkg-config \
#     mingw-w64-ucrt-x86_64-python
#
# Usage (from repo root, in MSYS2 UCRT64):
#   cd /e/Hardware/wokwi_clon/wokwi-libs/qemu-lcgamboa
#   bash build_libqemu-esp32-win.sh
#
# Output:
#   build/libqemu-xtensa.dll   — ESP32/ESP32-S3 (Xtensa LX6/LX7)
#   build/libqemu-riscv32.dll  — ESP32-C3 (RISC-V RV32IMC)

set -euo pipefail

# Which QEMU targets to build and re-link. Defaults to the two the
# ESP32 path needs; override to add others without touching the rest of
# the script, e.g. the STM32 runtime the desktop downloads as
# `libqemu-arm-windows-amd64`:
#
#   ARCHS="arm" bash build_libqemu-esp32-win.sh
#
# Each entry <a> produces build/libqemu-<a>.dll from qemu-system-<a>.exe.
ARCHS="${ARCHS:-xtensa riscv32}"
TARGET="$(echo "$ARCHS" | tr ' ' '\n' | sed 's/$/-softmmu/' | paste -sd, -)"
ncpu=$(nproc)

echo "=== Targets: ${TARGET} ==="

echo "=== [1/4] Patching meson.build for libiconv (Windows workaround) ==="
# Force libiconv linkage (transitive dep of libintl on MinGW)
# Check for the patched flags FIRST. meson.build now has two
# `qemu_ldflags` assignments — an empty one for darwin (Apple's ld64
# rejects both flags with "ld: unknown options") and the patched one
# for everything else. The old guard only looked for `= []`, so once
# the else-branch was patched a re-run matched the *darwin* branch and
# a `sed -z ... /g` clobbered it, silently breaking macOS builds of
# this fork.
if grep -q "qemu_ldflags = \['-liconv'" meson.build; then
    echo "  meson.build already patched — skipping"
elif grep -q "qemu_ldflags = \[\]" meson.build; then
    sed -z -i "s/qemu_ldflags = \[\]/qemu_ldflags = \['-liconv','-Wl,--allow-multiple-definition'\]/g" -- meson.build
    echo "  Patched meson.build"
else
    echo "  meson.build pattern not found — skipping"
fi

# -DLIBSLIRP_STATIC: MSYS2 ships both libslirp.a and libslirp.dll.a, and
# meson picks the static one — but slirp/libslirp.h decorates every
# prototype with __declspec(dllimport) unless this macro is set. Without
# it the objects reference __imp_slirp_* and the final link dies with
# ~20 "undefined reference to `__imp_slirp_new'" style errors. Only
# shows up on a configure from scratch, which is why a stale build/
# directory could hide it.
echo "=== [2/4] Configuring QEMU ==="
# WITH_TOOLS=1 also builds qemu-img, which the QEMU-Linux boards need:
# qemu_manager creates a per-session qcow2 overlay backed by the shared
# rootfs, and shells out to `qemu-img create` to do it. Off by default —
# --without-default-features drops the tools, and the ESP32/STM32 DLL
# builds have no use for them.
TOOLS_FLAG="--disable-tools"
[ "${WITH_TOOLS:-0}" = "1" ] && TOOLS_FLAG="--enable-tools"

./configure \
    --target-list="${TARGET}" \
    "${TOOLS_FLAG}" \
    --disable-werror \
    --disable-alsa \
    --enable-tcg \
    --enable-system \
    --enable-gcrypt \
    --enable-slirp \
    --enable-iconv \
    --enable-debug \
    --enable-debug-info \
    --without-default-features \
    --extra-cflags="-DCONFIG_PICSIMLAB -DESP32_PICSIMLAB_SOFT_CACHE=1 -DLIBSLIRP_STATIC" \
    || { echo "configure failed — check meson-logs/meson-log.txt"; cat meson-logs/meson-log.txt 2>/dev/null || true; exit 1; }

echo "=== [3/4] Building QEMU (${ncpu} cores) ==="
# Only build the qemu-system-* targets we need to extract link commands
# from. Plain `make` triggers ninja's "all" target which also builds the
# unit-test binaries — and `tests/unit/test-vmstate.exe` fails to link
# on MINGW64 with "undefined reference to qemu_ftruncate64" (it's a
# host-side test, not part of the runtime we ship). Driving ninja
# directly with explicit targets sidesteps the test-binary build
# entirely. The shared object archives we depend on (libqemuutil.a,
# libcommon.fa, target-specific libqemu-*-softmmu.fa) get built as
# transitive dependencies of the qemu-system-* targets.
ninja_targets=""
for a in $ARCHS; do
    ninja_targets="${ninja_targets} qemu-system-${a}.exe"
done
# shellcheck disable=SC2086
ninja -C build -j"${ncpu}" ${ninja_targets}

echo "=== [4/5] Re-linking as DLLs (removing main(), adding -shared) ==="
cd build

# Helper that re-links a qemu-system-* target as a shared library.
# The previous approach captured the link command by parsing ninja's
# verbose stdout — fragile across ninja versions and easy to break
# (multi-step output, multi-digit progress markers, rsp generation
# steps interleaved with the link line).
#
# This version is data-driven: we use the auto-generated `*.rsp` file
# that ninja writes for the link step (preserved by `-d keeprsp`),
# patch it in place to drop softmmu_main.c.o and rewrite -o to point
# at our DLL, then run `cc @rsp` directly — no shell parsing of
# compiler flags, robust across ninja revisions.
relink_as_dll() {
    local arch="$1"   # xtensa or riscv32
    local exe="qemu-system-${arch}.exe"
    # Newer ninja (and the version in current MSYS2) writes the link
    # response file as `<target>.rsp` — i.e. `qemu-system-xtensa.exe.rsp`,
    # not `qemu-system-xtensa.rsp`. Older builds used the bare name
    # so we accept either.
    local exe_rsp="${exe}.rsp"
    local bare_rsp="qemu-system-${arch}.rsp"
    local dll="libqemu-${arch}.dll"
    local working_rsp="${dll}.rsp"

    echo "  Re-linking ${dll} ..."

    # Force ninja to regenerate the link step (and its rsp file).
    rm -f "qemu-system-${arch}" "$exe" "$exe_rsp" "$bare_rsp" "$working_rsp"
    ninja -d keeprsp "$exe" > /dev/null

    # Locate whichever rsp file ninja actually produced.
    local source_rsp=""
    if [ -f "$exe_rsp" ]; then
        source_rsp="$exe_rsp"
    elif [ -f "$bare_rsp" ]; then
        source_rsp="$bare_rsp"
    else
        echo "  ERROR: ninja did not produce ${exe_rsp} or ${bare_rsp}" >&2
        echo "  rsp files in build dir:" >&2
        ls -la *.rsp 2>&1 | head -10 >&2
        echo "  ninja -v output for diagnosis:" >&2
        ninja -v -d keeprsp "$exe" 2>&1 | tail -20 >&2
        return 1
    fi
    echo "  Using rsp source: ${source_rsp}"

    # Copy to a working file so we never mutate the canonical one
    # ninja just wrote (helpful when the same step rebuilds twice in
    # one run, e.g. xtensa then riscv32).
    cp "$source_rsp" "$working_rsp"

    # Patch the rsp:
    #   1. Drop softmmu_main.c.o (its main() conflicts with -shared).
    #   2. Insert -shared + --export-all-symbols and rewrite the output
    #      filename from qemu-system-${arch}.exe to libqemu-${arch}.dll.
    sed -i "s|qemu-system-${arch}.p/softmmu_main.c.o||g" "$working_rsp"
    sed -i "s|-o ${exe}|-shared -Wl,--export-all-symbols -o ${dll}|g" "$working_rsp"
    # Belt-and-braces in case the linker line emits the bare name (no
    # .exe) on some MINGW64 setups:
    sed -i "s|-o qemu-system-${arch} |-shared -Wl,--export-all-symbols -o ${dll} |g" "$working_rsp"

    # Drive the linker straight from the patched rsp file. `cc @file`
    # is the canonical way to pass long argument lists on Windows where
    # CLI line length is capped at 32 KiB.
    cc -ggdb "@${working_rsp}"
    if [ -f "$dll" ]; then
        echo "  => build/${dll} created ($(stat -c '%s' "$dll" 2>/dev/null || echo "?") bytes)"
    else
        echo "  ERROR: ${dll} not produced by cc @${working_rsp}" >&2
        return 1
    fi
}

# SKIP_RELINK=1 stops after the qemu-system-*.exe targets. The Velxio
# desktop ships a real qemu-system-aarch64.exe for the QEMU-Linux boards
# (Raspberry Pi, UNIHIKER) — those run a full system emulator as a child
# process, not an embedded library, so the DLL re-link is dead weight
# there and its rsp surgery is one more thing to fail.
if [ "${SKIP_RELINK:-0}" = "1" ]; then
    echo ""
    echo "=== [5/5] SKIP_RELINK=1 — keeping the .exe targets, no DLLs ==="
    for a in $ARCHS; do
        exe="build/qemu-system-${a}.exe"
        [ -f "$exe" ] && echo "  ${exe} ($(stat -c '%s' "$exe") bytes)"
    done
else
    for a in $ARCHS; do
        echo ""
        echo "=== [5/5] Re-linking ${a} as DLL ==="
        relink_as_dll "$a"
    done
fi

echo ""
echo "=== Build complete ==="
echo ""
echo "Next steps:"
echo "  1. Verify exports:"
for a in $ARCHS; do
    echo "     objdump -p libqemu-${a}.dll | grep -E 'qemu_picsimlab|slirp'"
done
echo "  2. Copy DLLs to backend:"
echo "     cp build/libqemu-xtensa.dll  /e/Hardware/wokwi_clon/backend/app/services/"
echo "     cp build/libqemu-riscv32.dll /e/Hardware/wokwi_clon/backend/app/services/"
echo "  3. Quick test:"
echo "     python -c \"import ctypes; lib=ctypes.CDLL('./backend/app/services/libqemu-xtensa.dll'); print('Xtensa OK')\""
echo "     python -c \"import ctypes; lib=ctypes.CDLL('./backend/app/services/libqemu-riscv32.dll'); print('RISC-V OK')\""
