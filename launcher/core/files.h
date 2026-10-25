#pragma once

// Writing the settings files the game reads (keyboard.ini, display.ini).

#include "paths.h"

#include <string>
#include <string_view>

namespace launcher {

// The file's new text all at once: written beside it, then renamed over it,
// so that the game never reads half of it. A link is followed: the file it
// leads to is replaced, and keeps its mode (a new one gets the umask's).
// The folder is made when it is missing.
bool replace_file(const fs::path& file, std::string_view text, std::string* error = nullptr);

} // namespace launcher
