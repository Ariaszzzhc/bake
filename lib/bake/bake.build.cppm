module;

// bake.build — build.cpp API, source-distributed.
//
// Only depends on import std;. No Bake runtime or third-party JSON library is
// linked into user build scripts. Builder writes the versioned declaration to
// BAKE_DECLARATION_PATH; stdout and stderr remain available to user code.
//
// Input context arrives via environment variables:
//   BAKE_MOID_ID           — canonical source identity
//   BAKE_MOID_NAME         — manifest Moid name
//   BAKE_MOID_VERSION      — manifest Moid version
//   BAKE_SOURCE_DIR        — absolute path to the Moid root
//   BAKE_BUILD_DIR         — absolute path to out/
//   BAKE_DECLARATION_PATH  — destination for declaration JSON
//   BAKE_FEATURES          — length-prefixed active feature names
//   BAKE_DECLARATION_OPTIONS / BAKE_DECLARATION_DEPENDENCIES
//                         — typed JSON fragments supplied by Bake

export module bake.build;

import std;

// ============================================================
// Internal helpers (module scope, not exported)
// ============================================================

namespace {

std::string json_escape(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default: {
                const auto byte = static_cast<unsigned char>(c);
                if (byte <= 0x1F) {
                    out += "\\u00";
                    out += hex[(byte >> 4) & 0x0F];
                    out += hex[byte & 0x0F];
                } else {
                    out += c;
                }
            }
        }
    }
    return out;
}

std::string json_string(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

std::string json_array(const std::vector<std::string>& items) {
    if (items.empty()) return "[]";
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += json_string(items[i]);
    }
    out += "]";
    return out;
}

bool glob_match(std::string_view text, std::string_view pattern) {
    std::size_t ti = 0, pi = 0;
    std::size_t star_t = std::string_view::npos, star_p = std::string_view::npos;

    while (ti < text.size() || pi < pattern.size()) {
        if (pi < pattern.size()) {
            if (pi + 1 < pattern.size() && pattern[pi] == '*' && pattern[pi + 1] == '*') {
                pi += 2;
                if (pi < pattern.size() && pattern[pi] == '/') pi++;
                star_p = pi;
                star_t = ti;
                continue;
            }
            if (pattern[pi] == '*') {
                star_p = pi;
                star_t = ti;
                pi++;
                continue;
            }
            if (pi < pattern.size() && ti < text.size() &&
                (pattern[pi] == '?' || pattern[pi] == text[ti])) {
                if (pattern[pi] != '?' || text[ti] != '/') {
                    pi++;
                    ti++;
                    continue;
                }
            }
        }
        if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            if (star_t < text.size() && text[star_t] != '/') {
                star_t++;
                ti = star_t;
                continue;
            }
        }
        return false;
    }
    return true;
}

std::vector<std::string> expand_glob(const std::string& pattern,
                                      const std::string& base_dir) {
    std::vector<std::string> result;
    namespace fs = std::filesystem;

    if (pattern.find_first_of("*?") == std::string::npos) {
        fs::path full = fs::path(base_dir) / pattern;
        if (fs::exists(full))
            result.push_back(pattern);
        return result;
    }

    // Split at the first component containing a wildcard: the literal
    // leading components name an existing directory to search, the
    // remainder is matched against paths relative to it. Wildcard
    // components ("**", "*.c") never exist on disk, so they must stay
    // on the pattern side — a plain parent_path() would treat "**" as
    // a real directory and match nothing.
    fs::path fixed_prefix;
    fs::path wild_pattern;
    bool saw_wildcard = false;
    for (const auto& part : fs::path(pattern)) {
        auto piece = part.string();
        if (!saw_wildcard && piece.find_first_of("*?") == std::string::npos)
            fixed_prefix /= piece;
        else {
            saw_wildcard = true;
            wild_pattern /= piece;
        }
    }
    if (wild_pattern.empty()) return result;

    fs::path search_dir = fixed_prefix;
    if (search_dir.is_relative()) search_dir = fs::path(base_dir) / search_dir;
    if (!fs::exists(search_dir)) return result;
    const bool absolute = fs::path(pattern).is_absolute();
    try {
        for (auto& entry : fs::recursive_directory_iterator(search_dir)) {
            if (!entry.is_regular_file()) continue;
            auto rel = fs::relative(entry.path(), search_dir);
            if (!glob_match(rel.generic_string(),
                            wild_pattern.generic_string()))
                continue;
            result.push_back(absolute
                                 ? entry.path().generic_string()
                                 : (fixed_prefix / rel).generic_string());
        }
    } catch (...) {}

    std::sort(result.begin(), result.end());
    return result;
}

