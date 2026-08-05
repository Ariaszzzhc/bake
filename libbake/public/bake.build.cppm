module;

// bake.build.cppm — Thin C++ wrapper over the bake C ABI.
// Distributed as source; compiled fresh per project.

#include "bake_cabi.h"

export module bake.build;

import std;

// ============================================================
// bake.build — C++ API for build.cpp scripts
// ============================================================

export namespace bake {

class Builder;
class Step;
class Dependency;

// Options for source files added via Target::sources().
struct SourceOptions {
    std::vector<std::string> flags;  // extra compiler flags for these sources
};

class Target {
    friend class Builder;
    bake_target* handle_ = nullptr;
public:
    Target() = default;
    explicit Target(bake_target* h) : handle_(h) {}

    Target& std(const char* ver) { bake_target_std(handle_, ver); return *this; }
    Target& sources(const char* pattern) { bake_target_sources(handle_, pattern); return *this; }
    Target& sources(std::initializer_list<const char*> patterns) {
        for (const char* pattern : patterns) bake_target_sources(handle_, pattern);
        return *this;
    }
    Target& sources(const char* pattern, const SourceOptions& opts) {
        std::vector<const char*> flags(opts.flags.size());
        for (size_t i = 0; i < opts.flags.size(); ++i) flags[i] = opts.flags[i].c_str();
        bake_target_sources_with_flags(handle_, pattern, flags.data(),
                                       static_cast<int>(flags.size()));
        return *this;
    }
    Target& sources(std::initializer_list<const char*> patterns, const SourceOptions& opts) {
        for (const char* pattern : patterns) sources(pattern, opts);
        return *this;
    }
    Target& sources(Dependency& dependency, const char* pattern);
    Target& sources(Dependency& dependency,
                    std::initializer_list<const char*> patterns);
    Target& include_dirs(const char* dirs) { bake_target_include_dirs(handle_, dirs); return *this; }
    Target& include_dirs(std::initializer_list<const char*> dirs) {
        for (const char* dir : dirs) bake_target_include_dirs(handle_, dir);
        return *this;
    }
    Target& include_dirs(Dependency& dependency, const char* dirs);
    Target& private_include_dirs(const char* dirs) {
        bake_target_private_include_dirs(handle_, dirs); return *this;
    }
    Target& private_include_dirs(Dependency& dependency, const char* dirs);
    Target& define(const char* name, const char* value = "") {
        bake_target_define(handle_, name, value); return *this;
    }
    Target& private_define(const char* name, const char* value = "") {
        bake_target_private_define(handle_, name, value); return *this;
    }
    Target& link(Target& other) { bake_target_link(handle_, other.handle_); return *this; }
    Target& link_system(const char* lib) { bake_target_link_system(handle_, lib); return *this; }
    Target& link_framework(const char* framework) {
        bake_target_link_framework(handle_, framework); return *this;
    }
    Target& depends_on(Step& step);

    bake_target* raw() const { return handle_; }
};

class Step {
    friend class Builder;
    friend class Target;
    bake_step* handle_ = nullptr;
public:
    Step() = default;
    explicit Step(bake_step* h) : handle_(h) {}

    Step& outputs(const char* pattern) { bake_step_outputs(handle_, pattern); return *this; }
    Step& run(const char* cmd, std::initializer_list<const char*> args = {}) {
        std::vector<const char*> v(args);
        bake_step_run(handle_, cmd, v.data(), static_cast<int>(v.size()));
        return *this;
    }

    bake_step* raw() const { return handle_; }
};

inline Target& Target::depends_on(Step& step) {
    bake_target_depends_on_step(handle_, step.raw());
    return *this;
}

class Dependency {
    friend class Builder;
    bake_dependency* handle_ = nullptr;
public:
    Dependency() = default;
    explicit Dependency(bake_dependency* h) : handle_(h) {}

    const char* src_dir() const { return bake_dep_src_dir(handle_); }
    void link_to(Target& t) { bake_dep_link_to(handle_, t.raw()); }

    bake_dependency* raw() const { return handle_; }
};

inline Target& Target::sources(Dependency& dependency, const char* pattern) {
    bake_target_dependency_sources(handle_, dependency.raw(), pattern);
    return *this;
}

inline Target& Target::sources(
    Dependency& dependency, std::initializer_list<const char*> patterns) {
    for (const char* pattern : patterns)
        bake_target_dependency_sources(handle_, dependency.raw(), pattern);
    return *this;
}

inline Target& Target::include_dirs(Dependency& dependency, const char* dirs) {
    bake_target_dependency_include_dirs(handle_, dependency.raw(), dirs);
    return *this;
}

inline Target& Target::private_include_dirs(Dependency& dependency,
                                            const char* dirs) {
    bake_target_dependency_private_include_dirs(handle_, dependency.raw(), dirs);
    return *this;
}

class Builder {
    bake_builder* handle_ = nullptr;
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<std::unique_ptr<Step>> steps_;
    std::vector<std::unique_ptr<Dependency>> deps_;
public:
    Builder() : handle_(bake_builder_new()) {}

    ~Builder() {
        if (handle_) bake_builder_free(handle_);
    }

    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    Target& executable(const char* name) {
        auto* t = bake_builder_executable(handle_, name);
        targets_.push_back(std::make_unique<Target>(t));
        return *targets_.back();
    }

    Target& static_lib(const char* name) {
        auto* t = bake_builder_static_lib(handle_, name);
        targets_.push_back(std::make_unique<Target>(t));
        return *targets_.back();
    }

    Target& shared_lib(const char* name) {
        auto* t = bake_builder_shared_lib(handle_, name);
        targets_.push_back(std::make_unique<Target>(t));
        return *targets_.back();
    }

    Step& step(const char* name) {
        auto* s = bake_builder_step(handle_, name);
        steps_.push_back(std::make_unique<Step>(s));
        return *steps_.back();
    }

    Dependency& dependency(const char* name) {
        auto* d = bake_builder_dependency(handle_, name);
        deps_.push_back(std::make_unique<Dependency>(d));
        return *deps_.back();
    }

    bool option_bool(const char* name) const {
        return bake_builder_option_bool(handle_, name) != 0;
    }
    int64_t option_int(const char* name) const {
        return bake_builder_option_int(handle_, name);
    }
    const char* option_str(const char* name) const {
        return bake_builder_option_str(handle_, name);
    }
    const char* source_dir() const { return bake_builder_source_dir(handle_); }
    const char* build_dir() const { return bake_builder_build_dir(handle_); }

    int build() { return bake_builder_build(handle_); }

    bake_builder* raw() const { return handle_; }
};

} // namespace bake
