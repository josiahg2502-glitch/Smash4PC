// Writing the settings files (see files.h).

#include "files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

namespace launcher {

bool replace_file(const fs::path& file, std::string_view text, std::string* error) {
    const auto fail = [&](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    // A link (a setting kept elsewhere, e.g. with the user's dotfiles) stays
    // one: the file it leads to is written, beside itself.
    fs::path target = file;
    for (int links = 0;; ++links) {
        std::error_code ec;
        if (!fs::is_symlink(target, ec)) break;
        const fs::path to = fs::read_symlink(target, ec);
        if (ec) return fail("cannot follow the link " + file.string() + ": " + ec.message());
        // A loop: every link in it reads fine, so ec has no reason to give.
        if (links == 40) {
            return fail("cannot follow the link " + file.string() + ": " +
                        std::make_error_code(std::errc::too_many_symbolic_link_levels).message());
        }
        target = to.is_absolute() ? to : target.parent_path() / to;
    }
    const fs::path folder = target.parent_path();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return fail("cannot make " + folder.string() + ": " + ec.message());
    // A new file gets the mode the umask gives (as any program's), the file
    // replaced keeps its own (its owner can still read and write it).
    struct stat old{};
    const bool replacing = ::stat(target.c_str(), &old) == 0 && S_ISREG(old.st_mode);
    std::string temporary;
    int fd = -1;
    for (int n = 0; fd < 0 && n < 100; ++n) {
        temporary = (folder / ("." + target.filename().string() + "." + std::to_string(::getpid()) + "." +
                               std::to_string(n)))
                        .string();
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) return fail("cannot write in " + folder.string() + ": " + std::strerror(errno));
    if (replacing) ::fchmod(fd, (old.st_mode & 07777) | 0600);
    std::string why;
    for (size_t written = 0; written < text.size() && why.empty();) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) why = std::strerror(errno);
        else if (n == 0) why = "nothing could be written";
        else written += static_cast<size_t>(n);
    }
    if (why.empty() && ::fsync(fd) != 0) why = std::strerror(errno);
    if (::close(fd) != 0 && why.empty()) why = std::strerror(errno);
    if (why.empty() && ::rename(temporary.c_str(), target.c_str()) != 0) why = std::strerror(errno);
    if (!why.empty()) {
        ::unlink(temporary.c_str());
        return fail("cannot write " + target.string() + ": " + why);
    }
    return true;
}

} // namespace launcher
