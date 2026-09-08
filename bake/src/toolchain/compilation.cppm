module;

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"

#include <functional>
#include <map>
#include <mutex>

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
//   - optional preamble PCH build / load
//   - code completion at a position
//   - symbol occurrence collection (decls, refs, macros)
//
// This is the compilation abstraction every tool that needs to
// *inspect* a compile rather than produce its outputs builds on
// (the language server serves diagnostics, completion and the
// index through it). It keeps the stock cc1 path (bake cc /
// bake c++) untouched.
//
// Clang cc1 holds process-global state (diagnostic registries,
// backend setup), so concurrent invocations are serialized by
// one process-wide mutex regardless of the calling thread.
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

// Load a cached preamble PCH while parsing: the implicit include,
// the recorded byte range the preprocessor must skip because the
// PCH already covers it, and the region cap the PCH was built
// under (natural bounds when unset). Module-importing files use a
// cap so the PCH stops before the first import/module line and
// the imports re-run against real module files.
export struct PchLoad {
    std::string path;
    unsigned preamble_size = 0;
    bool preamble_ends_at_start_of_line = false;
    std::size_t region_limit = std::size_t(-1);

    bool empty() const { return path.empty(); }
};

export struct CompileOptions {
    PchLoad pch;
};

export CompileOutcome run_syntax_check(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const CompileOptions& options = {});

export struct PreambleBuild {
    bool ok = false;
    unsigned preamble_size = 0;    // effective region size
    bool preamble_ends_at_start_of_line = false;
    CompileOutcome outcome;
};

// Compile the preamble region of `main_file` (taken from the
// remapped buffer) into a PCH at `pch_out`. The region is
// min(ComputePreamble, region_limit): callers cap it before the
// first import/module line for module TUs (PCHs cannot carry
// module imports), so the skipped bytes and the PCH content stay
// in lockstep. The recorded bounds are the effective region's.
export PreambleBuild run_preamble_build(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const std::string& main_file,
        const std::string& pch_out,
        std::size_t region_limit);

export struct CompletionItemData {
    std::string label;        // text to type (the TypedText chunks)
    std::string detail;       // full render, e.g. "[#int#]foo()"
    std::string kind;         // "keyword" | "macro" | decl kind name
    int priority = 0;         // Clang's completion priority (lower first)
};

// Run code completion at a 1-based position inside `main_file`
// (remapped buffer when present).
export std::vector<CompletionItemData> run_code_complete(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const std::string& main_file,
        unsigned line,
        unsigned column);

export struct SymbolOccurrence {
    std::string usr;          // stable symbol identity (Clang USR)
    std::string name;
    std::string kind;         // Decl kind name, or "macro"
    bool is_decl = false;
    bool is_def = false;
    bool is_ref = false;
    bool is_call = false;
    std::string file;
    unsigned line = 1;
    unsigned column = 1;
    // Where the referenced declaration lives (equals the occurrence
    // for declarations themselves). This is the jump target when the
    // defining file has not been indexed yet.
    std::string decl_file;
    unsigned decl_line = 0;
    unsigned decl_column = 0;
};

// Collect every symbol occurrence (declarations, definitions,
// references, calls, macros) produced by parsing `main_file`.
export std::vector<SymbolOccurrence> run_symbol_collection(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const std::string& main_file);

