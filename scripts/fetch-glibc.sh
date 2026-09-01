#!/usr/bin/env bash
# fetch-glibc.sh — Vendor glibc for linux-gnu targets, from upstream only.
#
#   glibc/                    source subset: crt + libc_nonshared members,
#                             internal include tree, sysdeps subset (2 arches)
#   glibc/abilists            symbol/version table (bake text format)
#   glibc/VERSION, REVISION   provenance
#   include/generic-glibc/    arch-independent public headers (split), from
#                             the NEWEST glibc, features.h patched so
#                             __GLIBC__/__GLIBC_MINOR__ are -D-overridable
#                             (bake pins them to the target version)
#   include/x86_64-linux-gnu/, aarch64-linux-gnu/   per-arch bits
#
# Stages (STAGE=headers|source|abi|all):
#   headers  install-headers of $ABI_SEED_VERSION per arch (Docker), split
#            generic vs per-arch by content comparison, patch features.h
#   source   copy source subset from the $GLIBC_VERSION tarball, generate
#            libc-modules.h, empty config.h
#   abi      read $ABI_SEED_VERSION tarball abilist files → abilists
#            via scripts/glibc-abi-gen.py (host-side, no build)
#
# Header/source split: headers come from the newest glibc so post-2.28
# declarations exist at all; the crt/nonshared SOURCE stays at $GLIBC_VERSION
# (the last release whose nonshared-era forms Scrt1/libc_nonshared use).
# At compile time bake pins __GLIBC_MINOR__ to the target version, so an
# older target closes exactly the gates __GLIBC_PREREQ governs.
#
# Kernel UAPI headers (any-linux-any/, *-linux-any/) are NOT produced here —
# they are shared infrastructure owned by fetch-linux-headers.sh.
#
# Requires Docker. Re-runnable — overwrites previous output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBC="$ROOT/lib/libc"
SCRIPTS="$ROOT/scripts"

GLIBC_VERSION="2.28"      # source subset (nonshared-era forms)
ABI_SEED_VERSION="2.42"   # headers + abilist files (newest release)
SHA256_2_28="b1900051afad76f7a4f73e71413df4826dce085ef8ddb785a945b66d7d513082"
SHA256_2_42="d1775e32e4628e64ef930f435b67bb63af7599acb6be2b335b9f19f16509f17f"
MIRROR="${GLIBC_MIRROR:-https://ftp.jaist.ac.jp/pub/GNU/glibc}"
IMAGE="${GLIBC_IMAGE:-debian:trixie}"
STAGE="${STAGE:-all}"

WORK="$ROOT/.tmp/fetch-glibc"
mkdir -p "$WORK"

# Kernel header directories owned by fetch-linux-headers.sh — excluded from
# the glibc header split.
KERNEL_DIRS="linux asm asm-generic drm rdma scsi sound video xen misc mtd cxl regulator fwctl"

die() { echo "error: $*" >&2; exit 1; }

# ════════════════════════════════════════════════════════════════════════
# Tarballs
# ════════════════════════════════════════════════════════════════════════
fetch_tarball() {
    local ver="$1" want_sha="$2"
    local tb="$WORK/glibc-$ver.tar.xz"
    if [ ! -f "$tb" ] || ! xz -t "$tb" 2>/dev/null; then
        echo "==> Downloading glibc $ver"
        curl -fsSL --retry 5 -o "$tb" "$MIRROR/glibc-$ver.tar.xz" \
            || curl -fsSL --retry 5 -o "$tb" "https://ftp.gnu.org/gnu/glibc/glibc-$ver.tar.xz"
    fi
    local got_sha
    got_sha="$(shasum -a 256 "$tb" | cut -d' ' -f1)"
    [ "$got_sha" = "$want_sha" ] || die "sha256 mismatch for glibc-$ver: $got_sha"
}

