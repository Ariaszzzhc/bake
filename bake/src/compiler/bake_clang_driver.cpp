//===-- bake_clang_driver.cpp - Clang GCC-Compatible Driver ---------------===//
//
// bake — in-process Clang driver entry point with LLD link interception.
//
// This is the entry point to the clang driver for bake. It is a thin wrapper
// for functionality in the Driver clang library, with two key differences from
// stock clang:
//
//   1. -cc1 compilation runs in-process (no subprocess) via the Driver::CC1Main
//      callback — identical to stock `-fintegrated-cc1`.
//   2. Link jobs are intercepted and dispatched to the in-process LLD driver
//      (bake_lld_link) instead of spawning a system linker subprocess.
//
// Originally part of the LLVM Project, under the Apache License v2.0 with
// LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

module;

#include "clang/Driver/Driver.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/HeaderInclude.h"
#include "clang/Basic/Stack.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Options/Options.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/ChainedDiagnosticConsumer.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/SerializedDiagnosticPrinter.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Config/llvm-config.h" // for LLVM_ON_UNIX
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/BuryPointer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include <lld/Common/Driver.h>

module bake.llvm;

import std;
import bake.util;
import bake.compiler;

using namespace clang;
using namespace clang::driver;
using namespace llvm::opt;

std::string GetExecutablePath(const char *Argv0, bool CanonicalPrefixes) {
  if (!CanonicalPrefixes) {
    SmallString<128> ExecutablePath(Argv0);
    // Do a PATH lookup if Argv0 isn't a valid path.
    if (!llvm::sys::fs::exists(ExecutablePath))
      if (llvm::ErrorOr<std::string> P =
              llvm::sys::findProgramByName(ExecutablePath))
        ExecutablePath = *P;
    return std::string(ExecutablePath);
  }

  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void *P = (void*) (intptr_t) GetExecutablePath;
  return llvm::sys::fs::getMainExecutable(Argv0, P);
}

static const char *GetStableCStr(llvm::StringSet<> &SavedStrings, StringRef S) {
  return SavedStrings.insert(S).first->getKeyData();
}

extern int cc1_main(ArrayRef<const char *> Argv, const char *Argv0,
                    void *MainAddr);

// cc1as_main is compiled from Clang's cc1as_main.cpp (as bake_clang_cc1as_main.cpp).
int cc1as_main(ArrayRef<const char *> Argv, const char *Argv0,
               void *MainAddr);

static void insertTargetAndModeArgs(const ParsedClangName &NameParts,
                                    SmallVectorImpl<const char *> &ArgVector,
                                    llvm::StringSet<> &SavedStrings) {
  // Put target and mode arguments at the start of argument list so that
  // arguments specified in command line could override them. Avoid putting
  // them at index 0, as an option like '-cc1' must remain the first.
  int InsertionPoint = 0;
  if (ArgVector.size() > 0)
    ++InsertionPoint;

  if (NameParts.DriverMode) {
    // Add the mode flag to the arguments.
    ArgVector.insert(ArgVector.begin() + InsertionPoint,
                     GetStableCStr(SavedStrings, NameParts.DriverMode));
  }

  if (NameParts.TargetIsValid) {
    const char *arr[] = {"-target", GetStableCStr(SavedStrings,
                                                  NameParts.TargetPrefix)};
    ArgVector.insert(ArgVector.begin() + InsertionPoint,
                     std::begin(arr), std::end(arr));
  }
}

static void getCLEnvVarOptions(std::string &EnvValue, llvm::StringSaver &Saver,
                               SmallVectorImpl<const char *> &Opts) {
  llvm::cl::TokenizeWindowsCommandLine(EnvValue, Saver, Opts);
  // The first instance of '#' should be replaced with '=' in each option.
  for (const char *Opt : Opts)
    if (char *NumberSignPtr = const_cast<char *>(::strchr(Opt, '#')))
      *NumberSignPtr = '=';
}

template <class T>
static T checkEnvVar(const char *EnvOptSet, const char *EnvOptFile,
                     std::string &OptFile) {
  const char *Str = ::getenv(EnvOptSet);
  if (!Str)
    return T{};

  T OptVal = Str;
  if (const char *Var = ::getenv(EnvOptFile))
    OptFile = Var;
  return OptVal;
}

static bool SetBackdoorDriverOutputsFromEnvVars(Driver &TheDriver) {
  TheDriver.CCPrintOptions =
      checkEnvVar<bool>("CC_PRINT_OPTIONS", "CC_PRINT_OPTIONS_FILE",
                        TheDriver.CCPrintOptionsFilename);
  if (checkEnvVar<bool>("CC_PRINT_HEADERS", "CC_PRINT_HEADERS_FILE",
                        TheDriver.CCPrintHeadersFilename)) {
    TheDriver.CCPrintHeadersFormat = HIFMT_Textual;
    TheDriver.CCPrintHeadersFiltering = HIFIL_None;
  } else {
    std::string EnvVar = checkEnvVar<std::string>(
        "CC_PRINT_HEADERS_FORMAT", "CC_PRINT_HEADERS_FILE",
        TheDriver.CCPrintHeadersFilename);
    if (!EnvVar.empty()) {
      TheDriver.CCPrintHeadersFormat =
          stringToHeaderIncludeFormatKind(EnvVar.c_str());
      if (!TheDriver.CCPrintHeadersFormat) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var)
            << 0 << EnvVar;
        return false;
      }

      const char *FilteringStr = ::getenv("CC_PRINT_HEADERS_FILTERING");
      if (!FilteringStr) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var_invalid_format)
            << EnvVar;
        return false;
      }
      HeaderIncludeFilteringKind Filtering;
      if (!stringToHeaderIncludeFiltering(FilteringStr, Filtering)) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var)
            << 1 << FilteringStr;
        return false;
      }

      if ((TheDriver.CCPrintHeadersFormat == HIFMT_Textual &&
           Filtering != HIFIL_None) ||
          (TheDriver.CCPrintHeadersFormat == HIFMT_JSON &&
           Filtering == HIFIL_None)) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var_combination)
            << EnvVar << FilteringStr;
        return false;
      }
      TheDriver.CCPrintHeadersFiltering = Filtering;
    }
  }

  TheDriver.CCLogDiagnostics =
      checkEnvVar<bool>("CC_LOG_DIAGNOSTICS", "CC_LOG_DIAGNOSTICS_FILE",
                        TheDriver.CCLogDiagnosticsFilename);
  TheDriver.CCPrintProcessStats =
      checkEnvVar<bool>("CC_PRINT_PROC_STAT", "CC_PRINT_PROC_STAT_FILE",
                        TheDriver.CCPrintStatReportFilename);
  TheDriver.CCPrintInternalStats =
      checkEnvVar<bool>("CC_PRINT_INTERNAL_STAT", "CC_PRINT_INTERNAL_STAT_FILE",
                        TheDriver.CCPrintInternalStatReportFilename);

  return true;
}