namespace {

std::mutex g_compile_mutex;  // serializes all in-process cc1 runs

bool role_has(clang::index::SymbolRoleSet set, clang::index::SymbolRole role) {
    return (set & static_cast<clang::index::SymbolRoleSet>(role)) != 0;
}

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

clang::CodeCompleteOptions completion_base_options() {
    clang::CodeCompleteOptions opts;
    opts.IncludeMacros = true;
    opts.IncludeGlobals = true;
    opts.IncludeBriefComments = false;
    return opts;
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

class CollectingCompletion : public clang::CodeCompleteConsumer {
public:
    explicit CollectingCompletion(std::vector<CompletionItemData>* out)
        : clang::CodeCompleteConsumer(completion_base_options()),
          m_out(out), m_cctu_info(m_allocator) {}

    void ProcessCodeCompleteResults(
            clang::Sema& sema, clang::CodeCompletionContext context,
            clang::CodeCompletionResult* results,
            unsigned num_results) override {
        for (unsigned i = 0; i < num_results; ++i) {
            clang::CodeCompletionResult& r = results[i];
            CompletionItemData item;
            item.priority = r.Priority;

            if (r.Kind == clang::CodeCompletionResult::RK_Keyword) {
                item.kind = "keyword";
                item.label = r.getKeyword();
            } else if (r.Kind == clang::CodeCompletionResult::RK_Macro) {
                item.kind = "macro";
                item.label = r.Macro->getName().str();
            } else {
                clang::CodeCompletionString* string = r.CreateCodeCompletionString(
                    sema, context, *m_allocator, m_cctu_info,
                    /*IncludeBriefComments=*/false);
                if (!string) continue;
                if (r.Kind == clang::CodeCompletionResult::RK_Declaration)
                    item.kind = r.Declaration->getDeclKindName();
                else
                    item.kind = "pattern";
                item.label = string->getAllTypedText();
                item.detail = string->getAsString();
            }
            if (!item.label.empty())
                m_out->push_back(std::move(item));
        }
    }

    void ProcessOverloadCandidates(
            clang::Sema& sema, unsigned current_arg,
            [[maybe_unused]] clang::CodeCompleteConsumer::OverloadCandidate*
                candidates,
            [[maybe_unused]] unsigned num_candidates,
            [[maybe_unused]] clang::SourceLocation open_paren,
            [[maybe_unused]] bool braced) override {
        // Overload chunks are not offered yet; the candidate count
        // is kept for future use.
        (void)sema;
        (void)current_arg;
    }

    clang::CodeCompletionAllocator& getAllocator() override {
        return *m_allocator;
    }
    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() override {
        return m_cctu_info;
    }

private:
    std::vector<CompletionItemData>* m_out;
    std::shared_ptr<clang::GlobalCodeCompletionAllocator> m_allocator =
        std::make_shared<clang::GlobalCodeCompletionAllocator>();
    clang::CodeCompletionTUInfo m_cctu_info;
};

class CollectingIndex : public clang::index::IndexDataConsumer {
public:
    explicit CollectingIndex(std::vector<SymbolOccurrence>* out)
        : m_out(out) {}

    void initialize(clang::ASTContext& ctx) override {
        m_sm = &ctx.getSourceManager();
    }

    bool handleDeclOccurrence(
            const clang::Decl* decl, clang::index::SymbolRoleSet roles,
            llvm::ArrayRef<clang::index::SymbolRelation> relations,
            clang::SourceLocation loc,
            [[maybe_unused]] ASTNodeInfo node) override {
        (void)relations;
        if (loc.isInvalid() || !m_sm) return true;

        const clang::SourceManager& sm = *m_sm;
        if (sm.isInSystemHeader(loc)) return true;

        clang::PresumedLoc presumed = sm.getPresumedLoc(loc);
        const bool presumed_ok = presumed.isValid() &&
                                 presumed.getFilename() &&
                                 *presumed.getFilename();
        if (!presumed_ok) return true;

        // Name first (the fallback identity needs it).
        std::string name;
        if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl)) {
            name = named->getDeclName().getAsString();
            if (name.empty()) name = named->getNameAsString();
            if (name.empty()) return true;  // unnamed: not navigable
        } else {
            return true;
        }

        SymbolOccurrence occ;
        llvm::SmallString<128> usr;
        // The bool is extracted and the result validated: the vendored
        // libclangIndex boundary can return a garbage truth value that
        // depends on the call site's register allocation (an ABI drift
        // between the installed archive and its headers). A well-formed
        // USR always starts with the C-language tag, so garbage fails
        // the prefix check and takes the stable fallback instead of
        // poisoning the index.
        const bool generated_ok =
            !clang::index::generateUSRForDecl(decl, usr);
        const bool usr_ok =
            generated_ok && llvm::StringRef(usr).starts_with("c:");
        if (!usr_ok) {
            // Fallback identity: the declaration's own location (the
            // `decl` received for a reference IS the referenced
            // declaration, so definitions and references agree).
            clang::PresumedLoc decl_loc =
                sm.getPresumedLoc(decl->getLocation());
            std::string where =
                decl_loc.isValid() && decl_loc.getFilename()
                    ? std::string(decl_loc.getFilename()) + ":" +
                      std::to_string(decl_loc.getLine())
                    : std::string(presumed.getFilename()) + ":" +
                      std::to_string(presumed.getLine());
            occ.usr = std::string(decl->getDeclKindName()) + ":" + name +
                      ":" + where;
        } else {
            occ.usr = usr.str().str();
        }
        occ.name = std::move(name);

        occ.kind = decl->getDeclKindName();
        occ.is_decl = role_has(roles, clang::index::SymbolRole::Declaration);
        occ.is_def = role_has(roles, clang::index::SymbolRole::Definition);
        occ.is_ref = role_has(roles, clang::index::SymbolRole::Reference);
        occ.is_call = role_has(roles, clang::index::SymbolRole::Call);
        occ.file = presumed.getFilename();
        occ.line = static_cast<unsigned>(presumed.getLine());
        occ.column = static_cast<unsigned>(presumed.getColumn());
        clang::PresumedLoc decl_loc =
            sm.getPresumedLoc(decl->getLocation());
        if (decl_loc.isValid() && decl_loc.getFilename() &&
            *decl_loc.getFilename()) {
            occ.decl_file = decl_loc.getFilename();
            occ.decl_line = static_cast<unsigned>(decl_loc.getLine());
            occ.decl_column =
                static_cast<unsigned>(decl_loc.getColumn());
        } else {
            occ.decl_file = occ.file;
            occ.decl_line = occ.line;
            occ.decl_column = occ.column;
        }
        m_out->push_back(std::move(occ));
        return true;
    }

