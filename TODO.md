# Platform Roadmap

Current targets: **linux-musl** (aarch64, x86_64), **macos/darwin** (aarch64, x86_64).

Extension point: `resolve_libc_family()` + `prepare_runtime()` switch in
`bake.compiler.cppm`. Adding a new libc family only requires a new enum value,
a resolve case, a prepare case, and a builder function. The linker dispatch
(`bake_clang_driver.cpp`) stays untouched.

---

## windows-gnu (MinGW-w64)

Triple: `x86_64-windows-gnu` / `aarch64-windows-gnu`

### Zig's approach

Zig vendors full MinGW-w64 source in `lib/libc/mingw/` and builds CRT from
source (source-distributed, same model as musl):

- **CRT**: 3 artifacts built from vendored C sources:
  - `crt2.o` — exe entry point (from `crt/crtexe.c`)
  - `dllcrt2.o` — DLL entry point (from `crt/crtdll.c`)
  - `libmingw32.a` — static helper library (from `lib-common/`, `lib64/`, etc.)
- **Always-link libs** (20): `api-ms-win-crt-*` (14), `advapi32`, `kernel32`,
  `ntdll`, `shell32`, `user32`
- **Import libraries**: built on demand from `.def` files for system DLLs
- **Headers**: vendored in `lib/libc/mingw/include/`
- LLD COFF flavor (`lld-link`) handles linking

### bake TODO

- [ ] `LibcFamily::Windows` + case in `resolve_libc_family()`
- [ ] `TargetSpec::is_windows()` — `triple_.contains("windows")`
- [ ] `parse_target`: normalize `w64-mingw32` → `windows-gnu`
- [ ] Vendor MinGW-w64 source (headers + CRT sources + lib sources)
- [ ] `ensure_mingw_objects()`: build `crt2.o`, `dllcrt2.o`, `libmingw32.a` from source
- [ ] Link libs: inject `always_link_libs` (kernel32, advapi32, etc.)
- [ ] Driver: `inject_vendored_headers` Windows header block
- [ ] compiler-rt / write_archive: COFF object format support
- [ ] Import library generation (.def → .lib) for system DLLs

---

## linux-gnu (glibc)

Triple: `aarch64-linux-gnu` / `x86_64-linux-gnu`

### Zig's approach

Zig does NOT build full glibc. It vendors a small subset + metadata, then
synthesizes stub shared libraries for link-time symbol resolution:

- **Vendored source** (`lib/libc/glibc/`): only CRT startup (`csu/start.S`),
  `libc_nonshared.a` sources (~10 .c files), and `abilists` (242KB metadata
  blob mapping symbols → versions → targets)
- **CRT built from source**: `Scrt1.o` (from `sysdeps/<arch>/start.S`) +
  `libc_nonshared.a` (atexit, pthread_atfork, stack_chk_fail_local, …)
- **NO crti.o / crtn.o** — omitted in vendored path; modern glibc uses
  `.init_array`/`.fini_array` sections managed by crt1 + linker
- **Stub .so files**: synthesized from `abilists` — each symbol becomes a
  `.symver` directive in generated assembly. Produces `libc.so.6`,
  `libm.so.6`, `ld-linux.so.2`, etc. These carry symbol versioning only,
  no implementations. Real glibc expected on target at runtime.
- **Headers** (4 layers in `lib/libc/include/`):
  - `<arch>-linux-gnu/` — arch-specific bits
  - `generic-glibc/` — multi-version public headers (one set, `#ifdef` per version)
  - `<arch>-linux-any/` — arch-specific kernel UAPI (`asm/`)
  - `any-linux-any/` — kernel UAPI (`linux/`, `asm-generic/`, `drm/`, …)

### bake TODO

- [ ] `LibcFamily::Glibc` + case in `resolve_libc_family()`
- [ ] `TargetSpec::is_linux_gnu()` — `triple_.contains("linux") && triple_.contains("gnu")`
- [ ] Vendor glibc CRT subset + `abilists` metadata
- [ ] Vendor glibc headers (4-layer layout) + Linux kernel UAPI headers
- [ ] `ensure_glibc_objects()`: build `crt1.o`/`Scrt1.o` + `libc_nonshared.a`
- [ ] `ensure_glibc_stubs()`: generate stub `.so` files from `abilists`
- [ ] Link flags: `-lm -lpthread -lc -ldl -lrt -lutil` (or glibc ≥2.34: `-lm -lc`)
- [ ] Dynamic linker path per arch (`/lib64/ld-linux-x86-64.so.2`, etc.)
- [ ] Driver: `inject_vendored_headers` glibc path (4-layer layout)
- [ ] libc++ already compiles for linux; just needs glibc headers visible

---

## linux-ohos (OpenHarmony)

Triple: `aarch64-linux-ohos`

### Zig's approach

Zig recognizes OHOS as an ABI tag (`Target.Abi.ohos`, `.ohoseabi`) but does
NOT vendor any OHOS libc, headers, or CRT. OHOS is not in `available_libcs`,
so `canBuildLibC()` returns false. Users must provide their own sysroot/NDK.

Zig's OHOS support is limited to:
- Triple parsing (`aarch64-linux-ohos`)
- Link flags: `-lm -lc -ldl` (same as Android)
- LLVM codegen environment name: `"ohos"`

### bake TODO

- [ ] LLVM built with OHOS target support (check if our LLVM build includes it)
- [ ] `TargetSpec::is_ohos()` — `triple_.contains("ohos")`
- [ ] Vendor OHOS NDK headers (based on musl + OHOS extensions)
- [ ] Vendor OHOS CRT objects (OHOS uses a patched musl)
- [ ] `ensure_ohos_objects()` or reuse `LibcFamily::Musl` with OHOS-specific headers
- [ ] Link flags: `-lm -lc -ldl` (Android-style)
- [ ] Driver: `inject_vendored_headers` OHOS header block
- [ ] Decide: self-contained (build from OHOS musl source) vs NDK-dependent
