#include "kernel.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "cafe/export.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cafe::os {
namespace {

using namespace osthread;

std::mutex g_kernel;
std::unordered_map<uint32_t, Thread*> g_threads; // OSThread address -> host thread
thread_local Thread* t_current = nullptr;
uint16_t g_next_thread_id = 1;
uint32_t g_sda_base = 0;
uint32_t g_sda2_base = 0;

// Guest work that must run on a thread other than the requester (thread
// deallocators run after their thread is gone).
std::deque<std::function<void(PPCContext&)>> g_deferred;
Thread* g_system_thread = nullptr;
Thread* g_main_thread = nullptr;
uint32_t g_preinit_user = 0;

// Guest threads may recurse deeply; give each host thread room for it.
constexpr size_t kHostStackSize = 64u << 20;

uint8_t state_of(uint32_t thread) { return *guest<uint8_t>(thread + kState); }
void set_state(uint32_t thread, uint8_t state) { *guest<uint8_t>(thread + kState) = state; }

void init_guest_thread(uint32_t thread, uint32_t entry, uint32_t stack_top, uint32_t stack_size,
                       int32_t priority, uint8_t attr) {
    std::memset(guest_pointer(thread), 0, kSize);
    // Default quantization formats, as the kernel sets for every thread.
    field<uint32_t>(thread, kGqr + 2 * 4) = 0x00040004u;
    field<uint32_t>(thread, kGqr + 3 * 4) = 0x00050005u;
    field<uint32_t>(thread, kGqr + 4 * 4) = 0x00060006u;
    field<uint32_t>(thread, kGqr + 5 * 4) = 0x00070007u;
    field<uint32_t>(thread, kMagic) = kMagicValue;
    set_state(thread, kReady);
    *guest<uint8_t>(thread + kAttr) = attr;
    field<uint16_t>(thread, kId) = g_next_thread_id++;
    field<int32_t>(thread, kSuspend) = 1;
    field<int32_t>(thread, kEffectivePriority) = priority;
    field<int32_t>(thread, kBasePriority) = priority;
    field<uint32_t>(thread, kStackBase) = stack_top;
    field<uint32_t>(thread, kStackEnd) = stack_top - stack_size;
    field<uint32_t>(thread, kEntry) = entry;
    field<uint32_t>(thread, kType) = 2; // application thread
}

int core_for(uint8_t attr, int fallback) {
    const uint8_t mask = attr & kAffinityMask;
    if (mask == 0 || mask == kAffinityMask) return fallback;
    for (int core = 0; core < 3; ++core) {
        if (mask & (1 << core)) return core;
    }
    return fallback;
}

void prepare_context(Thread* t, uint32_t argc, uint32_t argv) {
    const uint32_t thread = t->guest;
    PPCContext& ctx = t->ctx;
    ctx = PPCContext{};
    // The ABI wants a terminated back chain at the top of the stack.
    const uint32_t sp = (field<uint32_t>(thread, kStackBase) - 8) & ~7u;
    ppc::st32(guest_base(), sp, 0);
    ctx.r[1] = sp;
    ctx.r[2] = g_sda2_base;
    ctx.r[13] = g_sda_base;
    ctx.r[3] = argc;
    ctx.r[4] = argv;
    for (int i = 0; i < 8; ++i) ctx.gqr[i] = field<uint32_t>(thread, kGqr + i * 4);
    field<uint32_t>(thread, kUpir) = static_cast<uint32_t>(t->core);
    ctx.host_thread = t;
}

void finish(Thread* t, uint32_t exit_value) {
    KernelLock lock(g_kernel);
    const uint32_t thread = t->guest;
    t->finished = true;
    if (t->interrupts_disabled) {
        bool held = false;
        release_interrupt_lock_for_wait(t, held);
    }
    field<uint32_t>(thread, kExitValue) = exit_value;
    const bool detached = (*guest<uint8_t>(thread + kAttr) & kDetached) != 0;
    if (detached) {
        set_state(thread, kNone);
        const uint32_t deallocator = field<uint32_t>(thread, kDeallocator);
        if (deallocator != 0) {
            const uint32_t stack = field<uint32_t>(thread, kStackEnd);
            g_deferred.push_back([deallocator, thread, stack](PPCContext& ctx) {
                call_guest(ctx, deallocator, {thread, stack});
            });
            wake(kWaitDeferred);
        }
        g_threads.erase(thread);
    } else {
        set_state(thread, kMoribund);
    }
    wake(thread); // joiners
}

#ifdef _WIN32
// TTT2_PROFILE="start,seconds": from `start` seconds after start-up, every
// millisecond for `seconds`, the instruction pointer of each guest thread's
// host thread, as "OSThread offset-in-executable" lines in
// TTT2_PROFILE_OUT (default profile.txt). Symbolize the offsets with
// llvm-symbolizer --obj=smash.exe (adding the image base).
std::mutex g_profile_mutex;
std::vector<std::pair<uint32_t, HANDLE>> g_profile_threads;

// (Every guest thread: F10 can start a profile any time, request_profile.)
void note_profile_thread(uint32_t guest) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::lock_guard lock(g_profile_mutex);
    g_profile_threads.push_back({guest, h});
}

