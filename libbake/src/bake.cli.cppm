module;

#include <toml.hpp>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdlib>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

export module bake.cli;

import std;
import bake.util;
import bake.project;
import bake.compiler;
import bake.engine;
import bake.package;

// ============================================================
// bake.cli — multicall dispatch, argument parsing, commands
// ============================================================

#ifndef BAKE_VERSION
#define BAKE_VERSION "0.1.0"
#endif

#ifndef BAKE_SRC_DIR
#define BAKE_SRC_DIR "."
#endif

#ifndef BAKE_LIB_DIR
#define BAKE_LIB_DIR "."
#endif

// Phase 5: LLVM compiler integration — C ABI
extern "C" {
    int bake_clang_main(int argc, const char** argv);
    int bake_has_llvm(void);
}

namespace bake::cli {

namespace {

Path running_executable() {
#ifdef __APPLE__
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(buffer.data(), ec);
    return ec ? Path(buffer.data()) : Path(canonical);
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                      static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) return {};
    return Path(std::filesystem::path(buffer.data(), buffer.data() + length));
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    buffer[static_cast<size_t>(length)] = '\0';
    return Path(buffer.data());
#else
    return {};
#endif
}

struct BuildScriptRuntime {
    Path wrapper;
    Path cabi_header;
    Path library_dir;
};

BuildScriptRuntime find_build_script_runtime() {
    const Path fallback_source(BAKE_SRC_DIR);
    BuildScriptRuntime runtime{
        fallback_source / "libbake" / "public" / "bake.build.cppm",
        fallback_source / "libbake" / "src" / "cabi" / "bake_cabi.h",
        Path(BAKE_LIB_DIR)};

    Path executable = running_executable();
    if (executable.string().empty()) return runtime;

    Path prefix = executable.parent().parent();
    Path share = prefix / "share" / "bake";
    Path installed_wrapper = share / "bake.build.cppm";
    Path installed_header = share / "bake_cabi.h";
    Path installed_lib_dir = prefix / "lib";

    bool has_library =
        (installed_lib_dir / "libbake.dylib").is_regular_file() ||
        (installed_lib_dir / "libbake.so").is_regular_file() ||
        (installed_lib_dir / "bake.lib").is_regular_file();
    if (installed_wrapper.is_regular_file() &&
        installed_header.is_regular_file() && has_library) {
        runtime.wrapper = installed_wrapper;
        runtime.cabi_header = installed_header;
        runtime.library_dir = installed_lib_dir;
    }
    return runtime;
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::string name, const std::string& value)
        : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()))
            previous_ = previous;
#ifdef _WIN32
        active_ = _putenv_s(name_.c_str(), value.c_str()) == 0;
#else
        active_ = ::setenv(name_.c_str(), value.c_str(), 1) == 0;
#endif
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() {
        if (!active_) return;
#ifdef _WIN32
        (void)_putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
#else
        if (previous_)
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        else
            (void)::unsetenv(name_.c_str());
#endif
    }

    bool active() const { return active_; }

private:
    std::string name_;
    std::optional<std::string> previous_;
    bool active_ = false;
};

class ScopedBuildContext {
public:
    explicit ScopedBuildContext(const Layout& layout)
        : source_root_("BAKE_INTERNAL_SOURCE_ROOT",
                       layout.root.absolute().string()),
          project_out_("BAKE_INTERNAL_PROJECT_OUT",
                       layout.out_dir.absolute().string()),
          package_name_("BAKE_INTERNAL_PACKAGE_NAME",
                        layout.dependency_layout ? layout.package_name : "") {}

    bool active() const {
        return source_root_.active() && project_out_.active() &&
               package_name_.active();
    }

private:
    ScopedEnvironmentVariable source_root_;
    ScopedEnvironmentVariable project_out_;
    ScopedEnvironmentVariable package_name_;
};

} // namespace

// ===== Argument parsing =====

export struct ParsedArgs {
    std::string command;                // subcommand (empty if none)
    std::vector<std::string> positional; // positional arguments
    std::vector<std::string> options;    // --flag or --key=value
    bool help = false;
    bool version = false;

    // Get an option value (--key=value, --key value, -k value, or -k=value)
    std::optional<std::string> get_option(std::string_view name) const {
        std::string long_prefix = std::string("--") + std::string(name) + "=";
        std::string short_prefix = std::string("-") + std::string(name) + "=";
        for (auto& opt : options) {
            if (opt == long_prefix.substr(0, long_prefix.size() - 1) ||
                opt == short_prefix.substr(0, short_prefix.size() - 1)) {
                return std::string("true");  // flag without value
            }
            if (starts_with(opt, long_prefix)) {
                return opt.substr(long_prefix.size());
            }
            if (starts_with(opt, short_prefix)) {
                return opt.substr(short_prefix.size());
            }
        }
        return std::nullopt;
    }

    bool has_option(std::string_view name) const {
        std::string long_flag = std::string("--") + std::string(name);
        std::string short_flag = std::string("-") + std::string(name);
        std::string long_prefix = long_flag + "=";
        std::string short_prefix = short_flag + "=";
        for (auto& opt : options) {
            if (opt == long_flag || opt == short_flag) return true;
            if (starts_with(opt, long_prefix) || starts_with(opt, short_prefix)) return true;
        }
        return false;
    }
};

export ParsedArgs parse_args(int argc, char* argv[]) {
    ParsedArgs args;

    // Options that consume the next argument as a value
    auto takes_value = [](std::string_view opt) {
        return opt == "--tag" || opt == "--type" || opt == "--std" ||
               opt == "--target" || opt == "--glibc-version" ||
               opt == "--option" || opt == "-j" || opt == "-p";
    };

    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--version" || arg == "-V") {
            args.version = true;
        } else if (starts_with(arg, "--") || (arg.size() > 1 && arg[0] == '-' && arg[1] != '\0')) {
            // It's an option
            if (contains(arg, "=")) {
                args.options.push_back(arg);
            } else if (takes_value(arg)) {
                if (i + 1 < argc) {
                    args.options.push_back(arg + "=" + argv[i + 1]);
                    ++i;
                } else {
                    // Missing value — push as empty to trigger validation errors
                    args.options.push_back(std::string(arg) + "=");
                }
            } else {
                args.options.push_back(arg);
            }
        } else if (args.command.empty()) {
            args.command = arg;
        } else {
            args.positional.push_back(arg);
        }
        ++i;
    }

    return args;
}

namespace {

struct OptionOverride {
    std::string name;
    std::optional<std::string> value;
};

std::optional<std::vector<OptionOverride>> parse_option_overrides(
        const ParsedArgs& args) {
    std::vector<OptionOverride> overrides;
    for (const auto& option : args.options) {
        if (!starts_with(option, "--option=")) continue;

        const std::string declaration = option.substr(9);
        if (declaration.empty()) {
            std::println(std::cerr,
                         "bake: --option requires <name> or <name>=<value>");
            return std::nullopt;
        }

        const size_t equals = declaration.find('=');
        OptionOverride override;
        override.name = declaration.substr(0, equals);
        if (override.name.empty()) {
            std::println(std::cerr, "bake: build option name cannot be empty");
            return std::nullopt;
        }
        if (equals != std::string::npos) {
            override.value = declaration.substr(equals + 1);
        }
        overrides.push_back(std::move(override));
    }
    return overrides;
}

std::optional<bool> parse_bool_option(std::string_view value) {
    if (value == "true" || value == "1" || value == "on" || value == "yes")
        return true;
    if (value == "false" || value == "0" || value == "off" || value == "no")
        return false;
    return std::nullopt;
}

std::string_view build_option_type_name(BuildOption::Type type) {
    switch (type) {
        case BuildOption::Type::Bool: return "boolean";
        case BuildOption::Type::Int: return "integer";
        case BuildOption::Type::String: return "string";
    }
    return "unknown";
}

std::string build_option_value(const BuildOption& option) {
    switch (option.type) {
        case BuildOption::Type::Bool:
            return option.bool_value ? "true" : "false";
        case BuildOption::Type::Int:
            return std::to_string(option.int_value);
        case BuildOption::Type::String:
            return nlohmann::json(option.str_value).dump();
    }
    return "<unknown>";
}

bool validate_dependency_options(
        const Manifest& manifest,
        const std::map<std::string, BuildOption>& dependency_options) {
    const std::string package_name =
        manifest.package && !manifest.package->name.empty()
            ? manifest.package->name
            : manifest.project_dir.string();

    for (const auto& [name, configured] : dependency_options) {
        auto declared = manifest.options.find(name);
        if (declared == manifest.options.end()) {
            std::println(
                std::cerr,
                "bake: dependency option '{}' is not declared by package '{}'",
                name, package_name);
            return false;
        }
        if (declared->second.type != configured.type) {
            std::println(
                std::cerr,
                "bake: dependency option '{}' for package '{}' expects {}, got {}",
                name, package_name,
                build_option_type_name(declared->second.type),
                build_option_type_name(configured.type));
            return false;
        }
    }
    return true;
}

std::optional<std::map<std::string, BuildOption>> effective_build_options(
        const Manifest& manifest,
        const std::map<std::string, BuildOption>& dependency_options,
        const ParsedArgs* cli_args) {
    auto options = manifest.options;

    // A parent configures this package only through the options nested in its
    // dependency declaration. Validate those values against this package's
    // own [options] schema before evaluating its recipe.
    if (!validate_dependency_options(manifest, dependency_options))
        return std::nullopt;
    for (const auto& [name, configured] : dependency_options) {
        auto it = options.find(name);
        it->second = configured;
    }

    if (!cli_args) return options;

    auto overrides = parse_option_overrides(*cli_args);
    if (!overrides) return std::nullopt;
    for (const auto& override : *overrides) {
        auto it = options.find(override.name);
        if (it == options.end()) {
            std::println(std::cerr,
                         "bake: unknown build option '{}' (declare it in [options])",
                         override.name);
            return std::nullopt;
        }

        const auto type = it->second.type;
        if (type == BuildOption::Type::Bool) {
            if (!override.value) {
                it->second = BuildOption::from_bool(true);
                continue;
            }
            auto value = parse_bool_option(*override.value);
            if (!value) {
                std::println(std::cerr,
                             "bake: option '{}' expects a boolean, got '{}'",
                             override.name, *override.value);
                return std::nullopt;
            }
            it->second = BuildOption::from_bool(*value);
        } else if (type == BuildOption::Type::Int) {
            if (!override.value || override.value->empty()) {
                std::println(std::cerr,
                             "bake: option '{}' expects an integer value",
                             override.name);
                return std::nullopt;
            }
            int64_t value = 0;
            const char* first = override.value->data();
            const char* last = first + override.value->size();
            auto [end, error] = std::from_chars(first, last, value);
            if (error != std::errc{} || end != last) {
                std::println(std::cerr,
                             "bake: option '{}' expects an integer, got '{}'",
                             override.name, *override.value);
                return std::nullopt;
            }
            it->second = BuildOption::from_int(value);
        } else {
            if (!override.value) {
                std::println(std::cerr,
                             "bake: option '{}' expects a string value",
                             override.name);
                return std::nullopt;
            }
            it->second = BuildOption::from_string(*override.value);
        }
    }
    return options;
}

bool validate_option_names(const Manifest& manifest, const ParsedArgs& args) {
    auto overrides = parse_option_overrides(args);
    if (!overrides) return false;

    for (const auto& override : *overrides) {
        if (!manifest.options.contains(override.name)) {
            std::println(std::cerr,
                         "bake: unknown build option '{}' (declare it in [options])",
                         override.name);
            return false;
        }
    }
    return true;
}

bool write_effective_options(const Layout& layout, const Manifest& manifest,
                             const ParsedArgs& args,
                             const std::map<std::string, BuildOption>&
                                 dependency_options,
                             bool apply_cli_options) {
    auto options = effective_build_options(
        manifest, dependency_options, apply_cli_options ? &args : nullptr);
    if (!options) return false;

    nlohmann::json document;
    document["schema"] = 1;
    document["options"] = nlohmann::json::object();
    for (const auto& [name, option] : *options) {
        switch (option.type) {
            case BuildOption::Type::Bool:
                document["options"][name] = option.bool_value;
                break;
            case BuildOption::Type::Int:
                document["options"][name] = option.int_value;
                break;
            case BuildOption::Type::String:
                document["options"][name] = option.str_value;
                break;
        }
    }

    const Path path = layout.bake_dir / "options.json";
    if (!atomic_write_file(path, document.dump(2))) {
        std::println(std::cerr, "bake: failed to write {}", path.string());
        return false;
    }
    return true;
}

} // namespace

