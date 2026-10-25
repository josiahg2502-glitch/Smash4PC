// coreinit FS (synchronous API) and nn_save over the virtual filesystem.
// FSClient and FSCmdBlock are opaque to us; only the last status per client
// is kept, for FSGetLastError.

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/vfs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>

namespace cafe::os {
namespace {

std::mutex g_fs_mutex;

// TTT2_TRACE_FS=1: every open and read, with the time since start-up.
const bool g_trace = std::getenv("TTT2_TRACE_FS") != nullptr;
// On the steady clock, like TTT2_TRACE_STALLS.
double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::unordered_map<uint32_t, int32_t> g_last_status; // FSClient -> last FSStatus

// FSErrorFlag bits a caller lists in errMask to receive that status back
// instead of the system's fatal error handling.
uint32_t mask_bit(int32_t status) {
    switch (status) {
    case vfs::kMax: return 0x1;
    case vfs::kAlreadyOpen: return 0x2;
    case vfs::kExists: return 0x4;
    case vfs::kNotFound: return 0x8;
    case vfs::kNotFile: return 0x10;
    case vfs::kNotDir: return 0x20;
    case vfs::kAccessError: return 0x40;
    case vfs::kPermissionError: return 0x80;
    case vfs::kStorageFull: return 0x200;
    default: return 0;
    }
}

int32_t finish(GuestAddress client, int32_t status, uint32_t mask, const char* operation,
               std::string_view path = {}) {
    {
        std::lock_guard lock(g_fs_mutex);
        g_last_status[client.value] = status;
    }
    if (status < 0 && status != vfs::kEnd && !(mask & mask_bit(status))) {
        // On the console this is a fatal error screen; keep running and say so.
        std::fprintf(stderr, "ttt2: FS %s %.*s failed with unmasked status %d\n", operation,
                     static_cast<int>(path.size()), path.data(), status);
    }
    return status;
}

// FSStat, 0x64 bytes, packed.
void write_stat(uint8_t* out, const vfs::Stat& stat) {
    std::memset(out, 0, 0x64);
    *reinterpret_cast<be<uint32_t>*>(out + 0x00) = stat.directory ? 0x80000000u : 0x01000000u;
    *reinterpret_cast<be<uint32_t>*>(out + 0x04) = 0x666; // permissions
    *reinterpret_cast<be<uint32_t>*>(out + 0x10) = static_cast<uint32_t>(stat.size);
    *reinterpret_cast<be<uint32_t>*>(out + 0x14) = static_cast<uint32_t>(stat.size);
}

int32_t FSInit() { return 0; }
void FSShutdown() {}
int32_t FSAddClient(GuestAddress, uint32_t) { return 0; }
int32_t FSDelClient(GuestAddress, uint32_t) { return 0; }
void FSInitCmdBlock(GuestAddress block) { std::memset(block.as<uint8_t>(), 0, 0xA80); }
int32_t FSSetCmdPriority(GuestAddress, uint32_t) { return 0; }
void FSSetStateChangeNotification(GuestAddress, GuestAddress) {}
void FSCancelAllCommands(GuestAddress) {}

int32_t FSGetLastError(GuestAddress client) {
    std::lock_guard lock(g_fs_mutex);
    const auto it = g_last_status.find(client.value);
    return it == g_last_status.end() ? 0 : it->second;
}

int32_t FSGetLastErrorCodeForViewer(GuestAddress client) {
    // Error codes shown to the user; 0 means "no error to show".
    return FSGetLastError(client) < 0 ? 1503100 : 0;
}

int32_t FSOpenFile(GuestAddress client, GuestAddress, uint32_t path, uint32_t mode,
                   be<int32_t>* handle, uint32_t mask) {
    int32_t h = -1;
    const auto status = vfs::open_file(guest_string(path), guest_string(mode), h);
    if (status == vfs::kOk && handle) *handle = h;
    if (g_trace) {
        const std::string_view name = guest_string(path);
        std::fprintf(stderr, "fs: %8.3f open %.*s -> %d\n", seconds(), static_cast<int>(name.size()), name.data(),
                     status == vfs::kOk ? h : status);
    }
    return finish(client, status, mask, "open", guest_string(path));
}

int32_t read_common(GuestAddress client, uint8_t* destination, uint32_t size, uint32_t count,
                    std::optional<uint64_t> position, int32_t handle, uint32_t mask) {
    if (size == 0 || count == 0) return finish(client, 0, mask, "read");
    uint64_t transferred = 0;
    const double begin = g_trace ? seconds() : 0.0;
    const auto status = vfs::read_file(handle, destination, uint64_t{size} * count, position, transferred);
    if (g_trace) {
        std::fprintf(stderr, "fs: %8.3f read %d %llu bytes in %.1f ms\n", begin, handle,
                     static_cast<unsigned long long>(transferred), (seconds() - begin) * 1000.0);
    }
    if (status != vfs::kOk) return finish(client, status, mask, "read");
    return finish(client, static_cast<int32_t>(transferred / size), mask, "read");
}

int32_t FSReadFile(GuestAddress client, GuestAddress, uint8_t* destination, uint32_t size,
                   uint32_t count, int32_t handle, uint32_t, uint32_t mask) {
    return read_common(client, destination, size, count, std::nullopt, handle, mask);
}

int32_t FSReadFileWithPos(GuestAddress client, GuestAddress, uint8_t* destination, uint32_t size,
                          uint32_t count, uint32_t position, int32_t handle, uint32_t, uint32_t mask) {
    return read_common(client, destination, size, count, position, handle, mask);
}

int32_t write_common(GuestAddress client, const uint8_t* source, uint32_t size, uint32_t count,
                     std::optional<uint64_t> position, int32_t handle, uint32_t mask) {
    if (size == 0 || count == 0) return finish(client, 0, mask, "write");
    uint64_t transferred = 0;
    const auto status = vfs::write_file(handle, source, uint64_t{size} * count, position, transferred);
    if (g_trace) {
        std::fprintf(stderr, "fs: %8.3f write %d %llu bytes\n", seconds(), handle, static_cast<unsigned long long>(transferred));
    }
    if (status != vfs::kOk) return finish(client, status, mask, "write");
    return finish(client, static_cast<int32_t>(transferred / size), mask, "write");
}

int32_t FSWriteFile(GuestAddress client, GuestAddress, const uint8_t* source, uint32_t size,
                    uint32_t count, int32_t handle, uint32_t, uint32_t mask) {
    return write_common(client, source, size, count, std::nullopt, handle, mask);
}

int32_t FSWriteFileWithPos(GuestAddress client, GuestAddress, const uint8_t* source, uint32_t size,
                           uint32_t count, uint32_t position, int32_t handle, uint32_t, uint32_t mask) {
    return write_common(client, source, size, count, position, handle, mask);
}

// Pre-extends a file in the save area; nothing to reserve on the host.
int32_t FSAppendFile(GuestAddress client, GuestAddress, uint32_t, uint32_t count, int32_t, uint32_t,
                     uint32_t mask) {
    return finish(client, static_cast<int32_t>(count), mask, "append");
}

int32_t FSSetPosFile(GuestAddress client, GuestAddress, int32_t handle, uint32_t position, uint32_t mask) {
    return finish(client, vfs::seek_file(handle, position), mask, "seek");
}

int32_t FSCloseFile(GuestAddress client, GuestAddress, int32_t handle, uint32_t mask) {
    return finish(client, vfs::close_file(handle), mask, "close");
}

int32_t FSGetStat(GuestAddress client, GuestAddress, uint32_t path, uint8_t* out, uint32_t mask) {
    vfs::Stat stat;
    const auto status = vfs::stat_path(guest_string(path), stat);
    if (status == vfs::kOk && out) write_stat(out, stat);
    return finish(client, status, mask, "stat", guest_string(path));
}

int32_t FSGetStatFile(GuestAddress client, GuestAddress, int32_t handle, uint8_t* out, uint32_t mask) {
    vfs::Stat stat;
    const auto status = vfs::stat_file(handle, stat);
    if (status == vfs::kOk && out) write_stat(out, stat);
    return finish(client, status, mask, "stat");
}

int32_t FSOpenDir(GuestAddress client, GuestAddress, uint32_t path, be<int32_t>* handle, uint32_t mask) {
    int32_t h = -1;
    const auto status = vfs::open_dir(guest_string(path), h);
    if (status == vfs::kOk && handle) *handle = h;
    return finish(client, status, mask, "opendir", guest_string(path));
}

// FSDirEntry: FSStat (0x64) then the name (128 bytes).
int32_t FSReadDir(GuestAddress client, GuestAddress, int32_t handle, uint8_t* out, uint32_t mask) {
    vfs::DirEntry entry;
    const auto status = vfs::read_dir(handle, entry);
    if (status == vfs::kOk && out) {
        write_stat(out, entry.stat);
        char* name = reinterpret_cast<char*>(out + 0x64);
        std::memset(name, 0, 128);
        std::strncpy(name, entry.name.c_str(), 127);
    }
    return finish(client, status, mask, "readdir");
}

int32_t FSCloseDir(GuestAddress client, GuestAddress, int32_t handle, uint32_t mask) {
    return finish(client, vfs::close_dir(handle), mask, "closedir");
}

// ---------------------------------------------------------------- nn_save
// Account slot 0xFF is the title's common save area; other slots are the
// local user's directory, named by persistent id.
std::string save_directory(uint8_t slot) { return slot == 0xFF ? "common" : "80000001"; }

// The title's save folders exist once it is installed, whether or not it
// calls SAVEInitSaveDir.
int32_t SAVEInit() {
    vfs::make_dir("/vol/save/" + save_directory(0));
    vfs::make_dir("/vol/save/common");
    return 0;
}
void SAVEShutdown() {}

int32_t SAVEInitSaveDir(uint8_t slot) {
    vfs::make_dir("/vol/save/" + save_directory(slot));
    vfs::make_dir("/vol/save/common");
    return 0;
}

int32_t SAVEOpenFile(GuestAddress client, GuestAddress block, uint8_t slot, uint32_t path, uint32_t mode,
                     be<int32_t>* handle, uint32_t mask) {
    const std::string full = "/vol/save/" + save_directory(slot) + "/" + std::string(guest_string(path));
    int32_t h = -1;
    const auto status = vfs::open_file(full, guest_string(mode), h);
    if (status == vfs::kOk && handle) *handle = h;
    if (g_trace) {
        std::fprintf(stderr, "fs: %8.3f save open %s (%.*s) -> %d\n", seconds(), full.c_str(),
                     static_cast<int>(guest_string(mode).size()), guest_string(mode).data(), status == vfs::kOk ? h : status);
    }
    (void)block;
    return finish(client, status, mask, "SAVEOpenFile", full);
}

// The save area's paths, relative to the slot's directory.
std::string save_path(uint8_t slot, uint32_t path) {
    return "/vol/save/" + save_directory(slot) + "/" + std::string(guest_string(path));
}
int32_t SAVEMakeDir(GuestAddress client, GuestAddress, uint8_t slot, uint32_t path, uint32_t mask) {
    const std::string full = save_path(slot, path);
    return finish(client, vfs::make_dir(full), mask, "SAVEMakeDir", full);
}
int32_t SAVEOpenDir(GuestAddress client, GuestAddress, uint8_t slot, uint32_t path, be<int32_t>* handle,
                    uint32_t mask) {
    const std::string full = save_path(slot, path);
    int32_t h = -1;
    const auto status = vfs::open_dir(full, h);
    if (status == vfs::kOk && handle) *handle = h;
    return finish(client, status, mask, "SAVEOpenDir", full);
}
int32_t SAVEGetStat(GuestAddress client, GuestAddress, uint8_t slot, uint32_t path, uint8_t* out, uint32_t mask) {
    const std::string full = save_path(slot, path);
    vfs::Stat stat;
    const auto status = vfs::stat_path(full, stat);
    if (status == vfs::kOk && out) write_stat(out, stat);
    return finish(client, status, mask, "SAVEGetStat", full);
}
// Free space: plenty (the host disk's real figure is not the console's quota).
constexpr uint64_t kFreeSpace = 512ull << 20;
int32_t SAVEGetFreeSpaceSize(GuestAddress client, GuestAddress, uint8_t, be<uint64_t>* size, uint32_t mask) {
    if (size) *size = kFreeSpace;
    return finish(client, 0, mask, "SAVEGetFreeSpaceSize");
}

// Paths of another title's shared data (system data titles: word filters,
// fonts) and shared saves, as the console forms them. They are not part of a
// game dump, so opening them finds nothing.
int32_t write_path(char* out, uint32_t size, const char* format, uint64_t title, uint32_t name) {
    if (!out || size == 0) return -1;
    std::snprintf(out, size, format, static_cast<uint32_t>(title >> 32), static_cast<uint32_t>(title),
                  std::string(guest_string(name)).c_str());
    return 0;
}
int32_t SAVEGetSharedDataTitlePath(uint64_t title, uint32_t name, char* out, uint32_t size) {
    return write_path(out, size, "/vol/storage_mlc01/sys/title/%08x/%08x/content/%s", title, name);
}
int32_t SAVEGetSharedSaveDataPath(uint64_t title, uint32_t name, char* out, uint32_t size) {
    return write_path(out, size, "/vol/storage_mlc01/usr/save/%08x/%08x/user/common/%s", title, name);
}

// An SD card is always inserted (as in Cemu): FSGetMountSource fills an
// FSMountSource (type at 0x0, path at 0x4) for it and FSMount mounts it at
// /vol/external01, which the VFS keeps beside the saves.
constexpr const char* kSdMountPath = "/vol/external01";
int32_t FSGetMountSource(GuestAddress client, GuestAddress, uint32_t type, uint8_t* source, uint32_t mask) {
    if (type != 0 || !source) return finish(client, vfs::kNotFound, mask, "getmountsource");
    std::memset(source, 0, 0x284); // FSMountSource: type, then a 0x280-byte path
    *reinterpret_cast<be<uint32_t>*>(source) = 0u; // FS_SOURCE_TYPE_EXTERNAL
    std::strcpy(reinterpret_cast<char*>(source + 4), "/sd");
    vfs::make_dir(kSdMountPath);
    return finish(client, 0, mask, "getmountsource");
}
int32_t FSMount(GuestAddress client, GuestAddress, GuestAddress, char* target, uint32_t size, uint32_t mask) {
    if (target && size > std::strlen(kSdMountPath)) std::strcpy(target, kSdMountPath);
    return finish(client, 0, mask, "mount");
}
int32_t FSUnmount(GuestAddress client, GuestAddress, GuestAddress, uint32_t mask) {
    return finish(client, 0, mask, "unmount");
}
// Async commands run at once, then complete as FSAsyncData {callback, param,
// queue} asks: callback(client, block, status, param), or an OSMessage to the
// queue whose data is the FSAsyncResult {async data, message, client, block,
// status}, kept at the end of the command block (opaque to the title).
constexpr uint32_t kAsyncResultOffset = 0xA80 - 0x28;
constexpr uint32_t kMessageTypeFsAsync = 8; // shortcut: type from Cemu's FS; check it if a title filters on it.
int32_t complete_async(PPCContext& ctx, GuestAddress client, GuestAddress block, int32_t status, const be<uint32_t>* async) {
    if (!async) return status;
    const uint32_t callback = async[0], param = async[1], queue = async[2];
    if (callback != 0) {
        call_guest(ctx, callback, {client.value, block.value, static_cast<uint32_t>(status), param});
    } else if (queue != 0) {
        const uint32_t result = block.value + kAsyncResultOffset;
        auto* words = guest<be<uint32_t>>(result);
        for (int i = 0; i < 3; ++i) words[i] = async[i];
        words[3] = result;             // message: data
        words[4] = words[5] = 0u;
        words[6] = kMessageTypeFsAsync; // message: type
        words[7] = client.value;
        words[8] = block.value;
        words[9] = static_cast<uint32_t>(status);
        send_message(queue, result + 0xC, 1);
    }
    return 0; // the command was accepted
}

int32_t FSOpenFileAsync(PPCContext& ctx, GuestAddress client, GuestAddress block, uint32_t path, uint32_t mode,
                        be<int32_t>* handle, uint32_t mask, const be<uint32_t>* async) {
    return complete_async(ctx, client, block, FSOpenFile(client, block, path, mode, handle, mask), async);
}
int32_t FSCloseFileAsync(PPCContext& ctx, GuestAddress client, GuestAddress block, int32_t handle, uint32_t mask,
                         const be<uint32_t>* async) {
    return complete_async(ctx, client, block, FSCloseFile(client, block, handle, mask), async);
}
int32_t FSGetStatAsync(PPCContext& ctx, GuestAddress client, GuestAddress block, uint32_t path, uint8_t* out,
                       uint32_t mask, const be<uint32_t>* async) {
    return complete_async(ctx, client, block, FSGetStat(client, block, path, out, mask), async);
}
// The FSAsyncResult of a completion message from a queue: the message's data word.
uint32_t FSGetAsyncResult(const be<uint32_t>* message) { return message ? uint32_t{message[0]} : 0u; }
// Writes go to the host file as they happen; nothing waits in a cache.
int32_t FSFlushFile(GuestAddress client, GuestAddress, int32_t, uint32_t mask) { return finish(client, 0, mask, "flush"); }

int32_t FSMakeDir(GuestAddress client, GuestAddress, uint32_t path, uint32_t mask) {
    return finish(client, vfs::make_dir(guest_string(path)), mask, "mkdir", guest_string(path));
}
int32_t FSRemove(GuestAddress client, GuestAddress, uint32_t path, uint32_t mask) {
    return finish(client, vfs::remove(guest_string(path)), mask, "remove", guest_string(path));
}
int32_t FSRename(GuestAddress client, GuestAddress, uint32_t from, uint32_t to, uint32_t mask) {
    return finish(client, vfs::rename(guest_string(from), guest_string(to)), mask, "rename", guest_string(from));
}
int32_t SAVERemove(GuestAddress client, GuestAddress, uint8_t slot, uint32_t path, uint32_t mask) {
    const std::string full = save_path(slot, path);
    return finish(client, vfs::remove(full), mask, "SAVERemove", full);
}
int32_t FSGetFreeSpaceSize(GuestAddress client, GuestAddress, uint32_t, be<uint64_t>* size, uint32_t mask) {
    if (size) *size = kFreeSpace;
    return finish(client, 0, mask, "getfreespace");
}
int32_t FSFlushQuota(GuestAddress client, GuestAddress, uint32_t, uint32_t mask) {
    return finish(client, 0, mask, "flushquota");
}

int32_t SAVEFlushQuota(GuestAddress, GuestAddress, uint8_t, uint32_t) { return 0; }
int32_t SAVERollbackQuota(GuestAddress, GuestAddress, uint8_t, uint32_t) { return 0; }

} // namespace

CAFE_EXPORT(coreinit, FSInit, FSInit);
CAFE_EXPORT(coreinit, FSShutdown, FSShutdown);
CAFE_EXPORT(coreinit, FSAddClient, FSAddClient);
CAFE_EXPORT(coreinit, FSDelClient, FSDelClient);
CAFE_EXPORT(coreinit, FSInitCmdBlock, FSInitCmdBlock);
CAFE_EXPORT(coreinit, FSSetCmdPriority, FSSetCmdPriority);
CAFE_EXPORT(coreinit, FSSetStateChangeNotification, FSSetStateChangeNotification);
CAFE_EXPORT(coreinit, FSCancelAllCommands, FSCancelAllCommands);
CAFE_EXPORT(coreinit, FSGetLastError, FSGetLastError);
CAFE_EXPORT(coreinit, FSGetLastErrorCodeForViewer, FSGetLastErrorCodeForViewer);
CAFE_EXPORT(coreinit, FSOpenFile, FSOpenFile);
CAFE_EXPORT(coreinit, FSReadFile, FSReadFile);
CAFE_EXPORT(coreinit, FSReadFileWithPos, FSReadFileWithPos);
CAFE_EXPORT(coreinit, FSWriteFile, FSWriteFile);
CAFE_EXPORT(coreinit, FSWriteFileWithPos, FSWriteFileWithPos);
CAFE_EXPORT(coreinit, FSAppendFile, FSAppendFile);
CAFE_EXPORT(coreinit, FSSetPosFile, FSSetPosFile);
CAFE_EXPORT(coreinit, FSCloseFile, FSCloseFile);
CAFE_EXPORT(coreinit, FSGetStat, FSGetStat);
CAFE_EXPORT(coreinit, FSGetStatFile, FSGetStatFile);
CAFE_EXPORT(coreinit, FSOpenDir, FSOpenDir);
CAFE_EXPORT(coreinit, FSReadDir, FSReadDir);
CAFE_EXPORT(coreinit, FSCloseDir, FSCloseDir);

CAFE_EXPORT(nn_save, SAVEInit, SAVEInit);
CAFE_EXPORT(nn_save, SAVEShutdown, SAVEShutdown);
CAFE_EXPORT(nn_save, SAVEInitSaveDir, SAVEInitSaveDir);
CAFE_EXPORT(nn_save, SAVEOpenFile, SAVEOpenFile);
CAFE_EXPORT(nn_save, SAVEMakeDir, SAVEMakeDir);
CAFE_EXPORT(nn_save, SAVEGetSharedDataTitlePath, SAVEGetSharedDataTitlePath);
CAFE_EXPORT(nn_save, SAVEGetSharedSaveDataPath, SAVEGetSharedSaveDataPath);
CAFE_EXPORT(nn_save, SAVEOpenDir, SAVEOpenDir);
CAFE_EXPORT(nn_save, SAVEGetStat, SAVEGetStat);
CAFE_EXPORT(nn_save, SAVEGetFreeSpaceSize, SAVEGetFreeSpaceSize);
CAFE_EXPORT(coreinit, FSGetMountSource, FSGetMountSource);
CAFE_EXPORT(coreinit, FSMount, FSMount);
CAFE_EXPORT(coreinit, FSUnmount, FSUnmount);
CAFE_EXPORT(coreinit, FSMakeDir, FSMakeDir);
CAFE_EXPORT(coreinit, FSOpenFileAsync, FSOpenFileAsync);
CAFE_EXPORT(coreinit, FSCloseFileAsync, FSCloseFileAsync);
CAFE_EXPORT(coreinit, FSGetStatAsync, FSGetStatAsync);
CAFE_EXPORT(coreinit, FSGetAsyncResult, FSGetAsyncResult);
CAFE_EXPORT(coreinit, FSFlushFile, FSFlushFile);
CAFE_EXPORT(coreinit, FSRemove, FSRemove);
CAFE_EXPORT(coreinit, FSRename, FSRename);
CAFE_EXPORT(nn_save, SAVERemove, SAVERemove);
CAFE_EXPORT(coreinit, FSGetFreeSpaceSize, FSGetFreeSpaceSize);
CAFE_EXPORT(coreinit, FSFlushQuota, FSFlushQuota);
CAFE_EXPORT(nn_save, SAVEFlushQuota, SAVEFlushQuota);
CAFE_EXPORT(nn_save, SAVERollbackQuota, SAVERollbackQuota);

} // namespace cafe::os
