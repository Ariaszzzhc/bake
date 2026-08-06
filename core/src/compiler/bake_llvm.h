#ifndef BAKE_LLVM_H
#define BAKE_LLVM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BAKE_LLD_ELF = 0,
    BAKE_LLD_COFF = 1,
    BAKE_LLD_MACHO = 2,
    BAKE_LLD_WASM = 3,
} BakeLldFlavor;

/* In-process LLD link. argv[0] should be the linker name.
 * Returns 0 on success, 1 on failure. */
int bake_lld_link(int flavor, int argc, const char **argv);

/* In-process archive write (replaces `ar rcs`).
 * archive_kind: 0=GNU, 1=BSD, 2=DARWIN, 3=COFF
 * Returns 0 on success, non-zero on error. */
int bake_ar_write(const char *archive_name,
                  const char **members, size_t member_count,
                  int archive_kind);

/* Full Clang Driver entry point with in-process cc1 + LLD link interception.
 * Implemented in bake_clang_driver.cpp. Returns exit code. */
int bake_clang_main(int argc, const char **argv);

/* Returns 1 if bake was built with LLVM support. */
int bake_has_llvm(void);

#ifdef __cplusplus
}
#endif

#endif /* BAKE_LLVM_H */
