module;

#include <kota/async/runtime/task.h>

export module bake.buildsystem.declaration;

import std;
import bake.util;
import bake.json;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.cmdgen;
import bake.buildsystem.source;
import bake.core.runtime;
import bake.toolchain.target;
import bake.toolchain.runtime;

namespace json = bake::json;

namespace bake {
// ===== Default input declaration =====
//
// Discovers inputs from the default src/ and public/ layout.

export MoidDeclaration declare_default_inputs(
        const Manifest& manifest, const Layout& layout,
        const std::string& canonical_id,
        const std::vector<std::string>& active_features,
        const std::vector<MoidDependency>& dependencies) {
    MoidDeclaration declaration;
    declaration.id = canonical_id;
    declaration.name = manifest.moid->name;
    declaration.version = manifest.moid->version;
    declaration.type = manifest.moid->type;
    declaration.root = layout.root.absolute().string();
    declaration.cxx_std = manifest.moid->cxx_std;
    declaration.c_std = manifest.moid->c_std;
    declaration.active_features = active_features;
    declaration.dependencies = dependencies;

    const Path root = layout.root.absolute();
    auto source_pattern = [&](const Path& source) {
        std::error_code error;
        auto relative = std::filesystem::relative(
            source.fs(), root.fs(), error);
        if (!error && !relative.empty())
            return relative.lexically_normal().generic_string();
        return source.fs().lexically_normal().generic_string();
    };
    auto is_public_source = [&](const Path& source) {
        if (!layout.public_dir.is_directory()) return false;
        std::error_code error;
        auto relative = std::filesystem::relative(
            source.fs(), layout.public_dir.fs(), error);
        if (error || relative.empty()) return false;
        auto first = relative.begin();
        return first != relative.end() && *first != "..";
    };
    auto add_source = [&](const Path& source, bool is_public) {
        SourceGroup group;
        group.pattern = source_pattern(source);
        group.is_public = is_public;
        declaration.sources.push_back(std::move(group));
    };

    auto source_ext = manifest.resolve_source_ext();
    auto sources = discover_sources(layout.source_dir, layout.public_dir, source_ext);
    for (const auto& source : sources.cpp_files)
        add_source(source, false);
    for (const auto& source : sources.c_files)
        add_source(source, false);
    for (const auto& source : sources.module_interfaces)
        add_source(source, is_public_source(source));

    if (layout.public_dir.is_directory())
        declaration.public_include_dirs.push_back("public");

    for (const auto& [name, dependency] : manifest.dependencies) {
        if (!dependency.is_path_dep) continue;
        Path dependency_dir = layout.root / dependency.path;
        if ((dependency_dir / "bake.toml").is_regular_file()) continue;

        Path dependency_public = dependency_dir / "public";
        if (dependency_public.is_directory())
            declaration.public_include_dirs.push_back(
                source_pattern(dependency_public));

        Path dependency_source = dependency_dir / "src";
        if (!dependency_source.is_directory()) continue;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(dependency_source.fs())) {
            if (!entry.is_regular_file()) continue;
            Path source(entry.path());
            // Match against the configured extensions
            auto match_ext = [&source](const std::vector<std::string>& exts) {
                for (const auto& e : exts)
                    if (source.has_extension(e)) return true;
                return false;
            };
            if (match_ext(source_ext.source_ext) || match_ext(source_ext.module_ext))
                add_source(source, false);
        }
    }

    std::sort(declaration.sources.begin(), declaration.sources.end(),
              [](const SourceGroup& left, const SourceGroup& right) {
                  return left.pattern < right.pattern;
              });
    return declaration;
}

