#pragma once

// A small JSON reader for what the character importer prints (launcher.py
// status and owned): one document in, a tree of values out. Reading a member
// that is missing, or of another type, gives the fallback, so a newer or
// older importer with other fields still reads.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher::json {

class Value {
public:
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::kNull; }
    bool is_string() const { return type_ == Type::kString; }
    bool is_array() const { return type_ == Type::kArray; }
    bool is_object() const { return type_ == Type::kObject; }

    bool boolean(bool fallback = false) const { return type_ == Type::kBool ? bool_ : fallback; }
    double number(double fallback = 0) const { return type_ == Type::kNumber ? number_ : fallback; }
    std::string string(std::string_view fallback = {}) const {
        return type_ == Type::kString ? string_ : std::string(fallback);
    }

    // An array's items (none for anything else).
    const std::vector<Value>& items() const { return type_ == Type::kArray ? items_ : empty(); }
    // An object's member names and values, in the document's order.
    const std::vector<std::string>& keys() const { return keys_; }
    const std::vector<Value>& values() const { return type_ == Type::kObject ? items_ : empty(); }

    // A member (the last one of that name), or null.
    const Value& operator[](std::string_view key) const;
    // An array item, or null.
    const Value& operator[](size_t index) const;

private:
    friend class Parser;
    static const std::vector<Value>& empty();

    Type type_ = Type::kNull;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> items_; // an array's items, an object's values
    std::vector<std::string> keys_;
};

// One JSON document, white space around it allowed. None when it is not
// valid JSON, with where and why in `error`.
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

} // namespace launcher::json