static void FixupDiagPrefixExeName(TextDiagnosticPrinter *DiagClient,
                                   const std::string &Path) {
  // If the clang binary happens to be named cl.exe for compatibility reasons,
  // use clang-cl.exe as the prefix to avoid confusion between clang and MSVC.
  StringRef ExeBasename(llvm::sys::path::stem(Path));
  if (ExeBasename.equals_insensitive("cl"))
    ExeBasename = "clang-cl";
  DiagClient->setPrefix(std::string(ExeBasename));
}

static int ExecuteCC1Tool(SmallVectorImpl<const char *> &ArgV,
                          const llvm::ToolContext &ToolContext) {
  // If we call the cc1 tool from the clangDriver library (through
  // Driver::CC1Main), we need to clean up the options usage count.
  llvm::cl::ResetAllOptionOccurrences();

  llvm::BumpPtrAllocator A;
  llvm::cl::ExpansionContext ECtx(A, llvm::cl::TokenizeGNUCommandLine);
  if (llvm::Error Err = ECtx.expandResponseFiles(ArgV)) {
    llvm::errs() << toString(std::move(Err)) << '\n';
    return 1;
  }
  StringRef Tool = ArgV[1];
  void *GetExecutablePathVP = (void *)(intptr_t)GetExecutablePath;
  if (Tool == "-cc1")
    return cc1_main(ArrayRef(ArgV).slice(1), ArgV[0], GetExecutablePathVP);
  if (Tool == "-cc1as")
    return cc1as_main(ArrayRef(ArgV).slice(2), ArgV[0], GetExecutablePathVP);
  llvm::errs()
      << "error: unknown integrated tool '" << Tool << "'. "
      << "Valid tools include '-cc1' and '-cc1as'.\n";
  return 1;
}

//===----------------------------------------------------------------------===//
// macOS SDK detection (for framework support only)
//===----------------------------------------------------------------------===//

/// Detect the macOS SDK path via `xcrun --show-sdk-path`.
/// Cached after first call. Returns empty string if not found.
/// Used ONLY for framework header/link paths — never for libc++ or libSystem.
static std::string &getMacosSdkPath() {
  static std::string sdk_path;
  static bool initialized = false;
  if (initialized) return sdk_path;
  initialized = true;

#ifdef __APPLE__
  FILE *pipe = ::popen("xcrun --show-sdk-path 2>/dev/null", "r");
  if (pipe) {
    char buf[4096];
    if (fgets(buf, sizeof(buf), pipe)) {
      size_t len = strlen(buf);
      while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
      if (len > 0) sdk_path = buf;
    }
    pclose(pipe);
  }
#endif
  return sdk_path;
}

//===----------------------------------------------------------------------===//
// LLD link interception helpers
//===----------------------------------------------------------------------===//

/// Return true if the executable name looks like a linker invocation.
static bool bakeIsLinkCommand(StringRef ExeName) {
  // Strip any version suffix (e.g. "ld64.lld-14" → "ld64.lld").
  StringRef Stem = llvm::sys::path::stem(ExeName);
  return Stem.contains_insensitive("ld") || Stem == "link" ||
         Stem.contains_insensitive("link.exe");
}

/// Determine the LLD flavor and linker name from the target triple.
/// This is the primary mechanism — the executable name alone is unreliable
/// (e.g. macOS uses plain "ld" which would default to ELF incorrectly).
static void bakeGetLLDInfo(const llvm::Triple &Triple,
                           const char *&LinkerName, LldFlavor &Flavor) {
  switch (Triple.getObjectFormat()) {
  case llvm::Triple::MachO:
    LinkerName = "ld64.lld";
    Flavor = LldFlavor::MACHO;
    break;
  case llvm::Triple::COFF:
    // MinGW (windows-gnu) uses the MinGW driver, which accepts GNU-style
    // args and translates them to COFF internally. MSVC targets use lld-link.
    if (Triple.getEnvironment() == llvm::Triple::GNU) {
      LinkerName = "ld.lld";
      Flavor = LldFlavor::MinGW;
    } else {
      LinkerName = "lld-link";
      Flavor = LldFlavor::COFF;
    }
    break;
  case llvm::Triple::Wasm:
    LinkerName = "wasm-ld";
    Flavor = LldFlavor::WASM;
    break;
  default:
    LinkerName = "ld.lld";
    Flavor = LldFlavor::ELF;
    break;
  }
}

// Explicit -target spec from the current invocation, preserving bake's
// glibc version suffix (LLVM triples cannot encode it). Set during
// argument preprocessing, consumed by the link interception.
static bake::TargetSpec g_explicit_target;
/// Semantics of a libclang_rt.<component>.<ext> reference on a link
/// line — the single parser both dispatch and argument-rewriting use.
enum class SanRtRef {
  Asan,     // asan, asan_osx_dynamic — the runtime itself
  Ubsan,    // ubsan* (standalone and dynamic names)
  Helper,   // asan_cxx/asan_static/*thunk — covered by what bake links
  Builtins, // compiler-rt builtins (swapped for bake's archive)
  Unknown,  // msan, lsan, profile, fuzzer, ... — nothing vendored
  None,     // not a sanitizer reference at all
};
static SanRtRef parseSanRtRef(StringRef Arg, StringRef &Component) {
  auto Pos = Arg.find("libclang_rt.");
  if (Pos == StringRef::npos) return SanRtRef::None;
  StringRef Base = Arg.substr(Pos + 12);
  Component = Base.substr(0, Base.find('.'));
  if (Component.starts_with("asan"))
    return (Component == "asan" || Component == "asan_osx_dynamic")
               ? SanRtRef::Asan
               : SanRtRef::Helper;
  if (Component.starts_with("ubsan")) return SanRtRef::Ubsan;
  if (Component == "builtins") return SanRtRef::Builtins;
  return SanRtRef::Unknown;
}