export bool refresh_default_input_sources(
        MoidDeclaration &declaration, const SourceExtConfig &extensions) {
    const Path root(declaration.root);
    const Layout layout = Layout::detect(root);
    auto within = [](const Path &path, const Path &directory) {
        std::error_code error;
        auto relative = std::filesystem::relative(
            path.fs(), directory.fs(), error);
        if (error || relative.empty())
            return false;
        auto first = relative.begin();
        return first != relative.end() && *first != "..";
    };
    auto source_pattern = [&](const Path &source) {
        std::error_code error;
        auto relative = std::filesystem::relative(
            source.fs(), root.fs(), error);
        return !error && !relative.empty()
                   ? relative.lexically_normal().generic_string()
                   : source.fs().lexically_normal().generic_string();
    };

    std::vector<SourceGroup> sources;
    for (const auto &existing : declaration.sources) {
        const Path path = root / existing.pattern;
        if (!within(path, layout.source_dir) &&
            !within(path, layout.public_dir))
            sources.push_back(existing);
    }
    auto discovered =
        discover_sources(layout.source_dir, layout.public_dir, extensions);
    auto append = [&](const Path &path, bool is_public) {
        sources.push_back(SourceGroup{source_pattern(path), is_public});
    };
    for (const auto &path : discovered.cpp_files)
        append(path, false);
    for (const auto &path : discovered.c_files)
        append(path, false);
    for (const auto &path : discovered.module_interfaces)
        append(path, within(path, layout.public_dir));
    std::ranges::sort(sources, {}, &SourceGroup::pattern);

    auto include_dirs = declaration.public_include_dirs;
    auto public_entry = std::ranges::find(include_dirs, "public");
    if (layout.public_dir.is_directory() &&
        public_entry == include_dirs.end())
        include_dirs.push_back("public");
    if (!layout.public_dir.is_directory() &&
        public_entry != include_dirs.end())
        include_dirs.erase(public_entry);
    std::ranges::sort(include_dirs);

    auto same_sources = [](const std::vector<SourceGroup> &left,
                           const std::vector<SourceGroup> &right) {
        if (left.size() != right.size())
            return false;
        for (std::size_t i = 0; i < left.size(); ++i)
            if (left[i].pattern != right[i].pattern ||
                left[i].is_public != right[i].is_public)
                return false;
        return true;
    };
    if (same_sources(declaration.sources, sources) &&
        declaration.public_include_dirs == include_dirs)
        return false;
    declaration.sources = std::move(sources);
    declaration.public_include_dirs = std::move(include_dirs);
    return true;
}


// ===== Project declaration preparation =====
//
// For each moid, obtain its input description from default discovery or
// build.cpp and merge the authoritative bake.toml configuration.

namespace {

Path find_bake_build_source() {
  // 1. Source-tree layout (dev builds): <ws>/lib/bake/bake.build.cppm
  Path ws = find_workspace_root();
  if (!ws.string().empty()) {
    Path p = ws / "lib" / "bake" / "bake.build.cppm";
    if (p.is_regular_file())
      return p;
  }
  // 2. Installed layout: <prefix>/lib/bake/bake.build.cppm
  Path exe(get_self_exe_path());
  if (!exe.string().empty()) {
    Path prefix = exe.parent().parent();
    Path installed = prefix / "lib" / "bake" / "bake.build.cppm";
    if (installed.is_regular_file())
      return installed;
  }
  return {};
}

// Serialise active feature names as repeated name-length:name records.
std::string serialize_features(const std::vector<std::string> &features) {
  std::string out;
  for (const auto &name : features)
    out += std::to_string(name.size()) + ":" + name;
  return out;
}

json::Value
declaration_features_json(const std::vector<std::string> &features) {
  return json::Value(features);
}

std::string
serialize_declaration_dependencies(const std::vector<MoidEdge> &edges) {
  json::Value dependencies = json::Value::array();
  for (const auto &edge : edges) {
    json::Value dep_features = json::Value::array();
    for (const auto &feature : edge.features)
      dep_features.push_back(feature);
    dependencies.push_back({
        {"alias", edge.alias},
        {"id", edge.target.value},
        {"features", dep_features},
    });
  }
  return dependencies.dump();
}
} // namespace

// ===== build.cpp → MoidDeclaration =====
//
// Compiles build.cpp + bake.build.cppm into a small executable. The script
// persists its declaration at BAKE_DECLARATION_PATH, which is then read through
// the same strict codec used for every input declaration.

struct BuildCppEvaluation {
  MoidDeclaration declaration;
  std::string stdout_data;
  std::string stderr_data;
};

std::string unique_work_key() {
  std::random_device random;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return SHA256::hex(std::format("{}:{}:{}", now, random(), random()))
      .substr(0, 24);
}

class ScopedWorkDirectory {
public:
  explicit ScopedWorkDirectory(Path path) : path_(std::move(path)) {
    path_.mkdir_recursive();
  }
  ~ScopedWorkDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_.fs(), error);
  }
  const Path &path() const { return path_; }

private:
  Path path_;
};

