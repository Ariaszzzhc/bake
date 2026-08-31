#!/usr/bin/env bash
# fetch-musl.sh — Fetch musl source + Linux kernel UAPI headers for cross-compilation.
#
# Produces:
#   lib/libc/musl/                       — musl source tree (crt/, arch/, src/, include/)
#   lib/libc/include/generic-musl/        — musl public headers (arch-independent)
#   lib/libc/include/<triple>/bits/       — per-arch generated bits (alltypes.h etc.)
#   lib/libc/include/any-linux-any/       — Linux kernel UAPI (arch-independent)
#   lib/libc/include/x86-linux-any/asm/   — x86 kernel UAPI asm headers
#   lib/libc/include/aarch64-linux-any/asm/ — aarch64 kernel UAPI asm headers
#
# Requires Docker. Re-runnable — overwrites previous output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBC="$ROOT/lib/libc"
MUSL_VERSION="1.2.5"

# ── Preflight ──────────────────────────────────────────────────────────
command -v docker >/dev/null 2>&1 || {
  echo "error: docker not found. Install Docker Desktop." >&2
  exit 1
}

WORK="$ROOT/.tmp/fetch-musl"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "==> Work directory: $WORK"

# ════════════════════════════════════════════════════════════════════════
# 1. Download musl source
# ════════════════════════════════════════════════════════════════════════
echo "==> Downloading musl ${MUSL_VERSION}..."
TARBALL="musl-${MUSL_VERSION}.tar.gz"
curl -fsSL "https://musl.libc.org/releases/${TARBALL}" -o "$WORK/${TARBALL}"
tar xf "$WORK/${TARBALL}" -C "$WORK"
MUSL_SRC="$WORK/musl-${MUSL_VERSION}"

echo "==> Installing musl source → lib/libc/musl/"
rm -rf "$LIBC/musl"
mkdir -p "$LIBC/musl"
cp -R "$MUSL_SRC/"* "$LIBC/musl/"
echo "${MUSL_VERSION}" > "$LIBC/musl/VERSION"

# ── Strip build-system files (bake compiles musl in-process, not via configure/make) ──
rm -f  "$LIBC/musl/configure" "$LIBC/musl/Makefile" \
       "$LIBC/musl/dynamic.list" "$LIBC/musl/VERSION" \
       "$LIBC/musl/WHATSNEW" "$LIBC/musl/INSTALL" \
       "$LIBC/musl/README"
rm -rf "$LIBC/musl/dist" "$LIBC/musl/tools"

# ── Strip unsupported architectures (keep x86_64, aarch64, generic) ──
# Arch list from upstream musl 1.2.5. Only these are needed for now;
# add more back when bake gains cross-compile targets for them.
KEEP_ARCH="aarch64 generic x86_64"
for d in "$LIBC/musl/arch/"*; do
  name="$(basename "$d")"
  echo "$KEEP_ARCH" | grep -qw "$name" || rm -rf "$d"
done

# ── Strip per-arch crt/ subdirs (crti.c / crtn.c / crt_arch.h live in arch/) ──
# Keep only flat crt .c files: crt1.c, rcrt1.c, Scrt1.c
for d in "$LIBC/musl/crt/"*/; do
  rm -rf "$d"
done

# ════════════════════════════════════════════════════════════════════════
# 2. Configure musl per-arch → extract generated headers
# ════════════════════════════════════════════════════════════════════════
# Run musl's ./configure + make install-headers inside a Docker container
# matching the target architecture. configure generates alltypes.h from
# alltypes.h.in; install-headers copies all public + generated headers.

configure_and_install_headers() {
  local arch="$1" platform="$2"
  echo "==> Configuring musl for ${arch} (Docker ${platform})..."

  # Download musl fresh inside the container — avoids bind-mount issues
  # (configure creates temp files, obj/, config.mak in the source tree).
  docker run --rm --platform "$platform" \
    -v "$WORK/$arch:/out" \
    alpine:3.20 sh -c '
      set -e
      apk add --no-cache gcc musl-dev make perl curl >/dev/null 2>&1
      cd /tmp
      curl -fsSL "https://musl.libc.org/releases/musl-'"${MUSL_VERSION}"'.tar.gz" -o musl.tar.gz
      tar xf musl.tar.gz
      cd "musl-'"${MUSL_VERSION}"'"
      ./configure --prefix=/musl >/dev/null 2>&1
      make install-headers DESTDIR=/out >/dev/null 2>&1
    '
}

