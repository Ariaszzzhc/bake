module;
export module bake.stage0.semver;

// Semantic versions and the requirement syntax bake accepts on
// dependency edges (`version = "..."`):
//
//   "1.2.3"   exact        = 1.2.3
//   "^1.2.3"  caret        >= 1.2.3 < 2.0.0   (^0.2.3 → < 0.3.0,
//                                               ^0.0.3 → < 0.0.4)
//   "~1.2.3"  tilde        >= 1.2.3 < 1.3.0
//   "1.2"     partial      >= 1.2.0 < 1.3.0   ("1" → < 2.0.0)
//   "*"       any
//
// Prerelease handling follows node-semver: a prerelease candidate
// satisfies a requirement only when the requirement's operand carries a
// prerelease of the same major.minor.patch — so "^1.2.3" never matches
// "1.3.0-beta" while "^1.2.3-beta.1" matches "1.2.3-beta.4".

import std;

export namespace bake::semver {

constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool alnum_(char c) {
    return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool space_(char c) { return c == ' ' || c == '\t'; }

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::vector<std::string> pre;  // dot-separated prerelease identifiers
    std::string build;             // build metadata; ignored in precedence

    bool is_release() const { return pre.empty(); }

    // semver 2.0.0 precedence. Build metadata is ignored.
    std::strong_ordering operator<=>(const Version& o) const {
        if (auto c = major <=> o.major; c != 0) return c;
        if (auto c = minor <=> o.minor; c != 0) return c;
        if (auto c = patch <=> o.patch; c != 0) return c;
        // A release outranks any prerelease of the same triple.
        if (pre.empty() || o.pre.empty())
            return pre.empty() <=> o.pre.empty();  // false < true
        for (std::size_t i = 0; i < pre.size() && i < o.pre.size(); ++i) {
            const bool a_num = digit(pre[i][0]);
            const bool b_num = digit(o.pre[i][0]);
            if (a_num != b_num)
                return b_num <=> a_num;  // numeric identifiers rank lower
            if (a_num) {
                // Numeric identifiers compare numerically; parse has
                // already rejected leading zeros.
                unsigned long long a = std::stoull(pre[i]);
                unsigned long long b = std::stoull(o.pre[i]);
                if (a != b) return a <=> b;
            } else if (auto c = pre[i] <=> o.pre[i]; c != 0) {
                return c;
            }
        }
        return pre.size() <=> o.pre.size();  // longer prefix ranks higher
    }

    bool operator==(const Version& o) const {
        return major == o.major && minor == o.minor && patch == o.patch &&
               pre == o.pre;
    }

    std::string to_string() const {
        std::string s = std::format("{}.{}.{}", major, minor, patch);
        for (std::size_t i = 0; i < pre.size(); ++i) {
            s += i == 0 ? "-" : ".";
            s += pre[i];
        }
        if (!build.empty()) s += "+", s += build;
        return s;
    }

    // Parses a strict semver version with optional leading 'v', optional
    // prerelease and build metadata. Wildcards are rejected (Req handles
    // them); all three numbers are required.
    static std::optional<Version> parse(std::string_view text) {
        std::string body(text);
        if (!body.empty() && (body.front() == 'v' || body.front() == 'V'))
            body.erase(body.begin());
        if (body.empty()) return std::nullopt;

        // Split off build metadata, then prerelease.
        std::string build, pre;
        if (std::size_t plus = body.find('+'); plus != std::string::npos) {
            build = body.substr(plus + 1);
            if (build.empty()) return std::nullopt;
            body.resize(plus);
        }
        if (std::size_t dash = body.find('-'); dash != std::string::npos) {
            pre = body.substr(dash + 1);
            if (pre.empty()) return std::nullopt;
            body.resize(dash);
        }

        Version v;
        v.build = build;

        if (!pre.empty()) {
            std::size_t pos = 0;
            while (true) {
                std::size_t dot = pre.find('.', pos);
                std::string ident = pre.substr(
                    pos, dot == std::string::npos ? std::string::npos
                                                  : dot - pos);
                if (ident.empty()) return std::nullopt;
                bool numeric = digit(ident[0]);
                for (char c : ident) {
                    if (numeric && !digit(c)) return std::nullopt;
                    if (!alnum_(c) && c != '-') return std::nullopt;
                }
                if (numeric && ident.size() > 1 && ident[0] == '0')
                    return std::nullopt;  // leading zero
                v.pre.push_back(std::move(ident));
                if (dot == std::string::npos) break;
                pos = dot + 1;
            }
        }

        // Numeric triple, no leading zeros, exactly three components.
        int* fields[3] = {&v.major, &v.minor, &v.patch};
        std::size_t pos = 0;
        for (int i = 0; i < 3; ++i) {
            std::size_t dot = body.find('.', pos);
            std::string num = body.substr(
                pos, dot == std::string::npos ? std::string::npos
                                              : dot - pos);
            if (num.empty() || num.size() > 9) return std::nullopt;
            for (char c : num)
                if (!digit(c)) return std::nullopt;
            if (num.size() > 1 && num[0] == '0') return std::nullopt;
            *fields[i] = std::stoi(num);
            if (dot == std::string::npos) {
                if (i < 2) return std::nullopt;  // missing minor/patch
                pos += num.size();
                break;
            }
            pos = dot + 1;
        }
        if (pos != body.size()) return std::nullopt;  // trailing junk
        return v;
    }
};

struct Req {
    enum class Kind { Exact, Caret, Tilde, Any };
    Kind kind = Kind::Any;
    Version lo;     // operand; partials are zero-filled
    int given = 3;  // numeric components the operand named (1..3)
    std::string text;