// ===== Help text =====

export void print_version() {
    std::println("bake {}", BAKE_VERSION);
}

export void print_help() {
    std::println(
        "bake {} — All-in-One C/C++ toolchain\n"
        "\n"
        "USAGE:\n"
        "    bake <subcommand> [options]\n"
        "    bake <subcommand> --help\n"
        "\n"
        "SUBCOMMANDS:\n"
        "    init [name]     Create a new project scaffold\n"
        "    build           Build the project\n"
        "    add <url>       Add a dependency to bake.toml\n"
        "    update [dep]    Re-resolve dependencies and update bake.lock\n"
        "    run             Build and run the executable\n"
        "    test            Build and run tests\n"
        "    clean           Remove build artifacts\n"
        "    cc              Invoke embedded Clang C driver\n"
        "    c++             Invoke embedded Clang C++ driver\n"
        "    ar              Archiver (not yet implemented)\n"
        "    ranlib          Archive index (not yet implemented)\n"
        "\n"
        "GLOBAL OPTIONS:\n"
        "    -V, --version   Print version and exit\n"
        "    -h, --help      Print this help and exit\n"
        "\n"
        "BUILD OPTIONS:\n"
        "    --option <name>[=value]  Override a [options] value from bake.toml\n"
        "    --target=<triple>       Cross-compile target\n"
        "    --locked                Fail if lock is missing or stale\n"
        "    --offline               Never connect to the network\n"
        "    --frozen                Equivalent to --locked --offline\n"
        "    -p <member>             Build a specific workspace member\n"
        "    -j <n>                  Parallel job count\n"
        "\n"
        "For more information, visit: https://github.com/arias/bake",
        BAKE_VERSION
    );
}

export void print_command_help(std::string_view cmd) {
    if (cmd == "init") {
        std::println(
            "bake init — Create a new project scaffold\n"
            "\n"
            "USAGE:\n"
            "    bake init [name] [options]\n"
            "\n"
            "OPTIONS:\n"
            "    --type <executable|static-lib|shared-lib>  Package type (default: executable)\n"
            "    --std <c11|c17|c23|c++17|c++20|c++23>      Language standard (default: c++20)\n"
            "\n"
            "If [name] is omitted, scaffolds in the current directory."
        );
    } else if (cmd == "build") {
        std::println(
            "bake build — Build the project\n"
            "\n"
            "USAGE:\n"
            "    bake build [options]\n"
            "\n"
            "OPTIONS:\n"
            "    --option <name>[=value]  Override a [options] value\n"
            "    -j <n>                   Parallel job count\n"
            "    -p <member>              Build specific workspace member\n"
            "    --locked                 Fail if lock is missing/stale\n"
            "    --offline                No network access\n"
            "    --frozen                 --locked + --offline"
        );
    } else if (cmd == "clean") {
        std::println(
            "bake clean — Remove build artifacts\n"
            "\n"
            "USAGE:\n"
            "    bake clean"
        );
    } else {
        print_help();
    }
}

// ===== init command =====

export int cmd_init(const ParsedArgs& args) {
    // Determine project name
    std::string project_name;
    if (!args.positional.empty()) {
        project_name = args.positional[0];
    } else {
        // Use current directory name
        auto cwd = Path::current();
        project_name = cwd.filename_string();
        if (project_name.empty() || project_name == "/") {
            std::println(std::cerr, "bake: could not determine project name. Please specify: bake init <name>");
            return 1;
        }
    }

    // Determine package type
    PackageType pkg_type = PackageType::Executable;
    if (auto type_str = args.get_option("type")) {
        if (auto t = parse_package_type(*type_str)) {
            pkg_type = *t;
        } else {
            std::println(std::cerr, "bake: unknown package type '{}'", *type_str);
            return 1;
        }
    }

    // Determine std version
    std::string std_ver = "c++20";
    if (auto std_opt = args.get_option("std")) {
        std_ver = *std_opt;
    }

    // Determine target directory
    Path target_dir;
    bool create_subdir = false;
    if (!args.positional.empty()) {
        target_dir = Path::current() / project_name;
        create_subdir = true;
        if (target_dir.exists()) {
            std::println(std::cerr, "bake: directory '{}' already exists", project_name);
            return 1;
        }
    } else {
        target_dir = Path::current();
    }

    // Create directories
    (target_dir / "src").mkdir_recursive();
    (target_dir / "public").mkdir_recursive();
    (target_dir / "tests").mkdir_recursive();

    // Write bake.toml
    std::string toml_content = std::string("[package]\n")
        + "name = \"" + project_name + "\"\n"
        + "version = \"0.1.0\"\n"
        + "type = \"" + std::string(package_type_str(pkg_type)) + "\"\n"
        + "std = \"" + std_ver + "\"\n\n";

    write_file(target_dir / "bake.toml", toml_content);

    // Write source file. A C standard requests a genuine C scaffold.
    const bool use_c = is_c_standard(std_ver);
    if (pkg_type == PackageType::Executable) {
        if (use_c) {
            std::string main_c = std::string(
                "#include <stdio.h>\n\n"
                "int main(void) {\n"
                "    puts(\"Hello from " + project_name + "!\");\n"
                "    return 0;\n"
                "}\n"
            );
            write_file(target_dir / "src" / "main.c", main_c);
        } else {
            std::string main_cpp = std::string(
                "#include <iostream>\n\n"
                "int main() {\n"
                "    std::cout << \"Hello from " + project_name + "!\\n\";\n"
                "    return 0;\n"
                "}\n"
            );
            write_file(target_dir / "src" / "main.cpp", main_cpp);
        }
    } else {
        std::string lib_source = std::string(
            "// " + project_name + " library\n\n"
            "// Define your library functions here\n"
        );
        write_file(target_dir / "src" / (use_c ? "lib.c" : "lib.cpp"), lib_source);

        // Public header
        std::string public_dir = "public/" + project_name;
        (target_dir / public_dir).mkdir_recursive();
        std::string header = std::string(
            "#pragma once\n\n"
            "// " + project_name + " public API\n"
        );
        write_file(target_dir / public_dir / (project_name + (use_c ? ".h" : ".hpp")), header);
    }

    // Write .gitignore
    write_file(target_dir / ".gitignore",
        "/out/\n"
        "compile_commands.json\n"
    );

    // Print success
    if (create_subdir) {
        std::println("Created project '{}' in ./{}", project_name, project_name);
        std::println("  Next: cd {} && bake build", project_name);
    } else {
        std::println("Initialized project '{}' in current directory", project_name);
        std::println("  Next: bake build");
    }

    return 0;
}

// ===== build.cpp compilation + execution =====

// Generate std.cppm from the vendored libc++ source template.
// std.cppm.in contains @LIBCXX_MODULE_STD_INCLUDE_SOURCES@ which gets
// replaced by the concatenation of all modules/std/*.inc files.
// This mirrors what libc++'s CMake would do at configure time.
static Path generate_std_cppm(const Path& cache_dir) {
#ifdef BAKE_LIBCXX_MODULES_DIR
    Path modules_dir(BAKE_LIBCXX_MODULES_DIR);
    Path cppm_in = modules_dir / "std.cppm.in";
    if (!cppm_in.is_regular_file()) return Path();

    auto in_content = read_file(cppm_in);
    if (!in_content) return Path();

    // Collect all .inc files (glob returns sorted results)
    auto incs = glob(modules_dir / "std", "*.inc");
    if (incs.empty()) return Path();

    std::string inc_sources;
    for (const auto& inc : incs) {
        auto content = read_file(inc);
        if (content) {
            inc_sources += *content;
            inc_sources += "\n";
        }
    }

    // Replace the placeholder
    std::string cppm = *in_content;
    constexpr std::string_view placeholder = "@LIBCXX_MODULE_STD_INCLUDE_SOURCES@";
    auto pos = cppm.find(placeholder);
    if (pos == std::string::npos) return Path();
    cppm.replace(pos, placeholder.size(), inc_sources);

    // Write to cache
    Path result = cache_dir / "std.cppm";
    if (!write_file(result, cppm)) return Path();

    return result;
#else
    return Path();
#endif
}

