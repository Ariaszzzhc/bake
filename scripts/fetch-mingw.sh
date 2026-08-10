#!/usr/bin/env bash
# fetch-mingw.sh — Vendor MinGW-w64 source for windows-gnu cross-compilation.
#
# Produces:
#   lib/libc/mingw/                  — MinGW-w64 CRT sources + winpthreads + .def files
#   lib/libc/include/any-windows-any/ — MinGW-w64 public headers (.h + .inl only)
#
# The .def.in files are preprocessed for all architectures (x86, x86_64, arm32, arm64)
# into plain .def files, so bake can generate import libraries at runtime
# without needing a C preprocessor.
#
# Requires: curl, tar, and a C preprocessor (clang or gcc).
# Re-runnable — overwrites previous output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBC="$ROOT/lib/libc"

MINGW_VERSION="14.0.0"
TARBALL="v${MINGW_VERSION}.tar.gz"
URL="https://github.com/mingw-w64/mingw-w64/archive/refs/tags/${TARBALL}"

WORK="$ROOT/.tmp/fetch-mingw"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "==> Work directory: $WORK"

# ════════════════════════════════════════════════════════════════════════
# 1. Download MinGW-w64 source
# ════════════════════════════════════════════════════════════════════════
echo "==> Downloading MinGW-w64 v${MINGW_VERSION}..."
if [ ! -f "$WORK/$TARBALL" ]; then
  curl -fsSL "$URL" -o "$WORK/$TARBALL"
fi
tar xf "$WORK/$TARBALL" -C "$WORK"
SRC="$WORK/mingw-w64-${MINGW_VERSION}"
CRT="$SRC/mingw-w64-crt"
echo "==> Source: $SRC"

# ════════════════════════════════════════════════════════════════════════
# 2. Install public headers → lib/libc/include/any-windows-any/
#    Only .h and .inl files — .idl/.dlg/.rh etc. are not needed for compilation.
# ════════════════════════════════════════════════════════════════════════
echo "==> Installing public headers → lib/libc/include/any-windows-any/"
HEADERS_DST="$LIBC/include/any-windows-any"
rm -rf "$HEADERS_DST"
mkdir -p "$HEADERS_DST"

# MinGW-w64 v14+ has headers split across two directories.
# Copy only .h and .inl files from both, preserving subdirectory structure.
for hdr_src in "$SRC/mingw-w64-headers/include" "$SRC/mingw-w64-headers/crt"; do
  [ -d "$hdr_src" ] || continue
  (cd "$hdr_src" && find . \( -name '*.h' -o -name '*.inl' \) -print0) | \
    while IFS= read -r -d '' f; do
      dest="$HEADERS_DST/$f"
      mkdir -p "$(dirname "$dest")"
      cp "$hdr_src/$f" "$dest"
    done
done

# Generate _mingw_mac.h from .in (autotools @VAR@ substitution).
for mac_in in \
    "$SRC/mingw-w64-headers/include/_mingw_mac.h.in" \
    "$SRC/mingw-w64-headers/crt/_mingw_mac.h.in"; do
  if [ -f "$mac_in" ]; then
    sed -e 's/@DEFAULT_WIN32_WINNT@/0x0a00/g' \
        -e 's/@DEFAULT_MSVCRT_VERSION@/0xE00/g' \
        "$mac_in" > "$HEADERS_DST/_mingw_mac.h"
    break
  fi
done

# Generate _mingw.h from .in if needed.
for mingw_in in \
    "$SRC/mingw-w64-headers/include/_mingw.h.in" \
    "$SRC/mingw-w64-headers/crt/_mingw.h.in"; do
  if [ -f "$mingw_in" ] && [ ! -f "$HEADERS_DST/_mingw.h" ]; then
    sed -e 's/@DEFAULT_WIN32_WINNT@/0x0a00/g' \
        -e 's/@DEFAULT_MSVCRT_VERSION@/0xE00/g' \
        "$mingw_in" > "$HEADERS_DST/_mingw.h"
    break
  fi
done

echo "  → $(find "$HEADERS_DST" -type f \( -name '*.h' -o -name '*.inl' \) | wc -l | tr -d ' ') headers"

# ════════════════════════════════════════════════════════════════════════
# 3. Install CRT sources → lib/libc/mingw/
#    Only .c, .S, .s, .h files. Skip build-system files and non-source clutter.
# ════════════════════════════════════════════════════════════════════════
echo "==> Installing CRT sources → lib/libc/mingw/"
MINGW_DST="$LIBC/mingw"
rm -rf "$MINGW_DST"
mkdir -p "$MINGW_DST"

# Copy source files from each CRT directory (only .c, .S, .s, .h extensions).
for dir in crt complex gdtoa math misc stdio string cfguard intrincs libsrc ctype; do
  [ -d "$CRT/$dir" ] || continue
  mkdir -p "$MINGW_DST/$dir"
  (cd "$CRT/$dir" && find . \( -name '*.c' -o -name '*.S' -o -name '*.s' -o -name '*.h' \) -print0) | \
    while IFS= read -r -d '' f; do
      dest="$MINGW_DST/$dir/$f"
      mkdir -p "$(dirname "$dest")"
      cp "$CRT/$dir/$f" "$dest"
    done
done

# Internal CRT headers.
if [ -d "$CRT/include" ]; then
  mkdir -p "$MINGW_DST/include"
  cp "$CRT/include/"*.h "$MINGW_DST/include/" 2>/dev/null || true
