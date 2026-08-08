set -e
BAKE=/Users/arias/Projects/bake-pkgs/bake
LIB=/Users/arias/Projects/bake/lib
TARGET=aarch64-linux-musl
PREFIX=/Users/arias/Projects/bake/external/llvm-install-aarch64-linux-musl
WRAP=/Users/arias/Projects/bake/.tmp/cross-wrappers
SRC=external/llvm-project/llvm

mkdir -p "$WRAP"

# bake cc/c++ are complete cross-compilers — same role as `zig cc -target`.
for w in cc c++; do
cat > "$WRAP/$w" << EOF
#!/bin/sh
export BAKE_LIB_DIR=$LIB
exec $BAKE $w -target $TARGET "\$@"
EOF
cat > "$WRAP/native-$w" << EOF
#!/bin/sh
export BAKE_LIB_DIR=$LIB
exec $BAKE $w "\$@"
EOF
done
chmod +x "$WRAP/"*

# ── Step 1: Native LLVM → llvm-tblgen + clang-tblgen (runs on host) ──
NATIVE=external/llvm-project/build-native
if [ ! -f "$NATIVE/bin/llvm-tblgen" ]; then
    echo "=== Building native LLVM (for tablegen) ==="
    rm -rf "$NATIVE"
    cmake -G Ninja -B "$NATIVE" \
        -DCMAKE_C_COMPILER="$WRAP/native-cc" \
        -DCMAKE_CXX_COMPILER="$WRAP/native-c++" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_TARGETS_TO_BUILD="" \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_INCLUDE_DOCS=OFF \
        "$SRC"
    ninja -C "$NATIVE" llvm-tblgen clang-tblgen
fi

# ── Step 2: Cross-compile LLVM with bake cc ──
CROSS=external/llvm-project/build-cross
echo "=== Cross-compiling LLVM for $TARGET ==="
if [ ! -f "$CROSS/build.ninja" ]; then
    rm -rf "$CROSS"
    cmake -G Ninja -B "$CROSS" \
    -DCMAKE_CROSSCOMPILING=True \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$WRAP/cc" \
    -DCMAKE_CXX_COMPILER="$WRAP/c++" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_LINK_DEPENDS_USE_LINKER=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_LIBCXX=ON \
    -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_ENABLE_BINDINGS=OFF \
    -DLLVM_ENABLE_PLUGINS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_BUILD_TOOLS=OFF \
    -DLLVM_BUILD_STATIC=ON \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_BUILD_SHARED_LIBS=OFF \
    -DLLVM_ENABLE_PIC=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_LIBPFM=OFF \
    -DLLVM_ENABLE_Z3_SOLVER=OFF \
    -DLLVM_TABLEGEN="$(pwd)/$NATIVE/bin/llvm-tblgen" \
    -DCLANG_TABLEGEN="$(pwd)/$NATIVE/bin/clang-tblgen" \
    -DCLANG_BUILD_TOOLS=OFF \
    -DLLD_BUILD_TOOLS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    "$SRC"
fi
cmake --build "$CROSS"
cmake --install "$CROSS"