// Curated libc++ source file list (from Zig's libcxx.zig).
static const char* libcxx_base_files[] = {
    "algorithm.cpp", "any.cpp", "bind.cpp", "call_once.cpp", "charconv.cpp",
    "chrono.cpp", "error_category.cpp", "exception.cpp", "expected.cpp",
    "filesystem/directory_entry.cpp", "filesystem/directory_iterator.cpp",
    "filesystem/filesystem_clock.cpp", "filesystem/filesystem_error.cpp",
    "filesystem/int128_builtins.cpp", "filesystem/operations.cpp",
    "filesystem/path.cpp", "fstream.cpp", "functional.cpp", "hash.cpp",
    "ios.cpp", "ios.instantiations.cpp", "iostream.cpp", "locale.cpp",
    "memory.cpp", "memory_resource.cpp", "new.cpp", "new_handler.cpp",
    "new_helpers.cpp", "optional.cpp", "ostream.cpp", "print.cpp",
    "random.cpp", "random_shuffle.cpp", "regex.cpp",
    "ryu/d2fixed.cpp", "ryu/d2s.cpp", "ryu/f2s.cpp",
    "stdexcept.cpp", "string.cpp", "strstream.cpp", "system_error.cpp",
    "typeinfo.cpp", "valarray.cpp", "variant.cpp", "vector.cpp",
    "verbose_abort.cpp", "mutex_destructor.cpp", "condition_variable_destructor.cpp",
};
// Thread-related files — only needed when multi-threaded (always on macOS).
static const char* libcxx_thread_files[] = {
    "atomic.cpp", "barrier.cpp", "condition_variable.cpp", "future.cpp",
    "mutex.cpp", "shared_mutex.cpp", "thread.cpp",
};

// Curated libc++abi source file list.
static const char* libcxxabi_files[] = {
    "abort_message.cpp", "cxa_aux_runtime.cpp", "cxa_default_handlers.cpp",
    "cxa_demangle.cpp", "cxa_exception.cpp", "cxa_exception_storage.cpp",
    "cxa_guard.cpp", "cxa_handlers.cpp", "cxa_noexception.cpp",
    "cxa_personality.cpp", "cxa_thread_atexit.cpp", "cxa_vector.cpp",
    "cxa_virtual.cpp", "fallback_malloc.cpp", "private_typeinfo.cpp",
    "stdlib_exception.cpp", "stdlib_new_delete.cpp", "stdlib_stdexcept.cpp",
    "stdlib_typeinfo.cpp",
};

// Compile libc++ + libc++abi sources from the vendored LLVM tree into .o files.
// Results are cached globally. Returns the list of object file paths.
// This replaces dynamic linking against libc++.1.dylib with static linkage.
// Approach mirrors Zig's libcxx.zig: curated file lists, specific defines,
// libcxx/src as include path for internal headers.
export std::vector<Path> ensure_libcxx_objects(const Toolchain& tc) {
    if (!tc.is_clang()) return {};

#ifdef BAKE_SRC_DIR
    std::string src(BAKE_SRC_DIR);
#else
    return {};
#endif

    // Global cache: ~/.cache/bake/libcxx-objects/
    Path cache_root = get_cache_dir().parent() / "libcxx-objects";
    std::string key = "v1-arm64-apple";
    Path cache_dir = cache_root / key;

    // Check if already built (sentinel file)
    Path sentinel = cache_dir / ".done";
    if (sentinel.is_regular_file()) {
        std::vector<Path> objs;
        namespace fs = std::filesystem;
        if (fs::exists(cache_dir.fs())) {
            for (auto& entry : fs::directory_iterator(cache_dir.fs()))
                if (entry.path().extension() == ".o")
                    objs.push_back(Path(entry.path()));
        }
        std::sort(objs.begin(), objs.end());
        if (!objs.empty()) return objs;
    }

    cache_dir.mkdir_recursive();
    std::println("   Compiling libc++ + libc++abi from source (cached)");

    std::string libcxxabi_inc = src + "/external/llvm-project/libcxxabi/include";
    std::string libcxx_src    = src + "/external/llvm-project/libcxx/src";
    std::string libcxxabi_src = src + "/external/llvm-project/libcxxabi/src";
    std::string libc_inc      = src + "/external/llvm-project/libc";

    // Common compile flags. bake's driver already injects vendored header
    // paths (libcxx_inc, builtins, darwin). We add:
    //   -I libcxxabi/include  (for __cxxabi_config.h etc.)
    //   -I libcxx/src         (for internal headers: include/atomic_support.h)
    //   -I libc               (for shared/fp_bits.h used by charconv)
    auto make_flags = [&](bool for_abi) {
        std::vector<std::string> flags;
        for (auto& a : cxx_prefix(tc)) flags.push_back(a);
        flags.push_back("-c");
        flags.push_back("-std=c++23");
        flags.push_back("-DNDEBUG");
        flags.push_back("-I" + libcxxabi_inc);
        flags.push_back("-I" + libcxx_src);
        flags.push_back("-I" + libc_inc);
        flags.push_back("-fPIC");
        flags.push_back("-Os");
        flags.push_back("-D_LIBCPP_BUILDING_LIBRARY");
        flags.push_back("-DLIBCXX_BUILDING_LIBCXXABI");
        flags.push_back("-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS");
        flags.push_back("-fvisibility=hidden");
        flags.push_back("-fvisibility-inlines-hidden");
        flags.push_back("-faligned-allocation");
        if (for_abi)
            flags.push_back("-D_LIBCXXABI_BUILDING_LIBRARY");
        return flags;
    };

    auto compile_file = [&](const std::string& dir, const std::string& filename,
                            const std::string& prefix,
                            const std::vector<std::string>& flags) -> bool {
        std::string stem = filename.substr(0, filename.rfind('.'));
        // Flatten path separators in stem (e.g. "filesystem/dir" → "filesystem__dir")
        for (auto& c : stem) if (c == '/') c = '_';
        Path obj = cache_dir / (prefix + "__" + stem + ".o");
        if (obj.is_regular_file()) return true;

        std::vector<std::string> cmd = flags;
        cmd.push_back(dir + "/" + filename);
        cmd.push_back("-o");
        cmd.push_back(obj.string());

        auto result = run_process(cmd, Path(), true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to compile {}: {}", prefix, stem);
            return false;
        }
        return true;
    };

    auto base_flags = make_flags(false);
    auto abi_flags  = make_flags(true);
    int total = 0;

    // libc++ base files
    for (auto* f : libcxx_base_files) {
        if (!compile_file(libcxx_src, f, "libcxx", base_flags)) return {};
        ++total;
    }
    // libc++ thread files (skip duplicates already in base)
    std::set<std::string> base_set(std::begin(libcxx_base_files),
                                   std::end(libcxx_base_files));
    for (auto* f : libcxx_thread_files) {
        if (base_set.count(f)) continue;
        if (!compile_file(libcxx_src, f, "libcxx", base_flags)) return {};
        ++total;
    }
    // libc++abi — compiled on all platforms. On non-WASI, skip cxa_noexception
    // (conflicts with cxa_exception.cpp — both define __getExceptionClass etc.)
    for (auto* f : libcxxabi_files) {
        std::string fname = std::string("src/") + f;
        if (fname == "src/cxa_noexception.cpp") continue;  // only for WASI
        if (!compile_file(libcxxabi_src, f, "libcxxabi", abi_flags)) return {};
        ++total;
    }

    std::println("   Compiled {} sources", total);
    write_file(sentinel, "");

    // Collect .o files (libc++abi first, matching Zig's link order)
    std::vector<Path> objs;
    namespace fs = std::filesystem;
    for (auto& entry : fs::directory_iterator(cache_dir.fs())) {
        auto name = entry.path().filename().string();
        if (name.starts_with("libcxxabi__") && entry.path().extension() == ".o")
            objs.push_back(Path(entry.path()));
    }
    for (auto& entry : fs::directory_iterator(cache_dir.fs())) {
        auto name = entry.path().filename().string();
        if (name.starts_with("libcxx__") && entry.path().extension() == ".o")
            objs.push_back(Path(entry.path()));
    }
    return objs;
}

// Ensure the libc++ std module PCM is built in this project's output tree.
// Returns the path to std.pcm, or an empty Path on failure.
// For Clang only — GCC handles import std via its gcm cache.
export Path ensure_std_pcm(const Toolchain& tc, const Path& project_out) {
    if (!tc.is_clang()) return Path();

    // The global Bake cache is reserved for immutable downloaded sources.
    Path pcm_cache = project_out / ".bmi" / ".std";
    pcm_cache.mkdir_recursive();

    // Cache key: hash of compiler path + version output
    auto cxx_args = cxx_prefix(tc);
    cxx_args.push_back("--version");
    auto ver = run_process(cxx_args, Path(), true);
    std::string key_data = tc.cxx_path + "\n" + ver.stdout_output;
    std::string key = SHA256::hex(key_data).substr(0, 16);

    Path std_pcm = pcm_cache / ("std-" + key + ".pcm");
    if (std_pcm.is_regular_file()) return std_pcm;  // cached

    // Locate std.cppm relative to the compiler: <prefix>/share/libc++/v1/std.cppm
    Path cxx_dir = Path(tc.cxx_path).parent();       // bin/
    Path prefix = cxx_dir.parent();                   // llvm/<ver>/
#ifdef BAKE_LLVM_PREFIX
    // For BakeSelf, the bake binary isn't in the LLVM installation tree.
    // Use the LLVM prefix recorded at build time.
    if (tc.kind == CompilerKind::BakeSelf)
        prefix = Path(BAKE_LLVM_PREFIX);
#endif
    Path std_cppm = prefix / "share" / "libc++" / "v1" / "std.cppm";
    if (!std_cppm.is_regular_file()) {
        // Not installed — generate from vendored libc++ source template.
        std_cppm = generate_std_cppm(pcm_cache);
        if (std_cppm.string().empty()) return Path();
    }

    // libc++ include directory: <prefix>/include/c++/v1
    Path libcxx_inc = prefix / "include" / "c++" / "v1";

    std::println("   Preparing standard library module");

    std::vector<std::string> cmd;
    for (auto& a : cxx_prefix(tc)) cmd.push_back(a);
    cmd.push_back("-std=c++23");
    cmd.push_back("-stdlib=libc++");
    cmd.push_back("-nostdinc++");
    cmd.push_back("-I" + libcxx_inc.string());
    cmd.push_back("-Wno-reserved-module-identifier");
    cmd.push_back("-c");
    cmd.push_back(std_cppm.string());
    cmd.push_back("--precompile");
    cmd.push_back("-o");
    cmd.push_back(std_pcm.string());

    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::print(std::cerr, "{}", result.stderr_output);
        std::println(std::cerr, "bake: failed to pre-build std module");
        return Path();
    }

    return std_pcm;
}