# ════════════════════════════════════════════════════════════════════════
# Stage: headers — configure + install-headers per arch, then split
# ════════════════════════════════════════════════════════════════════════
install_headers_arch() {
    local arch="$1" platform="$2"
    echo "==> install-headers glibc $ABI_SEED_VERSION ($arch, Docker $platform)"
    rm -rf "$WORK/hdr-$arch"
    docker run --rm --platform "$platform" \
        -v "$WORK:/work" \
        "$IMAGE" bash -euc '
            apt-get update -qq && apt-get install -y -qq \
                gcc gawk bison make python3 texinfo xz-utils >/dev/null
            cd /work
            [ -d "glibc-'"$ABI_SEED_VERSION"'" ] || tar xf "glibc-'"$ABI_SEED_VERSION"'.tar.xz"
            rm -rf bh && mkdir bh && cd bh
            ../"glibc-'"$ABI_SEED_VERSION"'"/configure --prefix=/usr \
                --disable-werror --without-selinux --disable-crypt \
                MAKEINFO=true
            make install-headers install_root="/work/hdr-'"$arch"'"
        '
}

split_headers() {
    echo "==> Splitting headers: generic-glibc/ vs per-arch"
    local any="$LIBC/include/generic-glibc"
    rm -rf "$any" "$LIBC/include/x86_64-linux-gnu" "$LIBC/include/aarch64-linux-gnu"

    prune_kernel_dirs() {
        local dir="$1"
        for k in $KERNEL_DIRS; do rm -rf "$dir/$k"; done
    }

    for arch in x86_64 aarch64; do
        local src="$WORK/hdr-$arch/usr/include"
        [ -d "$src" ] || die "headers for $arch missing ($src)"
        prune_kernel_dirs "$src"
        # Flatten the multiarch dir (usr/include/<triple>/...) into the root.
        if [ -d "$src/$arch-linux-gnu" ]; then
            cp -R "$src/$arch-linux-gnu/." "$src/"
            rm -rf "$src/$arch-linux-gnu"
        fi
    done

    local X="$WORK/hdr-x86_64/usr/include" A="$WORK/hdr-aarch64/usr/include"
    mkdir -p "$any"
    # Files identical across arches → generic. Anything else → per-arch.
    ( cd "$X" && find . -type f ) | while read -r f; do
        if [ -f "$A/$f" ] && cmp -s "$X/$f" "$A/$f"; then
            mkdir -p "$any/$(dirname "$f")"
            cp "$X/$f" "$any/$f"
        fi
    done
    for arch in x86_64 aarch64; do
        local src="$WORK/hdr-$arch/usr/include" dst="$LIBC/include/$arch-linux-gnu"
        mkdir -p "$dst"
        ( cd "$src" && find . -type f ) | while read -r f; do
            if [ ! -f "$any/$f" ]; then
                mkdir -p "$dst/$(dirname "$f")"
                cp "$src/$f" "$dst/$f"
            fi
        done
    done

    # gnu/stubs.h is generated during glibc's full build (from the built
    # libc's symbol table) — install-headers does not produce it. Emit the
    # equivalent for this configuration: the standard linux stub set.
    for arch in x86_64 aarch64; do
        local d="$LIBC/include/$arch-linux-gnu"
        mkdir -p "$d/gnu"
        cat > "$d/gnu/stubs.h" <<'EOF'
/* Generated by scripts/fetch-glibc.sh — equivalent of glibc's build-time
   gnu/stubs.h for arch-linux-gnu (stub functions on this configuration).  */
#ifndef _GNU_LIBC_STUBS_H
#define _GNU_LIBC_STUBS_H

#define __stub_chflags
#define __stub_fchflags
#define __stub_gtty
#define __stub_revoke
#define __stub_setlogin
#define __stub_sigreturn
#define __stub_stty

#endif /* _GNU_LIBC_STUBS_H */
EOF
    done

    # features.h override patch: bake pins the reported glibc version to
    # the TARGET version via -D__GLIBC__/__GLIBC_MINOR__, so headers from
    # the newest release present the surface of the requested one (every
    # version-sensitive declaration in glibc headers gates on
    # __GLIBC_PREREQ). Guard the defines so a -D wins without a
    # redefinition diagnostic; defaults describe the vendored headers.
    local seed_minor="${ABI_SEED_VERSION#*.}"
    python3 - "$any/features.h" "$seed_minor" <<'PYEOF'
import re, sys
p, minor = sys.argv[1], sys.argv[2]
s = open(p).read()
s2 = re.sub(r"^#define\s+__GLIBC__\s+(\d+)",
            r"#ifndef __GLIBC__\n#define __GLIBC__ \1\n#endif", s, count=1, flags=re.M)
s2 = re.sub(r"^#define\s+__GLIBC_MINOR__\s+(\d+)",
            rf"#ifndef __GLIBC_MINOR__\n#define __GLIBC_MINOR__ {minor}\n#endif", s2, count=1, flags=re.M)
if s2 == s:
    sys.exit("features.h patch failed to match")
open(p, "w").write(s2)
PYEOF
    echo "  → features.h version-override patched (defaults: $ABI_SEED_VERSION)"

    echo "  → generic-glibc/       ($(find "$any" -name '*.h' | wc -l | tr -d ' ') headers)"
    for arch in x86_64 aarch64; do
        local d="$LIBC/include/$arch-linux-gnu"
        echo "  → $arch-linux-gnu/ ($(find "$d" -name '*.h' | wc -l | tr -d ' ') headers)"
    done
}

