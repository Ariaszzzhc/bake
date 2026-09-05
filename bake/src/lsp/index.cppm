export module bake.lsp.index;

import std;
import bake.json;
import bake.util;
import bake.util.blobdb;
import bake.util.cachestore;
import bake.buildsystem.query;
import bake.toolchain.compilation;  // SymbolOccurrence record

// ============================================================
// bake.lsp.index — shard storage, cross-TU lookup
//
// Shards — the symbol-occurrence list of one translation unit,
// keyed by the content hash of its main file plus its compile
// command — persist in a BlobDatabase, so cross-session reuse is
// a key lookup, not a recompile. The indexing compiles that
// produce occurrences run in worker processes; the server calls
// shard_key/has_shard, collects occurrences via the worker pool,
// and hands them to adopt(). This module never runs a compile — the compilation
// import carries only the shared SymbolOccurrence record.
//
// The in-memory project index merges the shards loaded this
// session: occurrences grouped by file (for position → symbol)
// and definitions grouped by USR (for symbol → definition).
// References are answered by scanning the per-file groups,
// which keeps memory proportional to indexed sources.
// ============================================================

namespace bake {

export struct IndexLocation {
    std::string file;    // as the compiler reported it
    unsigned line = 1;   // 1-based
    unsigned column = 1; // 1-based
};

// Compile-command adaptation shared by every inspection path:
// -c becomes -fsyntax-only, -o pairs and module output flags are
// dropped — no artifacts leave the frontend.
export std::vector<std::string> syntax_command(
        const std::vector<std::string>& command) {
    std::vector<std::string> argv;
    argv.reserve(command.size());
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (command[i] == "-c") {
            argv.push_back("-fsyntax-only");
            continue;
        }
        if (command[i] == "-o" && i + 1 < command.size()) {
            ++i;
            continue;
        }
        if (starts_with(command[i], "-o")) continue;
        if (starts_with(command[i], "-fmodule-output")) continue;
        argv.push_back(command[i]);
    }
    return argv;
}

export class ProjectIndex {
public:
    ProjectIndex(const Path& cache_root)
        : m_root(cache_root), m_db(cache_root) {
        load_all_shards();
    }

    // Shard identity for one TU's current content (the stored
    // text, or the on-disk file when `text` is null).
    std::string shard_key_of(const CompileEntry& entry,
                             const std::string* text) {
        return shard_key(entry, text ? *text : read_or_empty(entry.file));
    }

    // True when a shard exists for the entry's current on-disk
    // content — the freshness check background indexing starts
    // from.
    bool has_current_shard(const CompileEntry& entry) {
        return has_shard(shard_key(entry, read_or_empty(entry.file)));
    }

    // True when the shard for `key` is already known — the check
    // every indexing path starts from.
    bool has_shard(const std::string& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_shards.contains(key) || m_db.has(key);
    }

    // Store one TU's collected occurrences under the entry's
    // current-content key. Compilation happened in a worker; this
    // side only persists and integrates. Existing shards win.
    std::string adopt(const CompileEntry& entry, const std::string* text,
                      std::vector<SymbolOccurrence> occurrences) {
        std::string content = text ? *text : read_or_empty(entry.file);
        std::string key = shard_key(entry, content);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shards.contains(key)) return key;
        if (m_db.has(key)) {
            load_shard(key);
            return key;
        }
        store_shard(key, occurrences);
        return key;
    }

    // The occurrence at (or covering) a 1-based position.
    std::optional<SymbolOccurrence> occurrence_at(
            const std::string& file, unsigned line, unsigned column) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string canonical = canonical_key(file);
        auto it = m_by_file.find(canonical);
        if (it == m_by_file.end()) return std::nullopt;

        const SymbolOccurrence* best = nullptr;
        for (const auto& occ : it->second) {
            if (occ.line != line) continue;
            // Occurrences carry a start position only; accept the
            // one whose name spans the column.
            if (column >= occ.column &&
                column < occ.column + occ.name.size()) {
                if (!best ||
                    (best->is_ref && !occ.is_ref) ||
                    (best->is_call && (occ.is_decl || occ.is_def)))
                    best = &occ;
            }
        }
        if (best) return *best;
        return std::nullopt;
    }

    std::vector<IndexLocation> find_definitions(const std::string& usr) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<IndexLocation> out;
        auto it = m_definitions.find(usr);
        if (it == m_definitions.end()) return out;
        // Definitions before declarations — the primary target of a
        // jump is where the symbol is defined.
        for (const auto& occ : it->second)
            if (occ.is_def) out.push_back({occ.file, occ.line, occ.column});
        for (const auto& occ : it->second)
            if (!occ.is_def) out.push_back({occ.file, occ.line, occ.column});
        return out;
    }

    std::vector<IndexLocation> find_references(
            const std::string& usr, bool include_declaration) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<IndexLocation> out;
        for (const auto& [file, occs] : m_by_file) {
            (void)file;
            for (const auto& occ : occs) {
                if (occ.usr != usr) continue;
                bool is_decl_or_def = occ.is_decl || occ.is_def;
                if (is_decl_or_def && !include_declaration) continue;
                out.push_back({occ.file, occ.line, occ.column});
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return std::tie(a.file, a.line, a.column) <
                   std::tie(b.file, b.line, b.column);
        });
        return out;
    }

    // Definitions by bare name, for positions the index has not
    // seen (an unindexed file under the cursor).
    std::vector<IndexLocation> find_definitions_by_name(
            const std::string& name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<IndexLocation> out;
        for (const auto& [usr, occs] : m_definitions) {
            (void)usr;
            for (const auto& occ : occs) {
                if (occ.name != name) continue;
                out.push_back({occ.file, occ.line, occ.column});
            }
        }
        return out;
    }

    std::size_t shard_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_shards.size();
    }

    std::size_t occurrence_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_occurrence_count;
    }

    std::uintmax_t store_bytes() const { return m_db.size_bytes(); }

    // Objects unreferenced after an interrupted session; call
    // after background indexing settles.
    void enforce_quota() {
        // Reuse the shared content-addressed quota discipline: the
        // blob root already carries o/ + index.json.
        cache_enforce_quota(m_root, CacheQuota::from_env());
    }

