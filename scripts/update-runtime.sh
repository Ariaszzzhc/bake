#!/usr/bin/env bash
# update-runtime.sh — Update libc++, libc++abi, libunwind, compiler-rt in lib/
#
# Run after updating the external/llvm-project submodule (and building LLVM via
# build-llvm.sh). This script refreshes the runtime source in lib/ so
# the final bake distribution is self-contained — matching Zig's model.
#
# Usage:
#   ./scripts/update-runtime.sh          # update everything
#   ./scripts/update-runtime.sh --clean  # remove runtime dirs first
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_SRC="$ROOT/external/llvm-project"
LLVM_INSTALL="$ROOT/external/llvm-install"

# ── Preflight checks ─────────────────────────────────────────────────
if [ ! -d "$LLVM_SRC/libcxx" ]; then
  echo "error: $LLVM_SRC/libcxx not found. Did you init the git submodules?"
  exit 1
fi

if [ ! -f "$LLVM_INSTALL/include/c++/v1/__config_site" ]; then
  echo "error: $LLVM_INSTALL/include/c++/v1/__config_site not found."
  echo "       Run scripts/build-llvm.sh first to generate CMake headers."
  exit 1
fi

# ── Clean ────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--clean" ]]; then
  echo "==> Removing runtime source"
  rm -rf "$ROOT/lib/libcxx" "$ROOT/lib/libcxxabi" \
         "$ROOT/lib/libunwind" "$ROOT/lib/compiler-rt" \
         "$ROOT/lib/include"
fi

# ═══════════════════════════════════════════════════════════════════════
# libcxx
# ═══════════════════════════════════════════════════════════════════════
echo "==> Updating libcxx"

# include/ — C++ standard headers from source + generated files from install
mkdir -p "$ROOT/lib/libcxx"
rm -rf "$ROOT/lib/libcxx/include"
cp -R "$LLVM_SRC/libcxx/include" "$ROOT/lib/libcxx/include"
# Remove C++03 frozen headers (5.4M dead weight for C++11+ compilation)
rm -rf "$ROOT/lib/libcxx/include/__cxx03"
# Copy CMake-generated headers from the install tree
for f in __config_site __assertion_handler __cxxabi_config.h cxxabi.h \
         libcxx.imp module.modulemap stdbool.h stdint.h; do
  src="$LLVM_INSTALL/include/c++/v1/$f"
  [ -f "$src" ] && cp "$src" "$ROOT/lib/libcxx/include/$f"
done

# src/ — libc++ implementation source
rm -rf "$ROOT/lib/libcxx/src"
cp -R "$LLVM_SRC/libcxx/src" "$ROOT/lib/libcxx/src"

# modules/ — std.cppm.in + std/*.inc AND std.compat.cppm.in + std.compat/*.inc
# (not CMakeLists.txt, README, etc.)
rm -rf "$ROOT/lib/libcxx/modules"
mkdir -p "$ROOT/lib/libcxx/modules/std" \
         "$ROOT/lib/libcxx/modules/std.compat"
cp "$LLVM_SRC/libcxx/modules/std.cppm.in" \
   "$LLVM_SRC/libcxx/modules/std.compat.cppm.in" \
   "$ROOT/lib/libcxx/modules/"
cp "$LLVM_SRC/libcxx/modules/std/"*.inc \
   "$ROOT/lib/libcxx/modules/std/"
cp "$LLVM_SRC/libcxx/modules/std.compat/"*.inc \
   "$ROOT/lib/libcxx/modules/std.compat/"
git -C "$LLVM_SRC" rev-parse HEAD > "$ROOT/lib/libcxx/LLVM_REVISION"

# libc/ — curated subset matching Zig's distribution (119 files, ~700K)
rm -rf "$ROOT/lib/libcxx/libc"
mkdir -p "$ROOT/lib/libcxx/libc"

