module;

#include <kota/async/runtime/task.h>

export module bake.core.project;

import std;
import bake.util;
import bake.core.model;
import bake.core.runtime;
import bake.core.state;
import bake.core.snapshot;
import bake.buildsystem.project;
import bake.buildsystem.package;
import bake.buildsystem.graph;
import bake.buildsystem.declaration;
import bake.buildsystem.plan;
import bake.buildsystem.source;
import bake.toolchain.target;
import bake.toolchain.runtime;

namespace bake {

export struct ProjectRequest {
  Path root;
  std::optional<std::string> member;
  std::vector<std::string> features;
  std::string profile = "dev";
  std::optional<std::string> target_triple;
  bool locked = false;
  bool offline = false;
  Path compiler_executable;
  std::string compiler_version;
};

export struct ProjectMetrics {
  std::size_t dependency_resolutions = 0;
  std::size_t declaration_preparations = 0;
  std::size_t declaration_evaluations = 0;
  std::size_t source_updates = 0;
  std::size_t full_graph_lowerings = 0;
  std::size_t input_checks = 0;
  std::size_t artifact_tasks = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_hashed = 0;
  std::chrono::nanoseconds elapsed{};
};

export std::string format_project_metrics(const ProjectMetrics &metrics) {
  return std::format(
      "resolve={} declaration_prepare={} declaration_evaluate={} "
      "full_lower={} source_update={} input_checks={} artifact_tasks={} "
      "bytes_read={} bytes_hashed={} elapsed_ms={:.3f}",
      metrics.dependency_resolutions, metrics.declaration_preparations,
      metrics.declaration_evaluations, metrics.full_graph_lowerings,
      metrics.source_updates, metrics.input_checks, metrics.artifact_tasks,
      metrics.bytes_read, metrics.bytes_hashed,
      std::chrono::duration<double, std::milli>(metrics.elapsed).count());
}

export enum class ProjectOpenErrorCode {
  invalid_request,
  lock_unavailable,
  dependency_resolution,
  declaration_preparation,
  graph_lowering,
  state,
};

export enum class ProjectOpenState { failed };

export struct ProjectOpenError {
  ProjectOpenErrorCode code = ProjectOpenErrorCode::state;
  ProjectOpenState state = ProjectOpenState::failed;
  std::string message;
  std::optional<ProjectSnapshotId> previous_snapshot;
};

export struct ProjectHandle {
  ProjectContext context;
  ContextPaths paths;
  ProjectSnapshotId snapshot_id;
  std::shared_ptr<const ProjectSnapshot> snapshot;
  ProjectMetrics metrics;
  std::string declaration_stdout;
  std::string declaration_stderr;
};

namespace {

ProjectOpenError project_error(ProjectOpenErrorCode code, std::string message,
                               std::optional<ProjectSnapshotId> previous = {}) {
  return ProjectOpenError{code, ProjectOpenState::failed, std::move(message),
                          std::move(previous)};
}

struct ProjectOpenStart {
  Path root;
  Manifest manifest;
  std::optional<std::string> member;
  ProjectContext context;
  ProjectContextKey key;
  ContextPaths paths;
  std::optional<SuccessfulObservation> latest;
  std::optional<ProjectSnapshot> snapshot;
};

std::expected<ProjectOpenStart, std::string>
prepare_project_open(const ProjectRequest &request) {
  auto discovered_root = find_project_root(request.root);
  if (!discovered_root)
    return std::unexpected("no bake.toml found from " + request.root.string());
  std::error_code error;
  Path root(std::filesystem::canonical(discovered_root->fs(), error));
  if (error)
    return std::unexpected("failed to canonicalize project root: " +
                           discovered_root->string());
  auto manifest = Manifest::load(root);
  if (!manifest)
    return std::unexpected("failed to load bake.toml at " + root.string());
  manifest->project_dir = root;
  auto member = resolve_workspace_member_selection(*manifest, request.member);
  if (!member)
    return std::unexpected(member.error());
  std::optional<Path> member_root;
  if (*member)
    member_root = root / **member;
  if (request.target_triple &&
      std::count(request.target_triple->begin(), request.target_triple->end(),
                 '-') != 2) {
    return std::unexpected("invalid target triple '" + *request.target_triple +
                           "': expected exactly 3 segments (arch-os-libc)");
  }
  TargetSpec target = request.target_triple
                          ? parse_target(*request.target_triple)
                          : detect_host_target();
  auto compiler =
      identify_compiler(request.compiler_executable, request.compiler_version,
                        get_toolchain_cache_root() / ".identity");
  if (!compiler)
    return std::unexpected(compiler.error().message);
  auto context =
      make_project_context(root, member_root, request.features, request.profile,
                           std::move(target), std::move(*compiler));
  if (!context)
    return std::unexpected(context.error().message);
  auto key = context_key(*context);
  auto paths = context_paths(*context);
  if (!key || !paths)
    return std::unexpected(key ? paths.error().message : key.error().message);

  ProjectStateStore state(*paths, *key);
  std::optional<SuccessfulObservation> latest;
  std::optional<ProjectSnapshot> snapshot;
  auto observation = state.latest_successful_observation();
  if (observation) {
    latest = *observation;
    auto document =
        state.load_snapshot(observation->snapshot, observation->validation_id);
    if (document) {
      auto decoded = project_snapshot_from_json(document->payload, *key);
      if (decoded)
        snapshot = std::move(*decoded);
    }
  }
  return ProjectOpenStart{std::move(root),    std::move(*manifest),
                          std::move(*member), std::move(*context),
                          std::move(*key),    std::move(*paths),
                          std::move(latest),  std::move(snapshot)};
}

template <typename T, typename Work>
kota::task<std::expected<T, std::string>> run_project_work(CoreRuntime &runtime,
                                                           Work work) {
  auto request = runtime.submit_blocking(
      [work = std::move(work)](const CancellationToken &cancellation) mutable {
        BlockingResult result;
        if (cancellation.cancellation_requested()) {
          result.ok = false;
          result.error = "project work cancelled before start";
          return result;
        }
        auto value = work();
        if (!value) {
          result.ok = false;
          result.error = value.error();
          return result;
        }
        result.value = std::move(*value);
        return result;
      },
      {.lane = RuntimeLane::foreground});
  auto result = co_await request.wait();
  if (!result.succeeded())
    co_return std::unexpected(result.error.empty() ? "project work failed"
                                                   : result.error);
  try {
    co_return std::any_cast<T>(std::move(result.value));
  } catch (const std::bad_any_cast &) {
    co_return std::unexpected("project work returned an invalid value");
  }
}

std::string declaration_validation_id(const InputDeclarationRecord &record) {
  std::string value = "bake-declaration-inputs-v1\n" + record.context.value;
  for (const auto &input : record.inputs) {
    value += "\n" + input.canonical_path.string() + "\n";
    value += input.exists ? "1\n" : "0\n";
    value += std::to_string(input.mtime);
  }
  return SHA256::hex(value);
}

std::expected<InputIdentity, std::string>
read_identity(const Path &path, ProjectMetrics &metrics) {
  ++metrics.input_checks;
  auto content = read_file(path);
  if (!content)
    return std::unexpected("failed to read project input '" + path.string() +
                           "'");
  metrics.bytes_read += content->size();
  metrics.bytes_hashed += content->size();
  return InputIdentity{path.absolute().lexically_normal(),
                       SHA256::hex(*content),
                       static_cast<std::uintmax_t>(content->size())};
}

std::expected<SourceGraphSnapshot, std::string>
source_graph_from_build(const BuildGraph &graph, ProjectMetrics &metrics) {
  SourceGraphSnapshot source_graph;
  HeaderClosureScanner scanner;
  for (const auto &action : graph.actions) {
    if (!action.is_compile() || action.inputs.empty())
      continue;
    auto source = read_identity(action.inputs.front(), metrics);
    if (!source)
      return std::unexpected(source.error());

    SourceUnitRecord unit;
    unit.id = CompilationUnitId{action.id};
    unit.owner_moid = action.moid_id;
    unit.source = std::move(*source);
    std::vector<Path> include_dirs;
    for (std::size_t i = 0; i < action.command.size(); ++i) {
      const auto &argument = action.command[i];
      if (argument.starts_with("-I") && argument.size() > 2)
        include_dirs.emplace_back(argument.substr(2));
      else if ((argument == "-I" || argument == "-isystem" ||
                argument == "-iquote") &&
               i + 1 < action.command.size())
        include_dirs.emplace_back(action.command[++i]);
    }
    for (const auto &header_path :
         scanner.closure(unit.source.path, include_dirs, action.command)) {
      auto header = read_identity(header_path, metrics);
      if (!header)
        return std::unexpected(header.error());
      unit.headers.push_back(std::move(*header));
    }
    if (auto module = scan_module_file(unit.source.path)) {
      if (!module->module_name.empty())
        unit.provides_module = module->module_name;
      unit.imports = std::move(module->imports);
    }
    source_graph.units.push_back(std::move(unit));
  }
  std::ranges::sort(source_graph.units, {},
                    [](const SourceUnitRecord &unit) { return unit.id.value; });
  return source_graph;
}

std::expected<bool, std::string>
refresh_default_declarations(MoidGraph &graph,
                             const std::vector<ManifestRecord> &manifests) {
  bool changed = false;
  for (auto &[_, node] : graph.nodes) {
    const Path root(node.declaration.root);
    if ((root / "build.cpp").is_regular_file())
      continue;
    const Path manifest_path = root / "bake.toml";
    auto record =
        std::ranges::find_if(manifests, [&](const ManifestRecord &candidate) {
          return candidate.path == manifest_path;
        });
    if (record == manifests.end() || !record->source_extensions)
      return std::unexpected(
          "snapshot lacks default source discovery facts for moid '" +
          node.declaration.name + "'");
    changed = refresh_default_input_sources(node.declaration,
                                            *record->source_extensions) ||
              changed;
  }
  return changed;
}

std::vector<CompilationDescription>
compilations_from_build(const BuildGraph &graph,
                        const ProjectContextKey &context) {
  std::vector<CompilationDescription> descriptions;
  for (const auto &action : graph.actions) {
    if (!action.is_compile() || action.inputs.empty())
      continue;
    CompilationDescription description;
    description.id = CompilationUnitId{action.id};
    description.context = context;
    description.owner_moid = action.moid_id;
    description.language = action.type == BuildAction::Type::CompileModule
                               ? SourceLanguage::module_interface
                           : action.inputs.front().has_extension(".c")
                               ? SourceLanguage::c
                               : SourceLanguage::cxx;
    description.working_directory = graph.project_root;
    description.source = action.inputs.front().absolute().lexically_normal();
    if (!action.outputs.empty())
      description.planned_output = action.outputs.front();
    if (action.command.size() > 1)
      description.driver_arguments.assign(action.command.begin() + 1,
                                          action.command.end());
    for (const auto &argument : action.command) {
      constexpr std::string_view prefix = "-fmodule-file=";
      if (!argument.starts_with(prefix))
        continue;
      const auto equals = argument.find('=', prefix.size());
      if (equals == std::string::npos)
        continue;
      description.module_references.push_back(ModuleReference{
          argument.substr(prefix.size(), equals - prefix.size()),
          Path(argument.substr(equals + 1))});
    }
    descriptions.push_back(std::move(description));
  }
  std::ranges::sort(descriptions, {}, [](const CompilationDescription &item) {
    return item.id.value;
  });
  return descriptions;
}

std::vector<Path> manifest_paths(const ProjectSnapshot &snapshot) {
  std::vector<Path> paths;
  paths.reserve(snapshot.manifests.size());
  for (const auto &manifest : snapshot.manifests)
    paths.push_back(manifest.path);
  return paths;
}

std::vector<Path> build_script_paths(const ProjectSnapshot &snapshot) {
  std::vector<Path> paths;
  paths.reserve(snapshot.moid_graph.nodes.size());
  for (const auto &[_, node] : snapshot.moid_graph.nodes)
    paths.push_back(Path(node.declaration.root) / "build.cpp");
  return paths;
}

std::expected<InputDeclarationRecord, SnapshotError>
observe_snapshot_declarations(const ProjectSnapshot &snapshot,
                              const Path &workspace_lock) {
  auto key = context_key(snapshot.context);
  if (!key)
    return std::unexpected(SnapshotError{SnapshotErrorCode::context_mismatch,
                                         "context", key.error().message});
  return observe_declaration_inputs(*key, manifest_paths(snapshot),
                                    workspace_lock,
                                    build_script_paths(snapshot));
}

bool same_source_inventory(const SourceGraphSnapshot &left,
                           const SourceGraphSnapshot &right) {
  if (left.units.size() != right.units.size())
    return false;
  for (std::size_t i = 0; i < left.units.size(); ++i) {
    const auto &a = left.units[i];
    const auto &b = right.units[i];
    if (a.id != b.id || a.owner_moid != b.owner_moid ||
        a.source.path != b.source.path ||
        a.provides_module != b.provides_module || a.imports != b.imports ||
        a.headers.size() != b.headers.size())
      return false;
    for (std::size_t j = 0; j < a.headers.size(); ++j)
      if (a.headers[j].path != b.headers[j].path)
        return false;
  }
  return true;
}

bool same_source_content(const SourceGraphSnapshot &left,
                         const SourceGraphSnapshot &right) {
  if (!same_source_inventory(left, right))
    return false;
  for (std::size_t i = 0; i < left.units.size(); ++i) {
    if (!(left.units[i].source == right.units[i].source) ||
        left.units[i].headers != right.units[i].headers)
      return false;
  }
  return true;
}

std::vector<ManifestRecord> manifest_records(const Manifest &root,
                                             const MoidGraph &graph) {
  std::vector<ManifestRecord> records;
  ManifestRecord root_record;
  root_record.path = root.project_dir / "bake.toml";
  if (root.has_moid() && !graph.roots.empty())
    root_record.moid_id = graph.roots.front().value;
  if (root.has_moid())
    root_record.source_extensions = root.resolve_source_ext();
  if (root.workspace)
    root_record.workspace_members = root.workspace->members;
  records.push_back(std::move(root_record));
  for (const auto &[id, node] : graph.nodes) {
    const Path path = Path(node.declaration.root) / "bake.toml";
    if (path == records.front().path)
      continue;
    auto manifest = Manifest::load(Path(node.declaration.root));
    records.push_back(
        ManifestRecord{path,
                       id.value,
                       {},
                       manifest ? std::optional(manifest->resolve_source_ext())
                                : std::nullopt});
  }
  std::ranges::sort(records, {}, [](const ManifestRecord &record) {
    return record.path.string();
  });
  return records;
}

struct PreparedSources {
  SourceIndex index;
  std::size_t resolutions = 0;
};

std::expected<PreparedSources, std::string>
prepare_source_index(const Manifest &manifest, const Path &root,
                     const ProjectRequest &request) {
  if (Manifest::has_only_path_deps(manifest))
    return PreparedSources{};

  const Path lock_path = root / "bake.lock";
  auto lockfile = Lockfile::load(lock_path);
  const bool stale = !lockfile || !lockfile->is_consistent(manifest, root);
  if (request.locked) {
    if (stale)
      return std::unexpected("lock file missing or stale (--locked)");
    if (!verify_lock_cache(*lockfile, get_cache_dir()))
      return std::unexpected("cache verification failed for locked sources");
    return PreparedSources{source_index_from_lock(root), 0};
  }
  if (request.offline) {
    if (lockfile && !verify_lock_cache(*lockfile, get_cache_dir()))
      return std::unexpected("cache verification failed for offline sources");
    return PreparedSources{source_index_from_lock(root), 0};
  }

  Resolver resolver;
  if (!stale && lockfile) {
    if (!verify_lock_cache(*lockfile, get_cache_dir())) {
      if (!resolver.redownload(*lockfile, ResolverConfig{}) ||
          !verify_lock_cache(*lockfile, get_cache_dir())) {
        return std::unexpected(
            "failed to restore sources from the current lock");
      }
    }
    return PreparedSources{source_index_from_lock(root), 0};
  }

  std::set<std::string> features(request.features.begin(),
                                 request.features.end());
  auto resolved = resolver.resolve(manifest, ResolverConfig{},
                                   lockfile ? &*lockfile : nullptr, features);
  if (!resolved)
    return std::unexpected("failed to resolve dependencies");
  if (!resolved->save(lock_path))
    return std::unexpected("failed to persist bake.lock");
  return PreparedSources{source_index_from_lock(root), 1};
}

std::expected<bool, std::string>
validate_cached_sources(const ProjectSnapshot &snapshot, const Path &root,
                        const ProjectRequest &request) {
  if (snapshot.source_index.entries.empty())
    return false;
  auto lockfile = Lockfile::load(root / "bake.lock");
  if (!lockfile)
    return std::unexpected("project snapshot requires a missing bake.lock");
  if (verify_lock_cache(*lockfile, get_cache_dir()))
    return false;
  if (request.locked || request.offline)
    return std::unexpected("cached locked sources are missing or corrupted");
  Resolver resolver;
  if (!resolver.redownload(*lockfile, ResolverConfig{}) ||
      !verify_lock_cache(*lockfile, get_cache_dir()))
    return std::unexpected("failed to restore sources from the current lock");
  return true;
}

} // namespace

export class ProjectCore {
public:
  explicit ProjectCore(CoreRuntime &runtime) : runtime_(runtime) {}

