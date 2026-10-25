#include "text.h"

#include <stdexcept>

namespace cafe::text {
namespace {

[[noreturn]] void bad(std::string_view pattern, std::string_view why) {
    throw std::invalid_argument("text::format(\"" + std::string(pattern) + "\"): " + std::string(why));
}

void write(std::string& out, const Arg& a, bool zeros, size_t width, char type, std::string_view pattern) {
    std::string body;
    if (a.kind == Arg::Kind::number) {
        if (type != 0 && type != 'd' && type != 'x' && type != 'X') bad(pattern, "a number's type is d, x or X");
        const char* digits = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        const unsigned base = type == 'x' || type == 'X' ? 16 : 10;
        unsigned long long v = a.magnitude;
        do {
            body.insert(body.begin(), digits[v % base]);
            v /= base;
        } while (v != 0);
        const size_t length = body.size() + (a.negative ? 1 : 0);
        if (length < width) body.insert(0, width - length, zeros ? '0' : ' ');
        if (a.negative) body.insert(zeros ? 0 : body.find_first_not_of(' '), 1, '-');
        out += body;
        return;
    }
    if (zeros) bad(pattern, "0 is for numbers");
    if (type != 0 && type != (a.kind == Arg::Kind::character ? 'c' : 's')) bad(pattern, "text's type is s");
    if (a.kind == Arg::Kind::character) {
        out += a.character;
        if (width > 1) out.append(width - 1, ' ');
    } else {
        out += a.text;
        if (width > a.text.size()) out.append(width - a.text.size(), ' ');
    }
}

} // namespace

std::string format_args(std::string_view pattern, std::initializer_list<Arg> args) {
    std::string out;
    out.reserve(pattern.size() + 16 * args.size());
    size_t next = 0;       // the next {} field's argument
    int numbered = -1;     // fields are all {} (0) or all {N} (1)
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '}') {
            if (i + 1 >= pattern.size() || pattern[i + 1] != '}') bad(pattern, "} without {");
            out += '}';
            ++i;
            continue;
        }
        if (c != '{') {
            out += c;
            continue;
        }
        if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        const size_t end = pattern.find('}', i);
        if (end == std::string_view::npos) bad(pattern, "{ without }");
        std::string_view field = pattern.substr(i + 1, end - i - 1);
        i = end;
        const size_t colon = field.find(':');
        const std::string_view id = field.substr(0, colon);
        std::string_view spec = colon == std::string_view::npos ? std::string_view() : field.substr(colon + 1);
        size_t index = 0;
        if (id.empty()) {
            if (numbered == 1) bad(pattern, "{} and {N} together");
            numbered = 0;
            index = next++;
        } else {
            if (numbered == 0) bad(pattern, "{} and {N} together");
            numbered = 1;
            for (char d : id) {
                if (d < '0' || d > '9') bad(pattern, "a field's number");
                index = index * 10 + static_cast<size_t>(d - '0');
            }
        }
        if (index >= args.size()) bad(pattern, "more fields than arguments");
        bool zeros = false;
        if (!spec.empty() && spec[0] == '0') {
            zeros = true;
            spec.remove_prefix(1);
        }
        size_t width = 0;
        while (!spec.empty() && spec[0] >= '0' && spec[0] <= '9') {
            width = width * 10 + static_cast<size_t>(spec[0] - '0');
            spec.remove_prefix(1);
        }
        char type = 0;
        if (!spec.empty()) {
            type = spec[0];
            spec.remove_prefix(1);
        }
        if (!spec.empty()) bad(pattern, "a field's format");
        write(out, args.begin()[index], zeros, width, type, pattern);
    }
    return out;
}

} // namespace cafe::text