    bool handleMacroOccurrence(
            const clang::IdentifierInfo* name, const clang::MacroInfo*,
            clang::index::SymbolRoleSet roles,
            clang::SourceLocation loc) override {
        if (loc.isInvalid() || !name || !m_sm) return true;
        const clang::SourceManager& sm = *m_sm;
        if (sm.isInSystemHeader(loc)) return true;
        clang::PresumedLoc presumed = sm.getPresumedLoc(loc);
        if (!presumed.isValid() || !presumed.getFilename() ||
            !*presumed.getFilename())
            return true;
        SymbolOccurrence occ;
        occ.usr = "c:@macro@" + name->getName().str();
        occ.name = name->getName().str();
        occ.kind = "macro";
        occ.is_decl = role_has(roles, clang::index::SymbolRole::Declaration);
        occ.is_def = role_has(roles, clang::index::SymbolRole::Definition);
        occ.is_ref = role_has(roles, clang::index::SymbolRole::Reference);
        occ.file = presumed.getFilename();
        occ.line = static_cast<unsigned>(presumed.getLine());
        occ.column = static_cast<unsigned>(presumed.getColumn());
        m_out->push_back(std::move(occ));
        return true;
    }

private:
    std::vector<SymbolOccurrence>* m_out;
    clang::SourceManager* m_sm = nullptr;
};

// FrontendAction wrapper: the indexing consumer rides a real
// syntax-only parse, so declaration occurrences (not just
// preprocessor-level macros) are collected.
class IndexCollectAction : public clang::SyntaxOnlyAction {
public:
    IndexCollectAction(std::shared_ptr<CollectingIndex> consumer,
                       clang::index::IndexingOptions opts)
        : m_consumer(std::move(consumer)), m_opts(opts) {}

protected:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
            clang::CompilerInstance& ci,
            [[maybe_unused]] llvm::StringRef in_file) override {
        return clang::index::createIndexingASTConsumer(
            m_consumer, m_opts, ci.getPreprocessorPtr());
    }

private:
    std::shared_ptr<CollectingIndex> m_consumer;
    clang::index::IndexingOptions m_opts;
};

