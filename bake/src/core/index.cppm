export module bake.core.index;

import std;
import bake.core.model;
import bake.core.occurrences;
import bake.core.state;
import bake.json;
import bake.toolchain.compilation;
import bake.util;

namespace bake {

export struct IndexLocation {
    std::string file;
    unsigned line = 1;
    unsigned column = 1;
};

export std::vector<std::string> analysis_command(
        const CompilationDescription& description,
        const CompilerIdentity& compiler) {
    auto command = materialize_compiler_command(description, compiler);
    std::vector<std::string> result;
    result.reserve(command.size());
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (command[i] == "-c") {
            result.push_back("-fsyntax-only");
        } else if (command[i] == "-o" && i + 1 < command.size()) {
            ++i;
        } else if (!starts_with(command[i], "-o") &&
                   !starts_with(command[i], "-fmodule-output")) {
            result.push_back(command[i]);
        }
    }
    return result;
}

export struct IndexError { std::string message; };

export struct IndexShard {
    std::string key;
    std::vector<SymbolOccurrence> occurrences;
};

namespace index_detail {

using Json = json::Value;

std::string canonical_key(const std::string& file) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(file, error);
    return error ? file : canonical.generic_string();
}

std::expected<const Json*, IndexError> required(
        const Json& object, std::string_view name) {
    if (!object.is_object())
        return std::unexpected(IndexError{"index record must be an object"});
    if (const Json* value = object.find(name)) return value;
    return std::unexpected(IndexError{
        "index record is missing field '" + std::string(name) + "'"});
}

std::expected<std::string, IndexError> string_field(
        const Json& object, std::string_view name) {
    auto value = required(object, name);
    if (!value) return std::unexpected(value.error());
    if (!(*value)->is_string())
        return std::unexpected(IndexError{
            "index field '" + std::string(name) + "' must be a string"});
    return (*value)->get<std::string>();
}

std::expected<std::string, IndexError> shard_document(
        std::string_view key,
        const std::vector<SymbolOccurrence>& occurrences) {
    auto batch = encode_occurrence_batch(occurrences);
    if (!batch)
        return std::unexpected(IndexError{batch.error().message});
    Json root = Json::object();
    root["schema"] = "bake-analysis-index-v2";
    root["key"] = std::string(key);
    root["occurrences"] = std::move(*batch);
    return root.dump();
}

std::expected<std::vector<SymbolOccurrence>, IndexError> decode_shard(
        std::string_view content, std::string_view expected_key) {
    try {
        Json root = Json::parse(content);
        if (!root.is_object() || root.size() != 3)
            return std::unexpected(IndexError{
                "index shard has unexpected fields"});
        auto schema = string_field(root, "schema");
        auto key = string_field(root, "key");
        auto values = required(root, "occurrences");
        if (!schema) return std::unexpected(schema.error());
        if (!key) return std::unexpected(key.error());
        if (!values) return std::unexpected(values.error());
        if (*schema != "bake-analysis-index-v2" || *key != expected_key)
            return std::unexpected(IndexError{"index shard identity mismatch"});
        auto occurrences = decode_occurrence_batch(**values);
        if (!occurrences)
            return std::unexpected(IndexError{occurrences.error().message});
        return std::move(*occurrences);
    } catch (const json::Error& error) {
        return std::unexpected(IndexError{
            "invalid index shard JSON: " + std::string(error.what())});
    }
}

}  // namespace index_detail

export class ProjectIndex {
public:
    ProjectIndex(Path cache_root, ContextPaths paths)
        : m_shards(std::move(cache_root) / "shards"),
          m_paths(std::move(paths)) {
        m_shards.mkdir_recursive();
    }

    static std::string shard_key(
            const CompilationDescription& description,
            const CompilerIdentity& compiler,
            const std::map<std::string, std::string>& captured_sources,
            const std::vector<InputIdentity>& dependencies) {
        std::string identity = "bake-analysis-shard-v2\n";
        identity += description.context.value + "\n";
        identity += description.id.value + "\n";
        for (const auto& argument : analysis_command(description, compiler))
            identity += std::to_string(argument.size()) + ":" + argument + "\n";
        for (const auto& [path, content] : captured_sources) {
            identity += std::to_string(path.size()) + ":" + path + "\n";
            identity += SHA256::hex(content) + "\n";
        }
        auto ordered_dependencies = dependencies;
        std::ranges::sort(ordered_dependencies, {}, [](const auto& input) {
            return input.path.string();
        });
        for (const auto& input : ordered_dependencies)
            identity += input.path.string() + "\n" + input.digest + "\n";
        return SHA256::hex(identity);
    }

    bool is_current(const CompilationUnitId& unit,
                    std::string_view key) const {
        std::lock_guard lock(m_mutex);
        auto current = m_current.find(unit.value);
        return current != m_current.end() && current->second.key == key;
    }

