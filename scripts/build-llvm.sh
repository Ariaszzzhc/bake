#!/usr/bin/env bash
# build-llvm.sh — Build zlib, zstd, and LLVM/Clang/LLD from vendored submodules.
#
# All dependencies are built from source — no Homebrew or system packages required.
# Output: external/llvm-install/  (CMake install prefix)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NPROC=$(sysctl -n hw.logicalncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

SRC_LLVM="$ROOT/external/llvm-project"
SRC_ZLIB="$ROOT/external/zlib"
SRC_ZSTD="$ROOT/external/zstd"

BUILD_LLVM="$ROOT/external/llvm-build"
INSTALL_LLVM="$ROOT/external/llvm-install"
BUILD_ZLIB="$ROOT/external/zlib-build"
INSTALL_ZLIB="$ROOT/external/zlib-install"
BUILD_ZSTD="$ROOT/external/zstd-build"
INSTALL_ZSTD="$ROOT/external/zstd-install"

if [[ "${1:-}" == "--clean" ]]; then
  echo "==> Cleaning all build + install directories"
  rm -rf "$BUILD_LLVM" "$INSTALL_LLVM" "$BUILD_ZLIB" "$INSTALL_ZLIB" "$BUILD_ZSTD" "$INSTALL_ZSTD"
fi

# ── zlib ──────────────────────────────────────────────────────────────
echo "==> Building zlib (static, from source)"
cmake -G Ninja -S "$SRC_ZLIB" -B "$BUILD_ZLIB" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_ZLIB" \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$BUILD_ZLIB" --parallel "$NPROC"
cmake --install "$BUILD_ZLIB" --strip
rm -rf "$BUILD_ZLIB"

# ── zstd ──────────────────────────────────────────────────────────────
echo "==> Building zstd (static, from source)"
cmake -G Ninja -S "$SRC_ZSTD/build/cmake" -B "$BUILD_ZSTD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_ZSTD" \
  -DZSTD_BUILD_SHARED=OFF \
  -DZSTD_BUILD_STATIC=ON \
  -DZSTD_BUILD_PROGRAMS=OFF \
  -DZSTD_BUILD_CONTRIB=OFF \
  -DZSTD_BUILD_TESTS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$BUILD_ZSTD" --parallel "$NPROC"
cmake --install "$BUILD_ZSTD" --strip
rm -rf "$BUILD_ZSTD"

# ── LLVM/Clang/LLD ───────────────────────────────────────────────────
echo "==> Configuring LLVM (AArch64+X86, clang+lld, Release)"
cmake -G Ninja -S "$SRC_LLVM/llvm" -B "$BUILD_LLVM" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_LLVM" \
  -DZLIB_ROOT="$INSTALL_ZLIB" \
  -DZLIB_INCLUDE_DIR="$INSTALL_ZLIB/include" \
  -DZLIB_LIBRARY="$INSTALL_ZLIB/lib/libz.a" \
  -Dzstd_ROOT="$INSTALL_ZSTD" \
  -Dzstd_INCLUDE_DIR="$INSTALL_ZSTD/include" \
  -Dzstd_LIBRARY="$INSTALL_ZSTD/lib/libzstd.a" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_ENABLE_ZLIB=ON \
  -DLLVM_ENABLE_ZSTD=ON \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_ENABLE_ARCMT=OFF \
  -DCLANG_INCLUDE_TESTS=OFF \
  -DCLANG_BUILD_EXAMPLES=OFF \
  -DLLD_INCLUDE_TESTS=OFF \
  -DLLD_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DCLANG_BUILD_CLANG_DYLIB=OFF \
  -DCLANG_LINK_CLANG_DYLIB=OFF \
  -DCLANG_BUILD_TOOLS=OFF \
  -DLIBCLANG_BUILD_STATIC=ON

echo "==> Building LLVM ($NPROC jobs)"
cmake --build "$BUILD_LLVM" --parallel "$NPROC"

echo "==> Installing LLVM to $INSTALL_LLVM"
cmake --install "$BUILD_LLVM" --strip
rm -rf "$BUILD_LLVM"

echo "==> Done. LLVM installed at: $INSTALL_LLVM"
echo "    Size: $(du -sh "$INSTALL_LLVM" | cut -f1)"
echo "    zlib:  $INSTALL_ZLIB"
echo "    zstd:  $INSTALL_ZSTD"