// Shared cc1 plumbing for every run_* entry point above.
//
// on_parsed   — after CompilerInvocation::CreateFromArgs, with the
//               invocation's LangOpts available and before buffers
//               are attached; remaps can still be rewritten here
//               (the preamble build swaps the main-file buffer).
// on_instance — after the CompilerInstance exists; consumer hooks.
// execute     — the execution strategy; the default runs the stock
//               ExecuteCompilerInvocation.
struct FrontendHooks {
    std::function<void(clang::CompilerInvocation&,
                       std::map<std::string, llvm::MemoryBuffer*>&)>
        on_parsed;
    std::function<void(clang::CompilerInstance&)> on_instance;
    std::function<int(clang::CompilerInstance&)> execute;
};

int run_frontend_locked(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        CompileOutcome& outcome,
        const FrontendHooks& hooks) {
    // Remapped buffers must outlive the CompilerInstance; with
    // RetainRemappedFileBuffers the preprocessor never frees them,
    // and this vector reclaims them at scope exit.
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
    std::map<std::string, llvm::MemoryBuffer*> buffer_map;
    for (const auto& [path, content] : remapped_files) {
        buffers.push_back(llvm::MemoryBuffer::getMemBufferCopy(content, path));
        buffer_map[path] = buffers.back().get();
    }

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

        if (hooks.on_parsed)
            hooks.on_parsed(*invocation, buffer_map);

        auto& pp_opts = invocation->getPreprocessorOpts();
        for (const auto& [path, buffer] : buffer_map)
            pp_opts.addRemappedFile(path, buffer);
        pp_opts.RetainRemappedFileBuffers = true;

        auto clang = std::make_unique<clang::CompilerInstance>(
            std::move(invocation), std::move(pch_ops));

        clang->createDiagnostics(&collector, /*ShouldOwnClient=*/false);
        if (!clang->hasDiagnostics()) {
            outcome.status = 1;
            return 1;
        }

        if (hooks.on_instance)
            hooks.on_instance(*clang);

        if (hooks.execute) {
            outcome.status = hooks.execute(*clang);
        } else {
            bool success = clang::ExecuteCompilerInvocation(clang.get());
            outcome.status = success ? 0 : 1;
        }
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
    return rc;
}

} // namespace

export CompileOutcome run_syntax_check(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const CompileOptions& options) {
    std::lock_guard<std::mutex> lock(g_compile_mutex);
    CompileOutcome outcome;

    FrontendHooks hooks;
    if (!options.pch.empty()) {
        hooks.on_parsed =
            [&options](clang::CompilerInvocation& invocation,
                       std::map<std::string, llvm::MemoryBuffer*>& bufs) {
                // Safety: the PCH is only valid when the current
                // text's preamble region is byte-identical to the
                // one it was built from. Remaps here are aliases of
                // the one main file (spelling + absolute path); pick
                // the input filename's alias when it matches, else
                // any (the driver may resolve either spelling).
                if (bufs.empty()) return;
                std::string input;
                if (!invocation.getFrontendOpts().Inputs.empty())
                    input = invocation.getFrontendOpts()
                                .Inputs.front()
                                .getFile();
                llvm::MemoryBuffer* main = nullptr;
                for (auto& [key, buffer] : bufs) {
                    (void)key;
                    if (buffer->getBufferIdentifier() == input) {
                        main = buffer;
                        break;
                    }
                }
                if (!main) main = bufs.begin()->second;

                clang::PreambleBounds bounds = clang::Lexer::ComputePreamble(
                    main->getBuffer(), invocation.getLangOpts());
                std::size_t effective = std::min<std::size_t>(
                    bounds.Size, options.pch.region_limit);
                if (effective != options.pch.preamble_size) return;

                auto& pp_opts = invocation.getPreprocessorOpts();
                pp_opts.ImplicitPCHInclude = options.pch.path;
                pp_opts.PrecompiledPreambleBytes = std::make_pair(
                    effective, options.pch.preamble_ends_at_start_of_line);
            };
    }

    run_frontend_locked(driver_argv, remapped_files, outcome, hooks);
    return outcome;
}