std::set<std::string> parse_features_env() {
    std::set<std::string> features;
    const char* env = std::getenv("BAKE_FEATURES");
    if (!env) return features;

    std::string_view encoded(env);
    std::size_t cursor = 0;
    while (cursor < encoded.size()) {
        std::size_t length = 0;
        if (encoded[cursor] < '0' || encoded[cursor] > '9')
            return {};
        while (cursor < encoded.size() && encoded[cursor] >= '0' &&
               encoded[cursor] <= '9') {
            const std::size_t digit =
                static_cast<std::size_t>(encoded[cursor] - '0');
            if (length >
                (std::numeric_limits<std::size_t>::max() - digit) / 10)
                return {};
            length = length * 10 + digit;
            ++cursor;
        }
        if (cursor >= encoded.size() || encoded[cursor] != ':') return {};
        ++cursor;
        if (length > encoded.size() - cursor) return {};
        features.insert(std::string(encoded.substr(cursor, length)));
        cursor += length;
    }
    return features;
}

} // anonymous namespace

// ============================================================
// Exported API
// ============================================================

export namespace bake {

struct SourceGroup {
    std::string pattern;
    bool is_public = false;
};

class BinaryBuilder {
public:
    BinaryBuilder() = default;
    explicit BinaryBuilder(std::string n, std::string dir)
        : name(std::move(n)), source_dir_(std::move(dir)) {}

    BinaryBuilder& sources(std::string_view pattern) {
        for (auto& file : expand_glob(std::string(pattern), source_dir_))
            source_groups.push_back({std::move(file), false});
        return *this;
    }
    BinaryBuilder& sources(std::initializer_list<std::string_view> patterns) {
        for (auto pattern : patterns) sources(pattern);
        return *this;
    }

    BinaryBuilder& include_dirs(std::string_view dir) {
        include_dirs_.emplace_back(dir);
        return *this;
    }
    BinaryBuilder& include_dirs(std::initializer_list<std::string_view> dirs) {
        for (auto dir : dirs) include_dirs_.emplace_back(dir);
        return *this;
    }

    std::string name;
    std::vector<SourceGroup> source_groups;
    std::vector<std::string> include_dirs_;

private:
    std::string source_dir_;
};

class TestRegistration {
public:
    TestRegistration& set_default() {
        // Later calls override earlier ones: at most one default exists.
        if (siblings_) {
            for (auto& other : *siblings_)
                other.is_default = false;
        }
        is_default = true;
        return *this;
    }