  kota::task<std::expected<ProjectHandle, ProjectOpenError>>
  open(ProjectRequest request);

private:
  CoreRuntime &runtime_;
};

kota::task<std::expected<ProjectHandle, ProjectOpenError>>
ProjectCore::open(ProjectRequest request) {
  const auto started = std::chrono::steady_clock::now();
  ProjectMetrics metrics;
  auto prepared = co_await run_project_work<ProjectOpenStart>(
      runtime_, [&] { return prepare_project_open(request); });
  if (!prepared)
    co_return std::unexpected(
        project_error(ProjectOpenErrorCode::invalid_request, prepared.error()));
  const Path root = std::move(prepared->root);
  auto manifest = std::move(prepared->manifest);
  auto member = std::move(prepared->member);
  auto context = std::move(prepared->context);
  auto key = std::move(prepared->key);
  auto paths = std::move(prepared->paths);
  auto latest = std::move(prepared->latest);
  auto decoded = std::move(prepared->snapshot);
  ProjectStateStore state(paths, key);
  std::optional<ProjectSnapshotId> previous =
      latest ? std::optional(latest->snapshot) : std::nullopt;
  if (latest && decoded) {
    auto declaration_inputs =
        observe_snapshot_declarations(*decoded, root / "bake.lock");
    ++metrics.input_checks;
    if (declaration_inputs &&
        declaration_inputs_match(decoded->declaration_inputs,
                                 *declaration_inputs)) {
      auto cache_repair = co_await run_project_work<bool>(runtime_, [&] {
        return validate_cached_sources(*decoded, root, request);
      });
      if (!cache_repair) {
        co_return std::unexpected(
            project_error(ProjectOpenErrorCode::lock_unavailable,
                          cache_repair.error(), previous));
      }
      if (*cache_repair)
        ++metrics.artifact_tasks;

      auto defaults_changed = co_await run_project_work<bool>(runtime_, [&] {
        return refresh_default_declarations(decoded->moid_graph,
                                            decoded->manifests);
      });
      if (!defaults_changed) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state, defaults_changed.error(), previous));
      }