/// windows resolves the asan runtime DLL from the executable's
/// directory — after a successful link, place the DLL (sibling of the
/// import library bake linked against) next to the linked output.
static void copyMingwAsanDll(const Command *Cmd, StringRef ImportLib) {
  std::string Implib(ImportLib);
  auto Sfx = Implib.find(".dll.a");
  if (Sfx == std::string::npos) return;
  std::string Dll = Implib;
  Dll.replace(Sfx, 6, ".dll");

  std::string Out;
  auto Args = Cmd->getArguments();
  for (size_t i = 0; i + 1 < Args.size(); ++i)
    if (Args[i] && StringRef(Args[i]) == "-o") {
      Out = Args[i + 1] ? Args[i + 1] : "";
      break;
    }
  if (Out.empty()) return;

  std::error_code EC;
  std::filesystem::copy_file(
      Dll, std::filesystem::path(Out).parent_path() /
               std::filesystem::path(Dll).filename(),
      std::filesystem::copy_options::overwrite_existing, EC);
}
/// Execute a single job from the compilation.
///
/// Link jobs are intercepted and dispatched to the in-process LLD driver
/// via bake_lld_link — no linker subprocess is ever spawned.  All other
/// jobs (compile, assemble, etc.) are executed normally via Command::Execute,
/// which for -cc1 jobs uses the CC1Main callback to stay in-process.
static void bakeExecuteJob(const Command *Cmd, const llvm::Triple &Triple,
                           bool IsCxx, int &Res,
                           SmallVectorImpl<std::pair<int, const Command *>> &FailingCommands) {
  StringRef ExeName = llvm::sys::path::filename(Cmd->getExecutable());

  if (bakeIsLinkCommand(ExeName)) {
    // Intercept: dispatch to in-process LLD instead of spawning ld.
    const char *LinkerName = nullptr;
    LldFlavor Flavor = LldFlavor::ELF;
    bakeGetLLDInfo(Triple, LinkerName, Flavor);

    // Build a bake::Toolchain matching the LLVM Triple.
    bake::Toolchain tc;
    tc.exe_path = bake::get_self_exe_path();
    if (tc.exe_path.empty()) tc.exe_path = "bake";
    tc.target = !g_explicit_target.triple_.empty()
        ? g_explicit_target
        : bake::detect_host_target();

    bool is_darwin = Flavor == LldFlavor::MACHO;
    bool is_mingw = Flavor == LldFlavor::MinGW;
    bool is_gnu = !is_darwin && !is_mingw && tc.target.is_linux_gnu();

    // Parse link mode from user args.
    std::vector<std::string> user_args;
    for (const char *Arg : Cmd->getArguments())
      if (Arg) user_args.push_back(Arg);
    bake::LinkMode link_mode = bake::parse_link_mode(user_args);

    // glibc is dynamic-only (broken dlopen/NSS when static). Reject before
    if (is_gnu && link_mode != bake::LinkMode::Dynamic) {
      llvm::errs() << "bake: static linking is not supported for glibc targets\n"
                      "  (glibc's static mode has broken dlopen/NSS "
                      "semantics); use a linux-musl target for static builds.\n";
      FailingCommands.push_back({1, Cmd});
      if (!Res) Res = 1;
      return;
    }
    // Sanitizer runtime dispatch: resolve every libclang_rt.* reference
    // once, mapping components onto the runtimes bake builds from the
    // vendored compiler-rt sources (per-platform form: ELF archives,
    // darwin dylibs, mingw ubsan archive / asan DLL). Components without
    // a vendored runtime are rejected with a clear message (compiling
    // and instrumenting still works — only the link is rejected);
    // ensure_sanitizer_objects reports why a target is unavailable
    // (e.g. asan on non-x86_64 mingw).
    std::string san_ubsan_a, san_asan_a;
    for (const char *Arg : Cmd->getArguments()) {
      if (!Arg) continue;
      StringRef Component;
      SanRtRef Ref = parseSanRtRef(StringRef(Arg), Component);
      if (Ref != SanRtRef::Ubsan && Ref != SanRtRef::Asan &&
          Ref != SanRtRef::Unknown)
        continue;
      if (Ref == SanRtRef::Unknown) {
        llvm::errs()
          << "bake: sanitizer runtime '" << Component
          << "' is not vendored; supported sanitizers are 'address' and "
             "'undefined'.\n"
          << "  (-fsanitize=" << Component
          << " still instruments compile-only invocations; the link step "
             "is rejected.)\n";
        FailingCommands.push_back({1, Cmd});
        if (!Res) Res = 1;
        return;
      }
      std::string &Rt = Ref == SanRtRef::Ubsan ? san_ubsan_a : san_asan_a;
      if (Rt.empty()) {
        Rt = bake::ensure_sanitizer_objects(
                 tc, Ref == SanRtRef::Ubsan ? bake::SanitizerKind::Ubsan
                                            : bake::SanitizerKind::Asan)
                 .string();
        if (Rt.empty()) {
          FailingCommands.push_back({1, Cmd});
          if (!Res) Res = 1;
          return;
        }
      }
    }

    // One call — prepares ALL runtime artifacts for this target. The C++
    // runtime is also forced in for sanitize links: the sanitizer runtime
    // itself uses the C++ ABI (dynamic_cast/typeinfo) and unwinder.
    bool needs_cxx_runtime =
        IsCxx || !san_ubsan_a.empty() || !san_asan_a.empty();
    bake::RuntimeArtifacts rt =
        bake::prepare_runtime(tc, needs_cxx_runtime, link_mode);

    bool is_shared_lib = false;
    bool has_no_pie = false;
    for (auto& A : user_args) {
      if (A == "-shared" || A == "-Bshareable") is_shared_lib = true;
      if (A == "-no-pie") has_no_pie = true;
    }

    std::vector<std::string> Prefix;  // before user args
    std::vector<std::string> Suffix;  // after user args
    std::vector<const char *> LldArgs;
    LldArgs.push_back(LinkerName);

    // ── Prefix: crt entry + link dirs ──

    // CRT entry (musl: crt1.o; mingw: crt2.o; darwin: none)
    if (!rt.crt_entry.string().empty())
      Prefix.push_back(rt.crt_entry.string());

    if (is_darwin) {
      // Same-source deployment version: bake always owns
      // -platform_version (min from the target spec — explicit suffix,
      // built-in default, or detected host minimum; SDK version from the
      // vendored SDKSettings). Any driver-produced one is stripped in
      // the user-args loop below, keeping compile and link identical.
      Prefix.push_back("-platform_version");
      Prefix.push_back("macos");
      Prefix.push_back(rt.macos_deployment_target);
      Prefix.push_back(rt.macos_sdk_version);
      for (auto& dir : rt.link_dirs)
        Prefix.push_back("-L" + dir);
      for (auto& dir : rt.framework_dirs)
        Prefix.push_back("-F" + dir);
      Prefix.push_back("-lSystem");
      // Sanitized links load the runtime dylibs through their @rpath
      // install names from the bake cache — the layout the official
      // Clang uses for its bundled darwin runtimes.
      if (!san_ubsan_a.empty()) {
        Prefix.push_back("-rpath");
        Prefix.push_back(std::filesystem::path(san_ubsan_a)
                             .parent_path().string());
      }
      if (!san_asan_a.empty()) {
        Prefix.push_back("-rpath");
        Prefix.push_back(std::filesystem::path(san_asan_a)
                             .parent_path().string());
      }
    }


    if (is_gnu) {
      for (auto& dir : rt.link_dirs)
        Prefix.push_back("-L" + dir);
      if (!is_shared_lib) {
        // Executable: interpreter path. Scrt1.o serves both PIE and
        // non-PIE; bake does not force PIE (user objects may be non-PIC).
        if (!rt.dynamic_linker.empty()) {
          Prefix.push_back("-dynamic-linker");
          Prefix.push_back(rt.dynamic_linker);
        }
      }
    }
    if (is_mingw) {
      // Import library search paths for the MinGW driver.
      for (auto& dir : rt.link_dirs)
        Prefix.push_back("-L" + dir);
    }

    for (auto &S : Prefix)
      LldArgs.push_back(S.c_str());

    // ── User args ──
    // Filter target-specific libraries that bake replaces with its own
    // runtime injection:
    //   - darwin C++: skip -lc++ (use our static libc++.a instead)
    //   - mingw: skip GCC libs (-lgcc, -lgcc_eh, -lmoldname, -lmingwex,
    //     -lmingw32) since we inject libmingw32.a directly. Also skip
    //     -lmsvcrt since we inject api-ms-win-crt-* import libs.
    //
    // For mingw: generate import libraries on-demand for -l<name> flags.
    // Only libraries that are referenced are generated.
    if (is_mingw) {
      for (const char *Arg : Cmd->getArguments()) {
        if (!Arg) continue;
        StringRef A(Arg);
        if (A.starts_with("-l")) {
          std::string name(A.substr(2));
          if (!name.empty())
            bake::ensure_mingw_import_lib(tc, name);
        }
      }
    }

    // Known glibc library names whose -l references are replaced by the
    // synthesized stub set (stub files are lib<name>.so.<sover> — plain
    // -l<name> could not resolve them anyway).
    auto is_glibc_lib_name = [](StringRef n) {
      return n == "c" || n == "m" || n == "pthread" || n == "dl" ||
             n == "rt" || n == "resolv" || n == "util" || n == "ld" ||
             n == "anl" || n == "nsl" || n == "crypt" || n == "nss_db" ||
             n == "nss_dns" || n == "nss_files";
    };

    // darwin: the Clang driver points -rpath at its resource dir
    // (…/lib/darwin) for its bundled sanitizer dylibs; bake's dylibs
    // live in the cache dir (rpath injected in the Prefix). Strip the
    // stale pairs up front so the rewriting loop below stays simple.
    llvm::SmallVector<const char *> LinkArgs;
    if (is_darwin) {
      auto Src = Cmd->getArguments();
      for (size_t i = 0; i < Src.size(); ++i) {
        if (Src[i] && StringRef(Src[i]) == "-rpath" && i + 1 < Src.size() &&
            Src[i + 1] &&
            StringRef(Src[i + 1]).ends_with("/lib/darwin")) {
          ++i;
          continue;
        }
        LinkArgs.push_back(Src[i]);
      }
    } else {
      for (const char *A : Cmd->getArguments())
        LinkArgs.push_back(A);
    }

    bool skip_next_dynamic_linker = false;
    int skip_platform_values = 0;  // remaining -platform_version values
    bool bake_owns_platform_version = is_darwin;
    for (const char *Arg : LinkArgs) {
      if (is_darwin && IsCxx && Arg && StringRef(Arg) == "-lc++")
        continue;
      if (skip_platform_values > 0) {
        --skip_platform_values;
        continue;
      }
      if (bake_owns_platform_version && Arg &&
          StringRef(Arg) == "-platform_version") {
        skip_platform_values = 3;
        continue;
      }
      if (skip_next_dynamic_linker) {
        skip_next_dynamic_linker = false;
        continue;
      }
      if (Arg && StringRef(Arg) == "-dynamic-linker") {
        skip_next_dynamic_linker = true;
        continue;
      }
      // Sanitizer runtime references: swap the driver-emitted path for
      // the artifact bake built. (The .c_str() pointers stay valid:
      // san_*_a and the compiler_rt copy outlive the bake_lld_link
      // call.) ELF asan keeps the driver's --whole-archive markers
      // around it — __asan_preinit and the interceptors must survive,
      // and the archive lands before the libc archives, so its malloc
      // definition wins. darwin swaps the dylib reference (loaded via
      // the injected rpath); mingw links the import library.
      if (Arg) {
        StringRef Component;
        static std::string compiler_rt_path;
        switch (parseSanRtRef(StringRef(Arg), Component)) {
        case SanRtRef::Ubsan:
          LldArgs.push_back(san_ubsan_a.c_str());
          continue;
        case SanRtRef::Asan:
          LldArgs.push_back(san_asan_a.c_str());
          continue;
        case SanRtRef::Helper:
          // Covered by the runtime bake links — drop the reference.
          continue;
        case SanRtRef::Builtins:
          if (!rt.compiler_rt.string().empty()) {
            compiler_rt_path = rt.compiler_rt.string();
            LldArgs.push_back(compiler_rt_path.c_str());
            continue;
          }
          break;
        case SanRtRef::Unknown:
        case SanRtRef::None:
          break;
        }
      }
      if (is_mingw && Arg) {
        StringRef A(Arg);
        // GCC-specific libs not needed with Clang/LLD.
        if (A == "-lgcc" || A == "-lgcc_eh" || A == "-lgcc_s" ||
            A == "-lmoldname" || A == "-lmingwex" || A == "-lmingw32" ||
            A == "-lmsvcrt" || A == "-lstdc++" ||
            A == "-ladvapi32" || A == "-lkernel32" || A == "-luser32" ||
            A == "-lshell32" || A == "-lntdll")
          continue;
        // GCC CRT startup files — not needed with Clang/LLD (uses crt2.o).
        if (A == "crtbegin.o" || A == "crtend.o" ||
            A == "crtbeginS.o" || A == "crtendS.o")
          continue;
      }
      if (is_gnu && Arg) {
        StringRef A(Arg);
        // GCC unwinder/builtins: replaced by compiler-rt + libunwind.
        if (A == "-lgcc" || A == "-lgcc_s" || A == "-lgcc_eh" ||
            A == "-lstdc++")
          continue;
        // glibc libs: replaced by the synthesized stub set.
        if (A.starts_with("-l") && is_glibc_lib_name(A.substr(2)))
          continue;
        // CRT startup files — bake provides Scrt1.o + libc_nonshared.a.
        if (A == "crt1.o" || A == "Scrt1.o" || A == "crti.o" ||
            A == "crtn.o" || A == "crtbegin.o" || A == "crtend.o" ||
            A == "crtbeginS.o" || A == "crtendS.o" || A == "crtbeginT.o")
          continue;
      }
      LldArgs.push_back(Arg);
    }

    // ── Suffix: runtime archives (target-agnostic) ──
    // gnu: stubs first, under --as-needed so only used libs become
    // DT_NEEDED entries (a hello-world ends up with just libc.so.6).
    if (is_gnu) {
      Suffix.push_back("--as-needed");
      for (auto& stub : rt.gnu_stub_libs)
        Suffix.push_back(stub);
    }
    if (!rt.libcxxabi.string().empty())
      Suffix.push_back(rt.libcxxabi.string());
    if (!rt.libcxx.string().empty())
      Suffix.push_back(rt.libcxx.string());
    if (!rt.libunwind.string().empty())
      Suffix.push_back(rt.libunwind.string());
    if (!rt.libc.string().empty())
      Suffix.push_back(rt.libc.string());
    if (!rt.compiler_rt.string().empty())
      Suffix.push_back(rt.compiler_rt.string());

    // MinGW: always-link system libraries as full paths.
    if (is_mingw && !rt.mingw_import_dir.empty()) {
      for (auto& lib : rt.always_link_libs)
        Suffix.push_back(rt.mingw_import_dir + "/" + lib + ".lib");
    }
    for (auto &S : Suffix)
      LldArgs.push_back(S.c_str());

    int LinkRes = bake_lld_link(Flavor,
                                static_cast<int>(LldArgs.size()),
                                LldArgs.data());
    if (LinkRes != 0) {
      FailingCommands.push_back({LinkRes, Cmd});
      if (!Res)
        Res = LinkRes;
    } else if (is_mingw && !san_asan_a.empty()) {
      copyMingwAsanDll(Cmd, san_asan_a);
    }
    return;
  }

  // Non-link jobs: execute normally.
  // For -cc1 jobs, CC1Command::Execute() dispatches through the Driver::CC1Main
  // callback, keeping compilation fully in-process.
  std::string ErrMsg;
  bool ExecutionFailed = false;
  int Result = Cmd->Execute({}, &ErrMsg, &ExecutionFailed);

  if (ExecutionFailed) {
    if (!ErrMsg.empty())
      llvm::errs() << "bake: " << ErrMsg << "\n";
    Result = 1;
  }

  if (Result != 0) {
    FailingCommands.push_back({Result, Cmd});
    if (!Res)
      Res = Result;
  }
}

