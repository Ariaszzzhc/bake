export module bake.toml;

import std;

// ============================================================
// bake.toml — bake's own TOML reader (manifest subset).
//
// The interface owns the data model; parsing is a stage-selected
// implementation unit of this same module:
//
//   stage0/toml.cpp   hand-written subset parser (CMake bootstrap)
//   toml.cpp          toml++-backed (bake-pkgs "tomlplusplus" package)
//
// Supported subset — everything bake.toml uses: [table] headers
// (dotted, quoted segments), bare/quoted keys, basic and literal
// strings, integers, floats, booleans, arrays (multiline, trailing
// comma) and inline tables. Dates, multiline strings, [[array
// tables]] and dotted-key assignments are rejected with positions.
//
// Layout note: Table precedes Node (its entries hold Node by value
// through vector's deferred instantiation); every Table method that
// touches entry internals is defined out-of-line, after Node is
// complete. Node holds Table via unique_ptr, so Table is complete
// everywhere Node needs it.
// ============================================================

export namespace bake::toml {

class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class Node;
class Table;

using Array = std::vector<Node>;

class Table {
  public:
    Table() = default;

    // Absent keys yield a null Node — checked the same way toml++
    // node_views behave: as_table()/as_array() → nullptr,
    // value<T>() → nullopt. (Defined after Node, below.)
    const Node& operator[](std::string_view key) const;

    // nullptr when absent.
    const Node* get(std::string_view key) const;

    bool contains(std::string_view key) const { return get(key) != nullptr; }
    std::size_t size() const;
    bool empty() const;

    // Structured binding over (const std::string&, const Node&) pairs.
    std::vector<std::pair<std::string, Node>>::const_iterator begin() const;
    std::vector<std::pair<std::string, Node>>::const_iterator end() const;

  private:
    friend Table make_table(std::vector<std::pair<std::string, Node>> entries);
    friend void table_insert(Table& table, std::string key, Node value);
    friend std::pair<std::string, Node>* table_find(Table& table,
                                                    std::string_view key);

    std::vector<std::pair<std::string, Node>> entries_;
};

class Node {
  public:
    Node() = default;
    Node(bool v) : kind_(Kind::Boolean), boolean_(v) {}
    Node(std::int64_t v) : kind_(Kind::Integer), integer_(v) {}
    Node(double v) : kind_(Kind::Floating), floating_(v) {}
    Node(const char* v) : kind_(Kind::String), string_(v) {}
    Node(std::string_view v) : kind_(Kind::String), string_(v) {}
    Node(const std::string& v) : kind_(Kind::String), string_(v) {}
    Node(Array v) : kind_(Kind::Array), array_(std::move(v)) {}
    Node(Table v)
        : kind_(Kind::Table), table_(std::make_unique<Table>(std::move(v))) {}

    Node(const Node& other);
    Node& operator=(const Node& other);
    Node(Node&& other) noexcept = default;
    Node& operator=(Node&& other) noexcept = default;
    ~Node() = default;

    bool is_table() const { return kind_ == Kind::Table; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_integer() const { return kind_ == Kind::Integer; }
    bool is_floating() const { return kind_ == Kind::Floating; }
    bool is_boolean() const { return kind_ == Kind::Boolean; }

    // nullptr unless the node is a table/array.
    const Table* as_table() const {
        return kind_ == Kind::Table ? table_.get() : nullptr;
    }
    const Array* as_array() const {
        return kind_ == Kind::Array ? &array_ : nullptr;
    }

    // Chained lookup convenience: tbl["a"]["b"] on a table node; null
    // sentinel on anything else (toml++ node_view semantics).
    const Node& operator[](std::string_view key) const {
        static const Node sentinel;
        if (kind_ == Kind::Table) return (*table_)[key];
        return sentinel;
    }

    // Type-exact scalar access (toml++ semantics: value<int64_t>() on
    // a float is nullopt, not a conversion).
    template <class T>
    std::optional<T> value() const {
        if constexpr (std::is_same_v<T, std::string>) {
            if (kind_ == Kind::String) return string_;
        } else if constexpr (std::is_same_v<T, std::int64_t> ||
                             std::is_same_v<T, int>) {
            if (kind_ == Kind::Integer) return static_cast<T>(integer_);
        } else if constexpr (std::is_same_v<T, bool>) {
            if (kind_ == Kind::Boolean) return boolean_;
        } else if constexpr (std::is_same_v<T, double>) {
            if (kind_ == Kind::Floating) return floating_;
        } else {
            static_assert(
                !sizeof(T*),
                "bake.toml value<T>: T must be std::string, std::int64_t, "
                "int, bool or double");
        }
        return std::nullopt;
    }

  private:
    friend class Table;
    friend Table* node_table(Node& node);

    enum class Kind : std::uint8_t {
        Null, Boolean, Integer, Floating, String, Array, Table,
    };

    static const Node& absent() {
        static const Node sentinel;
        return sentinel;
    }

    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    std::int64_t integer_ = 0;
    double floating_ = 0.0;
    std::string string_;
    Array array_;
    std::unique_ptr<Table> table_;
};

// ── Table access (Node complete here) ──

inline const Node& Table::operator[](std::string_view key) const {
    const Node* found = get(key);
    return found ? *found : Node::absent();
}

inline const Node* Table::get(std::string_view key) const {
    for (const auto& [name, value] : entries_)
        if (name == key) return &value;
    return nullptr;
}

inline std::size_t Table::size() const { return entries_.size(); }
inline bool Table::empty() const { return entries_.empty(); }

inline std::vector<std::pair<std::string, Node>>::const_iterator
Table::begin() const {
    return entries_.begin();
}

inline std::vector<std::pair<std::string, Node>>::const_iterator
Table::end() const {
    return entries_.end();
}

// ── Node deep copy (unique_ptr storage deletes the implicit ones) ──

inline Node::Node(const Node& other)
    : kind_(other.kind_),
      boolean_(other.boolean_),
      integer_(other.integer_),
      floating_(other.floating_),
      string_(other.string_),
      array_(other.array_) {
    if (other.table_) table_ = std::make_unique<Table>(*other.table_);
}

inline Node& Node::operator=(const Node& other) {
    if (this != &other) {
        Node copy(other);
        *this = std::move(copy);
    }
    return *this;
}

// ── Module-internal construction surface for parse implementations ──
// (non-exported: invisible to importers, available to this module's
// implementation units)

inline Table make_table(std::vector<std::pair<std::string, Node>> entries) {
    Table table;
    table.entries_ = std::move(entries);
    return table;
}

inline void table_insert(Table& table, std::string key, Node value) {
    table.entries_.emplace_back(std::move(key), std::move(value));
}

inline std::pair<std::string, Node>* table_find(Table& table,
                                                std::string_view key) {
    for (auto& entry : table.entries_)
        if (entry.first == key) return &entry;
    return nullptr;
}

inline Table* node_table(Node& node) {
    return node.kind_ == Node::Kind::Table ? node.table_.get() : nullptr;
}

// Parses TOML text. `source` names the input in error messages.
// Throws Error on the first violation.
Table parse(std::string_view text, std::string_view source);

// Reads and parses a file. Throws Error on unreadable input too.
Table parse_file(const std::string& path);

}  // namespace bake::toml