      if (*defaults_changed) {
        auto lowered = co_await run_project_work<BuildGraph>(runtime_, [&] {
          return build_graph(decoded->moid_graph, context.target, paths.root,
                             root);
        });
        ++metrics.full_graph_lowerings;
        if (!lowered) {
          co_return std::unexpected(project_error(
              ProjectOpenErrorCode::graph_lowering, lowered.error(), previous));
        }
        decoded->build_graph = std::move(*lowered);
        decoded->compilations =
            compilations_from_build(decoded->build_graph, key);
      }

      auto current_sources =
          co_await run_project_work<SourceGraphSnapshot>(runtime_, [&] {
            return source_graph_from_build(decoded->build_graph, metrics);
          });
      if (!current_sources) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state, current_sources.error(), previous));
      }

      const bool structure_changed =
          !*defaults_changed &&
          !same_source_inventory(decoded->source_graph, *current_sources);
      const bool content_changed =
          !same_source_content(decoded->source_graph, *current_sources);
      if (!structure_changed && !content_changed) {
        metrics.elapsed = std::chrono::steady_clock::now() - started;
        co_return ProjectHandle{
            std::move(context), std::move(paths), latest->snapshot,
            std::make_shared<const ProjectSnapshot>(std::move(*decoded)),
            metrics, {}, {}};
      }

      if (structure_changed) {
        auto lowered = co_await run_project_work<BuildGraph>(runtime_, [&] {
          return build_graph(decoded->moid_graph, context.target, paths.root,
                             root);
        });
        ++metrics.full_graph_lowerings;
        if (!lowered) {
          co_return std::unexpected(project_error(
              ProjectOpenErrorCode::graph_lowering, lowered.error(), previous));
        }
        decoded->build_graph = std::move(*lowered);
        decoded->compilations =
            compilations_from_build(decoded->build_graph, key);
        current_sources =
            co_await run_project_work<SourceGraphSnapshot>(runtime_, [&] {
              return source_graph_from_build(decoded->build_graph, metrics);
            });
        if (!current_sources) {
          co_return std::unexpected(
              project_error(ProjectOpenErrorCode::graph_lowering,
                            current_sources.error(), previous));
        }
      }
      decoded->source_graph = std::move(*current_sources);
      ++metrics.source_updates;

      auto final_declarations =
          observe_snapshot_declarations(*decoded, root / "bake.lock");
      ++metrics.input_checks;
      if (!final_declarations ||
          !declaration_inputs_match(decoded->declaration_inputs,
                                    *final_declarations)) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state,
            "project declaration inputs changed during refresh", previous));
      }
      auto final_sources =
          co_await run_project_work<SourceGraphSnapshot>(runtime_, [&] {
            return source_graph_from_build(decoded->build_graph, metrics);
          });
      if (!final_sources ||
          !same_source_content(decoded->source_graph, *final_sources)) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state,
            "project source inputs changed during refresh", previous));
      }

      const std::string validation =
          declaration_validation_id(decoded->declaration_inputs);
      ProjectPublicationGuard guard(paths);
      if (!guard.locked()) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state,
            "failed to acquire project publication lock", previous));
      }
      auto published = state.publish_snapshot_recovering_corruption(
          validation, project_snapshot_to_json(*decoded), guard);
      if (!published) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state, published.error().message, previous));
      }
      auto observed =
          state.record_successful_observation(published->first, validation);
      if (!observed) {
        co_return std::unexpected(project_error(
            ProjectOpenErrorCode::state, observed.error().message, previous));
      }
      metrics.elapsed = std::chrono::steady_clock::now() - started;
      co_return ProjectHandle{
          std::move(context), std::move(paths), published->first,
          std::make_shared<const ProjectSnapshot>(std::move(*decoded)),
          metrics, {}, {}};
    }
  }

  if (request.profile != "dev" && request.profile != "release" &&
      !manifest.profiles.contains(request.profile)) {
    co_return std::unexpected(
        project_error(ProjectOpenErrorCode::invalid_request,
                      "unknown profile '" + request.profile + "'", previous));
  }

  auto prepared_sources = co_await run_project_work<PreparedSources>(
      runtime_, [&] { return prepare_source_index(manifest, root, request); });
  if (!prepared_sources) {
    co_return std::unexpected(
        project_error(ProjectOpenErrorCode::lock_unavailable,
                      prepared_sources.error(), previous));
  }
  metrics.dependency_resolutions += prepared_sources->resolutions;

  BuildSelection selection;
  selection.workspace_member = member;
  selection.root_features = request.features;
  selection.target_triple = context.target.triple_with_version();
  ResolvePolicy policy;
  policy.offline = request.offline;
  auto graph = co_await run_project_work<MoidGraph>(runtime_, [&] {
    return resolve_moid_graph(manifest, selection, policy,
                              prepared_sources->index);
  });
  ++metrics.dependency_resolutions;
  if (!graph) {
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::dependency_resolution, graph.error(), previous));
  }

  auto declarations = co_await prepare_moid_declarations(
      runtime_, *graph, context.target, paths.root, context.profile,
      context.compiler.executable);
  if (!declarations)
    co_return std::unexpected(
        project_error(ProjectOpenErrorCode::declaration_preparation,
                      declarations.error(), previous));
  metrics.declaration_preparations += declarations->prepared;
  metrics.declaration_evaluations += declarations->evaluated;

  auto build = co_await run_project_work<BuildGraph>(runtime_, [&] {
    return build_graph(*graph, context.target, paths.root, root);
  });
  ++metrics.full_graph_lowerings;
  if (!build)
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::graph_lowering, build.error(), previous));
  auto sources = co_await run_project_work<SourceGraphSnapshot>(
      runtime_, [&] { return source_graph_from_build(*build, metrics); });
  if (!sources)
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::graph_lowering, sources.error(), previous));

  ProjectSnapshot snapshot;
  snapshot.context = context;
  snapshot.source_index = std::move(prepared_sources->index);
  snapshot.manifests = manifest_records(manifest, *graph);
  snapshot.moid_graph = std::move(*graph);
  snapshot.source_graph = std::move(*sources);
  snapshot.build_graph = std::move(*build);
  snapshot.compilations = compilations_from_build(snapshot.build_graph, key);
  auto declaration_inputs =
      observe_snapshot_declarations(snapshot, root / "bake.lock");
  if (!declaration_inputs)
    co_return std::unexpected(project_error(ProjectOpenErrorCode::state,
                                            declaration_inputs.error().message,
                                            previous));
  snapshot.declaration_inputs = std::move(*declaration_inputs);

  auto final_declarations =
      observe_snapshot_declarations(snapshot, root / "bake.lock");
  ++metrics.input_checks;
  if (!final_declarations ||
      !declaration_inputs_match(snapshot.declaration_inputs,
                                *final_declarations)) {
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::state,
        "project declaration inputs changed during preparation", previous));
  }
  auto final_sources =
      co_await run_project_work<SourceGraphSnapshot>(runtime_, [&] {
        return source_graph_from_build(snapshot.build_graph, metrics);
      });
  if (!final_sources ||
      !same_source_content(snapshot.source_graph, *final_sources)) {
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::state,
        "project source inputs changed during preparation", previous));
  }

  const std::string validation =
      declaration_validation_id(snapshot.declaration_inputs);
  ProjectPublicationGuard guard(paths);
  if (!guard.locked())
    co_return std::unexpected(
        project_error(ProjectOpenErrorCode::state,
                      "failed to acquire project publication lock", previous));
  auto published = state.publish_snapshot_recovering_corruption(
      validation, project_snapshot_to_json(snapshot), guard);
  if (!published)
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::state, published.error().message, previous));
  auto observed =
      state.record_successful_observation(published->first, validation);
  if (!observed)
    co_return std::unexpected(project_error(
        ProjectOpenErrorCode::state, observed.error().message, previous));

  metrics.elapsed = std::chrono::steady_clock::now() - started;
  co_return ProjectHandle{
      std::move(context), std::move(paths), published->first,
      std::make_shared<const ProjectSnapshot>(std::move(snapshot)), metrics,
      std::move(declarations->stdout_data),
      std::move(declarations->stderr_data)};
}

} // namespace bake