private:
    static std::string canonical_key(const std::string& file) {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(
            std::filesystem::path(file), ec);
        if (ec) return file;
        return canonical.generic_string();
    }

    static std::string read_or_empty(const Path& path) {
        auto content = read_file(path);
        return content ? *content : std::string();
    }

    std::string shard_key(const CompileEntry& entry,
                          const std::string& content) const {
        std::string seed = entry.file.string() + std::string(1, '\0');
        seed += std::to_string(entry.command.size());
        for (const auto& arg : entry.command) seed += arg + "\0";
        seed += content;
        return "shard:" + SHA256::to_hex(SHA256::compute(seed));
    }

    void store_shard(const std::string& key,
                     const std::vector<SymbolOccurrence>& occurrences) {
        m_db.put(key, serialize(occurrences));
        integrate(key, occurrences);
    }

    void load_shard(const std::string& key) {
        auto blob = m_db.get(key);
        if (!blob) return;
        integrate(key, deserialize(*blob));
    }

    void load_all_shards() {
        for (const auto& key : m_db.keys_with_prefix("shard:"))
            load_shard(key);
    }

    void integrate(const std::string& key,
                   const std::vector<SymbolOccurrence>& occurrences) {
        if (m_shards.contains(key)) return;
        m_shards.insert(key);
        for (const auto& occ : occurrences) {
            if (occ.file.empty()) continue;  // unresolvable location
            m_by_file[canonical_key(occ.file)].push_back(occ);
            if (occ.is_decl || occ.is_def)
                m_definitions[occ.usr].push_back(occ);
            ++m_occurrence_count;
        }
    }

    static std::string serialize(const std::vector<SymbolOccurrence>& occs) {
        json::Value root = json::Value::array();
        for (const auto& occ : occs) {
            json::Value item = json::Value::object();
            item["usr"] = occ.usr;
            item["name"] = occ.name;
            item["kind"] = occ.kind;
            unsigned flags = 0;
            if (occ.is_decl) flags |= 1;
            if (occ.is_def) flags |= 2;
            if (occ.is_ref) flags |= 4;
            if (occ.is_call) flags |= 8;
            item["flags"] = std::int64_t(flags);
            item["file"] = occ.file;
            item["line"] = std::int64_t(occ.line);
            item["column"] = std::int64_t(occ.column);
            item["dfile"] = occ.decl_file;
            item["dline"] = std::int64_t(occ.decl_line);
            item["dcol"] = std::int64_t(occ.decl_column);
            root.push_back(std::move(item));
        }
        return root.dump();
    }

    static std::vector<SymbolOccurrence> deserialize(
            const std::string& blob) {
        std::vector<SymbolOccurrence> out;
        try {
            json::Value root = json::Value::parse(blob);
            if (!root.is_array()) return out;
            for (std::size_t i = 0; i < root.size(); ++i) {
                const json::Value& item = root[i];
                SymbolOccurrence occ;
                if (const json::Value* v = item.find("usr"))
                    occ.usr = v->get<std::string>();
                if (const json::Value* v = item.find("name"))
                    occ.name = v->get<std::string>();
                if (const json::Value* v = item.find("kind"))
                    occ.kind = v->get<std::string>();
                unsigned flags = 0;
                if (const json::Value* v = item.find("flags"))
                    flags = static_cast<unsigned>(
                        v->get<std::int64_t>());
                occ.is_decl = flags & 1;
                occ.is_def = flags & 2;
                occ.is_ref = flags & 4;
                occ.is_call = flags & 8;
                if (const json::Value* v = item.find("file"))
                    occ.file = v->get<std::string>();
                if (const json::Value* v = item.find("line"))
                    occ.line = static_cast<unsigned>(v->get<std::int64_t>());
                if (const json::Value* v = item.find("column"))
                    occ.column =
                        static_cast<unsigned>(v->get<std::int64_t>());
                if (const json::Value* v = item.find("dfile"))
                    occ.decl_file = v->get<std::string>();
                if (const json::Value* v = item.find("dline"))
                    occ.decl_line =
                        static_cast<unsigned>(v->get<std::int64_t>());
                if (const json::Value* v = item.find("dcol"))
                    occ.decl_column =
                        static_cast<unsigned>(v->get<std::int64_t>());
                out.push_back(std::move(occ));
            }
        } catch (const json::Error&) {
        }
        return out;
    }

    Path m_root;
    FileBlobDatabase m_db;
    mutable std::mutex m_mutex;
    std::set<std::string> m_shards;
    std::map<std::string, std::vector<SymbolOccurrence>> m_by_file;
    std::map<std::string, std::vector<SymbolOccurrence>> m_definitions;
    std::size_t m_occurrence_count = 0;
};

} // namespace bake