# hdr/ — 7 files
mkdir -p "$ROOT/lib/libcxx/libc/hdr/types"
for f in errno_macros.h fenv_macros.h float_macros.h limits_macros.h \
         stdint_proxy.h wchar_overlay.h types/wchar_t.h; do
  cp "$LLVM_SRC/libc/hdr/$f" "$ROOT/lib/libcxx/libc/hdr/$f"
done

# include/ — 11 files (llvm-libc-types + llvm-libc-macros)
mkdir -p "$ROOT/lib/libcxx/libc/include/llvm-libc-macros"
mkdir -p "$ROOT/lib/libcxx/libc/include/llvm-libc-types"
for f in \
  llvm-libc-macros/cfloat128-macros.h \
  llvm-libc-macros/cfloat16-macros.h \
  llvm-libc-macros/float-macros.h \
  llvm-libc-macros/float16-macros.h \
  llvm-libc-macros/stdfix-macros.h \
  llvm-libc-macros/wchar-macros.h \
  llvm-libc-types/cfloat128.h \
  llvm-libc-types/cfloat16.h \
  llvm-libc-types/float128.h \
  llvm-libc-types/wint_t.h \
; do
  cp "$LLVM_SRC/libc/include/$f" "$ROOT/lib/libcxx/libc/include/$f"
done

# shared/ — 4 files
mkdir -p "$ROOT/lib/libcxx/libc/shared"
for f in fp_bits.h libc_common.h str_to_float.h str_to_integer.h; do
  cp "$LLVM_SRC/libc/shared/$f" "$ROOT/lib/libcxx/libc/shared/$f"
done
# src/__support/ — only headers needed by fp_bits.h include chain
mkdir -p "$ROOT/lib/libcxx/libc/src"
for f in \
  src/__support/big_int.h \
  src/__support/common.h \
  src/__support/ctype_utils.h \
  src/__support/detailed_powers_of_ten.h \
  src/__support/high_precision_decimal.h \
  src/__support/libc_assert.h \
  src/__support/math_extras.h \
  src/__support/number_pair.h \
  src/__support/sign.h \
  src/__support/str_to_float.h \
  src/__support/str_to_integer.h \
  src/__support/str_to_num_result.h \
  src/__support/uint128.h \
  src/__support/wctype_utils.h \
  src/__support/CPP/array.h \
  src/__support/CPP/bit.h \
  src/__support/CPP/iterator.h \
  src/__support/CPP/limits.h \
  src/__support/CPP/optional.h \
  src/__support/CPP/string_view.h \
  src/__support/CPP/type_traits.h \
  src/__support/CPP/utility.h \
  src/__support/CPP/utility/declval.h \
  src/__support/CPP/utility/forward.h \
  src/__support/CPP/utility/in_place.h \
  src/__support/CPP/utility/integer_sequence.h \
  src/__support/CPP/utility/move.h \
  src/__support/FPUtil/FPBits.h \
  src/__support/FPUtil/rounding_mode.h \
  src/__support/macros/attributes.h \
  src/__support/macros/config.h \
  src/__support/macros/null_check.h \
  src/__support/macros/optimization.h \
  src/__support/macros/sanitizer.h \
  src/__support/macros/properties/architectures.h \
  src/__support/macros/properties/compiler.h \
  src/__support/macros/properties/complex_types.h \
  src/__support/macros/properties/cpu_features.h \
  src/__support/macros/properties/os.h \
  src/__support/macros/properties/types.h \
