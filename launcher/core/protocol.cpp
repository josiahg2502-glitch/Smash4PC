// The progress lines and the bar made of them (see protocol.h).

#include "protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>

namespace launcher {
namespace {

// The builder's steps in their order, with a share of the whole bar each
// (about the share of the time they take: compiling and copying the game's
// 16 GB are most of it).
struct BuildStep {
    const char* id;
    const char* text; // when the builder gives none
    double weight;
};
constexpr std::array kBuildSteps = {
    BuildStep{"check", "Checking the game", 1},
    BuildStep{"extract", "Reading the game's code", 2},
    BuildStep{"recompile", "Translating the game's code", 4},
    BuildStep{"compile", "Compiling the game program", 55},
    BuildStep{"link", "Linking the game program", 4},
    BuildStep{"copy", "Copying the game", 32},
    BuildStep{"finish", "Finishing", 2},
};

// The share of the bar the fighters take; what they share comes last.
constexpr double kFighters = 0.95;

// The word at the start of `s`, and `s` after it and the spaces that follow.
std::string_view word(std::string_view& s) {
    const size_t space = std::min(s.find(' '), s.size());
    const std::string_view w = s.substr(0, space);
    s.remove_prefix(space);
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return w;
}

bool number(std::string_view text, uint64_t& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

} // namespace

Line parse_line(std::string_view text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    Line line;
    line.text = std::string(text);
    if (!text.starts_with('@')) return line;
    std::string_view rest = text.substr(1);
    const std::string_view w = word(rest);
    Line out;
    if (w == "step" && !rest.empty()) {
        out.kind = Line::Kind::kStep;
        out.word = std::string(word(rest));
        out.text = std::string(rest);
    } else if (w == "progress") {
        out.kind = Line::Kind::kProgress;
        if (!number(word(rest), out.done) || !number(word(rest), out.total)) return line;
    } else if (w == "folder" && !rest.empty()) {
        out.kind = Line::Kind::kFolder;
        out.text = std::string(rest);
    } else if (w == "error") {
        out.kind = Line::Kind::kError;
        out.text = std::string(rest);
    } else if (w == "done" && rest.empty()) {
        out.kind = Line::Kind::kDone;
    } else if (w == "ok" && rest.empty()) {
        out.kind = Line::Kind::kOk;
    } else if (w == "added" && !rest.empty()) {
        out.kind = Line::Kind::kAdded;
        out.word = std::string(word(rest));
    } else if (w == "removed" && !rest.empty()) {
        out.kind = Line::Kind::kRemoved;
        out.word = std::string(word(rest));
    } else if (w == "skipped" && !rest.empty()) {
        out.kind = Line::Kind::kSkipped;
        out.word = std::string(word(rest));
        out.text = std::string(rest);
    } else {
        return line;
    }
    return out;
}

std::string thousands(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string gigabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

std::string Progress::name(const std::string& fighter) const {
    const auto it = names_.find(fighter);
    return it != names_.end() ? it->second : fighter;
}

bool Progress::feed(const Line& line) {
    switch (line.kind) {
    case Line::Kind::kStep:
        if (kind_ == Kind::kBuild) build_step(line);
        else if (kind_ == Kind::kImport) import_step(line);
        else {
            step_ = line.text.empty() ? line.word : line.text;
            detail_.clear();
            fraction_ = -1;
        }
        return true;
    case Line::Kind::kProgress:
        if (line.total == 0) return false;
        if (kind_ == Kind::kBuild) {
            const double part = static_cast<double>(std::min(line.done, line.total)) / static_cast<double>(line.total);
            if (step_weight_ > 0) fraction_ = step_start_ + step_weight_ * part;
            if (step_id_ == "copy") {
                char text[64];
                std::snprintf(text, sizeof text, "%.1f of %.1f GB", static_cast<double>(line.done) / 1e9,
                              static_cast<double>(line.total) / 1e9);
                detail_ = text;
            } else if (step_id_ == "compile") {
                detail_ = thousands(line.done) + " of " + thousands(line.total) + " files";
            } else {
                detail_ = thousands(line.done) + " of " + thousands(line.total);
            }
        } else if (kind_ == Kind::kDownload) {
            fraction_ = static_cast<double>(std::min(line.done, line.total)) / static_cast<double>(line.total);
            char text[64];
            std::snprintf(text, sizeof text, "%.1f of %.1f MB", static_cast<double>(line.done) / 1e6,
                          static_cast<double>(line.total) / 1e6);
            detail_ = text;
        } else {
            fraction_ = static_cast<double>(std::min(line.done, line.total)) / static_cast<double>(line.total);
            detail_ = thousands(line.done) + " of " + thousands(line.total);
        }
        return true;
    case Line::Kind::kAdded:
        if (kind_ == Kind::kImport && step_weight_ > 0) {
            fraction_ = step_start_ + step_weight_;
            return true;
        }
        return false;
    case Line::Kind::kDone:
        fraction_ = 1;
        detail_.clear();
        return true;
    default: return false;
    }
}

void Progress::build_step(const Line& line) {
    step_id_ = line.word;
    detail_.clear();
    double start = 0, total = 0;
    for (const BuildStep& s : kBuildSteps) total += s.weight;
    for (const BuildStep& s : kBuildSteps) {
        if (line.word == s.id) {
            step_ = line.text.empty() ? s.text : line.text;
            step_start_ = start / total;
            step_weight_ = s.weight / total;
            fraction_ = step_start_;
            return;
        }
        start += s.weight;
    }
    // A step this launcher does not know: the bar stays where it is.
    step_ = line.text.empty() ? line.word : line.text;
    step_weight_ = 0;
}

void Progress::import_step(const Line& line) {
    step_id_ = line.word;
    detail_.clear();
    step_weight_ = 0;
    if (line.word == "dump") {
        // Tekken 7 takes as long as it takes: no fraction.
        step_ = line.text.empty() ? "Saving the characters' moves from Tekken 7" : line.text;
        fraction_ = -1;
    } else if (line.word == "fighter") {
        std::string_view rest = line.text;
        const std::string fighter(word(rest));
        uint64_t i = 0, n = 0;
        step_ = "Adding " + name(fighter);
        if (number(word(rest), i) && number(word(rest), n) && n > 0 && i >= 1 && i <= n) {
            step_start_ = kFighters * static_cast<double>(i - 1) / static_cast<double>(n);
            step_weight_ = kFighters / static_cast<double>(n);
            fraction_ = step_start_;
            if (n > 1) detail_ = std::to_string(i) + " of " + std::to_string(n);
        }
    } else if (line.word == "shared") {
        step_ = line.text.empty() ? "Updating what the added characters share" : line.text;
        fraction_ = kFighters;
    } else {
        step_ = line.text.empty() ? line.word : line.text;
    }
}

} // namespace launcher
