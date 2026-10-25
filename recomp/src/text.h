#pragma once

// text::format: std::format's replacement fields for what the recompiler
// writes. GCC 16's libstdc++ keeps part of std::format's code in its shared
// library (GLIBCXX_3.4.35/36), so a program using it needs GCC 16's to run,
// and cafe-recomp runs on players' PCs with older ones (the launcher's kit,
// linked to the system's libstdc++: a static one would be the build PC's,
// compiled for its processor).
//
// A field is {} or {N}, with an optional :[0][width][type]: type x or X
// (hexadecimal) or d for whole numbers, s for text (bools are true and
// false); {{ and }} are braces.
// Numbers are right-aligned, zeros going after the sign; text is
// left-aligned. Anything else throws std::invalid_argument.

#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>

namespace cafe::text {

struct Arg {
    enum class Kind { text, character, number } kind;
    std::string_view text;
    char character = 0;
    unsigned long long magnitude = 0; // a number's absolute value
    bool negative = false;
};

template <typename T>
Arg arg(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return {Arg::Kind::text, value ? "true" : "false"};
    } else if constexpr (std::is_same_v<T, char>) {
        return {Arg::Kind::character, {}, value};
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) {
            const bool negative = value < 0;
            // (the magnitude of the most negative value too)
            const unsigned long long magnitude =
                negative ? 0ull - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
            return {Arg::Kind::number, {}, 0, magnitude, negative};
        } else {
            return {Arg::Kind::number, {}, 0, value, false};
        }
    } else {
        return {Arg::Kind::text, std::string_view(value)};
    }
}

std::string format_args(std::string_view pattern, std::initializer_list<Arg> args);

template <typename... Args>
std::string format(std::string_view pattern, const Args&... args) {
    return format_args(pattern, {arg(args)...});
}

} // namespace cafe::text