    std::string name;
    std::string binary;
    bool is_default = false;

private:
    friend class Builder;
    std::vector<TestRegistration>* siblings_ = nullptr;
};

class Builder {
public:
    Builder() {
        if (const char* value = std::getenv("BAKE_MOID_ID"))
            id_ = value;
        if (const char* value = std::getenv("BAKE_MOID_NAME"))
            name_ = value;
        if (const char* value = std::getenv("BAKE_MOID_VERSION"))
            version_ = value;
        if (const char* value = std::getenv("BAKE_MOID_TYPE"))
            type_ = value;
        if (const char* value = std::getenv("BAKE_MOID_CXX_STD"))
            cxx_std_ = value;
        if (const char* value = std::getenv("BAKE_MOID_C_STD"))
            c_std_ = value;
        if (const char* value = std::getenv("BAKE_SOURCE_DIR"))
            source_dir_ = value;
        if (const char* value = std::getenv("BAKE_BUILD_DIR"))
            build_dir_ = value;
        if (const char* value = std::getenv("BAKE_DECLARATION_PATH"))
            declaration_path_ = value;
        if (const char* value = std::getenv("BAKE_DECLARATION_FEATURES"))
            declaration_features_ = value;
        if (const char* value = std::getenv("BAKE_DECLARATION_DEPENDENCIES"))
            declaration_dependencies_ = value;
        if (const char* value = std::getenv("BAKE_TARGET"))
            target_ = value;
        features_ = parse_features_env();

        // Dependency source directories arrive as alias=/absolute/path.
        if (const char* env = std::getenv("BAKE_DEPS")) {
            std::istringstream stream(env);
            std::string line;
            while (std::getline(stream, line)) {
                auto equals = line.find('=');
                if (equals != std::string::npos)
                    dep_dirs_[line.substr(0, equals)] = line.substr(equals + 1);
            }
        }
    }

    Builder& sources(std::string_view pattern) {
        for (auto& file : expand_glob(std::string(pattern), source_dir_)) {
            source_groups_.push_back({std::move(file), false});
        }
        return *this;
    }
    Builder& sources(std::initializer_list<std::string_view> patterns) {
        for (auto pattern : patterns) sources(pattern);
        return *this;
    }

    // Public module interface files — compiled and exported to consumers.
    Builder& public_modules(std::string_view pattern) {
        for (auto& file : expand_glob(std::string(pattern), source_dir_)) {
            source_groups_.push_back({std::move(file), true});
        }
        return *this;
    }
    Builder& public_modules(std::initializer_list<std::string_view> patterns) {
        for (auto pattern : patterns) public_modules(pattern);
        return *this;
    }

    // Private include paths — visible during compilation only.
    Builder& include_dirs(std::string_view directory) {
        include_dirs_.emplace_back(directory);
        return *this;
    }
    Builder& include_dirs(std::initializer_list<std::string_view> directories) {
        for (auto directory : directories) include_dirs_.emplace_back(directory);
        return *this;
    }

    // Public header directories — exposed to consumers as include paths.
    Builder& public_headers(std::string_view path) {
        public_include_dirs_.emplace_back(path);
        return *this;
    }
    Builder& public_headers(std::initializer_list<std::string_view> paths) {
        for (auto path : paths) public_headers(path);
        return *this;
    }

    Builder& prebuilt_lib(std::string_view path) {
        prebuilt_libs_.emplace_back(path);
        return *this;
    }

    BinaryBuilder& binary(std::string_view name) {
        binaries_.emplace_back(std::string(name), source_dir_);
        return binaries_.back();
    }

    TestRegistration& add_test(std::string_view test_name,
                               std::string_view binary_name) {
        TestRegistration registration;
        registration.name = std::string(test_name);
        registration.binary = std::string(binary_name);
        registration.siblings_ = &tests_;
        tests_.push_back(std::move(registration));
        return tests_.back();
    }

    bool feature(std::string_view name) const {
        return features_.count(std::string(name)) != 0;
    }

    std::string_view source_dir() const { return source_dir_; }
    std::string_view build_dir() const { return build_dir_; }
    std::string_view target() const { return target_; }

    std::string_view dep_src_dir(std::string_view name) {
        std::string key(name);
        auto it = dep_dirs_.find(key);
        // Every query counts as consumption, including misses — configure
        // warns about declared source dependencies nothing ever asks for.
        used_source_deps_.insert(key);
        return it != dep_dirs_.end() ? std::string_view(it->second) : "";
    }

