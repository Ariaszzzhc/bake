export module bake.json;

import std;

// ============================================================
// bake.json — bake's own JSON DOM.
//
// The interface owns the data model and every inline accessor.
// Parsing and serialization are stage-selected implementation
// units of this same module:
//
//   stage0/json.cpp   hand-written parser + serializer (CMake bootstrap)
//   json.cpp          nlohmann/json-backed (bake-pkgs "json" package)
// ============================================================

export namespace bake::json {

class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class Value;

struct ArrayRepr {
    std::vector<Value> values;
};

struct ObjectRepr {
    std::vector<std::pair<std::string, Value>> entries;
};

class Value {
  public:
    // ── construction ──
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v)
        : kind_(Kind::Boolean), boolean_(v) {}
    Value(int v)
        : kind_(Kind::Integer), integer_(v) {}
    Value(std::int64_t v)
        : kind_(Kind::Integer), integer_(v) {}
    Value(std::size_t v)
        : kind_(Kind::Integer), integer_(static_cast<std::int64_t>(v)) {}
    Value(double v)
        : kind_(Kind::Double), double_(v) {}
    Value(const char* v)
        : kind_(Kind::String), string_(v) {}
    Value(std::string_view v)
        : kind_(Kind::String), string_(v) {}
    Value(const std::string& v)
        : kind_(Kind::String), string_(v) {}

    // Any vector of scalars becomes an array (covers vector<string>,
    // vector<std::size_t>, ... — everything the codecs serialize).
    template <class T>
    Value(const std::vector<T>& items)
        : kind_(Kind::Array),
          array_(std::make_unique<ArrayRepr>()) {
        array_->values.reserve(items.size());
        for (const auto& item : items) array_->values.emplace_back(item);
    }

    // Brace literal: all-2-element-arrays-with-string-head → object,
    // any other shape → array (nlohmann semantics).
    Value(std::initializer_list<Value> items) {
        bool as_object = items.size() > 0;
        for (const Value& item : items) {
            if (!(item.kind_ == Kind::Array &&
                  item.array_->values.size() == 2 &&
                  item.array_->values.front().kind_ == Kind::String)) {
                as_object = false;
                break;
            }
        }
        if (as_object) {
            kind_ = Kind::Object;
            object_ = std::make_unique<ObjectRepr>();
            for (const Value& item : items) {
                object_->entries.emplace_back(
                    item.array_->values.front().string_,
                    item.array_->values.back());
            }
        } else {
            kind_ = Kind::Array;
            array_ = std::make_unique<ArrayRepr>();
            array_->values.reserve(items.size());
            for (const Value& item : items) array_->values.push_back(item);
        }
    }

    static Value object() {
        Value v;
        v.kind_ = Kind::Object;
        v.object_ = std::make_unique<ObjectRepr>();
        return v;
    }
    static Value array() {
        Value v;
        v.kind_ = Kind::Array;
        v.array_ = std::make_unique<ArrayRepr>();
        return v;
    }

    // Deep copy (unique_ptr storage deletes the implicit ones).
    Value(const Value& other);
    Value& operator=(const Value& other);
    Value(Value&& other) noexcept = default;
    Value& operator=(Value&& other) noexcept = default;
    ~Value() = default;

    // Throws Error on invalid input. Defined per stage.
    static Value parse(std::string_view text);

    // ── type tests ──
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_boolean() const { return kind_ == Kind::Boolean; }
    bool is_integer() const { return kind_ == Kind::Integer; }
    bool is_number() const {
        return kind_ == Kind::Integer || kind_ == Kind::Double;
    }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_object() const { return kind_ == Kind::Object; }

    // ── conversion ──
    // T ∈ {bool, std::int64_t, int, double, std::string,
    //      std::vector<std::string>}; wrong kind throws.
    template <class T>
    T get() const {
        if constexpr (std::is_same_v<T, bool>) {
            require_boolean();
            return boolean_;
        } else if constexpr (std::is_same_v<T, std::int64_t> ||
                             std::is_same_v<T, int>) {
            require_integer();
            return static_cast<T>(integer_);
        } else if constexpr (std::is_same_v<T, double>) {
            require_number();
            return kind_ == Kind::Integer
                ? static_cast<double>(integer_)
                : double_;
        } else if constexpr (std::is_same_v<T, std::string>) {
            require_string();
            return string_;
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            if (kind_ != Kind::Array)
                throw Error("bake.json: expected an array of strings");
            std::vector<std::string> out;
            out.reserve(array_->values.size());
            for (const Value& item : array_->values)
                out.push_back(item.get<std::string>());
            return out;
        } else {
            static_assert(
                !sizeof(T*),
                "bake.json get<T>: T must be bool, std::int64_t, int, "
                "double, std::string or std::vector<std::string>");
        }
    }

    // ── object access ──
    bool contains(std::string_view key) const {
        return find(key) != nullptr;
    }

    // nullptr when absent or receiver is not an object.
    const Value* find(std::string_view key) const {
        if (kind_ != Kind::Object) return nullptr;
        for (const auto& [name, value] : object_->entries)
            if (name == key) return &value;
        return nullptr;
    }