std::atomic<bool> g_profiling{false};

// Samples every thread for `seconds`, from `start` seconds on, into `path` (one profile at a time).
void run_profile(double start, double seconds, std::string path) {
    if (g_profiling.exchange(true)) return;
    std::thread([start, seconds, path] {
        FILE* out = std::fopen(path.c_str(), "w");
        if (out == nullptr) {
            g_profiling = false;
            return;
        }
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const uintptr_t image_size = nt->OptionalHeader.SizeOfImage;
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        std::fprintf(stderr, "ttt2: profiling guest threads for %.1f s\n", seconds);
        while (std::chrono::steady_clock::now() < end) {
            std::vector<std::pair<uint32_t, HANDLE>> threads;
            {
                std::lock_guard lock(g_profile_mutex);
                threads = g_profile_threads;
            }
            // The runtime's own threads too (GPU, display, audio), labelled
            // with their Windows thread id.
            constexpr bool all = true;
            static std::vector<std::pair<uint32_t, HANDLE>> host_threads;
            if (all && host_threads.empty()) {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{sizeof te};
                std::vector<DWORD> guest_ids;
                for (const auto& [g, h] : threads) guest_ids.push_back(GetThreadId(h));
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                    if (std::find(guest_ids.begin(), guest_ids.end(), te.th32ThreadID) != guest_ids.end()) continue;
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)) {
                        host_threads.push_back({0xF0000000u | te.th32ThreadID, h});
                    }
                }
                CloseHandle(snap);
            }
            threads.insert(threads.end(), host_threads.begin(), host_threads.end());
            for (const auto& [guest, h] : threads) {
                if (SuspendThread(h) == static_cast<DWORD>(-1)) continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c)) {
                    // Outside the executable (a wait in Windows, the driver):
                    // the first caller inside it, by the unwind tables.
                    const uintptr_t pc = c.Rip;
                    uintptr_t caller = 0, chain[4] = {};
                    int found = 0;
                    if (pc - base >= image_size) {
                        CONTEXT u = c;
                        for (int depth = 0; depth < 32 && u.Rip != 0; ++depth) {
                            DWORD64 image = 0;
                            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &image, nullptr);
                            if (f == nullptr) {
                                u.Rip = *reinterpret_cast<DWORD64*>(u.Rsp); // a leaf
                                u.Rsp += 8;
                            } else {
                                void* handler_data = nullptr;
                                DWORD64 frame = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, f, &u, &handler_data, &frame, nullptr);
                            }
                            if (u.Rip - base < image_size) {
                                if (caller == 0) caller = u.Rip;
                                if (found < 4) chain[found++] = u.Rip - base;
                                if (found == 4) break;
                            }
                        }
                    }
                    std::fprintf(out, "%08X %llx %llx\n", guest, static_cast<unsigned long long>(pc - base),
                                 static_cast<unsigned long long>(caller ? caller - base : 0));
                    if (found > 1) {
                        std::fprintf(out, "  chain");
                        for (int i = 1; i < found; ++i) std::fprintf(out, " %llx", static_cast<unsigned long long>(chain[i]));
                        std::fprintf(out, "\n");
                    }
                }
                ResumeThread(h);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fclose(out);
        std::fprintf(stderr, "ttt2: profile written to %s\n", path.c_str());
        g_profiling = false;
    }).detach();
}

