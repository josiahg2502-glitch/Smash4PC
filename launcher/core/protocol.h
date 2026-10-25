#pragma once

// The lines the builder (ttt2-build) and the importer (import_character.py
// --progress) print about their progress, and the progress bar they make.
// A line starting with '@' and a known word is the protocol; anything else
// is log text for the user.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace launcher {

struct Line {
    enum class Kind { kText, kStep, kProgress, kFolder, kError, kDone, kOk, kAdded, kRemoved, kSkipped };
    Kind kind = Kind::kText;
    std::string word; // kStep: the step's id; kAdded, kRemoved, kSkipped: the fighter
    std::string text; // kStep, kSkipped: the rest; kFolder: the path; kError: the reason; kText: the line
    uint64_t done = 0, total = 0;
};

Line parse_line(std::string_view line);

// A task's progress as one bar: where it is, how far (0..1, or below 0 while
// that is unknown), and a detail ("412 of 1,203 files"). A download's steps
// are its text, and its progress in bytes ("1.2 of 2.6 MB").
class Progress {
public:
    enum class Kind { kBuild, kImport, kDownload };
    explicit Progress(Kind kind) : kind_(kind) {}

    // Fighters' names as the user sees them ("kazumi" -> "Kazumi Mishima").
    void set_names(std::map<std::string, std::string> names) { names_ = std::move(names); }

    // Whether the line changed what the bar shows.
    bool feed(const Line& line);

    const std::string& step() const { return step_; }
    const std::string& detail() const { return detail_; }
    double fraction() const { return fraction_; }

private:
    void build_step(const Line& line);
    void import_step(const Line& line);
    std::string name(const std::string& fighter) const;

    Kind kind_;
    std::map<std::string, std::string> names_;
    std::string step_id_, step_, detail_;
    double fraction_ = -1, step_start_ = 0, step_weight_ = 0;
};

// "1,203" and "4.2 GB".
std::string thousands(uint64_t n);
std::string gigabytes(uint64_t bytes);

} // namespace launcher
