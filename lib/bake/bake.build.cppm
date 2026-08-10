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
//   BAKE_OPTIONS           — length-prefixed name/value build option records
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

    fs::path p(pattern);
    bool is_abs = p.is_absolute();

    if (is_abs) {
        // Absolute pattern: search in pattern's parent, match filename.
        fs::path search_dir = p.parent_path();
        std::string match_pat = p.filename().string();
        if (!fs::exists(search_dir)) return result;
        try {
            for (auto& entry : fs::recursive_directory_iterator(search_dir)) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), search_dir);
                if (glob_match(rel.generic_string(), match_pat))
                    result.push_back(entry.path().generic_string());
            }
        } catch (...) {}
    } else {
        // Relative pattern: search in base_dir, match path relative to base_dir.
        std::string full_pattern = pattern;
        if (full_pattern.starts_with("./"))
            full_pattern = full_pattern.substr(2);
        fs::path search_dir = fs::path(base_dir) / fs::path(full_pattern).parent_path();
        if (!fs::exists(search_dir)) return result;
        try {
            for (auto& entry : fs::recursive_directory_iterator(
                     fs::path(base_dir) / fs::path(full_pattern).parent_path())) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), base_dir);
                std::string rel_str = rel.generic_string();
                if (glob_match(rel_str, full_pattern))
                    result.push_back(rel_str);
            }
        } catch (...) {}
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::map<std::string, std::string> parse_options_env() {
    std::map<std::string, std::string> options;
    const char* env = std::getenv("BAKE_OPTIONS");
    if (!env) return options;

    std::string_view encoded(env);
    std::size_t cursor = 0;
    auto read_length = [&](std::size_t& length) {
        if (cursor >= encoded.size() || encoded[cursor] < '0' ||
            encoded[cursor] > '9') {
            return false;
        }
        length = 0;
        while (cursor < encoded.size() && encoded[cursor] >= '0' &&
               encoded[cursor] <= '9') {
            const std::size_t digit =
                static_cast<std::size_t>(encoded[cursor] - '0');
            if (length >
                (std::numeric_limits<std::size_t>::max() - digit) / 10) {
                return false;
            }
            length = length * 10 + digit;
            ++cursor;
        }
        if (cursor >= encoded.size() || encoded[cursor] != ':') return false;
        ++cursor;
        return true;
    };

    while (cursor < encoded.size()) {
        std::size_t name_length = 0;
        std::size_t value_length = 0;
        if (!read_length(name_length) || !read_length(value_length) ||
            name_length > encoded.size() - cursor) {
            return {};
        }
        std::string name(encoded.substr(cursor, name_length));
        cursor += name_length;
        if (value_length > encoded.size() - cursor) return {};
        options[std::move(name)] =
            std::string(encoded.substr(cursor, value_length));
        cursor += value_length;
    }
    return options;
}

} // anonymous namespace

// ============================================================
// Exported API
// ============================================================

export namespace bake {

struct SourceOptions {
    std::vector<std::string> flags;
    std::vector<std::string> defines;
    std::vector<std::string> include_dirs;
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
        if (const char* value = std::getenv("BAKE_SOURCE_DIR"))
            source_dir_ = value;
        if (const char* value = std::getenv("BAKE_BUILD_DIR"))
            build_dir_ = value;
        if (const char* value = std::getenv("BAKE_DECLARATION_PATH"))
            declaration_path_ = value;
        if (const char* value = std::getenv("BAKE_DECLARATION_OPTIONS"))
            declaration_options_ = value;
        if (const char* value = std::getenv("BAKE_DECLARATION_DEPENDENCIES"))
            declaration_dependencies_ = value;
        if (const char* value = std::getenv("BAKE_TARGET"))
            target_ = value;
        options_ = parse_options_env();

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

    Builder& executable(std::string name) {
        name_ = std::move(name); type_ = "executable"; return *this;
    }
    Builder& lib(std::string name) {
        name_ = std::move(name); type_ = "lib"; return *this;
    }
    Builder& dylib(std::string name) {
        name_ = std::move(name); type_ = "dylib"; return *this;
    }

    Builder& std(std::string_view version) {
        std_version_ = version;
        return *this;
    }