kota::task<RuntimeResult> run_declaration_process(
    CoreRuntime &runtime, std::string file, std::vector<std::string> args,
    const Path &cwd,
    std::vector<EnvironmentOverride> environment = {}) {
  ProcessSpec process;
  process.file = std::move(file);
  process.args = std::move(args);
  process.cwd = cwd.string();
  process.environment = std::move(environment);
  auto request = runtime.submit_process(
      std::move(process), {.lane = RuntimeLane::foreground});
  co_return co_await request.wait();
}

kota::task<std::expected<BuildCppEvaluation, std::string>>
compile_and_run_build_cpp(CoreRuntime &runtime, const Path &moid_dir,
                          const Manifest &manifest, const MoidNode &node,
                          const TargetSpec &target, const Path &out_dir,
                          const Path &compiler_executable) {
  const std::string identity_key = SHA256::hex(node.id.value).substr(0, 24);
  const TargetSpec native_target = detect_host_target();
  ScopedWorkDirectory work(
      out_dir / ".bake" / "work" / "declaration" /
      (identity_key + "-" + unique_work_key()));
  const Path scripts_dir = work.path();

  const Path wrapper_src = find_bake_build_source();
  if (wrapper_src.string().empty())
    co_return std::unexpected("cannot find bake.build.cppm");

  const auto cache_info = bake_build_cache_info(native_target, wrapper_src);
  const Path build_cache_dir = cache_info.dir / "bake.build";
  build_cache_dir.mkdir_recursive();

  const Path wrapper_dst = build_cache_dir / "bake.build.cppm";
  auto wrapper_content = read_file(wrapper_src);
  if (!wrapper_content)
    co_return std::unexpected("failed to read bake.build.cppm");
  if (!atomic_write_file(wrapper_dst, *wrapper_content))
    co_return std::unexpected("failed to stage bake.build.cppm");

  const Path pcm = build_cache_dir / "bake.build.pcm";
  const Path wrapper_o = build_cache_dir / "bake.build.o";

  if (!pcm.is_regular_file() || !wrapper_o.is_regular_file()) {
    const std::string suffix = unique_work_key();
    const Path tmp_pcm(pcm.string() + "." + suffix + ".tmp");
    const Path tmp_o(wrapper_o.string() + "." + suffix + ".tmp");
    std::vector<std::string> args{
        compiler_executable.string(), "c++", "-c", "-std=c++23",
        "-stdlib=libc++",
        "-Wno-reserved-module-identifier", "-x", "c++-module",
        "-I" + build_cache_dir.string(),
        "-fmodule-output=" + tmp_pcm.string(), wrapper_dst.string(),
        "-o", tmp_o.string(),
    };
    auto result = co_await run_declaration_process(
        runtime, compiler_executable.string(), std::move(args), moid_dir);
    if (!result.succeeded()) {
      tmp_pcm.remove();
      tmp_o.remove();
      co_return std::unexpected(
          result.stderr_data.empty() ? result.error : result.stderr_data);
    }

    std::error_code error;
    std::filesystem::rename(tmp_pcm.fs(), pcm.fs(), error);
    if (error && !pcm.is_regular_file()) {
      tmp_o.remove();
      co_return std::unexpected("failed to publish bake.build.pcm: " +
                                error.message());
    }
    error.clear();
    std::filesystem::rename(tmp_o.fs(), wrapper_o.fs(), error);
    if (error && !wrapper_o.is_regular_file())
      co_return std::unexpected("failed to publish bake.build.o: " +
                                error.message());
  }

  const Path build_cpp = moid_dir / "build.cpp";
  const Path build_o = scripts_dir / "build.o";
  {
    std::vector<std::string> args{
        compiler_executable.string(), "c++", "-c", "-std=c++23",
        "-stdlib=libc++",
        "-Wno-reserved-module-identifier", "-I" + build_cache_dir.string(),
        "-fmodule-file=bake.build=" + pcm.string(), build_cpp.string(),
        "-o", build_o.string(),
    };
    auto result = co_await run_declaration_process(
        runtime, compiler_executable.string(), std::move(args), moid_dir);
    if (!result.succeeded())
      co_return std::unexpected(
          result.stderr_data.empty() ? result.error : result.stderr_data);
  }

  const Path build_app = scripts_dir / "build_app";
  {
    std::vector<std::string> args{
        compiler_executable.string(), "c++", wrapper_o.string(),
        build_o.string(),
        "-o", build_app.string(),
    };
    auto result = co_await run_declaration_process(
        runtime, compiler_executable.string(), std::move(args), moid_dir);
    if (!result.succeeded())
      co_return std::unexpected(
          result.stderr_data.empty() ? result.error : result.stderr_data);
  }

  const auto &features = node.declaration.active_features;
  const std::string features_str = serialize_features(features);
  const std::string declaration_features =
      declaration_features_json(features).dump();
  const std::string declaration_dependencies =
      serialize_declaration_dependencies(node.dependencies);
  std::string deps_str;
  for (const auto &[dep_name, dep] : manifest.dependencies) {
    if (!dep.is_path_dep) continue;
    const Path dep_dir = (moid_dir / dep.path).lexically_normal();
    deps_str += dep_name + "=" + dep_dir.absolute().string() + "\n";
  }
  for (const auto &[alias, dir] : node.source_deps)
    deps_str += alias + "=" + dir + "\n";

  const Path declaration_path = scripts_dir / "declaration.json";

  std::vector<EnvironmentOverride> environment{
      {"BAKE_SOURCE_DIR", node.declaration.root},
      {"BAKE_BUILD_DIR", out_dir.absolute().string()},
      {"BAKE_MOID_ID", node.id.value},
      {"BAKE_MOID_NAME", manifest.moid->name},
      {"BAKE_MOID_VERSION", manifest.moid->version},
      {"BAKE_MOID_TYPE", std::string(moid_type_str(manifest.moid->type))},
      {"BAKE_MOID_CXX_STD", manifest.moid->cxx_std},
      {"BAKE_MOID_C_STD", manifest.moid->c_std},
      {"BAKE_DECLARATION_PATH", declaration_path.string()},
      {"BAKE_FEATURES", features_str},
      {"BAKE_DECLARATION_FEATURES", declaration_features},
      {"BAKE_DECLARATION_DEPENDENCIES", declaration_dependencies},
      {"BAKE_DEPS", deps_str},
      {"BAKE_EXE", compiler_executable.string()},
      {"BAKE_TARGET", target.is_native()
                          ? detect_host_target().triple_with_version()
                          : target.triple_with_version()},
  };
  auto result = co_await run_declaration_process(
      runtime, build_app.string(), {build_app.string()}, moid_dir,
      std::move(environment));
  if (!result.succeeded())
    co_return std::unexpected(
        result.stderr_data.empty() ? result.error : result.stderr_data);

  auto declaration = read_moid_declaration(declaration_path);
  if (!declaration)
    co_return std::unexpected(declaration.error());
  co_return BuildCppEvaluation{
      std::move(*declaration), std::move(result.stdout_data),
      std::move(result.stderr_data)};
}
namespace {

// Merge bake.toml configuration (profile, target conditions, options, link)
// into a declaration. Called after obtaining the input description from
// either default discovery or build.cpp.
void merge_manifest_config(MoidDeclaration &decl, const Manifest &manifest,
                           const TargetSpec& target,
                           const std::string &profile_name) {
  // Language standards — manifest is the sole source of truth.
  decl.cxx_std = manifest.moid->cxx_std;
  decl.c_std = manifest.moid->c_std;

  // Profile → flags / link_flags / defines
  auto profile = manifest.resolve_profile(profile_name);
  bool is_release = (profile_name == "release");
  auto resolved = resolve_profile_flags(profile, is_release, target);

  decl.compile_flags = resolved.compile_flags;
  decl.link_flags = resolved.link_flags;

  // Target conditions (specificity-sorted: broad first, exact last)
  std::string current_triple =
      target.is_native() ? detect_host_target().triple_ : target.triple_;

  struct TargetMatch {
    const TargetCondition *cond;
    int wc;
  };
  std::vector<TargetMatch> matches;
  for (const auto &tgt : manifest.targets)
    if (triple_matches(current_triple, tgt.triple_pattern))
      matches.push_back(
          {&tgt, static_cast<int>(std::count(tgt.triple_pattern.begin(),
                                             tgt.triple_pattern.end(), '*'))});

  std::sort(
      matches.begin(), matches.end(),
      [](const TargetMatch &a, const TargetMatch &b) { return a.wc > b.wc; });

  std::vector<std::string> tgt_flags, tgt_libs, tgt_frameworks, tgt_defines,
      tgt_public_defines, tgt_includes;
  for (const auto &match : matches) {
    for (auto &f : match.cond->flags)
      tgt_flags.push_back(f);
    for (auto &l : match.cond->libraries)
      tgt_libs.push_back(l);
    for (auto &d : match.cond->defines)
      tgt_defines.push_back(d);
    for (auto &d : match.cond->public_defines)
      tgt_public_defines.push_back(d);
    for (auto &i : match.cond->include_dirs)
      tgt_includes.push_back(i);
    if (target.is_darwin())
      for (auto &f : match.cond->frameworks)
        tgt_frameworks.push_back(f);
  }
  for (auto &f : tgt_flags)
    decl.compile_flags.push_back(f);

  // Defines: auto macros + feature/target defines, split by visibility.
  // Everything applies to this moid's own translation units; only
  // `public_defines` entries and the informational BAKE_* auto macros
  // propagate to consumers (public_compile_defines → CompileUsage).
  decl.compile_defines.clear();
  decl.public_compile_defines.clear();
  auto add_define =
      [](std::vector<std::pair<std::string, std::string>> &out,
         const std::string &raw) {
          auto equals = raw.find('=');
          out.push_back(equals == std::string::npos
                            ? std::pair{raw, std::string{}}
                            : std::pair{raw.substr(0, equals),
                                        raw.substr(equals + 1)});
      };
  for (auto &[k, v] : generate_feature_macros(decl.name, manifest.features,
                                              decl.active_features)) {
    decl.compile_defines.push_back({k, v});
    decl.public_compile_defines.push_back({k, v});
  }
  for (const auto& feature_name : decl.active_features) {
    auto feature = manifest.features.find(feature_name);
    if (feature == manifest.features.end()) continue;
    for (const auto& define : feature->second.defines)
      add_define(decl.compile_defines, define);
    for (const auto& define : feature->second.public_defines) {
      add_define(decl.compile_defines, define);
      add_define(decl.public_compile_defines, define);
    }
  }
  for (auto &[k, v] : generate_package_macros(decl.name, decl.version)) {
    decl.compile_defines.push_back({k, v});
    decl.public_compile_defines.push_back({k, v});
  }
  // Profile defines (e.g. NDEBUG) describe this moid's own compile mode.
  for (auto &[k, v] : resolved.defines)
    decl.compile_defines.push_back({k, v});
  for (auto &d : tgt_defines)
    add_define(decl.compile_defines, d);
  for (auto &d : tgt_public_defines) {
    add_define(decl.compile_defines, d);
    add_define(decl.public_compile_defines, d);
  }

  for (auto &i : tgt_includes)
    decl.extra_include_dirs.push_back(i);

  // Libraries + frameworks: [link] + matching [target.*]
  decl.libraries.clear();
  decl.frameworks.clear();
  if (manifest.link) {
    for (auto &l : manifest.link->libraries)
      decl.libraries.push_back(l);
    for (auto &f : manifest.link->frameworks)
      decl.frameworks.push_back(f);
  }
  for (auto &l : tgt_libs)
    decl.libraries.push_back(l);
  for (auto &f : tgt_frameworks)
    decl.frameworks.push_back(f);
}

std::expected<void, std::string>
validate_resolved_declaration(const MoidDeclaration &declaration,
                              const MoidNode &node) {
  // The manifest is authoritative for type and name.
  // build.cpp cannot override these.
  if (declaration.type != node.declaration.type) {
    return std::unexpected("build.cpp declared type '" +
                           std::string(moid_type_str(declaration.type)) +
                           "' but bake.toml declares type '" +
                           std::string(moid_type_str(node.declaration.type)) +
                           "' for moid '" + node.declaration.name + "'");
  }
  if (declaration.name != node.declaration.name) {
    return std::unexpected("build.cpp declared name '" + declaration.name +
                           "' but bake.toml declares name '" +
                           node.declaration.name + "' for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.id != node.id.value) {
    return std::unexpected("moid declaration field 'id' does not match "
                           "resolved identity for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.root != node.declaration.root) {
    return std::unexpected("moid declaration field 'root' does not match "
                           "resolved root for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.version != node.declaration.version) {
    return std::unexpected("moid declaration field 'version' does not match "
                           "resolved version for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.active_features != node.declaration.active_features) {
    return std::unexpected("moid declaration field 'features' does not match "
                           "resolved features for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.dependencies.size() != node.declaration.dependencies.size()) {
    return std::unexpected("moid declaration field 'dependencies' does not "
                           "match resolved dependencies for moid '" +
                           node.declaration.name + "'");
  }
  for (std::size_t i = 0; i < declaration.dependencies.size(); ++i) {
    const auto &actual = declaration.dependencies[i];
    const auto &expected = node.declaration.dependencies[i];
    if (actual.alias != expected.alias || actual.id != expected.id ||
        actual.features != expected.features) {
      return std::unexpected("moid declaration field 'dependencies' does not "
                             "match resolved dependencies for moid '" +
                             node.declaration.name + "'");
    }
  }
  return {};
}

} // namespace

export struct DeclarationPreparation {
  std::size_t prepared = 0;
  std::size_t evaluated = 0;
  std::string stdout_data;
  std::string stderr_data;
};

export std::vector<std::string>
declaration_warnings(const MoidGraph &graph) {
  std::vector<std::string> warnings;
  for (const auto &[id, node] : graph.nodes) {
    (void)id;
    for (const auto &[alias, dir] : node.source_deps) {
      (void)dir;
      if (std::ranges::find(node.declaration.used_source_deps, alias) !=
          node.declaration.used_source_deps.end())
        continue;
      warnings.push_back(
          "'" + node.declaration.name + "' declares source dependency '" +
          alias + "' that is never consumed (query it in build.cpp via "
                  "dep_src_dir)");
    }
  }
  return warnings;
}

export kota::task<std::expected<DeclarationPreparation, std::string>>
prepare_moid_declarations(CoreRuntime &runtime, MoidGraph &graph,
                          const TargetSpec &target, const Path &out_dir,
                          const std::string &profile_name,
                          const Path &compiler_executable) {
  auto topology = configuration_topological_moids(graph);
  if (!topology)
    co_return std::unexpected(topology.error());

  (out_dir / ".bake").mkdir_recursive();
  DeclarationPreparation preparation;

  for (const auto &id : *topology) {
    auto &node = graph.nodes.at(id);
    const Path moid_dir(node.declaration.root);
    auto manifest = Manifest::load(moid_dir);
    if (!manifest || !manifest->has_moid()) {
      co_return std::unexpected("failed to load manifest for moid '" +
                                node.declaration.name + "'");
    }

    MoidDeclaration declaration;
    if ((moid_dir / "build.cpp").is_regular_file()) {
      ++preparation.prepared;
      auto evaluated = co_await compile_and_run_build_cpp(
          runtime, moid_dir, *manifest, node, target, out_dir,
          compiler_executable);
      if (!evaluated)
        co_return std::unexpected(evaluated.error());
      declaration = std::move(evaluated->declaration);
      preparation.stdout_data += evaluated->stdout_data;
      preparation.stderr_data += evaluated->stderr_data;
      ++preparation.evaluated;
    } else {
      const auto layout = Layout::detect(moid_dir);
      declaration = declare_default_inputs(
          *manifest, layout, node.id.value,
          node.declaration.active_features, node.declaration.dependencies);
    }

    if ((moid_dir / "build.cpp").is_regular_file() &&
        declaration.sources.empty() &&
        declaration.public_include_dirs.empty()) {
      const auto layout = Layout::detect(moid_dir);
      auto discovered = declare_default_inputs(
          *manifest, layout, node.id.value,
          node.declaration.active_features, node.declaration.dependencies);
      declaration.sources = std::move(discovered.sources);
      declaration.public_include_dirs =
          std::move(discovered.public_include_dirs);
    }

    merge_manifest_config(declaration, *manifest, target, profile_name);
    auto validated = validate_resolved_declaration(declaration, node);
    if (!validated)
      co_return std::unexpected(validated.error());

    node.declaration = std::move(declaration);
    if (node.declaration.type != MoidType::Executable)
      continue;
    for (const auto &[_, consumer] : graph.nodes) {
      for (const auto &edge : consumer.dependencies) {
        if (edge.target != id)
          continue;
        co_return std::unexpected(
            "moid '" + consumer.declaration.name +
            "' cannot use executable moid '" + node.declaration.name +
            "' as a normal dependency");
      }
    }
  }

  co_return preparation;
}

} // namespace bake
