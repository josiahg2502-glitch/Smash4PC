#include <zarchive/zarchivereader.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
private:
    int value_;
};

[[noreturn]] void system_error(const std::string& operation) {
    const int error = errno;
    throw std::runtime_error(operation + ": " + std::strerror(error));
}

void validate_component(std::string_view name) {
    if (name.empty() || name == "." || name == "..")
        throw std::runtime_error("Archive contains an unsafe path component");
    for (const unsigned char c : name) {
        // Also reject controls so each manifest entry remains exactly one TSV row.
        if (c < 32 || c == 127 || c == '/' || c == '\\')
            throw std::runtime_error("Archive contains an unsafe path component");
    }
}

struct Entry {
    std::string path;
    ZArchiveNodeHandle node;
    uint64_t size;
};

std::vector<Entry> inventory(ZArchiveReader& reader) {
    struct Directory { std::string path; ZArchiveNodeHandle node; unsigned depth; };
    const auto root = reader.LookUp("", false, true);
    if (root == ZARCHIVE_INVALID_NODE)
        throw std::runtime_error("Archive has no valid root directory");
    std::vector<Directory> pending{{"", root, 0}};
    std::unordered_set<ZArchiveNodeHandle> visited;
    std::unordered_set<std::string> paths;
    std::vector<Entry> files;
    while (!pending.empty()) {
        auto dir = std::move(pending.back());
        pending.pop_back();
        if (dir.depth > 256 || !visited.insert(dir.node).second)
            throw std::runtime_error("Archive has cyclic or excessively deep directories");
        const auto count = reader.GetDirEntryCount(dir.node);
        for (uint32_t i = 0; i < count; ++i) {
            ZArchiveReader::DirEntry entry{};
            if (!reader.GetDirEntry(dir.node, i, entry) || entry.isFile == entry.isDirectory)
                throw std::runtime_error("Invalid archive directory entry");
            validate_component(entry.name);
            std::string path = dir.path.empty() ? std::string(entry.name)
                                               : dir.path + "/" + std::string(entry.name);
            if (!paths.insert(path).second)
                throw std::runtime_error("Duplicate archive path: " + path);
            const auto node = reader.LookUp(path, entry.isFile, entry.isDirectory);
            if (node == ZARCHIVE_INVALID_NODE)
                throw std::runtime_error("Cannot look up archive entry: " + path);
            if (entry.isDirectory)
                pending.push_back({std::move(path), node, dir.depth + 1});
            else
                files.push_back({std::move(path), node, reader.GetFileSize(node)});
        }
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return files;
}

Fd child_directory(int parent, const std::string& name) {
    if (::mkdirat(parent, name.c_str(), 0755) != 0 && errno != EEXIST)
        system_error("Cannot create directory " + name);
    const int fd = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        system_error("Cannot safely open directory " + name);
    return Fd(fd);
}

Fd output_directory(const fs::path& output) {
    // Traverse every ancestor with O_NOFOLLOW, including ancestors of OUTPUT_DIR.
    const auto absolute = fs::absolute(output).lexically_normal();
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0)
        system_error("Cannot open filesystem root");
    for (const auto& component : absolute.relative_path())
        current = child_directory(current.get(), component.string());
    return current;
}

bool analysis_file(const std::string& path) {
    // WUA paths are TITLEID_VERSION/code/... and TITLEID_VERSION/meta/....
    const auto first = path.find('/');
    if (first == std::string::npos) return false;
    const auto second = path.find('/', first + 1);
    if (second == std::string::npos) return false;
    const auto directory = path.substr(first + 1, second - first - 1);
    return directory == "code" || directory == "meta";
}

void extract_file(ZArchiveReader& reader, const Entry& entry, int root) {
    const int duplicate = ::fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) system_error("Cannot duplicate output directory handle");
    Fd parent(duplicate);
    const fs::path relative(entry.path);
    for (const auto& component : relative.parent_path())
        parent = child_directory(parent.get(), component.string());
    const auto name = relative.filename().string();
    Fd output(::openat(parent.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644));
    if (output.get() < 0)
        system_error("Cannot create output file (existing files are never overwritten) " + entry.path);
    try {
        std::array<unsigned char, 1024 * 1024> buffer{};
        uint64_t offset = 0;
        while (offset < entry.size) {
            const auto length = std::min<uint64_t>(buffer.size(), entry.size - offset);
            if (reader.ReadFromFile(entry.node, offset, length, buffer.data()) != length)
                throw std::runtime_error("Short archive read: " + entry.path);
            size_t written = 0;
            while (written < length) {
                const auto result = ::write(output.get(), buffer.data() + written, length - written);
                if (result < 0 && errno == EINTR) continue;
                if (result < 0) system_error("Cannot write " + entry.path);
                if (result == 0) throw std::runtime_error("Short output write: " + entry.path);
                written += static_cast<size_t>(result);
            }
            offset += length;
        }
        if (::close(output.release()) != 0)
            system_error("Cannot close output file " + entry.path);
    } catch (...) {
        ::unlinkat(parent.get(), name.c_str(), 0);
        throw;
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    const bool list = command == "list";
    const bool analysis = command == "extract-analysis";
    const bool all = command == "extract-all";
    if ((!list && !analysis && !all) || argc != (list ? 3 : 4)) {
        std::cerr << "Usage:\n  wua_extract list ARCHIVE\n"
                  << "  wua_extract extract-analysis ARCHIVE OUTPUT_DIR\n"
                  << "  wua_extract extract-all ARCHIVE OUTPUT_DIR\n";
        return 2;
    }
    try {
        std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(argv[2]));
        if (!reader) throw std::runtime_error("Cannot open WUA/ZArchive: " + std::string(argv[2]));
        const auto files = inventory(*reader);
        if (list) {
            std::cout << "path\tsize_bytes\n";
            for (const auto& entry : files)
                std::cout << entry.path << '\t' << entry.size << '\n';
        } else {
            auto root = output_directory(argv[3]);
            uint64_t bytes = 0;
            size_t count = 0;
            for (const auto& entry : files) {
                if (analysis && !analysis_file(entry.path)) continue;
                extract_file(*reader, entry, root.get());
                bytes += entry.size;
                ++count;
                std::cout << entry.path << '\n';
            }
            std::cerr << "Extracted " << count << " files (" << bytes << " bytes)\n";
            if (analysis && count == 0)
                throw std::runtime_error("No TITLEID_VERSION/code or /meta files found");
        }
        if (!std::cout) throw std::runtime_error("Cannot write standard output");
    } catch (const std::exception& error) {
        std::cerr << "wua_extract: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