configure_and_install_headers x86_64   linux/amd64
configure_and_install_headers aarch64  linux/arm64

# ════════════════════════════════════════════════════════════════════════
# 3. Extract overlay headers → lib/libc/include/
# ════════════════════════════════════════════════════════════════════════
echo "==> Extracting overlay headers → lib/libc/include/"

# 3a. Per-arch bits (generated alltypes.h + arch-specific bits headers)
for target in x86_64 aarch64; do
  INSTALL="$WORK/${target}/musl/include"
  DST="$LIBC/include/${target}-linux-musl/bits"
  rm -rf "$DST"
  mkdir -p "$DST"

  if [ -d "$INSTALL/bits" ]; then
    cp -R "$INSTALL/bits/"* "$DST/"
  fi

  # Fallback: copy arch bits from musl source
  if [ -z "$(ls -A "$DST" 2>/dev/null)" ]; then
    echo "  warning: no generated bits for $target, using source arch/bits"
    cp -R "$LIBC/musl/arch/${target}/bits/"* "$DST/"
  fi

  echo "  → ${target}-linux-musl/bits/ ($(find "$DST" -name '*.h' | wc -l | tr -d ' ') headers)"
done

# 3b. Generic musl headers (arch-independent public headers: stdio.h, stdlib.h, ...)
#     Use x86_64's install as reference — these headers are identical across arches.
GENERIC_DST="$LIBC/include/generic-musl"
rm -rf "$GENERIC_DST"
mkdir -p "$GENERIC_DST"
INSTALL="$WORK/x86_64/musl/include"
if [ -d "$INSTALL" ]; then
  for item in "$INSTALL"/*; do
    name="$(basename "$item")"
    [ "$name" = "bits" ] && continue
    cp -R "$item" "$GENERIC_DST/"
  done
fi
echo "  → generic-musl/ ($(find "$GENERIC_DST" -name '*.h' | wc -l | tr -d ' ') headers)"

# ════════════════════════════════════════════════════════════════════════
# 4. Linux kernel UAPI headers (shared infrastructure)
# ════════════════════════════════════════════════════════════════════════
# any-linux-any/ and <arch>-linux-any/ are libc-agnostic: consumed by the
# musl AND gnu header chains. Owned and produced solely by
# fetch-linux-headers.sh — this script only bootstraps them when absent.
LINUX_UAPI_DIRS=(
  "$LIBC/include/any-linux-any"
  "$LIBC/include/x86-linux-any"
  "$LIBC/include/aarch64-linux-any"
)
missing=0
for d in "${LINUX_UAPI_DIRS[@]}"; do
  [ -d "$d" ] || missing=1
done
if [ "$missing" -eq 1 ]; then
  echo "==> Kernel UAPI headers missing — running fetch-linux-headers.sh"
  "$ROOT/scripts/fetch-linux-headers.sh"
else
  echo "==> Kernel UAPI headers present (kernel $(cat "$LIBC/include/linux-uapi-VERSION" 2>/dev/null || echo '?') ," \
       "owned by fetch-linux-headers.sh)"
fi
# ════════════════════════════════════════════════════════════════════════
# Summary
# ════════════════════════════════════════════════════════════════════════
echo ""
echo "==> Done."
printf "    %-42s %s\n" "musl/ (source)" "$(du -sh "$LIBC/musl" | cut -f1)"
for d in generic-musl x86_64-linux-musl aarch64-linux-musl; do
  if [ -d "$LIBC/include/$d" ]; then
    count=$(find "$LIBC/include/$d" -name '*.h' | wc -l | tr -d ' ')
    printf "    %-42s %s  (%s .h)\n" "include/$d/" "$(du -sh "$LIBC/include/$d" | cut -f1)" "$count"
  fi
done
echo "    (kernel UAPI: any-linux-any/, *-linux-any/ — owned by fetch-linux-headers.sh)"
printf "    %-42s %s\n" "TOTAL lib/libc/" "$(du -sh "$LIBC" | cut -f1)"
