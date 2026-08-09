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
    LinkerName = "lld-link";
    Flavor = LldFlavor::COFF;
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
    tc.target.native = false;
    tc.target.arch = Triple.getArchTypeName(Triple.getArch());
    if (Triple.isOSDarwin()) {
      tc.target.os = "macos";
      tc.target.abi = "darwin";
    } else if (Triple.getOS() == llvm::Triple::Linux) {
      tc.target.os = "linux";
      tc.target.abi = (Triple.getEnvironment() == llvm::Triple::Musl)
                           ? "musl" : "gnu";
    }

    bool is_darwin = Flavor == LldFlavor::MACHO;

    // Parse link mode from user args.
    std::vector<std::string> user_args;
    for (const char *Arg : Cmd->getArguments())
      if (Arg) user_args.push_back(Arg);
    bake::LinkMode link_mode = bake::parse_link_mode(user_args);

    // One call — prepares ALL runtime artifacts for this target.
    bake::RuntimeArtifacts rt = bake::prepare_runtime(tc, IsCxx, link_mode);

    std::vector<std::string> Prefix;  // before user args
    std::vector<std::string> Suffix;  // after user args
    std::vector<const char *> LldArgs;
    LldArgs.push_back(LinkerName);

    // ── Prefix: crt entry + darwin link dirs ──

    // CRT entry (musl: crt1.o / rcrt1.o / Scrt1.o; darwin: none)
    if (!rt.crt_entry.string().empty())
      Prefix.push_back(rt.crt_entry.string());

    if (is_darwin) {
      // Ensure -platform_version (Clang driver omits it on non-macOS hosts).
      bool has_pv = false;
      for (const char *A : Cmd->getArguments())
        if (A && StringRef(A) == "-platform_version") { has_pv = true; break; }
      if (!has_pv) {
        Prefix.push_back("-platform_version");
        Prefix.push_back("macos");
        Prefix.push_back(rt.macos_deployment_target);
        Prefix.push_back(rt.macos_deployment_target);
      }
      for (auto& dir : rt.link_dirs)
        Prefix.push_back("-L" + dir);
      for (auto& dir : rt.framework_dirs)
        Prefix.push_back("-F" + dir);
      Prefix.push_back("-lSystem");
    }

    for (auto &S : Prefix)
      LldArgs.push_back(S.c_str());

    // ── User args ──
    // For darwin C++ links: filter out Clang's auto-added -lc++ to avoid
    // conflict with our static archives.
    for (const char *Arg : Cmd->getArguments()) {
      if (is_darwin && IsCxx && Arg && StringRef(Arg) == "-lc++")
        continue;
      LldArgs.push_back(Arg);
    }

    // ── Suffix: runtime archives (target-agnostic) ──
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

    for (auto &S : Suffix)
      LldArgs.push_back(S.c_str());

    int LinkRes = bake_lld_link(Flavor,
                                static_cast<int>(LldArgs.size()),
                                LldArgs.data());
    if (LinkRes != 0) {
      FailingCommands.push_back({LinkRes, Cmd});
      if (!Res)
        Res = LinkRes;
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

/// Remove -arch and -isysroot flags — they are host-specific (Apple Clang)
/// and interfere with bake's target-driven model.
static void strip_host_flags(SmallVectorImpl<const char *> &Args) {
  for (size_t i = 0; i < Args.size();) {
    if (Args[i] && (StringRef(Args[i]) == "-arch" ||
                    StringRef(Args[i]) == "-isysroot")) {
      size_t remove_count = (i + 1 < Args.size()) ? 2 : 1;
      Args.erase(Args.begin() + i, Args.begin() + i + remove_count);
    } else if (Args[i] && StringRef(Args[i]).starts_with("-isysroot=")) {
      Args.erase(Args.begin() + i);
    } else {
      ++i;
    }
  }
}

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

  if (is_darwin) {
    // Determine SDK layout: native → use system SDK; cross → vendored.
    bake::TargetSpec darwin_target;
    darwin_target.native = false;
    darwin_target.os = "macos";
    darwin_target.abi = "darwin";
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

    // Deployment target from vendored SDKSettings.json.
    std::string ver = "15.0";
    bake::Path settings = bake::find_lib_dir() / "libc" / "darwin" / "SDKSettings.json";
    if (auto content = bake::read_file(settings)) {
      std::string key = "\"MinimalDisplayName\":\"";
      auto pos = content->find(key);
      if (pos != std::string::npos) {
        auto start = pos + key.size();
        auto end = content->find('"', start);
        if (end != std::string::npos)
          ver = content->substr(start, end - start);
      }
    }
    Args.push_back(Saver.save(
        ("-mmacosx-version-min=" + ver).c_str()).data());
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
  std::string target_triple;
  for (size_t i = 0; i + 1 < Args.size(); ++i) {
    if (StringRef(Args[i]) == "-target") {
      target_triple = Args[i + 1];
      break;
    }
  }
  if (target_triple.empty())
    target_triple = default_triple;

  strip_host_flags(Args);

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