    // Null converts to an object; missing keys insert null.
    Value& operator[](std::string_view key) {
        if (kind_ == Kind::Null) {
            kind_ = Kind::Object;
            object_ = std::make_unique<ObjectRepr>();
        }
        if (kind_ != Kind::Object)
            throw Error("bake.json: object index on a non-object value");
        for (auto& [name, value] : object_->entries)
            if (name == key) return value;
        object_->entries.emplace_back(std::string(key), Value());
        return object_->entries.back().second;
    }

    // Absent keys (or non-object receivers) yield a null sentinel.
    const Value& operator[](std::string_view key) const {
        const Value* found = find(key);
        return found ? *found : absent();
    }

    template <class T>
    T value(std::string_view key, const T& fallback) const {
        const Value* found = find(key);
        return found ? found->get<T>() : fallback;
    }
    std::string value(std::string_view key, const char* fallback) const {
        const Value* found = find(key);
        return found ? found->get<std::string>() : std::string(fallback);
    }

    // ── sizing ──
    std::size_t size() const {
        switch (kind_) {
            case Kind::Array:  return array_->values.size();
            case Kind::Object: return object_->entries.size();
            default:           return 0;
        }
    }

    // ── array access ──
    // Out-of-range or non-array receivers throw.
    Value& operator[](std::size_t index) {
        if (kind_ != Kind::Array || index >= array_->values.size())
            throw Error("bake.json: array index out of range");
        return array_->values[index];
    }
    const Value& operator[](std::size_t index) const {
        if (kind_ != Kind::Array || index >= array_->values.size())
            throw Error("bake.json: array index out of range");
        return array_->values[index];
    }

    // Null converts to an array; non-arrays throw.
    void push_back(Value item) {
        if (kind_ == Kind::Null) {
            kind_ = Kind::Array;
            array_ = std::make_unique<ArrayRepr>();
        }
        if (kind_ != Kind::Array)
            throw Error("bake.json: push_back on a non-array value");
        array_->values.push_back(std::move(item));
    }

    // ── iteration ──
    // Arrays expose their elements; anything else iterates empty.
    const Value* begin() const {
        return kind_ == Kind::Array ? array_->values.data() : nullptr;
    }
    const Value* end() const {
        return kind_ == Kind::Array
            ? array_->values.data() + array_->values.size()
            : nullptr;
    }

    class Member {
      public:
        const std::string& key() const { return *key_; }
        const Value& value() const { return *value_; }
      private:
        friend class Value;
        Member() = default;
        Member(const std::string* key, const Value* value)
            : key_(key), value_(value) {}
        const std::string* key_ = nullptr;
        const Value* value_ = nullptr;
    };

    class Items {
      public:
        using Base =
            std::vector<std::pair<std::string, Value>>::const_iterator;

        class Iterator {
          public:
            const Member& operator*() const { return member_; }
            const Member* operator->() const { return &member_; }
            Iterator& operator++() {
                ++it_;
                sync();
                return *this;
            }
            bool operator!=(const Iterator& other) const {
                return it_ != other.it_;
            }
          private:
            friend class Items;
            Iterator(Base it, Base end) : it_(it), end_(end) { sync(); }
            void sync() {
                if (it_ != end_)
                    member_ = Member(&it_->first, &it_->second);
            }
            Base it_;
            Base end_;
            Member member_{};
        };

        Iterator begin() const { return Iterator(begin_, end_); }
        Iterator end() const { return Iterator(end_, end_); }
      private:
        friend class Value;
        Items(Base b, Base e) : begin_(b), end_(e) {}
        Base begin_;
        Base end_;
    };

    // Object member iteration; non-objects iterate empty.
    Items items() const {
        static const std::vector<std::pair<std::string, Value>> empty = {};
        const auto& entries =
            kind_ == Kind::Object ? object_->entries : empty;
        return Items(entries.begin(), entries.end());
    }

    // ── serialization ──
    // indent < 0 → compact; otherwise nlohmann-style pretty print.
    // Defined per stage.
    std::string dump(int indent = -1) const;

  private:
    enum class Kind : std::uint8_t {
        Null, Boolean, Integer, Double, String, Array, Object,
    };

    void require_boolean() const {
        if (kind_ != Kind::Boolean)
            throw Error("bake.json: expected a boolean value");
    }
    void require_integer() const {
        if (kind_ != Kind::Integer)
            throw Error("bake.json: expected an integer value");
    }
    void require_number() const {
        if (!is_number())
            throw Error("bake.json: expected a number value");
    }
    void require_string() const {
        if (kind_ != Kind::String)
            throw Error("bake.json: expected a string value");
    }

    static const Value& absent() {
        static const Value sentinel;
        return sentinel;
    }

    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    std::int64_t integer_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::unique_ptr<ArrayRepr> array_;
    std::unique_ptr<ObjectRepr> object_;
};

// ── deep copy (defined with the Reprs and Value complete) ──

inline Value::Value(const Value& other)
    : kind_(other.kind_),
      boolean_(other.boolean_),
      integer_(other.integer_),
      double_(other.double_),
      string_(other.string_) {
    if (other.array_) array_ = std::make_unique<ArrayRepr>(*other.array_);
    if (other.object_) object_ = std::make_unique<ObjectRepr>(*other.object_);
}

inline Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

}  // namespace bake::json
