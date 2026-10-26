#!/bin/sh

set -e

target="xtensa-softmmu,riscv32-softmmu"

case "$(uname)" in
  Linux)
    ncpu="$(nproc)"
    ;;
  Darwin)
    ncpu="$(sysctl -n hw.physicalcpu)"
    ;;
esac

flags="$(./configure --help | perl -ne 'print if s/^  ([a-z][\w-]*) .*/\1/' | tail -n +2 | awk '{print "--disable-"$1}' ORS=' ')"

${2:-.}/configure --target-list=$target --extra-cflags="-fPIC -DESP32_PICSIMLAB_SOFT_CACHE=1" --disable-slirp $flags --enable-tcg \
	--enable-system --disable-werror --disable-alsa  \
        --enable-debug --enable-debug-info \
        --enable-gcrypt --enable-slirp 
	#--enable-gtk

make clean >/dev/null

# Build everything as usual
make "-j$ncpu" 

# Build a shared library, without softmmu/main.o and otherwise *exactly* the
# same flags. We do this by re-using the .rsp file that ninja wrote during
# the link of qemu-system-<arch> — patching it in place to drop system_main
# and rewrite -o to point at libqemu-<arch>.so, then driving the linker
# directly with `cc @rsp`. No shell-eval, no parsing of ninja's verbose
# stdout (which is fragile across ninja versions and hosts: macOS produces
# `qemu-system-xtensa-unsigned` whereas Linux produces `qemu-system-xtensa`,
# and ninja chooses different rsp filenames per platform).
cd build

relink_as_so() {
    local arch="$1"
    local out="libqemu-${arch}.so"

    # Some platforms (macOS) don't use ninja response files at all —
    # the link command goes directly on argv. Others (Linux, MinGW) do.
    # Strategy: grab the link command from ninja's verbose stdout
    # rather than from any disk artifact, then patch + replay it.

    # Force ninja to RE-link by removing all possible output names.
    rm -f "qemu-system-${arch}" "qemu-system-${arch}-unsigned" \
          "qemu-system-${arch}.exe" \
          "qemu-system-${arch}.rsp" "qemu-system-${arch}-unsigned.rsp" \
          "qemu-system-${arch}.exe.rsp"

    # Re-run ninja for whichever target name this host uses. macOS
    # builds `*-unsigned` first; Linux + MinGW build the bare name.
    local logfile="ninja-link-${arch}.log"
    ninja -v -d keeprsp "qemu-system-${arch}-unsigned" > "$logfile" 2>&1 \
      || ninja -v -d keeprsp "qemu-system-${arch}"     > "$logfile" 2>&1 \
      || ninja -v -d keeprsp "qemu-system-${arch}.exe" > "$logfile" 2>&1

    # Extract the LINK command line: the line that contains both
    #   `-o qemu-system-${arch}` (any suffix) AND a system_main object.
    # NOTE: system_main appears as `qemu-system-${arch}{,-unsigned}.p/system_main.c.o`
    # so the byte immediately before `system_main` is `/`, NOT a space.
    # Don't require a space in the regex.
    # Strip the leading `[N/M] ` ninja progress prefix tolerantly
    # (any number of digits).
    local link_cmd
    link_cmd=$(grep -E "^\\[[0-9]+/[0-9]+\\] .*-o qemu-system-${arch}.*system_main" "$logfile" | tail -1)
    if [ -z "$link_cmd" ]; then
        # macOS's verbose output sometimes splits the command — try
        # without the leading progress marker.
        link_cmd=$(grep -E -- "-o qemu-system-${arch}.*system_main" "$logfile" | tail -1)
    fi
    if [ -z "$link_cmd" ]; then
        echo "  ERROR: could not find link command for ${arch} in ninja output" >&2
        echo "  ninja log tail:" >&2
        tail -30 "$logfile" >&2
        return 1
    fi
    # Strip leading [N/M] progress marker (any digit count).
    link_cmd=$(echo "$link_cmd" | sed -E 's|^\[[0-9]+/[0-9]+\] *||')

    # Patch the command:
    #   1. Drop the system_main object (handles both -unsigned and
    #      bare target names — same .p/ subdir naming).
    link_cmd=$(echo "$link_cmd" | sed -E "s| qemu-system-${arch}(-unsigned)?\\.p/system_main\\.c\\.o||g")
    #   2. Rewrite -o <target> to -shared -o lib<arch>.so. Match the
    #      -unsigned form first to avoid partial-prefix corruption.
    link_cmd=$(echo "$link_cmd" | sed -E "s|-o qemu-system-${arch}-unsigned|-shared -o ${out}|g")
    link_cmd=$(echo "$link_cmd" | sed -E "s|-o qemu-system-${arch}([[:space:]])|-shared -o ${out}\\1|g")

    echo "  Re-linking ${out}..."
    # Run via sh -c so any quoting in the command line works as intended.
    sh -c "$link_cmd -ggdb"

    if [ ! -f "$out" ]; then
        echo "  ERROR: ${out} not produced — patched command was:" >&2
        echo "  $link_cmd" >&2
        return 1
    fi
    echo "  => build/${out} created ($(stat -c '%s' "$out" 2>/dev/null \
        || stat -f '%z' "$out" 2>/dev/null || echo '?') bytes)"
}

relink_as_so xtensa
relink_as_so riscv32