// Inject statically-compiled libc++ + libc++abi objects into all C++ link
// actions in a plan. Replaces dynamic -lc++ / libc++.1.dylib dependency.
// For BakeSelf only: the in-process LLD has no system libc++.tbd.
export void inject_static_libcxx(BuildPlan& plan, const Toolchain& tc,
                                  const Path& std_pcm) {
    if (!tc.is_clang() || !std_pcm.is_regular_file())
        return;
    if (tc.kind != CompilerKind::BakeSelf)
        return;

    auto libcxx_objs = ensure_libcxx_objects(tc);
    if (libcxx_objs.empty())
        return;

    for (auto& action : plan.actions) {
        if (action.type != BuildAction::Type::Link) continue;

        // Detect C++ link by checking if the compiler is invoked as "c++"
        bool is_cxx = false;
        for (auto& arg : action.command) {
            if (arg == "c++") { is_cxx = true; break; }
        }
        if (!is_cxx) continue;

        action.command.push_back("-nodefaultlibs");
        for (auto& obj : libcxx_objs)
            action.command.push_back(obj.string());
        action.command.push_back("-lsystem");
    }
}


export int build_with_build_cpp(
        const Layout& layout, const ParsedArgs& args,
        const std::map<std::string, BuildOption>& dependency_options = {},
        bool apply_cli_options = true) {
    const Path& root = layout.root;
    auto tc = Toolchain::detect();
    auto runtime = find_build_script_runtime();
    Path bake_dir = layout.bake_dir;
    bake_dir.mkdir_recursive();

    auto manifest = Manifest::load(root);
    if (!manifest) {
        std::println(std::cerr, "bake: failed to load {}", (root / "bake.toml").string());
        return 1;
    }
    if (!write_effective_options(layout, *manifest, args, dependency_options,
                                 apply_cli_options))
        return 1;

    Path build_cpp = root / "build.cpp";
    Path wrapper_src = runtime.wrapper;
    Path cabi_header = runtime.cabi_header;

    // Copy wrapper + header to .bake/
    Path wrapper_dst = bake_dir / "bake.build.cppm";
    Path cabi_dst = bake_dir / "bake_cabi.h";
    auto wrapper_content = read_file(wrapper_src);
    auto cabi_content = read_file(cabi_header);
    if (!wrapper_content || !cabi_content) {
        std::println(std::cerr, "bake: cannot find bake.build.cppm or bake_cabi.h");
        return 1;
    }
    write_file(wrapper_dst, *wrapper_content);
    write_file(cabi_dst, *cabi_content);

    // Pre-build the libc++ std module PCM (one-time per project/compiler).
    auto std_pcm = ensure_std_pcm(tc, layout.out_dir);

    // Step 1: Compile wrapper module → BMI + .o
    Path pcm = bake_dir / "bake.build.pcm";
    Path wrapper_o = bake_dir / "bake.build.o";

    {
        std::vector<std::string> cmd;
        for (auto& a : cxx_prefix(tc)) cmd.push_back(a);
        cmd.push_back("-c");
        cmd.push_back("-std=c++23");
        if (tc.is_clang()) {
            cmd.push_back("-stdlib=libc++");
            cmd.push_back("-Wno-reserved-module-identifier");
        }
        cmd.push_back("-x");
        cmd.push_back("c++-module");
        cmd.push_back("-I" + bake_dir.string());
        if (tc.is_clang() && std_pcm.is_regular_file()) {
            cmd.push_back("-fmodule-file=std=" + std_pcm.string());
        }
        cmd.push_back("-fmodule-output=" + pcm.string());
        cmd.push_back(wrapper_dst.string());
        cmd.push_back("-o");
        cmd.push_back(wrapper_o.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to compile bake.build.cppm");
            return 1;
        }
    }

    // Step 2: Compile build.cpp → .o
    Path build_o = bake_dir / "build.o";
    {
        std::vector<std::string> cmd;
        for (auto& a : cxx_prefix(tc)) cmd.push_back(a);
        cmd.push_back("-c");
        cmd.push_back("-std=c++23");
        if (tc.is_clang()) {
            cmd.push_back("-stdlib=libc++");
            cmd.push_back("-Wno-reserved-module-identifier");
        }
        cmd.push_back("-I" + bake_dir.string());
        if (tc.is_clang()) {
            if (std_pcm.is_regular_file()) {
                cmd.push_back("-fmodule-file=std=" + std_pcm.string());
            }
            cmd.push_back("-fmodule-file=bake.build=" + pcm.string());
        }
        cmd.push_back(build_cpp.string());
        cmd.push_back("-o");
        cmd.push_back(build_o.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to compile build.cpp");
            return 1;
        }
    }

    // Step 3: Link → build_app
    Path build_app = bake_dir / "build_app";
#ifdef _WIN32
    build_app = bake_dir / "build_app.exe";
#endif
    {
        std::vector<std::string> cmd;
        for (auto& a : cxx_prefix(tc)) cmd.push_back(a);
        if (tc.is_clang() && tc.kind != CompilerKind::BakeSelf) {
            cmd.push_back("-stdlib=libc++");
        }
        cmd.push_back(wrapper_o.string());
        cmd.push_back(build_o.string());
        // BakeSelf: inject static libc++ objects (no -lc++ available).
        if (tc.kind == CompilerKind::BakeSelf && std_pcm.is_regular_file()) {
            auto libcxx_objs = ensure_libcxx_objects(tc);
            if (!libcxx_objs.empty()) {
                cmd.push_back("-nodefaultlibs");
                for (auto& obj : libcxx_objs)
                    cmd.push_back(obj.string());
                cmd.push_back("-lsystem");
            }
        }
#ifdef _WIN32
        // Windows: link against the import library directly
        cmd.push_back((runtime.library_dir / "bake.lib").string());
#else
        cmd.push_back("-L" + runtime.library_dir.string());
        cmd.push_back("-lbake");
        cmd.push_back("-Wl,-rpath," + runtime.library_dir.string());
#endif
        cmd.push_back("-o");
        cmd.push_back(build_app.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to link build_app");
            return 1;
        }
    }

    // Step 4: Run build_app → .bake/build.json
    // Set BAKE_EXE so build_app's Toolchain::detect() uses the real bake
    // binary (not build_app itself) for BakeSelf compile/link commands.
    {
        ScopedBuildContext context(layout);
        if (!context.active()) {
            std::println(std::cerr,
                         "bake: failed to establish build script context");
            return 1;
        }
        std::string self = get_self_exe_path();
        if (!self.empty()) ::setenv("BAKE_EXE", self.c_str(), 1);
        auto result = run_process({build_app.string()}, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: build_app failed");
            return 1;
        }
    }

    // Step 5: Read build.json → execute
    Path build_json = bake_dir / "build.json";
    if (!build_json.is_regular_file()) {
        std::println(std::cerr, "bake: build_app did not produce .bake/build.json");
        return 1;
    }

    auto plan = read_build_json(build_json, root);

    // Inject import std; support into C++ compile actions.
    // build.json was generated by bake_builder_build() which doesn't know
    // about the std module — we add it here, same as the workspace path.
    if (std_pcm.is_regular_file()) {
        for (auto& action : plan.actions) {
            if (action.type != BuildAction::Type::Compile &&
                action.type != BuildAction::Type::CompileModule)
                continue;
            action.command.push_back("-fmodule-file=std=" + std_pcm.string());
            if (tc.is_clang()) {
                action.command.push_back("-stdlib=libc++");
                action.command.push_back("-Wno-reserved-module-identifier");
            }
        }
    }

    // Inject static libc++ objects into C++ link actions.
    inject_static_libcxx(plan, tc, std_pcm);

    // Write compile_commands.json
    write_compile_commands(
        plan,
        layout.dependency_layout
            ? layout.package_dir / "compile_commands.json"
            : root / "compile_commands.json");

    // Get job count
    int jobs = 0;
    if (auto j = args.get_option("j")) {
        jobs = std::atoi(j->c_str());
    }

    return execute_plan(plan, jobs);
}

// ===== Lock enforcement =====

// Check and resolve the lockfile. Returns 0 if the lock is consistent
// or was successfully (re)resolved. Returns non-zero on failure.
// On success, root/bake.lock is guaranteed to exist and be consistent.
// Cache integrity is verified on ALL builds to detect tampering.
export int enforce_lock(const Path& root, const Manifest& manifest,
                        const ParsedArgs& args,
                        const Path& package_lock_path = {}) {
    if (Lockfile::has_only_path_deps(manifest)) return 0;

    bool offline = args.has_option("offline") || args.has_option("frozen");
    bool locked = args.has_option("locked") || args.has_option("frozen");

    const bool isolated_package_lock =
        !package_lock_path.string().empty();
    Path lock_path = isolated_package_lock
        ? package_lock_path
        : root / "bake.lock";

    std::optional<Lockfile> lockfile;
    bool save_selected_lock = false;
    if (isolated_package_lock) {
        // A versioned lock beside a path package is immutable input. Copy its
        // resolved graph into this consumer's out tree; never update the
        // dependency source directory. If it is absent/stale, reuse a valid
        // consumer-local resolution before going to the network.
        auto source_lock = Lockfile::load(root / "bake.lock");
        if (source_lock && source_lock->is_consistent(manifest)) {
            lockfile = std::move(source_lock);
            save_selected_lock = true;
        } else {
            lockfile = Lockfile::load(lock_path);
        }
    } else {
        lockfile = Lockfile::load(lock_path);
    }

    bool needs_resolve = false;
    if (!lockfile) {
        needs_resolve = true;
    } else if (!lockfile->is_consistent(manifest)) {
        needs_resolve = true;
    }

    // Verify cache integrity on all builds, not just frozen/locked.
    // This detects tampered or corrupted cached dependencies before they
    // enter the build graph.
    if (!needs_resolve && lockfile) {
        if (!verify_lock_cache(*lockfile, get_cache_dir())) {
            std::println(std::cerr,
                "bake: cache verification failed — cached sources missing or corrupted");
            // We must NOT re-resolve tags — that would move locked commits.
            // Instead, re-download using the existing locked commit hashes.
            // For now, report the error clearly.
            if (locked || offline) return 1;
            // Re-download using locked commits (not re-resolve)
            Resolver resolver;
            auto re_downloaded = resolver.redownload(*lockfile, ResolverConfig{});
            if (!re_downloaded || !verify_lock_cache(*lockfile, get_cache_dir())) {
                std::println(std::cerr,
                    "bake: re-download failed. Run 'bake update' to re-resolve from scratch.");
                return 1;
            }
            std::println("bake: cached sources re-downloaded using locked commits");
        }
    }

    if (!needs_resolve) {
        if (save_selected_lock && lockfile && !lockfile->save(lock_path)) {
            std::println(std::cerr,
                         "bake: failed to write package lock state at {}",
                         lock_path.string());
            return 1;
        }
        return 0;
    }

    if (locked) {
        std::println(std::cerr, "bake: lock file missing or stale (--locked)");
        return 1;
    }
    if (offline) {
        std::println(std::cerr, "bake: cannot resolve dependencies in offline mode");
        return 1;
    }

    Resolver resolver;
    auto new_lock = resolver.resolve(manifest, ResolverConfig{});
    if (!new_lock) {
        std::println(std::cerr, "bake: failed to resolve dependencies");
        return 1;
    }
    if (!new_lock->save(lock_path)) {
        std::println(std::cerr, "bake: failed to write bake.lock");
        return 1;
    }
    std::println("bake: dependencies resolved and locked");
    return 0;
}