void start_profiler() {
    // TTT2_DUMP_THREADS_AT=seconds: then, each guest thread's name and where it is in guest code
    // (its last call out: the LR and the saved return addresses), for finding what a title waits on.
    if (const char* at = std::getenv("TTT2_DUMP_THREADS_AT")) {
        const double seconds = std::atof(at);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            KernelLock lock(g_kernel);
            for (const auto& [guest, t] : g_threads) {
                const uint32_t name = field<uint32_t>(guest, kName);
                std::fprintf(stderr, "thread %08X \"%s\" state %u:\n", guest,
                             name ? std::string(guest_string(name)).c_str() : "", state_of(guest));
                dump_backtrace(t->ctx);
            }
        }).detach();
    }
    const char* spec = std::getenv("TTT2_PROFILE");
    if (spec == nullptr) return;
    double start = 0, seconds = 5;
    std::sscanf(spec, "%lf,%lf", &start, &seconds);
    const char* out_path = std::getenv("TTT2_PROFILE_OUT");
    run_profile(start, seconds, out_path ? out_path : "profile.txt");
}

// F10: the next ten seconds, as profile-<n>.txt beside the executable (all threads, as TTT2_PROFILE_ALL).
void request_profile_now() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    static int count = 0;
    const std::string path = (std::filesystem::path(exe).parent_path() / ("profile-" + std::to_string(++count) + ".txt")).string();
    run_profile(0, 10, path);
}
#else
void note_profile_thread(uint32_t) {}
void start_profiler() {}
void request_profile_now() {}
#endif

void* host_main(void* argument) {
    Thread* t = static_cast<Thread*>(argument);
    note_profile_thread(t->guest);
    // "OSThread address/core", for debuggers and profilers.
    char name[16];
    std::snprintf(name, sizeof(name), "%08X/%d", t->guest, t->core);
    pthread_setname_np(pthread_self(), name);
    t_current = t;
    set_current_context(&t->ctx);
    uint32_t exit_value;
    if (setjmp(t->exit_jump) == 0) {
        const uint32_t entry = field<uint32_t>(t->guest, kEntry);
        if (t == g_main_thread && g_preinit_user != 0) run_preinit_user(t->ctx, g_preinit_user);
        cafe_ppc_lookup(entry)(t->ctx, guest_base());
        exit_value = t->ctx.r[3];
    } else {
        exit_value = t->ctx.r[3]; // OSExitThread left its argument here
    }
    finish(t, exit_value);
    return nullptr;
}

void start_host_thread(Thread* t) {
#ifdef _WIN32
    // The stack reserved, not committed: winpthreads commits all of it, 64 MB a thread (2.7 GB for Smash's 40),
    // and online play's extra threads then ran the system's commit out.
    HANDLE h = CreateThread(
        nullptr, kHostStackSize,
        [](LPVOID arg) -> DWORD {
            host_main(arg);
            return 0;
        },
        t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h == nullptr) {
        // (Windows refuses new threads once the process is exiting: the window was closed.)
        if (GetLastError() == ERROR_ACCESS_DENIED) return;
        fatal("cannot create a host thread for guest thread 0x%08X (error %lu)", t->guest, GetLastError());
    }
    CloseHandle(h);
    t->host_started = true;
    return;
#endif
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    if (pthread_create(&t->host, &attr, host_main, t) != 0) {
        fatal("cannot create a host thread for guest thread 0x%08X", t->guest);
    }
    pthread_attr_destroy(&attr);
    pthread_detach(t->host);
    t->host_started = true;
}

// Runs deferred guest work (thread deallocators) in its own context.
void* system_thread_main(void*) {
    Thread* t = g_system_thread;
    t_current = t;
    set_current_context(&t->ctx);
    for (;;) {
        std::function<void(PPCContext&)> work;
        {
            KernelLock lock(g_kernel);
            wait_channel(kWaitDeferred).wait(lock, [] { return !g_deferred.empty(); });
            work = std::move(g_deferred.front());
            g_deferred.pop_front();
        }
        work(t->ctx);
    }
    return nullptr;
}

// A guest thread for runtime-internal work: OSThread structure and stack in
// system memory, its own context, not visible in the game's thread lists.
Thread* make_internal_thread(const char* name, uint32_t stack_size, int core) {
    const uint32_t thread = system_alloc(kSize, 8);
    const uint32_t stack = system_alloc(stack_size, 16);
    init_guest_thread(thread, 0, stack + stack_size, stack_size, 0, static_cast<uint8_t>(1u << core));
    const uint32_t name_address = system_alloc(static_cast<uint32_t>(std::strlen(name) + 1));
    std::memcpy(guest_pointer(name_address), name, std::strlen(name) + 1);
    field<uint32_t>(thread, kName) = name_address;
    set_state(thread, kRunning);
    field<int32_t>(thread, kSuspend) = 0;
    auto* t = new Thread;
    t->guest = thread;
    t->core = core;
    prepare_context(t, 0, 0);
    return t;
}

} // namespace