# ════════════════════════════════════════════════════════════════════════
# Stage: source — subset copy from the tarball
# ════════════════════════════════════════════════════════════════════════
copy_source_subset() {
    echo "==> Copying glibc source subset ($GLIBC_VERSION)"
    local SRC="$WORK/glibc-$GLIBC_VERSION"
    [ -d "$SRC" ] || (cd "$WORK" && tar xf "glibc-$GLIBC_VERSION.tar.xz")
    local DST="$LIBC/glibc"
    rm -rf "$DST"
    mkdir -p "$DST"

    # crt + nonshared members + their internal headers.
    cp -R "$SRC/csu"            "$DST/"
    cp -R "$SRC/include"        "$DST/"
    cp -R "$SRC/nptl"           "$DST/"
    mkdir -p "$DST/stdlib" "$DST/io" "$DST/debug" "$DST/sysdeps"
    cp "$SRC/stdlib/atexit.c" "$SRC/stdlib/at_quick_exit.c" "$SRC/stdlib/exit.h" "$DST/stdlib/"
    for f in stat fstat lstat stat64 fstat64 lstat64 fstatat fstatat64 mknod mknodat; do
        cp "$SRC/io/$f.c" "$DST/io/"
    done
    cp "$SRC/debug/stack_chk_fail_local.c" "$DST/debug/"
    for d in x86_64 x86 aarch64 wordsize-64 nptl pthread gnu posix generic init_array; do
        [ -d "$SRC/sysdeps/$d" ] && cp -R "$SRC/sysdeps/$d" "$DST/sysdeps/"
    done
    mkdir -p "$DST/sysdeps/unix"
    ( cd "$SRC/sysdeps/unix" && find . -maxdepth 1 -type f ) | while read -r f; do
        cp "$SRC/sysdeps/unix/$f" "$DST/sysdeps/unix/" 2>/dev/null || true
    done
    for d in sysv; do
        mkdir -p "$DST/sysdeps/unix/$d"
        ( cd "$SRC/sysdeps/unix/$d" && find . -maxdepth 1 -type f ) | while read -r f; do
            cp "$SRC/sysdeps/unix/$d/$f" "$DST/sysdeps/unix/$d/" 2>/dev/null || true
        done
    done
    # unix/sysv/linux: top-level files + our arches + generic + include.
    mkdir -p "$DST/sysdeps/unix/sysv/linux"
    ( cd "$SRC/sysdeps/unix/sysv/linux" && find . -maxdepth 1 -type f ) | while read -r f; do
        cp "$SRC/sysdeps/unix/sysv/linux/$f" "$DST/sysdeps/unix/sysv/linux/" 2>/dev/null || true
    done
    for d in x86_64 x86 aarch64 generic include; do
        [ -d "$SRC/sysdeps/unix/sysv/linux/$d" ] && \
            cp -R "$SRC/sysdeps/unix/sysv/linux/$d" "$DST/sysdeps/unix/sysv/linux/"
    done
    # unix arch dirs (syscalls etc.).
    for d in x86_64 x86 aarch64; do
        [ -d "$SRC/sysdeps/unix/$d" ] && cp -R "$SRC/sysdeps/unix/$d" "$DST/sysdeps/unix/"
    done

    # Prune test scaffolding from nptl.
    find "$DST/nptl" -name '*.c' ! -name 'pthread_atfork.c' -delete 2>/dev/null || true

    # Arch nptl tls.h pulls build-generated tcb-offsets.h. The crt and
    # libc_nonshared members need no real TLS layout — <tls.h> resolving
    # to sysdeps/generic/tls.h (the freestanding stub) is sufficient and
    # is exactly how the reference implementation prunes this tree.
    rm -f "$DST"/sysdeps/*/nptl/tls.h "$DST"/sysdeps/nptl/tls.h
    find "$DST" -type d -name tests -prune -exec rm -rf {} + 2>/dev/null || true

    # Internal headers redirect to the real headers in the source dirs
    # (include/stdlib.h → <stdlib/stdlib.h>, include/sys/cdefs.h →
    # <misc/sys/cdefs.h>, include/sys/stat.h → <io/sys/stat.h>, ...).
    # Copy every top-level source header (depth 2) so the whole redirect
    ( cd "$SRC" && find . -name '*.h' \
        -not -path './sysdeps/*' -not -path './scripts/*' \
        -not -path './manual/*' -not -path './po/*' \
        -not -path './localedata/*' -not -path './iconvdata/*' \
        -not -path './include/*' \
        -not -path './htl/*' -not -path './nptl/*' \
        -not -path '*/tests/*' -not -path '*/test-*' \
      ) | while read -r f; do
        mkdir -p "$DST/$(dirname "$f")"
        cp "$SRC/$f" "$DST/$f"
    done

    # libc-modules.h: upstream generates this from configure's all-subdirs,
    # but libc-symbols.h only compares MODULE_<name> numbers against
    # MODULE_LIBS_BEGIN, and the bake subset compiles everything with
    # -DMODULE_NAME=libc. Emit the minimal correct mapping.
    cat > "$DST/include/libc-modules.h" <<'EOF'
/* Generated by scripts/fetch-glibc.sh — minimal mapping for bake's
   vendored subset (everything compiles as MODULE_NAME=libc). */
#define MODULE_LIBS_BEGIN 1
#define MODULE_libc 2
EOF
    # config.h stays empty — nothing in the subset needs configure results.

    # abi-tag.h: generated by configure from the top-level abi-tags file.
    # Every bake gnu target is *-linux-* → ABI tag (0, 2.0.0), so the
    # generated content is constant.
    cat > "$DST/csu/abi-tag.h" <<'EOF'
/* Generated by scripts/fetch-glibc.sh from the abi-tags rule
   ".*-.*-linux.*  0  2.0.0".  */
#define __ABI_TAG_OS 0
#ifndef __ABI_TAG_VERSION
# define __ABI_TAG_VERSION 2,0,0
#endif
EOF
    : > "$DST/include/config.h"

    # bake-owned patch series over the upstream files (kept outside the
    # regenerated tree, in scripts/glibc-patches/).
    if ls "$ROOT/scripts/glibc-patches"/*.patch >/dev/null 2>&1; then
        echo "  applying patches:"
        for p in "$ROOT/scripts/glibc-patches"/*.patch; do
            echo "    $(basename "$p")"
            ( cd "$DST" && patch -p1 --silent < "$p" ) || die "patch failed: $p"
        done
    fi

    # Provenance + license.
    echo "$GLIBC_VERSION" > "$DST/VERSION"
    cat > "$DST/REVISION" <<EOF
source:   glibc $GLIBC_VERSION (GNU release tarball, sha256 $SHA256_2_28)
headers:  glibc $ABI_SEED_VERSION install-headers + features.h version
          override patch (generic-glibc + per-triple)
abilists: glibc $ABI_SEED_VERSION full builds (scripts/glibc-abi-gen.py)
kernel:   see lib/libc/include/linux-uapi-VERSION (fetch-linux-headers.sh)
EOF
    for f in COPYING COPYING.LIB LICENSES; do
        [ -f "$SRC/$f" ] && cp "$SRC/$f" "$DST/"
    done

    du -sh "$DST"
}

# ════════════════════════════════════════════════════════════════════════
# Stage: abi — full build of the seed version per arch, readelf → abilists
# ════════════════════════════════════════════════════════════════════════
gen_abilists() {
    echo "==> Generating abilists from glibc $ABI_SEED_VERSION abilist files"
    local SRC="$WORK/glibc-$ABI_SEED_VERSION"
    [ -d "$SRC" ] || (cd "$WORK" && tar xf "glibc-$ABI_SEED_VERSION.tar.xz")
    python3 "$SCRIPTS/glibc-abi-gen.py" \
        --seed-version "$ABI_SEED_VERSION" \
        --src "$SRC" \
        --out "$LIBC/glibc/abilists"
    wc -l "$LIBC/glibc/abilists"
}

# ════════════════════════════════════════════════════════════════════════
# Integrity assertions — everything ensure_glibc_objects will reference.
# ════════════════════════════════════════════════════════════════════════
assert_vendor() {
    echo "==> Asserting vendor integrity"
    local DST="$LIBC/glibc"
    local missing=0
    check() { [ -e "$1" ] || { echo "  MISSING: $1"; missing=1; } }

    # crt sources
    for arch in x86_64 aarch64; do
        check "$DST/sysdeps/$arch/start.S"
    done
    check "$DST/csu/abi-note.S"
    check "$DST/csu/init.c"
    # nonshared members
    check "$DST/csu/elf-init.c"
    check "$DST/stdlib/atexit.c"
    check "$DST/stdlib/at_quick_exit.c"
    check "$DST/nptl/pthread_atfork.c"
    check "$DST/debug/stack_chk_fail_local.c"
    for f in stat fstat lstat stat64 fstat64 lstat64 fstatat fstatat64 mknod mknodat; do
        check "$DST/io/$f.c"
    done
    # internal include chain anchors
    check "$DST/include/libc-symbols.h"
    check "$DST/include/libc-modules.h"
    check "$DST/include/config.h"
    for d in x86_64 x86 aarch64 wordsize-64 posix generic; do
        check "$DST/sysdeps/$d"
    done
    for d in x86_64 x86 aarch64 generic include; do
        check "$DST/sysdeps/unix/sysv/linux/$d"
    done
    check "$DST/abilists"

    # public headers
    check "$LIBC/include/generic-glibc/features.h"
    check "$LIBC/include/generic-glibc/stdio.h"
    for t in x86_64-linux-gnu aarch64-linux-gnu; do
        check "$LIBC/include/$t/gnu/stubs.h"
        check "$LIBC/include/generic-glibc/bits/stdio_lim.h"
    done
    grep -q '#ifndef __GLIBC_MINOR__' "$LIBC/include/generic-glibc/features.h" \
        || die "features.h version-override patch missing (re-run STAGE=headers)"

    # shared kernel UAPI (owned by fetch-linux-headers.sh)
    check "$LIBC/include/any-linux-any/linux/version.h"
    check "$LIBC/include/linux-uapi-VERSION"

    [ "$missing" -eq 0 ] || die "vendor integrity check failed"
    echo "  all present"
}

# ════════════════════════════════════════════════════════════════════════
# Run
# ════════════════════════════════════════════════════════════════════════
case "$STAGE" in
  headers)
    fetch_tarball "$ABI_SEED_VERSION" "$SHA256_2_42"
    install_headers_arch x86_64 linux/amd64
    install_headers_arch aarch64 linux/arm64
    split_headers
    ;;
  source)
    fetch_tarball "$GLIBC_VERSION" "$SHA256_2_28"
    copy_source_subset
    ;;
  abi)
    fetch_tarball "$ABI_SEED_VERSION" "$SHA256_2_42"
    gen_abilists
    ;;
  all)
    fetch_tarball "$GLIBC_VERSION" "$SHA256_2_28"
    fetch_tarball "$ABI_SEED_VERSION" "$SHA256_2_42"
    install_headers_arch x86_64 linux/amd64
    install_headers_arch aarch64 linux/arm64
    split_headers
    copy_source_subset
    gen_abilists
    assert_vendor
    ;;
  *) die "unknown STAGE='$STAGE'" ;;
esac

echo "==> Done (STAGE=$STAGE)"
