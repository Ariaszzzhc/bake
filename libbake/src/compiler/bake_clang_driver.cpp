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

#include "bake_llvm.h"

#include <lld/Common/Driver.h>

#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

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

// cc1as_main (integrated assembler) is not linked from the shared Clang
// library.  Provide a stub that reports the limitation so the symbol
// resolves at link time without dragging in a full cc1as implementation.
extern "C" int cc1as_main(ArrayRef<const char *> Argv, const char *Argv0,
                          void *MainAddr);
int cc1as_main(ArrayRef<const char *> Argv, const char *Argv0,
               void *MainAddr) {
  llvm::errs() << "bake: integrated assembler (-cc1as) is not supported.\n";
  return 1;
}

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
  // Try xcrun first (works with both Xcode and CommandLineTools).
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
  // Fallback: check known paths.
  if (sdk_path.empty()) {
    const char *candidates[] = {
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
      nullptr
    };
    for (auto *p = candidates; *p; ++p) {
      if (llvm::sys::fs::exists(*p)) {
        sdk_path = *p;
        break;
      }
    }
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
                           const char *&LinkerName, BakeLldFlavor &Flavor) {
  switch (Triple.getObjectFormat()) {
  case llvm::Triple::MachO:
    LinkerName = "ld64.lld";
    Flavor = BAKE_LLD_MACHO;
    break;
  case llvm::Triple::COFF:
    LinkerName = "lld-link";
    Flavor = BAKE_LLD_COFF;
    break;
  case llvm::Triple::Wasm:
    LinkerName = "wasm-ld";
    Flavor = BAKE_LLD_WASM;
    break;
  default:
    LinkerName = "ld.lld";
    Flavor = BAKE_LLD_ELF;
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
                           int &Res,
                           SmallVectorImpl<std::pair<int, const Command *>> &FailingCommands) {
  StringRef ExeName = llvm::sys::path::filename(Cmd->getExecutable());

  if (bakeIsLinkCommand(ExeName)) {
    // Intercept: dispatch to in-process LLD instead of spawning ld.
    const char *LinkerName = nullptr;
    BakeLldFlavor Flavor = BAKE_LLD_ELF;
    bakeGetLLDInfo(Triple, LinkerName, Flavor);

    std::vector<const char *> LldArgs;
    std::vector<std::string> OwnedStrings; // keep alive for pointer stability
    LldArgs.push_back(LinkerName); // argv[0] for LLD
    for (const char *Arg : Cmd->getArguments())
      LldArgs.push_back(Arg);

    // macOS library/framework search paths.
    // Follows Zig's model: when SDK is detected, use SDK's libSystem.tbd
    // (always up-to-date with the system). When no SDK, fall back to
    // vendored libSystem.tbd. libc++ is NEVER linked dynamically regardless.
    if (Flavor == BAKE_LLD_MACHO) {
      auto &sdk = getMacosSdkPath();
      if (!sdk.empty()) {
        // SDK mode: libSystem.tbd + frameworks from SDK.
        OwnedStrings.push_back("-L" + sdk + "/usr/lib");
        OwnedStrings.push_back("-F" + sdk + "/System/Library/Frameworks");
      } else {
        // Vendored mode: use our own libSystem.tbd.
#ifdef BAKE_DARWIN_LIB
        OwnedStrings.push_back(std::string("-L") + BAKE_DARWIN_LIB);
#endif
      }
      for (auto &S : OwnedStrings)
        LldArgs.push_back(S.c_str());
    }

    int LinkRes = bake_lld_link(static_cast<int>(Flavor),
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

  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver(A);

  // If invoked as "bake c++", set the Clang driver mode to g++ so it
  // links C++ runtime libraries (-lc++) automatically.  Without this,
  // the driver defaults to GCC mode (binary is "bake", not "clang++").
  if (Args.size() > 0 && StringRef(Args[0]) == "c++") {
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

  Driver TheDriver(Path, llvm::sys::getDefaultTargetTriple(), Diags,
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

  // Use vendored headers instead of system SDK (macOS).
  // -nostdinc disables ALL default include search; we re-add everything via -isystem.
  // Search order is critical: libc++ headers must come first so their wrapper
  // versions of stddef.h/stdlib.h are found before Clang builtins and C headers.
#ifdef __APPLE__
  Args.push_back(Saver.save("-nostdinc").data());
  bool IsCxx = false;
  for (const char *A : Args) {
    if (A && llvm::StringRef(A).contains("driver-mode=g++")) {
      IsCxx = true;
      break;
    }
  }
  if (IsCxx)
    Args.push_back(Saver.save("-nostdinc++").data());
  // 1. libc++ headers (C++ wrappers that must shadow C builtins)
#ifdef BAKE_LIBCXX_INC
  Args.push_back(Saver.save("-isystem").data());
  Args.push_back(Saver.save(BAKE_LIBCXX_INC).data());
#endif
  // 2. Clang builtin headers (stdarg.h, stddef.h — the real definitions)
#ifdef BAKE_RESOURCE_DIR
  Args.push_back(Saver.save("-isystem").data());
  Args.push_back(Saver.save(BAKE_RESOURCE_DIR "/include").data());
#endif
  // 3. Darwin C library headers (stdio.h, stdlib.h, etc.)
#ifdef BAKE_DARWIN_INC
  Args.push_back(Saver.save("-isystem").data());
  Args.push_back(Saver.save(BAKE_DARWIN_INC).data());
#endif
  // 4. macOS framework headers (CoreFoundation.h, AppKit.h, etc.)
  //    Detected via xcrun — framework headers only exist in the SDK.
  //    This does NOT affect libc++ or libSystem (those are vendored).
  {
    auto &sdk = getMacosSdkPath();
    if (!sdk.empty()) {
      Args.push_back(Saver.save("-iframework").data());
      Args.push_back(Saver.save((sdk + "/System/Library/Frameworks").c_str()).data());
    }
  }
#endif

  // Inject -resource-dir so Clang finds its builtin headers (stdarg.h,
  // stddef.h, etc.) from the vendored LLVM build tree, not a system Clang.
  // This replaces the old lib/clang symlink hack.
#ifdef BAKE_RESOURCE_DIR
  Args.push_back(Saver.save("-resource-dir").data());
  Args.push_back(Saver.save(BAKE_RESOURCE_DIR).data());
#endif

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
        bakeExecuteJob(&Cmd, TargetTriple, Res, FailingCommands);
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

extern "C" int bake_clang_main(int argc, const char **argv);
int bake_clang_main(int argc, const char **argv) {
  return clang_main(argc, argv, {argv[0], nullptr, false});
}