    // Upper bound (exclusive) implied by the requirement. nullopt for
    // Any and Exact.
    std::optional<Version> upper() const {
        switch (kind) {
            case Kind::Any:
            case Kind::Exact:
                return std::nullopt;
            case Kind::Caret:
                // Bump the leftmost nonzero component; missing trailing
                // components are wildcards, so an all-zero prefix bumps
                // at the width the operand named (^0 → <1.0.0,
                // ^0.0 → <0.1.0, ^0.0.3 → <0.0.4).
                if (lo.major > 0 || given == 1)
                    return Version{lo.major + 1, 0, 0};
                if (lo.minor > 0 || given == 2)
                    return Version{0, lo.minor + 1, 0};
                return Version{0, 0, lo.patch + 1};
            case Kind::Tilde:
                // ~1 → <2.0.0; ~1.2(.x) → <1.3.0; ~1.2.3 → <1.3.0.
                if (given == 1) return Version{lo.major + 1, 0, 0};
                return Version{lo.major, lo.minor + 1, 0};
        }
        return std::nullopt;
    }

    // Interval membership only — no prerelease gating. Whether a
    // prerelease candidate may match at all is a property of the whole
    // constraint set: see satisfies_all.
    bool satisfies(const Version& v) const {
        if (kind == Kind::Any) return true;
        if (kind == Kind::Exact) return v == lo;
        if (v < lo) return false;
        if (auto hi = upper()) return v < *hi;
        return true;
    }

    // node-semver's prerelease rule, per comparator: this requirement
    // vouches for prereleases of its operand's triple.
    bool admits_prerelease_of(const Version& v) const {
        return !lo.is_release() && lo.major == v.major &&
               lo.minor == v.minor && lo.patch == v.patch;
    }

    static std::optional<Req> parse(std::string_view text) {
        Req r;
        r.text = std::string(text);

        std::string body(text);
        while (!body.empty() && space_(body.front())) body.erase(body.begin());
        while (!body.empty() && space_(body.back())) body.pop_back();
        if (body.empty()) return std::nullopt;

        Kind kind = Kind::Exact;
        if (body.front() == '=') {
            body.erase(body.begin());
        } else if (body.front() == '^') {
            kind = Kind::Caret;
            body.erase(body.begin());
        } else if (body.front() == '~') {
            kind = Kind::Tilde;
            body.erase(body.begin());
        } else if (body.find_first_of(" \t<>") != std::string::npos) {
            return std::nullopt;  // comparator lists are not supported
        }
        while (!body.empty() && (body.front() == 'v' || body.front() == 'V'))
            body.erase(body.begin());
        if (body.empty()) return std::nullopt;

        if (body == "*" || body == "x" || body == "X") {
            r.kind = Kind::Any;
            return r;
        }

        // Tail (prerelease/build) belongs to the full operand only.
        std::string tail;
        if (std::size_t cut = body.find_first_of("-+");
            cut != std::string::npos) {
            tail = body.substr(cut);
            body.resize(cut);
        }

        // Wildcards in minor/patch positions: "1.x" ≡ "1", "1.2.x" ≡
        // "1.2". They cannot coexist with a tail.
        bool wild = false;
        {
            std::size_t dot = body.find('.');
            if (dot != std::string::npos) {
                std::string rest = body.substr(dot + 1);
                if (rest == "*" || rest == "x" || rest == "X") {
                    wild = true;
                    body.resize(dot);
                } else if (std::size_t dot2 = rest.find('.');
                           dot2 != std::string::npos) {
                    std::string last = rest.substr(dot2 + 1);
                    if (last == "*" || last == "x" || last == "X") {
                        wild = true;
                        body.resize(dot + dot2 + 1);
                    }
                }
            }
        }
        if (wild && !tail.empty()) return std::nullopt;

        // Count the numeric components, zero-fill partials, and delegate
        // strict parsing (including the tail) to Version::parse.
        int given = 1;
        for (char c : body)
            if (c == '.') ++given;
        if (given > 3) return std::nullopt;
        std::string padded = body;
        for (int i = given; i < 3; ++i) padded += ".0";

        auto lo = Version::parse(padded + tail);
        if (!lo) return std::nullopt;

        r.kind = kind;
        r.lo = *lo;
        r.given = given;
        // A bare partial ("1.2", "1") ranges like tilde.
        if (kind == Kind::Exact && given < 3) r.kind = Kind::Tilde;
        return r;
    }
};

// Evaluate a constraint set. Interval membership is required of every
// requirement; a prerelease candidate additionally needs one requirement
// whose operand carries a prerelease of the same major.minor.patch
// (node-semver's rule). `*` alone therefore never matches prereleases,
// while {=1.5.7-r1, ^1.5} admits 1.5.7-r1.
inline bool satisfies_all(const Version& v, const std::vector<Req>& reqs) {
    if (!v.is_release()) {
        bool admitted = false;
        for (const auto& req : reqs)
            if (req.admits_prerelease_of(v)) admitted = true;
        if (!admitted) return false;
    }
    for (const auto& req : reqs)
        if (!req.satisfies(v)) return false;
    return true;
}

}  // namespace bake::semver
