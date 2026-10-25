// The JSON reader (see json.h): recursive descent over RFC 8259's grammar,
// strictly (no comments, no trailing commas), with a depth limit so that
// no input can run the stack out.

#include "json.h"

#include <charconv>
#include <cstdint>

namespace launcher::json {

const std::vector<Value>& Value::empty() {
    static const std::vector<Value> none;
    return none;
}

const Value& Value::operator[](std::string_view key) const {
    static const Value null;
    if (type_ != Type::kObject) return null;
    for (size_t i = keys_.size(); i-- > 0;) {
        if (keys_[i] == key) return items_[i];
    }
    return null;
}

const Value& Value::operator[](size_t index) const {
    static const Value null;
    return type_ == Type::kArray && index < items_.size() ? items_[index] : null;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> document(std::string* error) {
        Value value;
        space();
        if (parse(value, 0)) {
            space();
            if (at_ == text_.size()) return value;
            fail("text after the document");
        }
        if (error) *error = "at byte " + std::to_string(at_) + ": " + why_;
        return std::nullopt;
    }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(const char* why) {
        if (why_.empty()) why_ = why;
        return false;
    }

    void space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return fail("unknown word");
        at_ += word.size();
        return true;
    }

    bool parse(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deep");
        if (at_ >= text_.size()) return fail("a value is missing");
        switch (text_[at_]) {
        case 'n': return literal("null");
        case 't':
            out.type_ = Value::Type::kBool;
            out.bool_ = true;
            return literal("true");
        case 'f':
            out.type_ = Value::Type::kBool;
            return literal("false");
        case '"': out.type_ = Value::Type::kString; return string(out.string_);
        case '[': return array(out, depth);
        case '{': return object(out, depth);
        default: return number(out);
        }
    }

    bool array(Value& out, int depth) {
        out.type_ = Value::Type::kArray;
        ++at_;
        space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return true;
        }
        for (;;) {
            out.items_.emplace_back();
            space();
            if (!parse(out.items_.back(), depth + 1)) return false;
            space();
            if (at_ >= text_.size()) return fail("an array is not closed");
            const char c = text_[at_++];
            if (c == ']') return true;
            if (c != ',') return fail("expected , or ] in an array");
        }
    }

    bool object(Value& out, int depth) {
        out.type_ = Value::Type::kObject;
        ++at_;
        space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return true;
        }
        for (;;) {
            space();
            if (at_ >= text_.size() || text_[at_] != '"') return fail("expected a member name");
            out.keys_.emplace_back();
            if (!string(out.keys_.back())) return false;
            space();
            if (at_ >= text_.size() || text_[at_++] != ':') return fail("expected : after a member name");
            space();
            out.items_.emplace_back();
            if (!parse(out.items_.back(), depth + 1)) return false;
            space();
            if (at_ >= text_.size()) return fail("an object is not closed");
            const char c = text_[at_++];
            if (c == '}') return true;
            if (c != ',') return fail("expected , or } in an object");
        }
    }

    static void utf8(uint32_t code, std::string& out) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | code >> 6);
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | code >> 12);
            out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | code >> 18);
            out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(uint32_t& code) {
        if (at_ + 4 > text_.size()) return fail("a \\u escape is cut short");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("a \\u escape is not hexadecimal");
        }
        return true;
    }

    bool string(std::string& out) {
        ++at_; // the opening quote
        for (;;) {
            if (at_ >= text_.size()) return fail("a string is not closed");
            const char c = text_[at_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("a control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) return fail("a string is not closed");
            switch (text_[at_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t code = 0;
                if (!hex4(code)) return false;
                // A character past U+FFFF comes as a pair of surrogates; a
                // lone one becomes U+FFFD rather than invalid UTF-8.
                if (code >= 0xD800 && code < 0xDC00 && text_.substr(at_, 2) == "\\u") {
                    const size_t back = at_;
                    at_ += 2;
                    uint32_t low = 0;
                    if (!hex4(low)) return false;
                    if (low >= 0xDC00 && low < 0xE000) code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    else at_ = back;
                }
                if (code >= 0xD800 && code < 0xE000) code = 0xFFFD;
                utf8(code, out);
                break;
            }
            default: return fail("an unknown escape in a string");
            }
        }
    }

    bool number(Value& out) {
        const size_t start = at_;
        const auto digits = [&] {
            const size_t from = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            return at_ > from;
        };
        if (at_ < text_.size() && text_[at_] == '-') ++at_;
        if (at_ < text_.size() && text_[at_] == '0') ++at_;
        else if (!digits()) return fail("not a value");
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (!digits()) return fail("a number has no digits after its point");
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
            if (!digits()) return fail("a number has no digits in its exponent");
        }
        out.type_ = Value::Type::kNumber;
        const auto [end, ec] = std::from_chars(text_.data() + start, text_.data() + at_, out.number_);
        if (ec == std::errc::result_out_of_range) return fail("a number out of range");
        return ec == std::errc() && end == text_.data() + at_ ? true : fail("not a number");
    }

    std::string_view text_;
    size_t at_ = 0;
    std::string why_;
};

std::optional<Value> parse(std::string_view text, std::string* error) { return Parser(text).document(error); }

} // namespace launcher::json
