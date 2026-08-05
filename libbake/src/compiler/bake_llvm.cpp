/*
 * bake_llvm.cpp — LLVM/LLD/AR C++ wrapper layer for bake.
 *
 * This file isolates all LLVM/LLD C++ API usage behind a C ABI so that
 * bake's C++23 module code (which uses `import std;`) can call into
 * LLVM without directly including its headers.
 *
 * This is a regular C++ translation unit — NOT a module unit.
 * Do NOT use `import std;` here.
 */

#include "bake_llvm.h"

#include <lld/Common/CommonLinkerContext.h>
#include <lld/Common/Driver.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>

// Forward-declare the LLD driver entry points for the flavors we support.
// In LLVM 17+, these are no longer declared in the public headers; the
// LLD_HAS_DRIVER macro generates the declarations.
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(wasm)

int bake_lld_link(int flavor, int argc, const char **argv) {
    std::vector<const char *> args(argv, argv + argc);

    bool ok = false;
    switch (flavor) {
    case BAKE_LLD_ELF:
        ok = lld::elf::link(args, llvm::outs(), llvm::errs(),
                            /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case BAKE_LLD_COFF:
        ok = lld::coff::link(args, llvm::outs(), llvm::errs(),
                             /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case BAKE_LLD_MACHO:
        ok = lld::macho::link(args, llvm::outs(), llvm::errs(),
                              /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    case BAKE_LLD_WASM:
        ok = lld::wasm::link(args, llvm::outs(), llvm::errs(),
                             /*exitEarly=*/false, /*disableOutput=*/false);
        break;
    default:
        llvm::errs() << "bake: unknown LLD flavor " << flavor << "\n";
        return 1;
    }

    // Clean up global LLD state so the linker can be invoked again.
    lld::CommonLinkerContext::destroy();

    return ok ? 0 : 1;
}

int bake_ar_write(const char *archive_name,
                  const char **members, size_t member_count,
                  int archive_kind) {
    // Map our public enum to LLVM's object::Archive::Kind.
    llvm::object::Archive::Kind kind;
    switch (archive_kind) {
    case 0: kind = llvm::object::Archive::K_GNU;    break;
    case 1: kind = llvm::object::Archive::K_BSD;    break;
    case 2: kind = llvm::object::Archive::K_DARWIN; break;
    case 3: kind = llvm::object::Archive::K_COFF;   break;
    default:
        llvm::errs() << "bake: unknown archive kind " << archive_kind << "\n";
        return 1;
    }

    llvm::SmallVector<llvm::NewArchiveMember, 4> new_members;
    for (size_t i = 0; i < member_count; i++) {
        llvm::Expected<llvm::NewArchiveMember> m =
            llvm::NewArchiveMember::getFile(members[i], /*Deterministic=*/true);
        if (llvm::Error err = m.takeError()) {
            llvm::consumeError(std::move(err));
            llvm::errs() << "bake: cannot read archive member '" << members[i]
                         << "'\n";
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

int bake_has_llvm(void) {
    return 1;
}