    std::expected<std::optional<IndexShard>, IndexError> read_shard(
            std::string key) const {
        if (!valid_key(key))
            return std::unexpected(IndexError{
                "index shard key must be a SHA-256 digest"});
        auto stored = read_file(shard_path(key));
        if (!stored) return std::optional<IndexShard>{};
        auto occurrences = index_detail::decode_shard(*stored, key);
        if (!occurrences) return std::optional<IndexShard>{};
        return std::optional<IndexShard>(IndexShard{
            std::move(key), std::move(*occurrences)});
    }

    std::expected<void, IndexError> install(
            const CompilationUnitId& unit, IndexShard shard,
            std::vector<Path> dependencies = {}) {
        std::lock_guard lock(m_mutex);
        if (!valid_key(shard.key))
            return std::unexpected(IndexError{
                "index shard key must be a SHA-256 digest"});
        auto current = m_current.find(unit.value);
        if (current == m_current.end() || current->second.key != shard.key)
            set_current(unit, std::move(shard.key),
                        std::move(shard.occurrences),
                        std::move(dependencies));
        return {};
    }

    std::expected<void, IndexError> adopt(
            const CompilationUnitId& unit, std::string key,
            std::vector<SymbolOccurrence> occurrences,
            std::vector<Path> dependencies = {}) {
        std::lock_guard lock(m_mutex);
        if (!valid_key(key))
            return std::unexpected(IndexError{
                "index shard key must be a SHA-256 digest"});
        auto path = shard_path(key);
        auto document = index_detail::shard_document(
            key, occurrences);
        if (!document) return std::unexpected(document.error());
        ProjectPublicationGuard guard(m_paths);
        auto published = publish_immutable_file_recovering_corruption(
            path, *document, guard,
            [&](std::string_view content) -> std::expected<void, StateError> {
                if (content.data() == document->data() &&
                    content.size() == document->size())
                    return {};
                auto decoded = index_detail::decode_shard(content, key);
                if (!decoded)
                    return std::unexpected(StateError{
                        StateErrorCode::malformed, path,
                        decoded.error().message});
                return {};
            });
        if (!published) {
            return std::unexpected(IndexError{
                "failed to publish index shard '" + path.string() +
                "': " + published.error().message});
        }
        auto current = m_current.find(unit.value);
        if (current == m_current.end() || current->second.key != key)
            set_current(unit, std::move(key), std::move(occurrences),
                        std::move(dependencies));
        return {};
    }

    void invalidate(const Path& changed_source) {
        std::lock_guard lock(m_mutex);
        const std::string key = index_detail::canonical_key(
            changed_source.string());
        std::vector<std::string> invalid;
        for (const auto& [unit, current] : m_current)
            if (current.dependencies.contains(key)) invalid.push_back(unit);
        for (const auto& unit : invalid) remove_current(unit);
    }

    std::optional<SymbolOccurrence> occurrence_at(
            const std::string& file, unsigned line, unsigned column) const {
        std::lock_guard lock(m_mutex);
        auto it = m_by_file.find(index_detail::canonical_key(file));
        if (it == m_by_file.end()) return std::nullopt;
        const SymbolOccurrence* best = nullptr;
        for (const auto& [unit, occurrences] : it->second)
            for (const auto& occurrence : occurrences) {
                (void)unit;
                if (occurrence.line != line || column < occurrence.column ||
                    column >= occurrence.column + occurrence.name.size())
                    continue;
                if (!best || (best->is_ref && !occurrence.is_ref) ||
                    (best->is_call &&
                     (occurrence.is_decl || occurrence.is_def)))
                    best = &occurrence;
            }
        return best ? std::optional(*best) : std::nullopt;
    }

    std::vector<IndexLocation> find_definitions(std::string_view usr) const {
        std::lock_guard lock(m_mutex);
        std::vector<IndexLocation> result;
        auto it = m_definitions.find(std::string(usr));
        if (it == m_definitions.end()) return result;
        for (const auto& [unit, occurrences] : it->second)
            for (const auto& occurrence : occurrences) {
                (void)unit;
                if (occurrence.is_def)
                    result.push_back({occurrence.file, occurrence.line,
                                      occurrence.column});
            }
        for (const auto& [unit, occurrences] : it->second)
            for (const auto& occurrence : occurrences) {
                (void)unit;
                if (!occurrence.is_def)
                    result.push_back({occurrence.file, occurrence.line,
                                      occurrence.column});
            }
        return result;
    }

    bool has_definition(std::string_view usr) const {
        std::lock_guard lock(m_mutex);
        auto found = m_definitions.find(std::string(usr));
        return found != m_definitions.end() &&
               std::ranges::any_of(found->second, [](const auto& unit) {
                   return std::ranges::any_of(
                       unit.second, [](const auto& occurrence) {
                           return occurrence.is_def;
                       });
               });
    }

