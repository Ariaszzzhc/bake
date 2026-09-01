#!/usr/bin/env bash
# fetch-linux-headers.sh — Fetch Linux kernel UAPI headers.
#
# Sole owner of the libc-agnostic kernel header directories:
#   lib/libc/include/any-linux-any/        — arch-independent UAPI (linux/, asm-generic/, ...)
#   lib/libc/include/x86-linux-any/asm/     — x86 (i386 + x86_64) UAPI asm headers
#   lib/libc/include/aarch64-linux-any/asm/ — aarch64 UAPI asm headers
#   lib/libc/include/linux-uapi-VERSION     — kernel version record
#
# Consumed by BOTH libc families (musl, gnu) via the header search chain
# (bake_clang_driver.cpp inject_vendored_headers). Updating one family never
# touches these; updating the kernel headers never requires re-fetching a libc.
#
# Requires Docker. Re-runnable — overwrites previous output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBC="$ROOT/lib/libc"

# Alpine's linux-headers package is `make headers_install` output, packaged.
# Pin the image tag; the kernel version lands in linux-uapi-VERSION.
ALPINE="alpine:3.20"

WORK="$ROOT/.tmp/fetch-linux-headers"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "==> Fetching Linux kernel UAPI headers (from $ALPINE linux-headers)"

# ── Common headers (arch-independent) ──
docker run --rm --platform linux/amd64 \
  -v "$WORK:/out" \
  "$ALPINE" sh -c '
    set -e
    apk add --no-cache linux-headers >/dev/null 2>&1
    mkdir -p /out/linux-uapi
    for d in linux asm-generic drm rdma scsi sound video xen misc mtd cxl regulator fwctl; do
      [ -d "/usr/include/$d" ] && cp -R "/usr/include/$d" "/out/linux-uapi/" || true
    done
    # Record the kernel version these headers came from.
    awk "/#define LINUX_VERSION_CODE/ { print \$3 }" /usr/include/linux/version.h \
      | tr -d "\n" > /out/kernel-code
  '

ANY="$LIBC/include/any-linux-any"
rm -rf "$ANY"
mkdir -p "$ANY"
cp -R "$WORK"/linux-uapi/* "$ANY/"

# Two scsi headers live in the libc layer, not the kernel UAPI package
# (musl ships them under its own include/; glibc distros get them from
# the C library). The sanitizer runtimes need them, so pull them from a
# glibc distro's libc6-dev.
docker run --rm --platform linux/amd64 \
  -v "$WORK:/out" \
  debian:bookworm bash -c '
    set -e
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq libc6-dev >/dev/null 2>&1
    cp /usr/include/scsi/scsi.h /usr/include/scsi/sg.h /out/linux-uapi/scsi/
  '
echo "  → any-linux-any/ ($(find "$ANY" -name '*.h' | wc -l | tr -d ' ') headers)"

# ── Per-arch asm/ headers ──
# /usr/include/asm on x86_64 Alpine = x86 UAPI (shared by i386 + x86_64);
# on aarch64 Alpine = arm64 UAPI.
fetch_arch_asm() {
    local alpine_arch="$1" platform="$2" dst_name="$3"
    docker run --rm --platform "$platform" \
      -v "$WORK:/out" \
      "$ALPINE" sh -c '
        set -e
        apk add --no-cache linux-headers >/dev/null 2>&1
        mkdir -p /out/'"$dst_name"'/asm
        cp -R /usr/include/asm/. /out/'"$dst_name"'/asm/
      '
    local dst="$LIBC/include/$dst_name"
    rm -rf "$dst"
    mkdir -p "$dst"
    cp -R "$WORK/$dst_name/asm" "$dst/asm"
    echo "  → $dst_name/ ($(find "$dst" -name '*.h' | wc -l | tr -d ' ') headers)"
}

fetch_arch_asm x86_64   linux/amd64 x86-linux-any
fetch_arch_asm aarch64  linux/arm64 aarch64-linux-any

# ── Version record ──
code="$(cat "$WORK/kernel-code")"
major=$((code / 65536))
minor=$(((code % 65536) / 256))
patch=$((code % 256))
printf '%s.%s.%s\n' "$major" "$minor" "$patch" > "$LIBC/include/linux-uapi-VERSION"
echo "  → linux-uapi-VERSION: $major.$minor.$patch (alpine image: $ALPINE)"

echo "==> Done."