//===----------------------------------------------------------------------===//
// Argument preprocessing helpers (used by clang_main)
//===----------------------------------------------------------------------===//


/// For musl link jobs: strip default system library references that don't
/// exist in a static musl environment, then add -nostdlib and -static so
/// Clang's driver stops injecting default crt/libc/libgcc paths.
static void filter_musl_link_flags(SmallVectorImpl<const char *> &Args,
                                   llvm::StringSaver &Saver) {
  // Check if this is a compile-only invocation (no linking).
  for (const char *A : Args) {
    if (A && (StringRef(A) == "-c" || StringRef(A) == "-S" ||
              StringRef(A) == "--precompile" ||
              StringRef(A) == "-fsyntax-only"))
      return;
  }

  // Remove system libraries that musl doesn't provide.
  Args.erase(std::remove_if(Args.begin(), Args.end(),
      [](const char *A) {
        if (!A) return false;
        StringRef s(A);
        return s == "-lrt" || s == "-ldl" || s == "-lm" ||
               s == "-lpthread" || s == "-lgcc_s" || s == "-lgcc" ||
               s == "-latomic";
      }), Args.end());

  Args.push_back(Saver.save("-nostdlib").data());

  // Add -static for executables (shared libs use default linking).
  bool is_shared = false;
  for (const char *A : Args) {
    if (A && (StringRef(A) == "-shared" || StringRef(A) == "-Bshareable")) {
      is_shared = true;
      break;
    }
  }
  if (!is_shared)
    Args.push_back(Saver.save("-static").data());
}

