# Platform Roadmap

Current targets: **linux-musl** (aarch64, x86_64), **macos/darwin** (aarch64, x86_64), **windows-gnu** (x86_64, aarch64).

bake can self-host on all three platforms. Use `./bootstrap/build <target>` to cross-compile bake itself.

Extension point: `resolve_libc_family()` + `prepare_runtime()` switch in
`bake.compiler.cppm`. Adding a new libc family only requires a new enum value,
a resolve case, a prepare case, and a builder function. The linker dispatch
(`bake_clang_driver.cpp`) stays untouched.

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

This is NOT a license issue — OpenHarmony's musl is MIT licensed (same as
upstream musl), fully compatible with vendoring. Zig treats OHOS the same as
Android (Bionic): ABI tag + link flags only, no self-contained libc. The
reasons are technical complexity (OHOS musl is heavily patched with
OHOS-specific syscalls and capability model) and low priority. OHOS NDK is
the official cross-compilation path.

### bake TODO

bake has two options, unlike Zig:
1. **Self-contained** — vendor OpenHarmony's musl source (MIT), build from
   source like we do with upstream musl. Requires tracking OHOS patches.
2. **NDK-dependent** — same as Zig, recognize the target + inject NDK
   headers/libs from user-provided sysroot.

- [ ] LLVM built with OHOS target support (check if our LLVM build includes it)
- [ ] `TargetSpec::is_ohos()` — `triple_.contains("ohos")`
- [ ] Link flags: `-lm -lc -ldl` (Android-style)
- [ ] Driver: `inject_vendored_headers` OHOS header block
- [ ] If self-contained: vendor OHOS musl source + `ensure_ohos_objects()`
- [ ] If NDK-dependent: sysroot discovery + header injection from NDK path