    Builder& sources(std::string_view pattern) {
        return sources(pattern, {});
    }
    Builder& sources(std::initializer_list<std::string_view> patterns) {
        for (auto pattern : patterns) sources(pattern);
        return *this;
    }
    Builder& sources(std::string_view pattern, const SourceOptions& options) {
        for (auto& file : expand_glob(std::string(pattern), source_dir_)) {
            source_groups_.push_back({std::move(file), false, options});
        }
        return *this;
    }
    Builder& sources(std::initializer_list<std::string_view> patterns,
                     const SourceOptions& options) {
        for (auto pattern : patterns) sources(pattern, options);
        return *this;
    }

    Builder& include_dirs(std::string_view directory) {
        include_dirs_.emplace_back(directory);
        return *this;
    }
    Builder& include_dirs(std::initializer_list<std::string_view> directories) {
        for (auto directory : directories) include_dirs_.emplace_back(directory);
        return *this;
    }

    Builder& define(std::string_view name, std::string_view value = "") {
        std::string definition(name);
        if (!value.empty()) definition += "=" + std::string(value);
        defines_.push_back(std::move(definition));
        return *this;
    }

    Builder& link_system(std::string_view library) {
        libraries_.emplace_back(library);
        return *this;
    }

    Builder& link_framework(std::string_view framework) {
        frameworks_.emplace_back(framework);
        return *this;
    }

    bool option_bool(std::string_view name) const {
        auto it = options_.find(std::string(name));
        return it != options_.end() &&
               (it->second == "true" || it->second == "1");
    }
    std::int64_t option_int(std::string_view name) const {
        auto it = options_.find(std::string(name));
        if (it == options_.end()) return 0;
        try { return std::stoll(it->second); } catch (...) { return 0; }
    }
    std::string_view option_str(std::string_view name) const {
        auto it = options_.find(std::string(name));
        return it != options_.end() ? std::string_view(it->second) : "";
    }

    std::string_view source_dir() const { return source_dir_; }
    std::string_view build_dir() const { return build_dir_; }
    std::string_view target() const { return target_; }

    std::string_view dep_src_dir(std::string_view name) const {
        auto it = dep_dirs_.find(std::string(name));
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
    struct SourceGroup {
        std::string pattern;
        bool is_public = false;
        SourceOptions options;
    };

    std::string serialize() const {
        std::string json;
        json += "{\n";
        json += "  \"id\": " + json_string(id_.empty() ? name_ : id_) + ",\n";
        json += "  \"name\": " + json_string(name_) + ",\n";
        json += "  \"version\": " + json_string(version_) + ",\n";
        json += "  \"type\": " + json_string(type_) + ",\n";
        json += "  \"root\": " + json_string(source_dir_) + ",\n";
        json += "  \"std\": " + json_string(std_version_) + ",\n";
        json += "  \"options\": " + declaration_options_ + ",\n";

        json += "  \"sources\": [";
        for (std::size_t i = 0; i < source_groups_.size(); ++i) {
            if (i != 0) json += ",";
            auto source_defines = source_groups_[i].options.defines;
            source_defines.insert(source_defines.end(),
                                  defines_.begin(), defines_.end());
            json += "\n    {\"pattern\": " +
                    json_string(source_groups_[i].pattern);
            json += ", \"visibility\": " +
                    json_string(source_groups_[i].is_public ? "public" : "private");
            json += ", \"flags\": " +
                    json_array(source_groups_[i].options.flags);
            json += ", \"defines\": " + json_array(source_defines);
            json += ", \"include_dirs\": " +
                    json_array(source_groups_[i].options.include_dirs) + "}";
        }
        json += source_groups_.empty() ? "],\n" : "\n  ],\n";

        json += "  \"public_include_dirs\": " + json_array(include_dirs_) + ",\n";
        json += "  \"link\": {\"libraries\": " + json_array(libraries_) +
                ", \"frameworks\": " + json_array(frameworks_) + "},\n";
        json += "  \"dependencies\": " + declaration_dependencies_ + "\n";
        json += "}\n";
        return json;
    }

    std::string id_;
    std::string name_;
    std::string version_;
    std::string type_ = "executable";
    std::string std_version_ = "c++23";
    std::string source_dir_;
    std::string build_dir_;
    std::string target_;
    std::string declaration_path_;
    std::string declaration_options_ = "{}";
    std::string declaration_dependencies_ = "[]";
    std::vector<SourceGroup> source_groups_;
    std::vector<std::string> include_dirs_;
    std::vector<std::string> defines_;
    std::vector<std::string> libraries_;
    std::vector<std::string> frameworks_;
    std::map<std::string, std::string> options_;
    std::map<std::string, std::string> dep_dirs_;
};

} // namespace bake