/// Inject vendored headers via -nostdinc + -isystem.
///
/// bake disables ALL default include search paths, then re-adds its own
/// vendored headers as -isystem (system group). User -I paths remain in
/// the user group, which Clang always searches first. This separation
/// lets library-specific headers (e.g. libunwind's unwind.h) take
/// priority over Clang's builtins when needed.
static void inject_vendored_headers(
    SmallVectorImpl<const char *> &Args, llvm::StringSaver &Saver,
    StringRef target_triple, bool IsCxx) {
  const std::string lib = bake::find_lib_dir().string();

  bool is_musl = target_triple.contains("linux") &&
                 target_triple.contains("musl");

  bool is_mingw = target_triple.contains("windows") &&
                  target_triple.contains("gnu");

  bake::TargetSpec host = bake::detect_host_target();
  bool is_darwin = !target_triple.empty()
      ? (target_triple.contains("darwin") ||
         target_triple.contains("apple") ||
         target_triple.contains("macos"))
      : host.is_darwin();

  if (is_musl) {
    Args.push_back(Saver.save("-nostdinc").data());
    if (IsCxx)
      Args.push_back(Saver.save("-nostdinc++").data());

    StringRef arch = target_triple.split('-').first;
    std::string arch_os_dir = (arch == "x86_64") ? "x86" : arch.str();

    auto add = [&](const std::string &path) {
      Args.push_back(Saver.save("-isystem").data());
      Args.push_back(Saver.save(path.c_str()).data());
    };

    if (IsCxx && !lib.empty()) {
      add(lib + "/libcxx/cross-config");
      add(lib + "/libcxx/include");
      add(lib + "/libcxxabi/include");
    }
    if (!lib.empty()) {
      add(lib + "/include");
      add(lib + "/libc/include/" + target_triple.str());
      add(lib + "/libc/include/generic-musl");
      add(lib + "/libc/include/" + arch_os_dir + "-linux-any");
      add(lib + "/libc/include/any-linux-any");
    }

  }
  // linux-gnu (glibc). Vendored layout: -nostdinc + layered chain
  // (per-triple bits → generic-glibc → kernel UAPI shared with musl).
  // SystemGnu (native glibc host, same arch): system /usr/include stays;
  // libc++ headers are always vendored regardless of layout.
  {
    bake::TargetSpec gnu_ts = !target_triple.empty()
        ? bake::parse_target(target_triple)
        : bake::detect_host_target();

    if (gnu_ts.is_linux_gnu()) {
      auto layout = bake::resolve_gnu_sdk(gnu_ts);

      if (IsCxx)
        Args.push_back(Saver.save("-nostdinc++").data());
      if (layout == bake::GnuSdkLayout::Vendored)
        Args.push_back(Saver.save("-nostdinc").data());

      auto add = [&](const std::string &path) {
        Args.push_back(Saver.save("-isystem").data());
        Args.push_back(Saver.save(path.c_str()).data());
      };

      // C++ headers (always vendored).
      if (IsCxx && !lib.empty()) {
        add(lib + "/libcxx/gnu-config");
        add(lib + "/libcxx/include");
        add(lib + "/libcxxabi/include");
      }

      if (!lib.empty()) {
        add(lib + "/include");
        if (layout == bake::GnuSdkLayout::Vendored) {
          StringRef arch = gnu_ts.triple_.c_str();
          arch = arch.split('-').first;
          std::string arch_os_dir = (arch == "x86_64") ? "x86" : arch.str();
          add(lib + "/libc/include/" + gnu_ts.triple_);
          add(lib + "/libc/include/generic-glibc");
          add(lib + "/libc/include/" + arch_os_dir + "-linux-any");
          add(lib + "/libc/include/any-linux-any");

          // Pin the header-reported glibc version to the TARGET version.
          // Headers are vendored from the newest release with features.h
          // patched to honor -D__GLIBC__/__GLIBC_MINOR__, so every
          // __GLIBC_PREREQ gate presents the requested surface. The
          // triple seen here is version-stripped; recover the explicit
          // target recorded during preprocessing. bake's own
          // glibc-internal compiles are exempt: they must see the
          // vendored source version, not the target's.
          bool internal = false;
          for (const char *a : Args)
            if (StringRef(a).contains("MODULE_NAME=libc"))
              internal = true;
          if (!internal) {
            bake::TargetSpec eff = gnu_ts;
            if (!g_explicit_target.triple_.empty() &&
                g_explicit_target.triple_ == gnu_ts.triple_)
              eff = g_explicit_target;
            Args.push_back(Saver.save(
                ("-D__GLIBC__=" + std::to_string(eff.glibc_major()))
                    .c_str()).data());
            Args.push_back(Saver.save(
                ("-D__GLIBC_MINOR__=" + std::to_string(eff.glibc_minor()))
                    .c_str()).data());
          }
        }
      }
    }
  }

  if (is_mingw) {
    Args.push_back(Saver.save("-nostdinc").data());
    if (IsCxx)
      Args.push_back(Saver.save("-nostdinc++").data());

    auto add = [&](const std::string &path) {
      Args.push_back(Saver.save("-isystem").data());
      Args.push_back(Saver.save(path.c_str()).data());
    };

    // C++ headers (vendored libc++ with MinGW config).
    if (IsCxx && !lib.empty()) {
      add(lib + "/libcxx/mingw-config");
      add(lib + "/libcxx/include");
      add(lib + "/libcxxabi/include");
    }
    // C runtime + Win32 API headers (MinGW-w64 public headers).
    // _mingw.h defaults to UCRT (__MSVCRT_VERSION__=0xE00 + _UCRT) when
    // __MSVCRT_VERSION__ is not explicitly defined. CRT sources override
    // with -D__MSVCRT_VERSION__=0x700 to stay in MSVCRT compat mode.
    if (!lib.empty()) {
      add(lib + "/include");
      add(lib + "/libc/include/any-windows-any");
    }
  }

  if (is_darwin) {
    // Determine SDK layout from the ACTUAL target: native (no -target)
    // → system SDK; explicit -target darwin → bake's layout resolution
    // for that target (cross-arch → vendored).
    bake::TargetSpec darwin_target = g_explicit_target.triple_.empty()
        ? bake::detect_host_target()
        : g_explicit_target;
    auto sdk_layout = bake::resolve_darwin_sdk(darwin_target);

    if (sdk_layout == bake::DarwinSdkLayout::Vendored) {
      // Cross-compile: block all system headers, use vendored only.
      Args.push_back(Saver.save("-nostdinc").data());
    }
    if (IsCxx)
      Args.push_back(Saver.save("-nostdinc++").data());

    auto add = [&](const std::string &path) {
      Args.push_back(Saver.save("-isystem").data());
      Args.push_back(Saver.save(path.c_str()).data());
    };

    // C++ headers: always vendored (never from system SDK).
    if (IsCxx && !lib.empty()) {
      add(lib + "/libcxx/include");
      add(lib + "/libcxxabi/include");
    }

    if (sdk_layout == bake::DarwinSdkLayout::Vendored) {
      // Cross-compile: C headers from vendored sources.
      if (!lib.empty()) {
        add(lib + "/include");
        add(lib + "/libc/darwin/include");
      }
    } else {
      // Native: Clang resource dir + system SDK C headers + frameworks.
      if (!lib.empty())
        add(lib + "/include");
      auto &sdk = getMacosSdkPath();
      if (!sdk.empty()) {
        add(sdk + "/usr/include");
        Args.push_back(Saver.save("-iframework").data());
        Args.push_back(Saver.save(
            (sdk + "/System/Library/Frameworks").c_str()).data());
      }
    }

    // Deployment minimum — the target query is authoritative: explicit
    // -target uses the version suffix or the built-in default; native
    // (no -target) uses the detected host minimum. Either way it is
    // injected (overriding any user-passed -mmacosx-version-min,
    // silencing clang's "overriding option" warning), keeping compile,
    // link and cache keys on one deterministic value.
    Args.push_back(Saver.save("-Wno-overriding-option").data());
    Args.push_back(Saver.save(
        ("-mmacosx-version-min=" + darwin_target.macos_deployment_min())
            .c_str()).data());
  }
}

