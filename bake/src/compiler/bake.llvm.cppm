// bake.llvm — LLVM/Clang/LLD bridge module interface.
//
// Exposes functions implemented across multiple module implementation units
// (bake_llvm.cpp, bake_clang_driver.cpp, bake_clang_cc1_main.cpp,
// bake_clang_cc1as_main.cpp). All signatures use built-in or LLVM-opaque
// types, so consumers never need to include LLVM headers.

export module bake.llvm;

import std;

// LLD linker flavor.
export enum class LldFlavor : int {
    ELF   = 0,
    COFF  = 1,
    MACHO = 2,
    WASM  = 3,
    MinGW = 4,  // GNU-style args → translated to COFF internally
};

// In-process LLD link. argv[0] should be the linker name.
export int bake_lld_link(LldFlavor flavor, int argc, const char **argv);

// In-process archive write (replaces `ar rcs`).
// archive_kind: 0=GNU, 1=BSD, 2=DARWIN, 3=COFF
export int bake_ar_write(const char *archive_name,
                         const char **members, std::size_t member_count,
                         int archive_kind);

// Clang driver entry point (bake cc / bake c++).
export int bake_clang_main(int argc, const char **argv);

// Returns 1 (LLVM support is always compiled in).
export int bake_has_llvm();