    int build() {
        if (declaration_path_.empty() || name_.empty()) return 1;
        std::ofstream output(declaration_path_, std::ios::trunc);
        if (!output) return 2;
        output << serialize();
        return output ? 0 : 3;
    }

private:
    std::string serialize() const {
        std::string json;
        json += "{\n";
        json += "  \"id\": " + json_string(id_.empty() ? name_ : id_) + ",\n";
        json += "  \"name\": " + json_string(name_) + ",\n";
        json += "  \"version\": " + json_string(version_) + ",\n";
        json += "  \"type\": " + json_string(type_) + ",\n";
        json += "  \"root\": " + json_string(source_dir_) + ",\n";
        json += "  \"cxx_std\": " + json_string(cxx_std_) + ",\n";
        json += "  \"c_std\": " + json_string(c_std_) + ",\n";
        json += "  \"features\": " + declaration_features_ + ",\n";

        json += "  \"sources\": [";
        for (std::size_t i = 0; i < source_groups_.size(); ++i) {
            if (i != 0) json += ",";
            json += "\n    {\"pattern\": " +
                    json_string(source_groups_[i].pattern);
            json += ", \"visibility\": " +
                    json_string(source_groups_[i].is_public ? "public" : "private");
            json += "}";
        }
        json += source_groups_.empty() ? "],\n" : "\n  ],\n";

        json += "  \"public_include_dirs\": " + json_array(public_include_dirs_) + ",\n";
        json += "  \"link\": {\"libraries\": [], \"frameworks\": []},\n";
        json += "  \"dependencies\": " + declaration_dependencies_ + ",\n";
        json += "  \"flags\": [],\n";
        json += "  \"defines\": [],\n";
        json += "  \"include_dirs\": " + json_array(include_dirs_) + ",\n";
        json += "  \"link_flags\": [],\n";
        json += "  \"prebuilt_libs\": " + json_array(prebuilt_libs_) + ",\n";

        json += "  \"binaries\": [";
        for (std::size_t i = 0; i < binaries_.size(); ++i) {
            if (i != 0) json += ",";
            json += "\n    {";
            json += "\"name\": " + json_string(binaries_[i].name);
            json += ", \"sources\": [";
            const auto& srcs = binaries_[i].source_groups;
            for (std::size_t j = 0; j < srcs.size(); ++j) {
                if (j != 0) json += ",";
                json += "{\"pattern\": " + json_string(srcs[j].pattern);
                json += ", \"visibility\": " +
                        json_string(srcs[j].is_public ? "public" : "private");
                json += "}";
            }
            json += "]";
            json += ", \"include_dirs\": " +
                    json_array(binaries_[i].include_dirs_);
            json += "}";
        }
        json += binaries_.empty() ? "],\n" : "\n  ],\n";

        json += "  \"tests\": [";
        for (std::size_t i = 0; i < tests_.size(); ++i) {
            if (i != 0) json += ",";
            json += "\n    {";
            json += "\"name\": " + json_string(tests_[i].name);
            json += ", \"binary\": " + json_string(tests_[i].binary);
            json += ", \"is_default\": " +
                    std::string(tests_[i].is_default ? "true" : "false");
            json += "}";
        }
        json += tests_.empty() ? "]\n" : "\n  ]\n";

        json += ",\n";
        json += "  \"used_source_deps\": " +
                json_array(std::vector<std::string>(
                    used_source_deps_.begin(), used_source_deps_.end())) +
                "\n";

        json += "}\n";
        return json;
    }

    std::string id_;
    std::string name_;
    std::string version_;
    std::string type_ = "executable";
    std::string cxx_std_ = "c++23";
    std::string c_std_ = "c17";
    std::string source_dir_;
    std::string build_dir_;
    std::string target_;
    std::string declaration_path_;
    std::string declaration_features_ = "[]";
    std::string declaration_dependencies_ = "[]";
    std::vector<SourceGroup> source_groups_;
    std::vector<std::string> include_dirs_;
    std::vector<std::string> public_include_dirs_;
    std::vector<std::string> prebuilt_libs_;
    std::vector<BinaryBuilder> binaries_;
    std::vector<TestRegistration> tests_;
    std::set<std::string> features_;
    std::map<std::string, std::string> dep_dirs_;
    std::set<std::string> used_source_deps_;
};

} // namespace bake