//===----------------------------------------------------------------------===//
// clang_main — driver entry point
//===----------------------------------------------------------------------===//

static int clang_main(int Argc, const char **Argv,
                      const llvm::ToolContext &ToolContext) {
  noteBottomOfStack();
  llvm::setBugReportMsg("PLEASE submit a bug report to " BUG_REPORT_URL
                        " and include the crash backtrace, preprocessed "
                        "source, and associated run script.\n");
  size_t argv_offset =
      (strcmp(Argv[1], "-cc1") == 0 || strcmp(Argv[1], "-cc1as") == 0) ? 0 : 1;
  SmallVector<const char *, 256> Args(Argv + argv_offset, Argv + Argc);

  if (llvm::sys::Process::FixupStandardFileDescriptors())
    return 1;

  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver(A);

  // Determine C vs C++ mode from the invocation: "bake cc" → C,
  // "bake c++" → C++.  This is the single source of truth for IsCxx —
  // never re-scan args later.
  bool IsCxx = (Args.size() > 0 && StringRef(Args[0]) == "c++");

  // If C++, set the Clang driver mode to g++ so it links C++ runtime
  // libraries (-lc++) automatically.
  if (IsCxx) {
    Args.insert(Args.begin() + 1, Saver.save("--driver-mode=g++").data());
  }

  const char *ProgName =
      ToolContext.NeedsPrependArg ? ToolContext.PrependArg : ToolContext.Path;

  bool ClangCLMode =
      IsClangCL(getDriverMode(ProgName, llvm::ArrayRef(Args).slice(1)));

  if (llvm::Error Err = expandResponseFiles(Args, ClangCLMode, A)) {
    llvm::errs() << toString(std::move(Err)) << '\n';
    return 1;
  }

  // Handle -cc1 integrated tools.
  if (Args.size() >= 2 && StringRef(Args[1]).starts_with("-cc1"))
    return ExecuteCC1Tool(Args, ToolContext);

  // Handle options that need handling before the real command line parsing in
  // Driver::BuildCompilation()
  bool CanonicalPrefixes = true;
  for (int i = 1, size = Args.size(); i < size; ++i) {
    // Skip end-of-line response file markers
    if (Args[i] == nullptr)
      continue;
    if (StringRef(Args[i]) == "-canonical-prefixes")
      CanonicalPrefixes = true;
    else if (StringRef(Args[i]) == "-no-canonical-prefixes")
      CanonicalPrefixes = false;
  }

  // Handle CL and _CL_ which permits additional command line options to be
  // prepended or appended.
  if (ClangCLMode) {
    // Arguments in "CL" are prepended.
    std::optional<std::string> OptCL = llvm::sys::Process::GetEnv("CL");
    if (OptCL) {
      SmallVector<const char *, 8> PrependedOpts;
      getCLEnvVarOptions(*OptCL, Saver, PrependedOpts);

      // Insert right after the program name to prepend to the argument list.
      Args.insert(Args.begin() + 1, PrependedOpts.begin(), PrependedOpts.end());
    }
    // Arguments in "_CL_" are appended.
    std::optional<std::string> Opt_CL_ = llvm::sys::Process::GetEnv("_CL_");
    if (Opt_CL_) {
      SmallVector<const char *, 8> AppendedOpts;
      getCLEnvVarOptions(*Opt_CL_, Saver, AppendedOpts);

      // Insert at the end of the argument list to append.
      Args.append(AppendedOpts.begin(), AppendedOpts.end());
    }
  }

  llvm::StringSet<> SavedStrings;
  // Handle CCC_OVERRIDE_OPTIONS, used for editing a command line behind the
  // scenes.
  if (const char *OverrideStr = ::getenv("CCC_OVERRIDE_OPTIONS")) {
    // FIXME: Driver shouldn't take extra initial argument.
    driver::applyOverrideOptions(Args, OverrideStr, SavedStrings,
                                 "CCC_OVERRIDE_OPTIONS", &llvm::errs());
  }

  std::string Path = GetExecutablePath(ToolContext.Path, CanonicalPrefixes);

  // bake always uses in-process cc1 — we never want to spawn a subprocess
  // for compilation. This overrides whatever CLANG_SPAWN_CC1 the linked
  // Clang library was built with.
  bool UseNewCC1Process = false;

  std::unique_ptr<DiagnosticOptions> DiagOpts = CreateAndPopulateDiagOpts(Args);
  // Driver's diagnostics don't use suppression mappings, so don't bother
  // parsing them. CC1 still receives full args, so this doesn't impact other
  // actions.
  DiagOpts->DiagnosticSuppressionMappingsFile.clear();

  TextDiagnosticPrinter *DiagClient =
      new TextDiagnosticPrinter(llvm::errs(), *DiagOpts);
  FixupDiagPrefixExeName(DiagClient, ProgName);

  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());

  DiagnosticsEngine Diags(DiagID, *DiagOpts, DiagClient);

  if (!DiagOpts->DiagnosticSerializationFile.empty()) {
    auto SerializedConsumer =
        clang::serialized_diags::create(DiagOpts->DiagnosticSerializationFile,
                                        *DiagOpts, /*MergeChildRecords=*/true);
    Diags.setClient(new ChainedDiagnosticConsumer(
        Diags.takeClient(), std::move(SerializedConsumer)));
  }

  auto VFS = llvm::vfs::getRealFileSystem();
  ProcessWarningOptions(Diags, *DiagOpts, *VFS, /*ReportDiags=*/false);

  // Capture the driver's default triple (from LLVM build config) before
  // the Driver potentially overrides it via -target args.
  std::string default_triple = llvm::sys::getDefaultTargetTriple();
  Driver TheDriver(Path, default_triple, Diags,
                   /*Title=*/"clang LLVM compiler", VFS);
  // Point the resource dir at bake's vendored tree: lib/include IS the
  // standard resource include layout (clang builtin headers), and runtime
  // archives (libclang_rt.*) live under lib/lib/<os>/. The exe-derived
  // default would be a non-existent path.
  if (std::string bake_lib = bake::find_lib_dir().string(); !bake_lib.empty())
    TheDriver.ResourceDir = bake_lib;
  auto TargetAndMode = ToolChain::getTargetAndModeFromProgramName(ProgName);
  TheDriver.setTargetAndMode(TargetAndMode);
  // If -canonical-prefixes is set, GetExecutablePath will have resolved Path
  // to the llvm driver binary, not clang. In this case, we need to use
  // PrependArg which should be clang-*. Checking just CanonicalPrefixes is
  // safe even in the normal case because PrependArg will be null so
  // setPrependArg will be a no-op.
  if (ToolContext.NeedsPrependArg || CanonicalPrefixes)
    TheDriver.setPrependArg(ToolContext.PrependArg);

  insertTargetAndModeArgs(TargetAndMode, Args, SavedStrings);

  if (!SetBackdoorDriverOutputsFromEnvVars(TheDriver))
    return 1;

  auto ExecuteCC1WithContext =
      [&ToolContext](SmallVectorImpl<const char *> &ArgV) {
        return ExecuteCC1Tool(ArgV, ToolContext);
      };
  if (!UseNewCC1Process) {
    TheDriver.CC1Main = ExecuteCC1WithContext;
    // Ensure the CC1Command actually catches cc1 crashes
    llvm::CrashRecoveryContext::Enable();
  }

  // ── Argument preprocessing ──

  // Detect target triple from -target flag, falling back to the driver's
  // default triple (baked into LLVM at build time) when not specified.
  // The raw spec (with bake's glibc version suffix, e.g.
  // "x86_64-linux-gnu.2.31") is kept for the link interception — LLVM's
  // normalized Triple cannot carry the version.
  std::string target_triple;
  bool has_explicit_target = false;
  for (size_t i = 0; i + 1 < Args.size(); ++i) {
    if (StringRef(Args[i]) == "-target") {
      StringRef Spec(Args[i + 1]);
      g_explicit_target = bake::parse_target(Spec.str());
      has_explicit_target = true;
      // Strip the version suffix before the driver parses the triple —
      // ".2.31" (glibc) and ".12" (darwin) are not valid LLVM environment
      // or OS components.
      StringRef Abi = Spec;
      auto Dash = Spec.rfind('-');
      if (Dash != StringRef::npos) Abi = Spec.substr(Dash + 1);
      if (Spec.contains("linux") && Abi.starts_with("gnu.")) {
        std::string Clean = (Spec.substr(0, Dash + 1) + "gnu").str();
        Args[i + 1] = Saver.save(Clean).data();
      } else {
        // darwin deployment-version suffix: os segment "macos.12" /
        // "darwin.14.1" (target query versions).
        auto OsDot = Abi.find('.');
        if (OsDot != StringRef::npos) {
          StringRef Os = Abi.substr(0, OsDot);
          StringRef Ver = Abi.substr(OsDot + 1);
          bool numeric = !Ver.empty() &&
              Ver.find_first_not_of(".0123456789") == StringRef::npos;
          if ((Os == "macos" || Os == "darwin") && numeric) {
            std::string Clean = (Spec.substr(0, Dash + 1) + Os).str();
            Args[i + 1] = Saver.save(Clean).data();
          }
        }
      }
      target_triple = Args[i + 1];
      break;
    }
  }
  if (target_triple.empty())
    target_triple = default_triple;


  // String matching on canonical triples is more reliable than llvm::Triple
  // for musl detection (the Triple parser mishandles 3-component triples).
  if (StringRef(target_triple).contains("musl"))
    filter_musl_link_flags(Args, Saver);

  // IsCxx was determined at line 650 from the invocation (cc vs c++).
  inject_vendored_headers(Args, Saver, target_triple, IsCxx);

  std::unique_ptr<Compilation> C(TheDriver.BuildCompilation(Args));

  Driver::ReproLevel ReproLevel = Driver::ReproLevel::OnCrash;
  if (Arg *A = C->getArgs().getLastArg(options::OPT_gen_reproducer_eq)) {
    auto Level =
        llvm::StringSwitch<std::optional<Driver::ReproLevel>>(A->getValue())
            .Case("off", Driver::ReproLevel::Off)
            .Case("crash", Driver::ReproLevel::OnCrash)
            .Case("error", Driver::ReproLevel::OnError)
            .Case("always", Driver::ReproLevel::Always)
            .Default(std::nullopt);
    if (!Level) {
      llvm::errs() << "Unknown value for " << A->getSpelling() << ": '"
                   << A->getValue() << "'\n";
      return 1;
    }
    ReproLevel = *Level;
  }
  if (!!::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"))
    ReproLevel = Driver::ReproLevel::Always;

  int Res = 1;
  bool IsCrash = false;
  Driver::CommandStatus CommandStatus = Driver::CommandStatus::Ok;
  // Pretend the first command failed if ReproStatus is Always.
  const Command *FailingCommand = nullptr;
  if (!C->getJobs().empty())
    FailingCommand = &*C->getJobs().begin();
  if (C && !C->containsError()) {
    SmallVector<std::pair<int, const Command *>, 4> FailingCommands;

    // Default to success — only set non-zero if a job actually fails.
    Res = 0;

    // ----------------------------------------------------------------
    // Manual job iteration with in-process LLD link interception.
    //
    // Instead of calling TheDriver.ExecuteCompilation() (which would
    // spawn subprocesses for link jobs), we iterate the compilation's
    // jobs ourselves:
    //   - Link jobs  → dispatched to in-process LLD (bake_lld_link).
    //   - Other jobs → executed normally via Command::Execute.
    //     For -cc1 jobs, the CC1Command override dispatches through
    //     the Driver::CC1Main callback set above, keeping compilation
    //     in-process too.
    // ----------------------------------------------------------------

    // Get the target triple for LLD flavor selection.
    const llvm::Triple &TargetTriple =
        C->getDefaultToolChain().getTriple();

    // Handle -### (dry-run): print commands without executing.
    if (C->getArgs().hasArg(options::OPT__HASH_HASH_HASH)) {
      C->getJobs().Print(llvm::errs(), "\n", true);
    } else {
      for (const auto &Cmd : C->getJobs())
        bakeExecuteJob(&Cmd, TargetTriple, IsCxx, Res, FailingCommands);
    }

    for (const auto &P : FailingCommands) {
      int CommandRes = P.first;
      FailingCommand = P.second;
      if (!Res)
        Res = CommandRes;

      // If result status is < 0, then the driver command signalled an error.
      // If result status is 70, then the driver command reported a fatal error.
      // On Windows, abort will return an exit code of 3.  In these cases,
      // generate additional diagnostic information if possible.
      IsCrash = CommandRes < 0 || CommandRes == 70;
#ifdef _WIN32
      IsCrash |= CommandRes == 3;
#endif
#if LLVM_ON_UNIX
      // When running in integrated-cc1 mode, the CrashRecoveryContext returns
      // the same codes as if the program crashed. See section "Exit Status for
      // Commands":
      // https://pubs.opengroup.org/onlinepubs/9699919799/xrat/V4_xcu_chap02.html
      IsCrash |= CommandRes > 128;
#endif
      CommandStatus =
          IsCrash ? Driver::CommandStatus::Crash : Driver::CommandStatus::Error;
      if (IsCrash)
        break;
    }
  }

  // Print the bug report message that would be printed if we did actually
  // crash, but only if we're crashing due to FORCE_CLANG_DIAGNOSTICS_CRASH.
  if (::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"))
    llvm::dbgs() << llvm::getBugReportMsg();
  if (FailingCommand != nullptr &&
    TheDriver.maybeGenerateCompilationDiagnostics(CommandStatus, ReproLevel,
                                                  *C, *FailingCommand))
    Res = 1;

  Diags.getClient()->finish();

  if (!UseNewCC1Process && IsCrash) {
    // When crashing in -fintegrated-cc1 mode, bury the timer pointers, because
    // the internal linked list might point to already released stack frames.
    llvm::BuryPointer(llvm::TimerGroup::acquireTimerGlobals());
  } else {
    // If any timers were active but haven't been destroyed yet, print their
    // results now.  This happens in -disable-free mode.
    llvm::TimerGroup::printAll(llvm::errs());
    llvm::TimerGroup::clearAll();
  }

#ifdef _WIN32
  // Exit status should not be negative on Win32, unless abnormal termination.
  // Once abnormal termination was caught, negative status should not be
  // propagated.
  if (Res < 0)
    Res = 1;
#endif

  // If we have multiple failing commands, we return the result of the first
  // failing command.
  return Res;
}

int bake_clang_main(int argc, const char **argv) {
  return clang_main(argc, argv, {argv[0], nullptr, false});
}
