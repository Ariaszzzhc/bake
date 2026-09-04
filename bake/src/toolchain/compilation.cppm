module;

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"

export module bake.toolchain.compilation;

import std;
import bake.toolchain.driver;

// ============================================================
// bake.toolchain.compilation — injectable in-process frontend
//
// The seam between bake's build system (or LSP) and the embedded
// Clang frontend: run one compiler invocation through the real
// driver lowering (vendored header injection, std-module
// provisioning, target normalization — one source of truth in
// driver.cppm) while overriding the cc1 execution with:
//
//   - remapped file buffers (unsaved editor content)
//   - a structured diagnostic collector
//
// This is the compilation abstraction every tool that needs to
// *inspect* a compile rather than produce its outputs builds on
// (the language server serves diagnostics through it). It keeps the
// stock cc1 path (bake cc / bake c++) untouched.
// ============================================================

namespace bake {

export struct SourceDiagnostic {
    std::string severity;  // "fatal" | "error" | "warning" | "note" | "remark"
    std::string file;
    unsigned line = 1;
    unsigned column = 1;
    std::string message;
};

export struct CompileOutcome {
    int status = 0;  // compiler exit status (0 = success)
    std::vector<SourceDiagnostic> diagnostics;
};

namespace {

std::string severity_name(clang::DiagnosticsEngine::Level level) {
    switch (level) {
        case clang::DiagnosticsEngine::Fatal:    return "fatal";
        case clang::DiagnosticsEngine::Error:    return "error";
        case clang::DiagnosticsEngine::Warning:  return "warning";
        case clang::DiagnosticsEngine::Note:     return "note";
        case clang::DiagnosticsEngine::Remark:   return "remark";
        default:                                 return "ignored";
    }
}

class CollectingDiagnostics : public clang::DiagnosticConsumer {
public:
    explicit CollectingDiagnostics(std::vector<SourceDiagnostic>* out)
        : m_out(out) {}

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& info) override {
        SourceDiagnostic d;
        d.severity = severity_name(level);

        llvm::SmallString<256> formatted;
        info.FormatDiagnostic(formatted);
        d.message = formatted.str();

        if (info.hasSourceManager() && info.getLocation().isValid()) {
            auto presumed = info.getSourceManager().getPresumedLoc(
                info.getLocation());
            if (presumed.isValid()) {
                if (const char* file = presumed.getFilename())
                    d.file = file;
                d.line = static_cast<unsigned>(presumed.getLine());
                d.column = static_cast<unsigned>(presumed.getColumn());
            }
        }
        m_out->push_back(std::move(d));
    }

private:
    std::vector<SourceDiagnostic>* m_out;
};

} // namespace

// Run one compile in-process. driver_argv has the shape of a build
// action command: {<bake exe>, "c++"|"cc", flags..., source} —
// typically with -fsyntax-only (no outputs are produced).
// remapped_files overlays unsaved editor content by path; the
// buffers live for the duration of the compile only.
export CompileOutcome run_syntax_check(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files) {
    CompileOutcome outcome;

    // Remapped buffers must outlive the CompilerInstance; with
    // RetainRemappedFileBuffers the preprocessor never frees them,
    // and this vector reclaims them at scope exit.
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;

    Cc1Hook hook = [&](const char* const* argv, int argc) -> int {
        // argv[1] is "-cc1"; the cc1 argument vector follows.
        std::vector<const char*> cc1_args(argv + 2, argv + argc);
        const char* argv0 = argv[0];

        llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diag_ids(
            new clang::DiagnosticIDs());

        auto pch_ops = std::make_shared<clang::PCHContainerOperations>();
        pch_ops->registerWriter(
            std::make_unique<clang::ObjectFilePCHContainerWriter>());
        pch_ops->registerReader(
            std::make_unique<clang::ObjectFilePCHContainerReader>());

        CollectingDiagnostics collector(&outcome.diagnostics);
        clang::DiagnosticOptions diag_opts;
        clang::DiagnosticsEngine diags(diag_ids, diag_opts, &collector,
                                       /*ShouldOwnClient=*/false);

        auto invocation = std::make_shared<clang::CompilerInvocation>();
        bool parsed = clang::CompilerInvocation::CreateFromArgs(
            *invocation, cc1_args, diags, argv0);
        if (!parsed) {
            outcome.status = 1;
            return 1;
        }

        auto& pp_opts = invocation->getPreprocessorOpts();
        for (const auto& [path, content] : remapped_files) {
            buffers.push_back(
                llvm::MemoryBuffer::getMemBufferCopy(content, path));
            pp_opts.addRemappedFile(path, buffers.back().get());
        }
        pp_opts.RetainRemappedFileBuffers = true;

        auto clang = std::make_unique<clang::CompilerInstance>(
            std::move(invocation), std::move(pch_ops));

        clang->createDiagnostics(&collector, /*ShouldOwnClient=*/false);
        if (!clang->hasDiagnostics()) {
            outcome.status = 1;
            return 1;
        }

        bool success = clang::ExecuteCompilerInvocation(clang.get());
        outcome.status = success ? 0 : 1;
        return outcome.status;
    };

    std::vector<const char*> argv;
    argv.reserve(driver_argv.size());
    for (const auto& a : driver_argv)
        argv.push_back(a.c_str());

    int rc = bake_clang_main_ex(static_cast<int>(argv.size()), argv.data(),
                                std::move(hook));
    if (outcome.status == 0)
        outcome.status = rc;
    return outcome;
}

} // namespace bake