; do
  src="$LLVM_SRC/libc/$f"
  dst="$ROOT/lib/libcxx/libc/$f"
  if [ -f "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
  fi
done
# CPP/type_traits/ — all headers (each is a small single-concept file)
mkdir -p "$ROOT/lib/libcxx/libc/src/__support/CPP"
cp -R "$LLVM_SRC/libc/src/__support/CPP/type_traits" \
      "$ROOT/lib/libcxx/libc/src/__support/CPP/type_traits"

# ═══════════════════════════════════════════════════════════════════════
# libcxxabi
# ═══════════════════════════════════════════════════════════════════════
echo "==> Updating libcxxabi"
rm -rf "$ROOT/lib/libcxxabi"
mkdir -p "$ROOT/lib/libcxxabi"
cp -R "$LLVM_SRC/libcxxabi/src" "$ROOT/lib/libcxxabi/src"
cp -R "$LLVM_SRC/libcxxabi/include" "$ROOT/lib/libcxxabi/include"
rm -f "$ROOT/lib/libcxxabi/include/CMakeLists.txt"

# ═══════════════════════════════════════════════════════════════════════
# libunwind
# ═══════════════════════════════════════════════════════════════════════
echo "==> Updating libunwind"
rm -rf "$ROOT/lib/libunwind"
mkdir -p "$ROOT/lib/libunwind"
cp -R "$LLVM_SRC/libunwind/src" "$ROOT/lib/libunwind/src"
cp -R "$LLVM_SRC/libunwind/include" "$ROOT/lib/libunwind/include"

# ═══════════════════════════════════════════════════════════════════════
# compiler-rt (builtins — aarch64 only for now)
# ═══════════════════════════════════════════════════════════════════════
echo "==> Updating compiler-rt builtins"
rm -rf "$ROOT/lib/compiler-rt"
mkdir -p "$ROOT/lib/compiler-rt/lib"
cp -R "$LLVM_SRC/compiler-rt/lib/builtins" "$ROOT/lib/compiler-rt/lib/builtins"
# Remove non-aarch64 architecture directories
for arch in arm avr hexagon i386 loongarch macho_embedded ppc riscv ve wasm x86_64; do
  rm -rf "$ROOT/lib/compiler-rt/lib/builtins/$arch"
done

# ── Sanitizer runtimes: ubsan (standalone) + tsan, with their shared
#    bases sanitizer_common and interception. Compiled per-target from
#    these sources by ensure_sanitizer_objects at link time. ──
echo "==> Updating compiler-rt sanitizers"
for d in sanitizer_common interception ubsan tsan asan lsan; do
  rm -rf "$ROOT/lib/compiler-rt/lib/$d"
  mkdir -p "$ROOT/lib/compiler-rt/lib/$d"
  cp -R "$LLVM_SRC/compiler-rt/lib/$d/." "$ROOT/lib/compiler-rt/lib/$d/"
done
# Trim test/script/build scaffolding in the sanitizer dirs only.
for d in sanitizer_common interception ubsan tsan asan lsan; do
  find "$ROOT/lib/compiler-rt/lib/$d" -type d \
       \( -name tests -o -name test -o -name scripts -o -name gn \) -exec rm -rf {} +
  find "$ROOT/lib/compiler-rt/lib/$d" -type f \
       \( -name '*.lit.cfg' -o -name 'CMakeLists.txt' -o -name 'BUILD.gn' \) -delete
done

# ═══════════════════════════════════════════════════════════════════════
# Clang builtin headers (stdarg.h, stddef.h, intrinsics, etc.)
# ═══════════════════════════════════════════════════════════════════════
echo "==> Updating clang builtin headers"
rm -rf "$ROOT/lib/include"
mkdir -p "$ROOT/lib/include"

# Source: the installed clang resource directory.
CLANG_VER_DIR=$(ls -d "$LLVM_INSTALL/lib/clang/"*/ 2>/dev/null | head -1)
if [ -z "$CLANG_VER_DIR" ]; then
  echo "error: cannot find clang resource dir under $LLVM_INSTALL/lib/clang/"
  exit 1
fi
cp -R "${CLANG_VER_DIR}include/"* "$ROOT/lib/include/"

# Clean up stale lib/clang/ if it exists from a previous layout.
rm -rf "$ROOT/lib/clang"

# ═══════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════
echo ""
echo "==> Done. Updated runtime source:"
for d in libc/darwin libcxx libcxxabi libunwind compiler-rt include; do
  if [ -d "$ROOT/lib/$d" ]; then
    printf "    %-30s %s\n" "$d" "$(du -sh "$ROOT/lib/$d" | cut -f1)"
  fi
done
echo "    $(printf '%-30s' 'TOTAL lib/') $(du -sh "$ROOT/lib/" | cut -f1)"
