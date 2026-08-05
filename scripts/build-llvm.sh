#!/usr/bin/env bash
# build-llvm.sh — Build LLVM/Clang/LLD from the vendored submodule source.
#
# Usage:
#   ./scripts/build-llvm.sh          # build (incremental)
#   ./scripts/build-llvm.sh --clean  # clean rebuild
#
# Output: external/llvm-install/  (CMake install prefix, ~1.5GB)
# Build artifacts in external/llvm-build/ are deleted after install.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/external/llvm-project"
BUILD="$ROOT/external/llvm-build"
INSTALL="$ROOT/external/llvm-install"
NPROC=$(sysctl -n hw.logicalncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

if [[ "${1:-}" == "--clean" ]]; then
  echo "==> Cleaning LLVM build + install directories"
  rm -rf "$BUILD" "$INSTALL"
fi

echo "==> Configuring LLVM (AArch64, clang+lld, Release)"
cmake -G Ninja -S "$SRC/llvm" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
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
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=ON \
  -DCLANG_BUILD_CLANG_DYLIB=ON

echo "==> Building LLVM ($NPROC jobs)"
cmake --build "$BUILD" --parallel "$NPROC"

echo "==> Installing LLVM to $INSTALL"
cmake --install "$BUILD" --strip

echo "==> Cleaning build artifacts (keeping install only)"
rm -rf "$BUILD"

echo "==> Done. LLVM installed at: $INSTALL"
echo "    Size: $(du -sh "$INSTALL" | cut -f1)"
echo "    Use: cmake -DLLVM_DIR=$INSTALL/lib/cmake/llvm ..."