struct PathMetaOptionRequest {
    BuildOption value;
    std::string dependency_path;
};

struct PathMetaPackage {
    Path root;
    Manifest manifest;
    std::string name;
    std::map<std::string, PathMetaOptionRequest> option_requests;
};

struct PathMetaGraph {
    std::map<std::string, PathMetaPackage> packages;
    std::map<std::string, std::string> package_sources;
    std::map<std::string, int> visit_state; // 0=unseen, 1=visiting, 2=done
    std::vector<std::string> build_order;
};

std::string format_dependency_path(const std::vector<std::string>& path) {
    std::string result;
    for (const auto& component : path) {
        if (!result.empty()) result += " -> ";
        result += component;
    }
    return result;
}

std::string manifest_package_name(const Manifest& manifest) {
    if (manifest.package && !manifest.package->name.empty())
        return manifest.package->name;
    auto filename = manifest.project_dir.filename_string();
    return filename.empty() ? manifest.project_dir.string() : filename;
}

std::string manifest_package_label(const Manifest& manifest) {
    const std::string name = manifest_package_name(manifest);
    if (!manifest.package || manifest.package->version.empty()) return name;
    return name + " v" + manifest.package->version;
}

bool valid_package_output_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const unsigned char c : name) {
        const bool ascii_alnum = (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9');
        if (!ascii_alnum && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

// First collect the complete path-package graph and unify every explicit
// option request. Defaults are deliberately not constraints: they are applied
// only after all incoming dependency edges have been considered.
int collect_path_meta_dependencies(
    const Manifest& manifest,
    const std::vector<std::string>& parent_path,
    PathMetaGraph& graph,
    const std::set<std::string>& skip_dirs = {}) {
    for (const auto& [name, dep] : manifest.dependencies) {
        if (!dep.is_path_dep) continue;

        Path dep_root = manifest.project_dir / dep.path;
        auto dep_manifest = Manifest::load(dep_root);
        if (!dep_manifest) continue; // raw path source, not a Bake package

        Path dep_build_cpp = dep_root / "build.cpp";
        if (!dep_build_cpp.is_regular_file()) continue;

        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(dep_root.fs(), ec);
        const std::string key = ec
            ? std::filesystem::absolute(dep_root.fs()).lexically_normal().string()
            : canonical.string();

        // Skip dependencies that are workspace members — they are built
        // by the workspace loop, not through .pkgs/.
        if (skip_dirs.contains(key)) continue;

        if (!validate_dependency_options(*dep_manifest, dep.options)) return 1;

        auto [package_it, inserted] = graph.packages.try_emplace(key);
        auto& package = package_it->second;
        if (inserted) {
            package.root = dep_root;
            package.manifest = *dep_manifest;
            package.name = manifest_package_name(*dep_manifest);
            if (!valid_package_output_name(package.name)) {
                std::println(
                    std::cerr,
                    "bake: package name '{}' cannot be used as an output directory",
                    package.name);
                return 1;
            }
            auto [source_it, source_inserted] =
                graph.package_sources.try_emplace(package.name, key);
            if (!source_inserted && source_it->second != key) {
                std::println(
                    std::cerr,
                    "bake: package name conflict: '{}' refers to both {} and {}",
                    package.name, source_it->second, key);
                return 1;
            }
        }

        auto request_path = parent_path;
        request_path.push_back(name);
        const std::string formatted_path =
            format_dependency_path(request_path);

        for (const auto& [option_name, requested_value] : dep.options) {
            auto [request_it, request_inserted] =
                package.option_requests.try_emplace(
                    option_name,
                    PathMetaOptionRequest{requested_value, formatted_path});
            if (!request_inserted &&
                request_it->second.value != requested_value) {
                std::println(
                    std::cerr,
                    "bake: option conflict for package '{}':\n"
                    "  option '{}' requested as {} by {}\n"
                    "  option '{}' requested as {} by {}\n"
                    "  one project builds one option configuration per package",
                    package.name,
                    option_name,
                    build_option_value(request_it->second.value),
                    request_it->second.dependency_path,
                    option_name,
                    build_option_value(requested_value),
                    formatted_path);
                return 1;
            }
        }

        int& state = graph.visit_state[key];
        if (state == 2) continue;
        if (state == 1) {
            std::println(std::cerr,
                         "bake: cycle in path meta packages: {}",
                         formatted_path);
            return 1;
        }
        state = 1;

        if (int rc = collect_path_meta_dependencies(
                package.manifest, request_path, graph); rc != 0) {
            return rc;
        }
        state = 2;
        graph.build_order.push_back(key);
    }
    return 0;
}

int build_path_meta_dependencies(
        const std::vector<const Manifest*>& manifests,
        const ParsedArgs& args,
        const Path& project_out,
        const std::set<std::string>& skip_dirs = {}) {
    PathMetaGraph graph;
    for (const Manifest* manifest : manifests) {
        if (!manifest) continue;
        if (int rc = collect_path_meta_dependencies(
                *manifest, {manifest_package_name(*manifest)}, graph,
                skip_dirs); rc != 0) {
            return rc;
        }
    }

    for (const auto& key : graph.build_order) {
        auto& package = graph.packages.at(key);
        std::map<std::string, BuildOption> configured;
        for (const auto& [name, request] : package.option_requests)
            configured.emplace(name, request.value);

        auto layout = Layout::for_dependency(
            package.root, project_out, package.name);
        if (int rc = enforce_lock(
                package.root, package.manifest, args,
                layout.bake_dir / "bake.lock"); rc != 0) {
            return rc;
        }

        std::println("  Compiling {} (dependency)",
                     manifest_package_label(package.manifest));
        if (int rc = build_with_build_cpp(layout, args, configured,
                                          /*apply_cli_options=*/false);
            rc != 0) {
            std::println(std::cerr,
                         "bake: failed to build dependency '{}'",
                         package.name);
            return rc;
        }
    }
    return 0;
}

int build_path_meta_dependencies(const Manifest& manifest,
                                 const ParsedArgs& args,
                                 const Path& project_out) {
    return build_path_meta_dependencies(
        std::vector<const Manifest*>{&manifest}, args, project_out);
}

std::optional<std::vector<PackageUsageRequirements>>
load_path_meta_package_requirements(const Manifest& manifest,
                                    const Path& project_out,
                                    const std::set<std::string>& skip_dirs = {}) {
    std::vector<PackageUsageRequirements> result;

    for (const auto& [name, dependency] : manifest.dependencies) {
        if (!dependency.is_path_dep) continue;

        const Path dependency_root = manifest.project_dir / dependency.path;
        auto dependency_manifest = Manifest::load(dependency_root);
        if (!dependency_manifest) continue; // raw path source

        // Convention-only path packages in a workspace are linked by the
        // workspace builder today. A package with build.cpp, however, must be
        // consumed through its exported metadata and never recompiled here.
        if (!(dependency_root / "build.cpp").is_regular_file()) continue;

        // Skip workspace members — they are built by the workspace loop.
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(dependency_root.fs(), ec);
        const std::string key = ec
            ? std::filesystem::absolute(dependency_root.fs()).lexically_normal().string()
            : canonical.string();
        if (skip_dirs.contains(key)) continue;

        const std::string package_name =
            manifest_package_name(*dependency_manifest);
        const Path metadata_path =
            project_out / ".pkgs" / package_name / "package.json";
        auto content = read_file(metadata_path);
        if (!content) {
            std::println(
                std::cerr,
                "bake: dependency '{}' did not produce package metadata at {}",
                name, metadata_path.string());
            return std::nullopt;
        }

        try {
            auto metadata = nlohmann::json::parse(*content);
            if (!metadata.is_object() || metadata.value("schema", 0) != 1) {
                std::println(
                    std::cerr,
                    "bake: dependency '{}' has unsupported package metadata at {}",
                    name, metadata_path.string());
                return std::nullopt;
            }

            PackageUsageRequirements requirements;
            requirements.dependency_name = name;

            auto read_paths = [&](std::string_view field,
                                  std::vector<Path>& destination) -> bool {
                const std::string key(field);
                if (!metadata.contains(key)) return true;
                const auto& values = metadata.at(key);
                if (!values.is_array()) return false;
                for (const auto& value : values) {
                    if (!value.is_string()) return false;
                    destination.emplace_back(value.get<std::string>());
                }
                return true;
            };

            auto read_strings = [&](std::string_view field,
                                    std::vector<std::string>& destination)
                    -> bool {
                const std::string key(field);
                if (!metadata.contains(key)) return true;
                const auto& values = metadata.at(key);
                if (!values.is_array()) return false;
                for (const auto& value : values) {
                    if (!value.is_string()) return false;
                    destination.push_back(value.get<std::string>());
                }
                return true;
            };

            if (!read_paths("include_dirs", requirements.include_dirs) ||
                !read_paths("link_inputs", requirements.link_inputs) ||
                !read_strings("system_libs", requirements.system_libs) ||
                !read_strings("frameworks", requirements.frameworks)) {
                std::println(
                    std::cerr,
                    "bake: dependency '{}' has malformed package metadata at {}",
                    name, metadata_path.string());
                return std::nullopt;
            }

            if (metadata.contains("defines")) {
                const auto& defines = metadata.at("defines");
                if (!defines.is_array()) {
                    std::println(
                        std::cerr,
                        "bake: dependency '{}' has malformed package metadata at {}",
                        name, metadata_path.string());
                    return std::nullopt;
                }
                for (const auto& define : defines) {
                    if (!define.is_object() ||
                        !define.contains("name") ||
                        !define.at("name").is_string() ||
                        (define.contains("value") &&
                         !define.at("value").is_string())) {
                        std::println(
                            std::cerr,
                            "bake: dependency '{}' has malformed package metadata at {}",
                            name, metadata_path.string());
                        return std::nullopt;
                    }
                    requirements.defines.push_back({
                        define.at("name").get<std::string>(),
                        define.value("value", std::string{})});
                }
            }

            if (metadata.contains("uses_cxx") &&
                !metadata.at("uses_cxx").is_boolean()) {
                std::println(
                    std::cerr,
                    "bake: dependency '{}' has malformed package metadata at {}",
                    name, metadata_path.string());
                return std::nullopt;
            }
            requirements.uses_cxx = metadata.value("uses_cxx", false);
            result.push_back(std::move(requirements));
        } catch (const std::exception& error) {
            std::println(
                std::cerr,
                "bake: cannot read package metadata for dependency '{}': {}",
                name, error.what());
            return std::nullopt;
        }
    }

    return result;
}

// ===== Dep source extraction (bridges bake.package types to bake.engine types) =====

// Extract bake-native dep sources and include dirs from a lockfile.
// Walks ALL lock nodes (root + transitive) in topological order so that
// dependencies are compiled before dependents.
static void extract_dep_info(
        const Lockfile& lockfile,
        const Path& cache_dir,
        std::vector<DepSourceEntry>& dep_sources,
        std::vector<Path>& dep_include_dirs) {

    // Topological sort of lock nodes by their dependency edges.
    // Nodes with no dependencies come first.
    std::vector<std::string> topo_order;
    {
        std::map<std::string, int> in_degree;
        std::map<std::string, std::vector<std::string>> dependents;

        for (auto& [id, node] : lockfile.nodes) {
            in_degree[id] = 0;
        }
        for (auto& [id, node] : lockfile.nodes) {
            for (auto& child_id : node.dependencies) {
                if (lockfile.nodes.count(child_id)) {
                    dependents[child_id].push_back(id);
                    in_degree[id]++;
                }
            }
        }

        // Kahn's algorithm
        std::vector<std::string> queue;
        for (auto& [id, deg] : in_degree) {
            if (deg == 0) queue.push_back(id);
        }
        std::sort(queue.begin(), queue.end());

        while (!queue.empty()) {
            std::string node_id = queue.front();
            queue.erase(queue.begin());
            topo_order.push_back(node_id);

            for (auto& dep_id : dependents[node_id]) {
                if (--in_degree[dep_id] == 0) {
                    queue.push_back(dep_id);
                }
            }
            std::sort(queue.begin(), queue.end());
        }

        // Append any remaining (cycle) nodes
        for (auto& [id, node] : lockfile.nodes) {
            if (std::find(topo_order.begin(), topo_order.end(), id) == topo_order.end()) {
                topo_order.push_back(id);
            }
        }
    }

    // Walk nodes in topological order
    for (auto& node_id : topo_order) {
        auto node_it = lockfile.nodes.find(node_id);
        if (node_it == lockfile.nodes.end()) continue;
        const auto& node = node_it->second;
        if (!node.native) continue;
        if (node.tree_sha256.empty()) continue;

        Path dep_cache = cache_dir / node.tree_sha256;
        if (!dep_cache.is_directory()) {
            std::println(std::cerr, "bake: cached source for '{}' not found at {}",
                         node_id, dep_cache.string());
            continue;
        }

        Path dep_public = dep_cache / "public";
        if (dep_public.is_directory()) {
            dep_include_dirs.push_back(dep_public);
        }

        Path dep_src = dep_cache / "src";
        if (dep_src.is_directory()) {
            auto dep_disc = discover_sources(dep_src, dep_public);
            for (auto& cpp : dep_disc.cpp_files) {
                dep_sources.push_back({node_id, cpp});
            }
            for (auto& c : dep_disc.c_files) {
                dep_sources.push_back({node_id, c});
            }
        }
    }
}

// ===== build command =====

export int cmd_build(const ParsedArgs& args) {
    // Find project root
    auto root = find_project_root();
    if (!root) {
        std::println(std::cerr, "bake: no bake.toml found in current directory or any parent");
        return 1;
    }

    // Load manifest (needed for lock enforcement before build.cpp dispatch)
    auto manifest = Manifest::load(*root);
    if (!manifest) {
        std::println(std::cerr, "bake: failed to load bake.toml");
        return 1;
    }

    if (!validate_option_names(*manifest, args)) return 1;

    // ===== Lock enforcement (before build.cpp so --locked/--frozen work universally)
    if (int rc = enforce_lock(*root, *manifest, args); rc != 0) return rc;

    if (manifest->has_package()) {
        std::println("   Building {}", manifest_package_label(*manifest));
    } else if (manifest->is_workspace()) {
        const std::string workspace_name = root->filename_string().empty()
            ? root->string()
            : root->filename_string();
        std::println("   Building workspace {}", workspace_name);
    }

    // Handle workspace: build all members with inter-member deps
    if (manifest->is_workspace()) {
        int result = 0;
        const Path project_out = Layout::detect(*root).out_dir;
        int jobs = 0;
        if (auto j = args.get_option("j")) {
            jobs = std::atoi(j->c_str());
        }

        // Enforce lock for the workspace: merge all members' remote deps into
        // a single combined manifest, then resolve once. This prevents each
        // member's enforce_lock from overwriting the shared bake.lock.
        {
            Manifest combined;
            combined.project_dir = *root;
            for (auto& member : manifest->workspace->members) {
                Path member_dir = *root / member;
                auto member_manifest = Manifest::load(member_dir);
                if (!member_manifest) continue;
                for (auto& [name, dep] : member_manifest->dependencies) {
                    if (dep.is_path_dep) continue;
                    // Detect conflicts: same dep name but different URL/tag
                    auto existing = combined.dependencies.find(name);
                    if (existing != combined.dependencies.end()) {
                        if (existing->second.url != dep.url || existing->second.tag != dep.tag) {
                            std::println(std::cerr,
                                "bake: dependency conflict — '{}' declared differently:\n"
                                "  member '{}': url=\"{}\", tag=\"{}\"\n"
                                "  member '{}': url=\"{}\", tag=\"{}\"",
                                name,
                                /* find which member declared the existing one */
                                "(earlier)", existing->second.url, existing->second.tag,
                                member, dep.url, dep.tag);
                            return 1;
                        }
                        // Same URL + tag — reuse, skip
                        continue;
                    }
                    combined.dependencies[name] = dep;
                }
            }
            if (!Lockfile::has_only_path_deps(combined)) {
                if (int rc = enforce_lock(*root, combined, args); rc != 0) {
                    return rc;
                }
            }
        }

        // Load the final lockfile (may have been updated by member enforcement)
        auto lockfile = Lockfile::load(*root / "bake.lock");

        // Extract dep sources + include dirs from lockfile
        std::vector<DepSourceEntry> dep_sources;
        std::vector<Path> dep_include_dirs;
        if (lockfile && !lockfile->empty()) {
            extract_dep_info(*lockfile, get_cache_dir(),
                             dep_sources, dep_include_dirs);
        }

        // Track built members for inter-member dependency resolution
        struct BuiltMember {
            std::string name;
            Path bmi_dir;
            Path lib_path;
            std::vector<std::string> module_names;
        };
        std::vector<BuiltMember> built;

        // Build the std module lazily: an all-C workspace must not require a
        // C++ standard library module at all.
        Path std_pcm;

        // Compute canonical paths of all workspace member directories so
        // path-meta-dependency collection can skip them. Workspace members
        // are built by the loop below, never through .pkgs/.
        std::set<std::string> workspace_member_dirs;
        for (const auto& member : manifest->workspace->members) {
            Path member_dir = *root / member;
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(member_dir.fs(), ec);
            workspace_member_dirs.insert(ec
                ? std::filesystem::absolute(member_dir.fs()).lexically_normal().string()
                : canonical.string());
        }

        // A workspace owns one dependency package graph. Collect every
        // selected member's requests before building any path meta package so
        // one package cannot be overwritten by a later member's options.
        std::vector<Manifest> selected_member_manifests;
        for (const auto& member : manifest->workspace->members) {
            auto member_manifest = Manifest::load(*root / member);
            if (!member_manifest || !member_manifest->has_package()) continue;
            if (auto p = args.get_option("p");
                p && *p != member_manifest->package->name) {
                continue;
            }
            selected_member_manifests.push_back(std::move(*member_manifest));
        }
        std::vector<const Manifest*> selected_member_refs;
        selected_member_refs.reserve(selected_member_manifests.size());
        for (const auto& selected : selected_member_manifests)
            selected_member_refs.push_back(&selected);
        if (int rc = build_path_meta_dependencies(
                selected_member_refs, args, project_out,
                workspace_member_dirs); rc != 0) {
            return rc;
        }

        for (auto& member : manifest->workspace->members) {
            Path member_dir = *root / member;
            auto member_manifest = Manifest::load(member_dir);
            if (!member_manifest || !member_manifest->has_package()) {
                std::println(std::cerr, "bake: skipping workspace member '{}' (no [package])", member);
                continue;
            }

            // Check -p flag
            if (auto p = args.get_option("p")) {
                if (*p != member_manifest->package->name) continue;
            }

            std::println("  Compiling {}",
                         manifest_package_label(*member_manifest));
            auto layout = Layout::detect(member_dir, *root);
            auto tc = Toolchain::detect();

            // ── build.cpp escape hatch ──
            // A member with build.cpp is built by its own script instead of
            // convention mode. The output is recorded in `built` so that
            // downstream workspace members can link against it.
            Path member_build_cpp = member_dir / "build.cpp";
            if (member_build_cpp.is_regular_file()) {
                if (!is_c_standard(member_manifest->package->std_version)) {
                    if (!std_pcm.is_regular_file())
                        std_pcm = ensure_std_pcm(tc, layout.out_dir);
                }

                if (int rc = build_with_build_cpp(layout, args); rc != 0) {
                    result = rc; break;
                }

                // Record this member for dependents
                BuiltMember bm;
                bm.name = member_manifest->package->name;
                bm.bmi_dir = layout.bmi_dir;

                // Read primary output from build.json
                Path build_json = layout.bake_dir / "build.json";
                if (build_json.is_regular_file()) {
                    auto bp = read_build_json(build_json, member_dir);
                    bm.lib_path = bp.primary_output;
                }

                // Collect module names from BMI directory
                if (bm.bmi_dir.is_directory()) {
                    for (auto& entry : std::filesystem::directory_iterator(bm.bmi_dir.fs())) {
                        if (entry.path().extension() == ".pcm") {
                            bm.module_names.push_back(entry.path().stem().string());
                        }
                    }
                }
                built.push_back(std::move(bm));
                continue;
            }

            // ── convention mode ──
            auto member_package_requirements =
                load_path_meta_package_requirements(
                    *member_manifest, project_out, workspace_member_dirs);
            if (!member_package_requirements) return 1;

            Path member_std_pcm;
            if (!is_c_standard(member_manifest->package->std_version)) {
                if (!std_pcm.is_regular_file())
                    std_pcm = ensure_std_pcm(tc, layout.out_dir);
                member_std_pcm = std_pcm;
            }

            auto plan = create_convention_plan(*member_manifest, layout, tc,
                                                member_manifest->options,
                                                dep_sources, dep_include_dirs,
                                                /*compile_path_deps=*/false,
                                                member_std_pcm,
                                                *member_package_requirements);

            // Inject external modules from built dependencies
            for (auto& action : plan.actions) {
                if (action.type != BuildAction::Type::Compile &&
                    action.type != BuildAction::Type::CompileModule) continue;

                for (auto& bm : built) {
                    if (!bm.bmi_dir.is_directory()) continue;
                    for (auto& mod_name : bm.module_names) {
                        Path bmi = bm.bmi_dir / (mod_name + ".pcm");
                        if (bmi.is_regular_file()) {
                            action.command.push_back("-fmodule-file=" + mod_name + "=" + bmi.string());
                        }
                    }
                }
                // Also inject std PCM so transitive import std; resolves
                if (std_pcm.is_regular_file()) {
                    action.command.push_back("-fmodule-file=std=" + std_pcm.string());
                    // Clang needs libc++ for import std
                    if (tc.is_clang()) {
                        action.command.push_back("-stdlib=libc++");
                        action.command.push_back("-Wno-reserved-module-identifier");
                    }
                }
            }

            // Inject library links from built dependencies
            for (auto& action : plan.actions) {
                if (action.type != BuildAction::Type::Link) continue;

                // Clang needs -stdlib=libc++ when import std is in use,
                // unless BakeSelf (which uses static libc++ objects instead).
                if (std_pcm.is_regular_file() && tc.is_clang() &&
                    tc.kind != CompilerKind::BakeSelf) {
                    auto prefix_len = cxx_prefix(tc).size();
                    action.command.insert(action.command.begin() + prefix_len, "-stdlib=libc++");
                }

                for (auto& bm : built) {
                    if (bm.lib_path.string().empty()) continue;
                    Path lib_dir = bm.lib_path.parent();
#ifdef _WIN32
                    // Windows: link the .lib directly
                    action.command.insert(action.command.end() - 2,
                        (lib_dir / (bm.name + ".lib")).string());
#else
                    action.command.insert(action.command.end() - 2,
                        {"-L" + lib_dir.string(), "-l" + bm.name});
                    action.command.insert(action.command.end() - 2,
                        "-Wl,-rpath," + lib_dir.string());
#endif
                }
            }

            // Inject static libc++ for BakeSelf (replaces -stdlib=libc++)
            inject_static_libcxx(plan, tc, std_pcm);

            int r = execute_plan(plan, jobs);
            if (r != 0) { result = r; break; }

            // Record this member for dependents
            BuiltMember bm;
            bm.name = member_manifest->package->name;
            bm.bmi_dir = layout.bmi_dir;
            bm.lib_path = plan.primary_output;

            // Collect module names from BMI directory
            if (bm.bmi_dir.is_directory()) {
                for (auto& entry : std::filesystem::directory_iterator(bm.bmi_dir.fs())) {
                    if (entry.path().extension() == ".pcm") {
                        bm.module_names.push_back(entry.path().stem().string());
                    }
                }
            }
            built.push_back(std::move(bm));
        }
        if (result == 0) {
            const std::string workspace_name = root->filename_string().empty()
                ? root->string()
                : root->filename_string();
            std::println("    Finished workspace {}", workspace_name);
        }
        return result;
    }

    if (!manifest->has_package()) {
        std::println(std::cerr, "bake: no [package] section in bake.toml");
        return 1;
    }

    // Check for build.cpp (single-package escape hatch)
    Path build_cpp = *root / "build.cpp";
    if (build_cpp.is_regular_file()) {
        auto layout = Layout::detect(*root);
        if (int rc = build_path_meta_dependencies(
                *manifest, args, layout.out_dir); rc != 0)
            return rc;
        return build_with_build_cpp(layout, args);
    }

    auto layout = Layout::detect(*root);
    if (int rc = build_path_meta_dependencies(
            *manifest, args, layout.out_dir); rc != 0)
        return rc;
    auto package_requirements =
        load_path_meta_package_requirements(*manifest, layout.out_dir);
    if (!package_requirements) return 1;

    // Single package build
    auto tc = Toolchain::detect();

    const bool package_uses_c = is_c_standard(manifest->package->std_version);
    std::println("   Toolchain {} ({})", tc.kind_name(),
                 package_uses_c ? tc.cc_path : tc.cxx_path);

    // A pure C package must not depend on libc++'s std module.
    Path std_pcm;
    if (!package_uses_c) {
        std_pcm = ensure_std_pcm(tc, layout.out_dir);
    }

    // Extract dep sources + include dirs from lockfile
    std::vector<DepSourceEntry> dep_sources;
    std::vector<Path> dep_include_dirs;
    auto lockfile = Lockfile::load(*root / "bake.lock");
    if (lockfile && !lockfile->empty()) {
        extract_dep_info(*lockfile, get_cache_dir(),
                         dep_sources, dep_include_dirs);
    }

    // Apply type-preserving option overrides.
    auto options = effective_build_options(*manifest, {}, &args);
    if (!options) return 1;

    auto plan = create_convention_plan(*manifest, layout, tc, *options,
                                        dep_sources, dep_include_dirs,
                                        /*compile_path_deps=*/true,
                                        std_pcm,
                                        *package_requirements);

    // Inject static libc++ for BakeSelf (replaces -stdlib=libc++)
    inject_static_libcxx(plan, tc, std_pcm);

    // Write compile_commands.json
    write_compile_commands(plan, *root / "compile_commands.json");

    // Get job count
    int jobs = 0;
    if (auto j = args.get_option("j")) {
        jobs = std::atoi(j->c_str());
    }

    return execute_plan(plan, jobs);
}

// ===== clean command =====

export int cmd_clean(const ParsedArgs& args) {
    auto root = find_project_root();
    if (!root) {
        std::println(std::cerr, "bake: no bake.toml found");
        return 1;
    }

    int removed = 0;

    // Remove unified output directory
    Path out_dir = *root / "out";
    if (out_dir.exists()) {
        out_dir.remove_all();
        std::println("Removed {}", out_dir.string());
        removed++;
    }

    if (removed == 0) {
        std::println("Nothing to clean");
    }

    return 0;
}

// ===== run command =====

export int cmd_run(const ParsedArgs& args) {
    // First build
    int build_result = cmd_build(args);
    if (build_result != 0) return build_result;

    // Find the built executable
    auto root = find_project_root();
    if (!root) return 1;

    Path exe_path;

    auto manifest = Manifest::load(*root);
    if (!manifest) {
        std::println(std::cerr, "bake: failed to load bake.toml");
        return 1;
    }

    // Workspace: find the executable member's output in out/bin/
    if (manifest->is_workspace()) {
        auto layout = Layout::detect(*root);
        for (auto& member : manifest->workspace->members) {
            auto member_manifest = Manifest::load(*root / member);
            if (!member_manifest || !member_manifest->has_package()) continue;
            if (member_manifest->package->type == PackageType::Executable) {
                // Check -p flag
                if (auto p = args.get_option("p")) {
                    if (*p != member_manifest->package->name) continue;
                }
                std::string exe_name = library_name(
                    member_manifest->package->name, PackageType::Executable);
                exe_path = layout.bin_dir / exe_name;
                break;
            }
        }
    } else if (manifest->has_package()) {
        // Single package: convention mode
        auto layout = Layout::detect(*root);

        if (manifest->package->type != PackageType::Executable) {
            std::println(std::cerr, "bake: cannot run non-executable package");
            return 1;
        }

        std::string exe_name = library_name(manifest->package->name, PackageType::Executable);
        exe_path = layout.bin_dir / exe_name;
    }

    if (!exe_path.exists()) {
        std::println(std::cerr, "bake: built executable not found at {}", exe_path.string());
        return 1;
    }

    // Build command line: executable + remaining args
    std::vector<std::string> run_args;
    for (auto& p : args.positional) {
        run_args.push_back(p);
    }

    auto result = run_process(exe_path.string(), run_args, *root);
    return result.exit_code;
}

// ===== add command =====

export int cmd_add(const ParsedArgs& args) {
    auto root = find_project_root();
    if (!root) {
        std::println(std::cerr, "bake: no bake.toml found");
        return 1;
    }

    if (args.positional.empty()) {
        std::println(std::cerr, "bake: add requires a URL");
        std::println(std::cerr, "Usage: bake add <url> --tag <tag> [name]");
        return 1;
    }

    std::string url = args.positional[0];
    std::string tag = args.get_option("tag").value_or("");
    std::string name;

    // Derive name from URL or use second positional
    if (args.positional.size() >= 2) {
        name = args.positional[1];
    } else {
        // Extract repo name from URL
        // e.g., https://github.com/fmtlib/fmt → fmt
        size_t last_slash = url.find_last_of('/');
        if (last_slash != std::string::npos) {
            name = url.substr(last_slash + 1);
            // Remove .git suffix
            if (ends_with(name, ".git")) {
                name = name.substr(0, name.size() - 4);
            }
        } else {
            name = "dep";
        }
    }

    if (tag.empty()) {
        std::println(std::cerr, "bake: add requires --tag <tag>");
        return 1;
    }

    // Read current bake.toml
    Path toml_path = *root / "bake.toml";
    auto content = read_file(toml_path);
    if (!content) {
        std::println(std::cerr, "bake: cannot read bake.toml");
        return 1;
    }

    // Check for duplicate dependency name by parsing the TOML properly.
    // This handles both "name = { ... }" (compact) and "name = {url=...}" forms.
    {
        try {
            auto tbl = toml::parse_file(toml_path.string());
            if (auto* deps = tbl["dependencies"].as_table()) {
                if (deps->contains(name)) {
                    std::println(std::cerr,
                        "bake: dependency '{}' already exists in bake.toml",
                        name);
                    return 1;
                }
            }
        } catch (...) {
            // If TOML parsing fails, fall through — the write will still work
        }
    }

    // Append dependency to [dependencies] section
    std::string dep_line = name + " = { url = \"" + url + "\", tag = \"" + tag + "\" }\n";

    // Check if [dependencies] section exists
    if (contains(*content, "[dependencies]")) {
        // Insert after the [dependencies] line
        size_t pos = content->find("[dependencies]");
        pos = content->find('\n', pos);
        if (pos == std::string::npos) pos = content->size();
        content->insert(pos + 1, dep_line);
    } else {
        // Add new [dependencies] section at end
        if (!content->empty() && content->back() != '\n') *content += "\n";
        *content += "\n[dependencies]\n" + dep_line;
    }

    if (!write_file(toml_path, *content)) {
        std::println(std::cerr, "bake: failed to write bake.toml");
        return 1;
    }

    std::println("Added dependency '{}' = {{ url = \"{}\", tag = \"{}\" }}",
                name, url, tag);
    std::println("Run 'bake build' to resolve and download.");
    return 0;
}

// ===== update command =====

export int cmd_update(const ParsedArgs& args) {
    auto root = find_project_root();
    if (!root) {
        std::println(std::cerr, "bake: no bake.toml found");
        return 1;
    }

    auto manifest = Manifest::load(*root);
    if (!manifest) {
        std::println(std::cerr, "bake: failed to load bake.toml");
        return 1;
    }

    if (manifest->dependencies.empty()) {
        std::println("bake: no dependencies to update");
        return 0;
    }

    // Determine which dep to update (empty = all)
    std::string filter_dep;
    if (!args.positional.empty()) {
        filter_dep = args.positional[0];
        if (manifest->dependencies.find(filter_dep) == manifest->dependencies.end()) {
            std::println(std::cerr, "bake: dependency '{}' not found in bake.toml", filter_dep);
            return 1;
        }
        auto& dep = manifest->dependencies[filter_dep];
        if (dep.is_path_dep) {
            std::println(std::cerr, "bake: '{}' is a path dependency (no resolution needed)", filter_dep);
            return 1;
        }
    }

    Path lock_path = *root / "bake.lock";

    // Load existing lock for comparison / preservation
    auto old_lock = Lockfile::load(lock_path);

    Resolver resolver;

    if (filter_dep.empty()) {
        // Full re-resolve (existing behavior)
        auto new_lock = resolver.resolve(*manifest, ResolverConfig{});
        if (!new_lock) {
            std::println(std::cerr, "bake: failed to resolve dependencies");
            return 1;
        }

        // Report changes
        if (old_lock) {
            for (auto& [name, node_id] : new_lock->root_deps) {
                auto old_it = old_lock->root_deps.find(name);
                if (old_it != old_lock->root_deps.end()) {
                    auto& old_node = old_lock->nodes[old_it->second];
                    auto& new_node = new_lock->nodes[node_id];
                    if (old_node.commit != new_node.commit) {
                        std::println("bake: {} updated: {} → {}",
                                    name, old_node.commit.substr(0, 12),
                                    new_node.commit.substr(0, 12));
                    }
                } else {
                    std::println("bake: {} added", name);
                }
            }
        }

        if (!new_lock->save(lock_path)) {
            std::println(std::cerr, "bake: failed to write bake.lock");
            return 1;
        }
    } else {
        // Single-dep update: resolve the dep AND its transitive closure,
        // then prune old transitive nodes and merge new ones.
        //
        // If the old lock is missing or corrupt, we can't safely do a
        // partial update (we'd lose other root deps). Fall back to full resolve.
        if (!old_lock || !old_lock->is_consistent(*manifest)) {
            std::println("bake: lock missing or stale, doing full re-resolve");
            auto new_lock = resolver.resolve(*manifest, ResolverConfig{});
            if (!new_lock) {
                std::println(std::cerr, "bake: failed to resolve dependencies");
                return 1;
            }
            if (!new_lock->save(lock_path)) {
                std::println(std::cerr, "bake: failed to write bake.lock");
                return 1;
            }
            std::println("bake: lock file updated");
            return 0;
        }

        auto& dep = manifest->dependencies[filter_dep];

        // Resolve dep + full transitive subtree
        auto subtree = resolver.resolve_subtree(dep, filter_dep);
        if (!subtree) {
            std::println(std::cerr, "bake: failed to resolve '{}'", filter_dep);
            return 1;
        }

        // Report change
        if (old_lock) {
            auto old_it = old_lock->root_deps.find(filter_dep);
            if (old_it != old_lock->root_deps.end()) {
                auto& old_node = old_lock->nodes[old_it->second];
                auto& new_node = subtree->nodes[subtree->root_deps[filter_dep]];
                if (old_node.commit != new_node.commit) {
                    std::println("bake: {} updated: {} → {}",
                                filter_dep, old_node.commit.substr(0, 12),
                                new_node.commit.substr(0, 12));
                } else {
                    std::println("bake: {} unchanged ({})", filter_dep,
                                new_node.commit.substr(0, 12));
                }
            } else {
                std::println("bake: {} added", filter_dep);
            }
        }

        // Build the merged lock:
        // 1. Start with a fresh lockfile
        // 2. Copy all old root_deps except the updated one
        // 3. Copy all old nodes except those exclusively reachable from the old dep
        // 4. Add all new nodes from the subtree
        Lockfile new_lock;

        // Find the old node ID for the updated dep
        std::string old_node_id;
        if (old_lock) {
            auto old_it = old_lock->root_deps.find(filter_dep);
            if (old_it != old_lock->root_deps.end()) {
                old_node_id = old_it->second;
            }
        }

        // Compute set of nodes reachable from old_node_id (the old subtree)
        std::set<std::string> old_subtree_nodes;
        if (!old_node_id.empty() && old_lock) {
            std::vector<std::string> queue = {old_node_id};
            while (!queue.empty()) {
                std::string nid = queue.back();
                queue.pop_back();
                if (old_subtree_nodes.count(nid)) continue;
                old_subtree_nodes.insert(nid);
                auto n_it = old_lock->nodes.find(nid);
                if (n_it != old_lock->nodes.end()) {
                    for (auto& child : n_it->second.dependencies) {
                        queue.push_back(child);
                    }
                }
            }
        }

        // Compute set of nodes reachable from OTHER root deps (keep these)
        std::set<std::string> keep_nodes;
        if (old_lock) {
            for (auto& [name, nid] : old_lock->root_deps) {
                if (name == filter_dep) continue;
                std::vector<std::string> queue = {nid};
                while (!queue.empty()) {
                    std::string id = queue.back();
                    queue.pop_back();
                    if (keep_nodes.count(id)) continue;
                    keep_nodes.insert(id);
                    auto n_it = old_lock->nodes.find(id);
                    if (n_it != old_lock->nodes.end()) {
                        for (auto& child : n_it->second.dependencies) {
                            queue.push_back(child);
                        }
                    }
                }
            }
        }

        // Copy old root_deps (except the updated dep)
        if (old_lock) {
            for (auto& [name, nid] : old_lock->root_deps) {
                if (name == filter_dep) continue;
                new_lock.root_deps[name] = nid;
            }
        }

        // Copy old nodes that are NOT exclusively in the old subtree
        if (old_lock) {
            for (auto& [id, node] : old_lock->nodes) {
                // Keep if not in old subtree, or if also reachable from other deps
                if (old_subtree_nodes.count(id) && !keep_nodes.count(id)) {
                    continue; // Prune stale transitive node
                }
                new_lock.nodes[id] = node;
            }
        }

        // Add all new nodes from the subtree
        for (auto& [id, node] : subtree->nodes) {
            new_lock.nodes[id] = node;
        }
        new_lock.root_deps[filter_dep] = subtree->root_deps[filter_dep];

        if (!new_lock.save(lock_path)) {
            std::println(std::cerr, "bake: failed to write bake.lock");
            return 1;
        }
    }

    std::println("bake: lock file updated");
    return 0;
}

// ===== Stub commands =====

export int cmd_test(const ParsedArgs& args) {
    std::println(std::cerr, "bake: test not yet implemented (Phase 7)");
    return 1;
}

// ===== Main dispatch =====

export int main(int argc, char* argv[]) {
    // Fast path: -cc1 / -cc1as are Clang driver internal modes spawned when
    // the driver falls back to subprocess execution.  These must bypass bake's
    // argument parser entirely (the args contain options like -triple that
    // look like flags to our parser).
    if (argc >= 2 && argv[1]) {
        std::string_view a1(argv[1]);
        if (a1 == "-cc1" || a1 == "-cc1as") {
            if (!bake_has_llvm()) {
                std::println(std::cerr, "bake: LLVM support not compiled in");
                return 1;
            }
            return bake_clang_main(argc, const_cast<const char**>(argv));
        }
    }

    ParsedArgs args = parse_args(argc, argv);

    // No command: handle --version / --help / default
    if (args.command.empty()) {
        if (args.version) {
            print_version();
            return 0;
        }
        print_help();
        return 0;
    }

    // Command-specific --help
    if (args.help) {
        print_command_help(args.command);
        return 0;
    }

    // Dispatch
    if (args.command == "init")     return cmd_init(args);
    if (args.command == "build")    return cmd_build(args);
    if (args.command == "add")      return cmd_add(args);
    if (args.command == "update")   return cmd_update(args);
    if (args.command == "run")      return cmd_run(args);
    if (args.command == "test")     return cmd_test(args);
    if (args.command == "clean")    return cmd_clean(args);

    // Phase 5: LLVM compiler integration — pass raw argv to Clang driver
    if (args.command == "cc" || args.command == "c++") {
        if (!bake_has_llvm()) {
            std::println(std::cerr, "bake {}: LLVM support not compiled in", args.command);
            return 1;
        }
        return bake_clang_main(argc, const_cast<const char**>(argv));
    }

    // Phase 6: LLVM binutils (stubs for now)
    if (args.command == "ar" || args.command == "ranlib") {
        std::println(std::cerr, "bake {} not yet implemented (Phase 6)", args.command);
        return 1;
    }

    // Version / help as commands
    if (args.command == "--version" || args.command == "-V") {
        print_version();
        return 0;
    }
    if (args.command == "--help" || args.command == "-h") {
        print_help();
        return 0;
    }

    std::println(std::cerr, "bake: unknown command '{}'", args.command);
    std::println(std::cerr, "Run 'bake --help' for usage.");
    return 1;
}

} // namespace bake::cli