export PreambleBuild run_preamble_build(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const std::string& main_file,
        const std::string& pch_out,
        std::size_t region_limit) {
    std::lock_guard<std::mutex> lock(g_compile_mutex);
    PreambleBuild result;
    std::unique_ptr<llvm::MemoryBuffer> preamble_buffer;

    FrontendHooks hooks;
    hooks.on_parsed =
        [&](clang::CompilerInvocation& invocation,
            std::map<std::string, llvm::MemoryBuffer*>& bufs) {
            auto it = bufs.find(main_file);
            if (it == bufs.end()) return;
            clang::PreambleBounds bounds = clang::Lexer::ComputePreamble(
                it->second->getBuffer(), invocation.getLangOpts());
            std::size_t effective = std::min<std::size_t>(
                bounds.Size, region_limit);
            result.preamble_size = static_cast<unsigned>(effective);
            result.preamble_ends_at_start_of_line =
                effective < bounds.Size
                    ? true  // a cap always lands on a line boundary
                    : bounds.PreambleEndsAtStartOfLine;
            // Replace the main-file buffer with the effective
            // preamble region: the PCH is generated from exactly
            // those bytes. Every remap alias of the main file
            // (spelling and absolute path — the driver resolves
            // relative inputs) must swap together, or the cc1
            // input filename picks up an untruncated alias.
            std::string preamble_text(
                it->second->getBuffer().substr(0, effective));
            preamble_buffer =
                llvm::MemoryBuffer::getMemBufferCopy(
                    llvm::StringRef(preamble_text),
                    it->second->getBufferIdentifier());
            llvm::MemoryBuffer* full = it->second;
            for (auto& [key, buffer] : bufs) {
                (void)key;
                if (buffer->getBuffer() == full->getBuffer())
                    buffer = preamble_buffer.get();
            }

            auto& front = invocation.getFrontendOpts();
            front.ProgramAction = clang::frontend::GeneratePCH;
            front.OutputFile = pch_out;
        };

    run_frontend_locked(driver_argv, remapped_files, result.outcome, hooks);
    result.ok = result.outcome.status == 0 &&
                std::filesystem::exists(std::filesystem::path(pch_out));
    return result;
}

export std::vector<CompletionItemData> run_code_complete(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const std::string& main_file,
        unsigned line,
        unsigned column) {
    std::lock_guard<std::mutex> lock(g_compile_mutex);
    std::vector<CompletionItemData> items;
    CompileOutcome outcome;

    FrontendHooks hooks;
    hooks.on_parsed =
        [&main_file, line, column](clang::CompilerInvocation& invocation,
                                   std::map<std::string,
                                            llvm::MemoryBuffer*>&) {
            auto& front = invocation.getFrontendOpts();
            front.CodeCompletionAt.FileName = main_file;
            front.CodeCompletionAt.Line = line;
            front.CodeCompletionAt.Column = column;
            front.SkipFunctionBodies = true;
        };
    hooks.on_instance =
        [&items](clang::CompilerInstance& instance) {
            instance.setCodeCompletionConsumer(
                new CollectingCompletion(&items));
        };

    run_frontend_locked(driver_argv, remapped_files, outcome, hooks);
    return items;
}

export std::vector<SymbolOccurrence> run_symbol_collection(
        const std::vector<std::string>& driver_argv,
        const std::map<std::string, std::string>& remapped_files,
        const std::string& main_file) {
    std::lock_guard<std::mutex> lock(g_compile_mutex);
    std::vector<SymbolOccurrence> occurrences;
    CompileOutcome outcome;
    (void)main_file;

    FrontendHooks hooks;
    hooks.execute = [&occurrences](clang::CompilerInstance& instance) -> int {
        auto consumer = std::make_shared<CollectingIndex>(&occurrences);
        clang::index::IndexingOptions index_opts;
        index_opts.IndexFunctionLocals = false;
        index_opts.IndexParametersInDeclarations = true;
        index_opts.IndexTemplateParameters = false;
        IndexCollectAction action(std::move(consumer), index_opts);
        instance.ExecuteAction(action);
        return instance.getDiagnostics().hasErrorOccurred() ? 1 : 0;
    };

    run_frontend_locked(driver_argv, remapped_files, outcome, hooks);
    return occurrences;
}

} // namespace bake