fi

# winpthreads from mingw-w64-libraries (both .c and .h).
WINPTHREADS="$SRC/mingw-w64-libraries/winpthreads/src"
if [ -d "$WINPTHREADS" ]; then
  mkdir -p "$MINGW_DST/winpthreads"
  cp "$WINPTHREADS/"*.c "$MINGW_DST/winpthreads/" 2>/dev/null || true
  cp "$WINPTHREADS/"*.h "$MINGW_DST/winpthreads/" 2>/dev/null || true
fi

# winpthreads public headers → public include dir.
WINPTHREADS_INC="$SRC/mingw-w64-libraries/winpthreads/include"
if [ -d "$WINPTHREADS_INC" ]; then
  cp "$WINPTHREADS_INC/"*.h "$HEADERS_DST/" 2>/dev/null || true
fi

# .def preprocessing helpers (needed at build time).
DEF_INCLUDE="$CRT/def-include"
if [ -d "$DEF_INCLUDE" ]; then
  cp -R "$DEF_INCLUDE" "$MINGW_DST/def-include"
fi

src_count=$(find "$MINGW_DST" -type f \( -name '*.c' -o -name '*.S' -o -name '*.s' \) | wc -l | tr -d ' ')
echo "  → $src_count source files"

# ════════════════════════════════════════════════════════════════════════
# 4. Install + preprocess .def files
#
# Layout (matching Zig's structure):
#   lib-common/  — all .def (preprocessed from .def.in), the fallback set
#   lib32/       — x86-specific .def files (from upstream lib32/)
#   lib64/       — x86_64-specific .def files (from upstream lib64/)
#   libarm32/    — arm32-specific .def files (from upstream libarm32/)
#   libarm64/    — arm64-specific .def files (from upstream libarm64/)
#
# bake's runtime searches arch-specific dir first, then falls back to lib-common.
# ════════════════════════════════════════════════════════════════════════
echo "==> Installing .def files..."

CPP=$(command -v clang || command -v gcc || command -v cpp)
if [ -z "$CPP" ]; then
  echo "warning: no C preprocessor found; .def.in files will be skipped" >&2
fi

# Helper: preprocess a single .def.in file for a given arch.
preprocess_def() {
  local src="$1" dst="$2" define="$3"
  if [ -n "$CPP" ]; then
    # -w suppresses harmless warnings from .def.in comments (apostrophes, etc.)
    "$CPP" -E -P -w -x c $define -I "$MINGW_DST/def-include" "$src" \
      | grep -v '^[[:space:]]*$' > "$dst"
  fi
}

# ── lib-common: preprocess ALL .def.in → .def, keep plain .def ──
# We need one preprocessed copy per arch. Use x86_64 as the "generic" set
# (most symbols are arch-independent). Per-arch dirs provide overrides.
mkdir -p "$MINGW_DST/lib-common"
echo "  → lib-common (x86_64 preprocessing)"
for f in "$CRT/lib-common"/*.def "$CRT/lib-common"/*.def.in; do
  [ -f "$f" ] || continue
  base=$(basename "$f")
  name="${base%.def.in}"; name="${name%.def}"
  if [[ "$base" == *.def.in ]]; then
    preprocess_def "$f" "$MINGW_DST/lib-common/${name}.def" "-D__x86_64__"
  else
    cp "$f" "$MINGW_DST/lib-common/${name}.def"
  fi
done

# ── Per-arch directories: copy upstream files, preprocess any .def.in ──
# Each arch dir gets ONLY its own upstream .def/.def.in files (not lib-common).
for pair in \
  "lib32:-D__i386__" \
  "lib64:-D__x86_64__" \
  "libarm32:-D__arm__" \
  "libarm64:-D__aarch64__"; do

  subdir=$(echo "$pair" | cut -d: -f1)
  define=$(echo "$pair" | cut -d: -f2)
  arch_src="$CRT/$subdir"

  [ -d "$arch_src" ] || continue
  mkdir -p "$MINGW_DST/$subdir"

  count=0
  for f in "$arch_src"/*.def "$arch_src"/*.def.in; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    name="${base%.def.in}"; name="${name%.def}"
    if [[ "$base" == *.def.in ]]; then
      preprocess_def "$f" "$MINGW_DST/$subdir/${name}.def" "$define"
    else
      cp "$f" "$MINGW_DST/$subdir/${name}.def"
    fi
    count=$((count + 1))
  done
  echo "  → $subdir/ ($count .def files)"
done

# ════════════════════════════════════════════════════════════════════════
# Summary
# ════════════════════════════════════════════════════════════════════════
echo ""
echo "==> Done."
printf "    %-42s %s\n" "mingw/ (CRT sources + .def)" "$(du -sh "$MINGW_DST" | cut -f1)"
for d in lib-common lib32 lib64 libarm32 libarm64; do
  [ -d "$MINGW_DST/$d" ] && \
    printf "      %-38s %s .def files\n" "$d/" "$(find "$MINGW_DST/$d" -name '*.def' | wc -l | tr -d ' ')"
done
printf "    %-42s %s\n" "include/any-windows-any/ (headers)" "$(du -sh "$HEADERS_DST" | cut -f1)"
printf "    %-42s %s\n" "TOTAL lib/libc/" "$(du -sh "$LIBC" | cut -f1)"