void bind_current_thread(Thread* t) {
    t_current = t;
    set_current_context(&t->ctx);
}

std::mutex& kernel_mutex() { return g_kernel; }
Thread* current_thread() { return t_current; }

std::condition_variable& wait_channel(uint32_t key) {
    static std::condition_variable channels[256];
    return channels[(key * 0x9E3779B1u) >> 24];
}

void wake(uint32_t key) { wait_channel(key).notify_all(); }

Thread* thread_for(uint32_t guest_thread) {
    const auto it = g_threads.find(guest_thread);
    return it == g_threads.end() ? nullptr : it->second;
}

Thread* create_internal_thread(const char* name, int core) {
    KernelLock lock(g_kernel);
    return make_internal_thread(name, 0x10000, core);
}

void set_preinit_user(uint32_t function) { g_preinit_user = function; }

int run_main_thread(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size,
                    uint32_t sda_base, uint32_t sda2_base) {
    g_sda_base = sda_base;
    g_sda2_base = sda2_base;
    {
        KernelLock lock(g_kernel);
        g_system_thread = make_internal_thread("cafe system", 0x10000, 2);
        pthread_t system;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kHostStackSize);
        pthread_create(&system, &attr, system_thread_main, nullptr);
        pthread_attr_destroy(&attr);
        pthread_detach(system);
    }

    // The main thread runs on core 1 with the stack size from the RPX file
    // info, but at least kMainStackMin: TTT2's says 64 KB, yet its main
    // thread nests two functions with 501 KB frames each (sub_02668F50 calls
    // sub_0266D4E0, reached from the menus), so the console's default thread
    // stacks must be larger (emulators that run the title give 1 MB).
    constexpr uint32_t kMainStackMin = 2u << 20;
    stack_size = std::max(stack_size, kMainStackMin);
    const uint32_t thread = system_alloc(kSize, 8);
    const uint32_t stack = system_alloc(stack_size, 16);
    auto* t = new Thread;
    g_main_thread = t;
    start_profiler();
    {
        KernelLock lock(g_kernel);
        init_guest_thread(thread, entry, stack + stack_size, stack_size, 16, 1u << 1);
        t->guest = thread;
        t->core = 1;
        prepare_context(t, argc, argv);
        field<int32_t>(thread, kSuspend) = 0;
        set_state(thread, kRunning);
        g_threads[thread] = t;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    pthread_t host;
    pthread_create(&host, &attr, host_main, t);
    pthread_attr_destroy(&attr);
    pthread_join(host, nullptr);
    // Returning from the RPX entry point ends the process with its result.
    return static_cast<int32_t>(field<uint32_t>(thread, kExitValue));
}

} // namespace cafe::os