    std::vector<Path> definition_sources(std::string_view usr) const {
        std::lock_guard lock(m_mutex);
        std::vector<Path> result;
        auto found = m_definitions.find(std::string(usr));
        if (found == m_definitions.end()) return result;
        for (const auto& [unit, occurrences] : found->second)
            for (const auto& occurrence : occurrences) {
                (void)unit;
                if (occurrence.is_def && !occurrence.file.empty())
                    result.emplace_back(occurrence.file);
            }
        std::ranges::sort(result, {}, [](const Path& path) {
            return index_detail::canonical_key(path.string());
        });
        result.erase(std::unique(result.begin(), result.end(),
                                 [](const Path& left, const Path& right) {
            return index_detail::canonical_key(left.string()) ==
                   index_detail::canonical_key(right.string());
        }), result.end());
        return result;
    }

    std::vector<IndexLocation> find_references(
            std::string_view usr, bool include_declaration) const {
        std::lock_guard lock(m_mutex);
        std::vector<IndexLocation> result;
        for (const auto& [file, units] : m_by_file) {
            (void)file;
            for (const auto& [unit, occurrences] : units)
                for (const auto& occurrence : occurrences) {
                    (void)unit;
                    if (occurrence.usr != usr) continue;
                    if (!include_declaration &&
                        (occurrence.is_decl || occurrence.is_def))
                        continue;
                    result.push_back({occurrence.file, occurrence.line,
                                      occurrence.column});
                }
        }
        std::ranges::sort(result, {}, [](const auto& location) {
            return std::tie(location.file, location.line, location.column);
        });
        return result;
    }

    std::vector<IndexLocation> find_definitions_by_name(
            std::string_view name) const {
        std::lock_guard lock(m_mutex);
        std::vector<IndexLocation> result;
        for (const auto& [usr, units] : m_definitions) {
            (void)usr;
            for (const auto& [unit, occurrences] : units)
                for (const auto& occurrence : occurrences) {
                    (void)unit;
                    if (occurrence.name == name)
                        result.push_back({occurrence.file, occurrence.line,
                                          occurrence.column});
                }
        }
        return result;
    }

    std::size_t shard_count() const {
        std::lock_guard lock(m_mutex);
        return m_current.size();
    }

    std::size_t occurrence_count() const {
        std::lock_guard lock(m_mutex);
        return m_occurrence_count;
    }

private:
    static bool valid_key(std::string_view key) {
        return key.size() == 64 &&
               std::ranges::all_of(key, [](unsigned char value) {
                   return std::isxdigit(value);
               });
    }

    struct Current {
        std::string key;
        std::set<std::string> dependencies;
        std::set<std::string> files;
        std::set<std::string> definitions;
        std::size_t occurrence_count = 0;
    };

    void set_current(const CompilationUnitId& unit, std::string key,
                     std::vector<SymbolOccurrence> occurrences,
                     std::vector<Path> dependencies) {
        remove_current(unit.value);
        std::map<std::string, std::string> canonical_paths;
        auto canonical = [&](const std::string& path) -> const std::string& {
            auto [found, inserted] = canonical_paths.try_emplace(path);
            if (inserted)
                found->second = index_detail::canonical_key(path);
            return found->second;
        };
        Current current;
        current.key = std::move(key);
        for (const auto& dependency : dependencies)
            current.dependencies.insert(canonical(dependency.string()));
        for (auto& occurrence : occurrences) {
            if (occurrence.file.empty()) continue;
            const std::string file = canonical(occurrence.file);
            current.files.insert(file);
            if (occurrence.is_decl || occurrence.is_def) {
                current.definitions.insert(occurrence.usr);
                m_definitions[occurrence.usr][unit.value].push_back(occurrence);
            }
            m_by_file[file][unit.value].push_back(std::move(occurrence));
            ++current.occurrence_count;
        }
        m_occurrence_count += current.occurrence_count;
        m_current[unit.value] = std::move(current);
    }

    Path shard_path(std::string_view key) const {
        return m_shards / (std::string(key) + ".json");
    }

    void remove_current(const std::string& unit) {
        auto current = m_current.find(unit);
        if (current == m_current.end()) return;
        for (const auto& file : current->second.files) {
            auto found = m_by_file.find(file);
            if (found == m_by_file.end()) continue;
            found->second.erase(unit);
            if (found->second.empty()) m_by_file.erase(found);
        }
        for (const auto& usr : current->second.definitions) {
            auto found = m_definitions.find(usr);
            if (found == m_definitions.end()) continue;
            found->second.erase(unit);
            if (found->second.empty()) m_definitions.erase(found);
        }
        m_occurrence_count -= current->second.occurrence_count;
        m_current.erase(current);
    }

    Path m_shards;
    ContextPaths m_paths;
    mutable std::mutex m_mutex;
    std::map<std::string, Current> m_current;
    std::map<std::string,
             std::map<std::string, std::vector<SymbolOccurrence>>> m_by_file;
    std::map<std::string,
             std::map<std::string, std::vector<SymbolOccurrence>>> m_definitions;
    std::size_t m_occurrence_count = 0;
};

}  // namespace bake
