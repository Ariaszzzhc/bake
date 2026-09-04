export module bake.util.blobdb;
import std;
import bake.json;
import bake.util;
import bake.util.cachestore;

// ============================================================
// bake.util.blobdb — key/blob storage with single-writer
// transactions and snapshot reads
//
// The interface deliberately carries LMDB semantics (one writer,
// atomic commits, immutable reads) because the known upgrade
// triggers — cross-shard transactions, mmap handle pressure,
// global-query latency — are written against it. The first
// implementation is content-addressed files:
//
//   <root>/o/<sha256(blob)>   immutable objects (shared across
//                             keys with identical content)
//   <root>/index.json         key → object hash, rewritten
//                             atomically at commit
//
// Objects never change, so readers hold true snapshots without
// locks; a crash between object write and index rewrite leaves
// an unreferenced object, which quota eviction reclaims.
// ============================================================

namespace bake {

export class BlobDatabase {
public:
    virtual ~BlobDatabase() = default;

    // Returns false when the identical key already maps to the
    // identical content (a no-op commit).
    virtual bool put(std::string_view key, std::string_view blob) = 0;
    virtual std::optional<std::string> get(std::string_view key) const = 0;
    virtual bool has(std::string_view key) const = 0;

    // Keys beginning with `prefix`, in lexicographic order.
    virtual std::vector<std::string> keys_with_prefix(
        std::string_view prefix) const = 0;

    virtual std::size_t count() const = 0;
    virtual std::uintmax_t size_bytes() const = 0;
};

export class FileBlobDatabase : public BlobDatabase {
public:
    explicit FileBlobDatabase(const Path& root) : m_root(root) {
        std::filesystem::create_directories((m_root / "o").fs());
        load_index();
    }

    bool put(std::string_view key, std::string_view blob) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string k(key);
        std::string digest = digest_hex(blob);

        auto it = m_index.find(k);
        if (it != m_index.end() && it->second == digest) return false;

        Path object = m_root / "o" / digest;
        if (!object.is_regular_file()) {
            write_file_atomic(object, blob);
            m_size_bytes += blob.size();
        }
        m_index[k] = digest;
        save_index();
        return true;
    }

    std::optional<std::string> get(std::string_view key) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(std::string(key));
        if (it == m_index.end()) return std::nullopt;
        auto content = read_file(m_root / "o" / it->second);
        if (!content) return std::nullopt;
        return content;
    }

    bool has(std::string_view key) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_index.contains(std::string(key));
    }

    std::vector<std::string> keys_with_prefix(
        std::string_view prefix) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> keys;
        for (const auto& [k, digest] : m_index) {
            (void)digest;
            if (k.starts_with(prefix)) keys.push_back(k);
        }
        return keys;  // map iteration is already ordered
    }

    std::size_t count() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_index.size();
    }

    std::uintmax_t size_bytes() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size_bytes;
    }

private:
    static std::string digest_hex(std::string_view blob) {
        return SHA256::to_hex(SHA256::compute(blob));
    }

    void write_file_atomic(const Path& path, std::string_view content) {
        Path temp = path.parent() /
            ("." + path.filename_string() + ".tmp");
        {
            std::ofstream out(temp.fs(), std::ios::binary | std::ios::trunc);
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        std::filesystem::rename(temp.fs(), path.fs());
    }

    void load_index() {
        m_size_bytes = 0;
        m_index.clear();
        auto text = read_file(m_root / "index.json");
        if (!text) return;

        try {
            json::Value parsed = json::Value::parse(*text);
            if (!parsed.is_object()) return;
            if (const json::Value* entries = parsed.find("entries");
                entries && entries->is_array()) {
                for (std::size_t i = 0; i < entries->size(); ++i) {
                    const json::Value& entry = (*entries)[i];
                    if (const json::Value* k = entry.find("k");
                        k && k->is_string())
                        if (const json::Value* d = entry.find("d");
                            d && d->is_string())
                            m_index[k->get<std::string>()] =
                                d->get<std::string>();
                }
            }
        } catch (const json::Error&) {
            return;  // a corrupt index rebuilds from misses
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator((m_root / "o").fs(), ec)) {
            if (ec || !entry.is_regular_file()) continue;
            std::error_code sec;
            m_size_bytes += entry.file_size(sec);
        }
    }

    void save_index() {
        // entries:[{k,d},…] — key strings may contain arbitrary
        // bytes, so escaping goes through bake.json rather than
        // string concatenation.
        json::Value root = json::Value::object();
        json::Value entries = json::Value::array();
        for (const auto& [k, digest] : m_index) {
            json::Value entry = json::Value::object();
            entry["k"] = k;
            entry["d"] = digest;
            entries.push_back(std::move(entry));
        }
        root["entries"] = std::move(entries);
        write_file_atomic(m_root / "index.json", root.dump());
    }


    Path m_root;
    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_index;
    std::uintmax_t m_size_bytes = 0;
};

} // namespace bake