// ===================================================================== exports
namespace cafe::os {
namespace {

// TTT2_TRACE_THREADS=1 logs thread creation and changes to priorities,
// affinities and names.
bool trace_threads() {
    static const bool enabled = [] {
        const char* v = std::getenv("TTT2_TRACE_THREADS");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return enabled;
}

GuestAddress OSGetCurrentThread() { return GuestAddress{current_thread()->guest}; }

uint32_t OSGetCoreId() { return static_cast<uint32_t>(current_thread()->core); }

bool OSCreateThread(GuestAddress thread, uint32_t entry, uint32_t argc, uint32_t argv,
                    uint32_t stack_top, uint32_t stack_size, int32_t priority, uint32_t attr) {
    KernelLock lock(g_kernel);
    if (thread_for(thread.value) != nullptr) {
        fatal("OSCreateThread on 0x%08X, which is still a live thread", thread.value);
    }
    init_guest_thread(thread.value, entry, stack_top, stack_size, priority,
                      static_cast<uint8_t>(attr));
    auto* t = new Thread;
    t->guest = thread.value;
    t->core = core_for(static_cast<uint8_t>(attr), current_thread()->core);
    prepare_context(t, argc, argv);
    g_threads[thread.value] = t;
    if (trace_threads()) {
        std::fprintf(stderr, "ttt2: thread 0x%08X created: entry 0x%08X, priority %d, attributes 0x%02X, core %d, "
                     "by 0x%08X\n", thread.value, entry, priority, attr, t->core, current_thread()->guest);
    }
    return true;
}

int32_t OSResumeThread(GuestAddress thread) {
    KernelLock lock(g_kernel);
    Thread* t = thread_for(thread.value);
    if (t == nullptr) {
        fatal("OSResumeThread on 0x%08X, which is not a thread", thread.value);
    }
    be<int32_t>& suspend = field<int32_t>(thread.value, kSuspend);
    const int32_t previous = suspend;
    if (previous > 0) {
        suspend = previous - 1;
        if (previous == 1 && !t->host_started) {
            set_state(thread.value, kRunning);
            start_host_thread(t);
        }
    }
    return previous;
}

void OSExitThread(PPCContext& ctx, uint32_t value) {
    Thread* t = current_thread();
    if (&ctx != &t->ctx) {
        fatal("OSExitThread called from a context that is not a guest thread");
    }
    ctx.r[3] = value;
    std::longjmp(t->exit_jump, 1);
}

bool OSJoinThread(PPCContext& ctx, GuestAddress thread, uint32_t* exit_value) {
    uint32_t deallocator = 0;
    {
        KernelLock lock(g_kernel);
        Thread* t = thread_for(thread.value);
        if (t == nullptr || (*guest<uint8_t>(thread.value + kAttr) & kDetached)) {
            return false;
        }
        wait_until(lock, thread.value, [&] { return t->finished; });
        if (exit_value != nullptr) {
            *reinterpret_cast<be<uint32_t>*>(exit_value) = field<uint32_t>(thread.value, kExitValue);
        }
        set_state(thread.value, kNone);
        deallocator = field<uint32_t>(thread.value, kDeallocator);
        g_threads.erase(thread.value);
        delete t;
    }
    if (deallocator != 0) {
        call_guest(ctx, deallocator, {thread.value, field<uint32_t>(thread.value, kStackEnd)});
    }
    return true;
}

void OSDetachThread(GuestAddress thread) {
    KernelLock lock(g_kernel);
    *guest<uint8_t>(thread.value + kAttr) |= kDetached;
    Thread* t = thread_for(thread.value);
    if (t != nullptr && t->finished) {
        set_state(thread.value, kNone);
        g_threads.erase(thread.value);
    }
}

bool OSIsThreadTerminated(GuestAddress thread) {
    KernelLock lock(g_kernel);
    const uint8_t state = state_of(thread.value);
    return state == kNone || state == kMoribund;
}

void OSSetThreadName(GuestAddress thread, uint32_t name) {
    field<uint32_t>(thread.value, kName) = name;
    if (trace_threads()) {
        std::fprintf(stderr, "ttt2: thread 0x%08X is \"%.*s\"\n", thread.value,
                     static_cast<int>(guest_string(name).size()), guest_string(name).data());
    }
}

bool OSSetThreadPriority(GuestAddress thread, int32_t priority) {
    if (priority < 0 || priority > 31) return false;
    if (trace_threads()) std::fprintf(stderr, "ttt2: thread 0x%08X priority %d\n", thread.value, priority);
    field<int32_t>(thread.value, kBasePriority) = priority;
    field<int32_t>(thread.value, kEffectivePriority) = priority;
    return true;
}

int32_t OSGetThreadPriority(GuestAddress thread) {
    return field<int32_t>(thread.value, kBasePriority);
}

bool OSSetThreadAffinity(GuestAddress thread, uint32_t affinity) {
    if (trace_threads()) std::fprintf(stderr, "ttt2: thread 0x%08X affinity 0x%X\n", thread.value, affinity);
    uint8_t& attr = *guest<uint8_t>(thread.value + kAttr);
    attr = static_cast<uint8_t>((attr & ~kAffinityMask) | (affinity & kAffinityMask));
    KernelLock lock(g_kernel);
    if (Thread* t = thread_for(thread.value)) {
        t->core = core_for(attr, t->core);
    }
    return true;
}

uint32_t OSGetThreadAffinity(GuestAddress thread) {
    return *guest<uint8_t>(thread.value + kAttr) & kAffinityMask;
}

void OSSetThreadSpecific(uint32_t slot, uint32_t value) {
    if (slot < 16) field<uint32_t>(current_thread()->guest, kSpecific + slot * 4) = value;
}

uint32_t OSGetThreadSpecific(uint32_t slot) {
    return slot < 16 ? static_cast<uint32_t>(field<uint32_t>(current_thread()->guest, kSpecific + slot * 4)) : 0;
}

uint32_t OSGetCoreCount() { return 3; }

// The main thread is core 1's default thread.
// shortcut: cores 0 and 2 have no default thread (0); create them if a title runs work on them (OSRunThread).
uint32_t OSGetDefaultThread(uint32_t core) { return core == 1 && g_main_thread ? g_main_thread->guest : 0; }

uint32_t OSSetThreadDeallocator(GuestAddress thread, uint32_t deallocator) {
    be<uint32_t>& slot = field<uint32_t>(thread.value, kDeallocator);
    const uint32_t previous = slot;
    slot = deallocator;
    return previous;
}

void OSSleepTicks(uint64_t ticks) {
    // TTT2_TRACE_SLEEPER=<thread name>: that thread's guest backtrace, the first time it sleeps.
    static const char* sleeper = std::getenv("TTT2_TRACE_SLEEPER");
    if (sleeper && t_current) {
        const uint32_t name = field<uint32_t>(t_current->guest, kName);
        static std::atomic<bool> printed{false};
        if (name && guest_string(name) == sleeper && !printed.exchange(true)) {
            std::fprintf(stderr, "sleeper %s:\n", sleeper);
            print_guest_state(t_current->ctx);
        }
    }
    const uint64_t ns = ticks / 62156250u * 1000000000u + ticks % 62156250u * 1000000000u / 62156250u;
    KernelLock lock(g_kernel);
    wait_until_for(lock, kWaitNothing, ns, [] { return false; });
}

void OSWaitMicroseconds(uint32_t us) { OSSleepTicks(uint64_t{us} * 62156250u / 1000000u); }

void OSYieldThread() { std::this_thread::yield(); }

// Called before exit; other threads must stop touching shared state. The
// process exits right after, so there is nothing to block on.
void OSBlockThreadsOnExit() {}

} // namespace

CAFE_EXPORT(coreinit, OSGetCurrentThread, OSGetCurrentThread);
CAFE_EXPORT(coreinit, OSGetCoreId, OSGetCoreId);
CAFE_EXPORT(coreinit, OSCreateThread, OSCreateThread);
CAFE_EXPORT(coreinit, OSResumeThread, OSResumeThread);
CAFE_EXPORT(coreinit, OSExitThread, OSExitThread);
CAFE_EXPORT(coreinit, OSJoinThread, OSJoinThread);
CAFE_EXPORT(coreinit, OSDetachThread, OSDetachThread);
CAFE_EXPORT(coreinit, OSIsThreadTerminated, OSIsThreadTerminated);
CAFE_EXPORT(coreinit, OSSetThreadName, OSSetThreadName);
CAFE_EXPORT(coreinit, OSSetThreadPriority, OSSetThreadPriority);
CAFE_EXPORT(coreinit, OSGetThreadPriority, OSGetThreadPriority);
CAFE_EXPORT(coreinit, OSSetThreadAffinity, OSSetThreadAffinity);
CAFE_EXPORT(coreinit, OSGetThreadAffinity, OSGetThreadAffinity);
CAFE_EXPORT(coreinit, OSSetThreadSpecific, OSSetThreadSpecific);
CAFE_EXPORT(coreinit, OSGetThreadSpecific, OSGetThreadSpecific);
CAFE_EXPORT(coreinit, OSGetCoreCount, OSGetCoreCount);
CAFE_EXPORT(coreinit, OSGetDefaultThread, OSGetDefaultThread);
CAFE_EXPORT(coreinit, OSSetThreadDeallocator, OSSetThreadDeallocator);
CAFE_EXPORT(coreinit, OSSleepTicks, OSSleepTicks);
CAFE_EXPORT(coreinit, OSWaitMicroseconds, OSWaitMicroseconds);
CAFE_EXPORT(coreinit, OSYieldThread, OSYieldThread);
CAFE_EXPORT(coreinit, OSBlockThreadsOnExit, OSBlockThreadsOnExit);

// F10 (host/window.cpp): profile the next ten seconds.
void request_profile() { request_profile_now(); }

} // namespace cafe::os
