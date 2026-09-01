/*
 * lld.cppm — bake.toolchain.lld: in-process LLD link + archive write.
 *
 * LLVM/LLD headers go in the global module fragment. LLD_HAS_DRIVER
 * entries must stay there so they get global mangling (matching the
 * LLD library definitions).
 */

module;

#include <lld/Common/CommonLinkerContext.h>
#include <lld/Common/Driver.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/raw_ostream.h>

// LLD driver entry points must be declared in the global module fragment
// so they get global mangling (matching the LLD library definitions).
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(wasm)

export module bake.toolchain.lld;

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
export int bake_lld_link(LldFlavor flavor, int argc, const char **argv) {
    std::vector<const char *> args(argv, argv + argc);

    bool ok = false;
    switch (flavor) {
    case LldFlavor::ELF:
        ok = lld::elf::link(args, llvm::outs(), llvm::errs(),
                            /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case LldFlavor::COFF:
        ok = lld::coff::link(args, llvm::outs(), llvm::errs(),
                             /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case LldFlavor::MACHO:
        ok = lld::macho::link(args, llvm::outs(), llvm::errs(),
                              /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case LldFlavor::WASM:
        ok = lld::wasm::link(args, llvm::outs(), llvm::errs(),
                             /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case LldFlavor::MinGW:
        ok = lld::mingw::link(args, llvm::outs(), llvm::errs(),
                              /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    }

    lld::CommonLinkerContext::destroy();
    return ok ? 0 : 1;
}

// In-process archive write (replaces `ar rcs`).
// archive_kind: 0=GNU, 1=BSD, 2=DARWIN, 3=COFF
export int bake_ar_write(const char *archive_name,
                         const char **members, std::size_t member_count,
                         int archive_kind) {
    llvm::object::Archive::Kind kind;
    switch (archive_kind) {
    case 0: kind = llvm::object::Archive::K_GNU;    break;
    case 1: kind = llvm::object::Archive::K_BSD;    break;
    case 2: kind = llvm::object::Archive::K_DARWIN; break;
    case 3: kind = llvm::object::Archive::K_COFF;   break;
    default:
        return 1;
    }

    llvm::SmallVector<llvm::NewArchiveMember, 4> new_members;
    for (std::size_t i = 0; i < member_count; i++) {
        llvm::Expected<llvm::NewArchiveMember> m =
            llvm::NewArchiveMember::getFile(members[i], /*Deterministic=*/true);
        if (llvm::Error err = m.takeError()) {
            llvm::consumeError(std::move(err));
            return 1;
        }
        new_members.push_back(std::move(*m));
    }

    llvm::Error err = writeArchive(archive_name, new_members,
                                   llvm::SymtabWritingMode::NormalSymtab, kind,
                                   /*Deterministic=*/true,
                                   /*Thin=*/false,
                                   /*OldArchiveBuf=*/nullptr);
    if (err) {
        llvm::consumeError(std::move(err));
        return 1;
    }

    return 0;
}
