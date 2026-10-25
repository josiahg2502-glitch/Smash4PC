#include "cafe/generated.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace cafe {

bool g_count_imports = std::getenv("TTT2_COUNT_IMPORTS") != nullptr || std::getenv("TTT2_FIRST_IMPORTS") != nullptr;

void count_import(const char* name) {
    static std::mutex mutex;
    static std::map<const char*, uint64_t> counts;
    static auto since = std::chrono::steady_clock::now();
    static const bool first_only = std::getenv("TTT2_FIRST_IMPORTS") != nullptr;
    std::lock_guard lock(mutex);
    if (first_only) { // TTT2_FIRST_IMPORTS=1: each export the first time the title calls it
        static std::map<const char*, bool> seen;
        if (!seen[name]) {
            seen[name] = true;
            std::fprintf(stderr, "first: %s\n", name);
        }
        return;
    }
    ++counts[name];
    const auto now = std::chrono::steady_clock::now();
    if (now - since < std::chrono::seconds(5)) return;
    since = now;
    std::vector<std::pair<uint64_t, const char*>> top;
    for (const auto& [n, c] : counts) top.push_back({c, n});
    std::sort(top.rbegin(), top.rend());
    std::fprintf(stderr, "imports (5 s):");
    for (size_t i = 0; i < top.size() && i < 120; ++i) std::fprintf(stderr, " %s %llu;", top[i].second, (unsigned long long)top[i].first);
    std::fprintf(stderr, "\n");
    counts.clear();
}

namespace {

thread_local PPCContext* t_current = nullptr;

void print_backtrace(const PPCContext& ctx) {
    std::fprintf(stderr, "guest backtrace (LR, then saved return addresses):\n");
    std::fprintf(stderr, "  0x%08X\n", ctx.lr);
    uint32_t frame = ctx.r[1];
    for (int depth = 0; depth < 48; ++depth) {
        if (!guest_memory_committed(frame, 8)) {
            break;
        }
        const uint32_t back = ppc::ld32(guest_base(), frame);
        if (back == 0 || back <= frame || !guest_memory_committed(back, 8)) {
            break;
        }
        std::fprintf(stderr, "  0x%08X\n", ppc::ld32(guest_base(), back + 4));
        frame = back;
    }
}

void report_fault(uintptr_t host, int signal) {
    const auto base = reinterpret_cast<uintptr_t>(guest_base());
    if (base != 0 && host >= base && host - base < (uint64_t{1} << 32) + 0x10000) {
        const uint64_t guest = host - base;
        std::fprintf(stderr, "\nguest memory fault: access to unmapped guest address 0x%08llX\n",
                     static_cast<unsigned long long>(guest));
        // Just below the stack pointer: a frame larger than the stack left.
        if (t_current != nullptr && guest < t_current->r[1] && t_current->r[1] - guest <= (4u << 20)) {
            std::fprintf(stderr, "(0x%llX bytes below the stack pointer: a stack overflow)\n",
                         static_cast<unsigned long long>(t_current->r[1] - guest));
        }
    } else {
        std::fprintf(stderr, "\nhost fault (signal %d) at 0x%llx\n", signal,
                     static_cast<unsigned long long>(host));
    }
}

void report_guest_state_and_exit(int signal) {
    if (t_current != nullptr) {
        print_guest_state(*t_current);
    }
    std::fflush(stderr);
    std::_Exit(128 + signal);
}

#ifdef _WIN32
LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        record.ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report_fault(record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? record.ExceptionInformation[1] : 0, 11);
    // Where in the recompiled code: `addr2line -f -e <executable> <address>`
    // (add the image base) names the guest function (sub_XXXXXXXX).
    const auto pc = static_cast<uintptr_t>(info->ContextRecord->Rip);
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &module)) {
        GetModuleFileNameA(module, name, sizeof name);
    }
    std::fprintf(stderr, "host code: %s+0x%llx\n", name,
                 static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(module)));
    report_guest_state_and_exit(11);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
void fault_handler(int signal, siginfo_t* info, void* context) {
    report_fault(reinterpret_cast<uintptr_t>(info->si_addr), signal);
    // Where in the recompiled code: `addr2line -f -e <executable> <offset>`
    // names the guest function (sub_XXXXXXXX).
    const auto* uc = static_cast<const ucontext_t*>(context);
    const auto pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    Dl_info where{};
    if (dladdr(reinterpret_cast<void*>(pc), &where) && where.dli_fname != nullptr) {
        std::fprintf(stderr, "host code: %s+0x%llx\n", where.dli_fname,
                     static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(where.dli_fbase)));
    }
    report_guest_state_and_exit(signal);
}
#endif

} // namespace

PPCContext* current_context() { return t_current; }
void dump_backtrace(const PPCContext& ctx) { print_backtrace(ctx); }
void set_current_context(PPCContext* ctx) { t_current = ctx; }

void print_guest_state(const PPCContext& ctx) {
    for (int i = 0; i < 32; i += 4) {
        std::fprintf(stderr, "  r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i,
                     ctx.r[i], i + 1, ctx.r[i + 1], i + 2, ctx.r[i + 2], i + 3, ctx.r[i + 3]);
    }
    std::fprintf(stderr, "  lr %08X  ctr %08X  cr %08X  xer %08X\n", ctx.lr, ctx.ctr,
                 ppc::mfcr(ctx), ppc::mfxer(ctx));
    print_backtrace(ctx);
}

void fatal(const char* format, ...) {
    std::fflush(stdout);
    std::fprintf(stderr, "\nttt2: ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    if (t_current != nullptr) {
        print_guest_state(*t_current);
    }
    std::fflush(stderr);
    // Not exit(): other guest threads still use the objects static
    // destructors would tear down.
    std::_Exit(70);
}

void install_fault_handler() {
#ifdef _WIN32
    // Room to report a stack overflow from the faulting thread.
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
    AddVectoredExceptionHandler(1, fault_handler);
#else
    static char alternate_stack[64 * 1024];
    stack_t stack{};
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof alternate_stack;
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
#endif
}

} // namespace cafe

// ------------------------------------------------- hooks for generated code
extern "C" {

void cafe_ppc_trap(cafe::PPCContext& ctx, uint32_t address) {
    cafe::set_current_context(&ctx);
    cafe::fatal("guest trap instruction at 0x%08X (an assertion in game code)", address);
}

void cafe_ppc_illegal(cafe::PPCContext& ctx, uint32_t address, uint32_t word,
                      const char* reason) {
    cafe::set_current_context(&ctx);
    cafe::fatal("cannot execute instruction 0x%08X at 0x%08X: %s", word, address, reason);
}

void cafe_ppc_null_call(cafe::PPCContext& ctx, uint32_t address) {
    cafe::set_current_context(&ctx);
    cafe::fatal("call through an unresolved weak symbol (address 0) at 0x%08X", address);
}

void cafe_ppc_bad_return(cafe::PPCContext& ctx, uint32_t expected) {
    cafe::set_current_context(&ctx);
    cafe::fatal("a guest call returned with LR=0x%08X instead of its return address "
                "0x%08X: the callee returned somewhere other than its caller "
                "(longjmp-style control flow the port does not model yet)",
                ctx.lr, expected);
}

// The Espresso time base runs at a quarter of the 248.625 MHz bus clock.
uint64_t cafe_ppc_timebase(void) {
    constexpr uint64_t kHz = 62156250;
    static const auto start = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    const auto t = static_cast<uint64_t>(ns);
    return (t / 1000000000u) * kHz + (t % 1000000000u) * kHz / 1000000000u;
}

void cafe_unimplemented_import(cafe::PPCContext& ctx, const char* module, const char* name) {
    // Miiverse (nn_olv, nn_olv2) and SpotPass (nn_boss) have closed, so the port does
    // not implement them: a call gets what a console without the service
    // would. Constructors and destructors leave their object in r3, queries
    // answer 0, everything else fails with an nn::Result.
    const std::string_view m = module, n = name;
    if (m == "nn_olv" || m == "nn_olv2" || m == "nn_boss") {
        static const bool trace = std::getenv("TTT2_TRACE_CLOSED") != nullptr;
        if (trace) std::fprintf(stderr, "closed: %s:%s (lr 0x%08X)\n", module, name, ctx.lr);
        if (n.starts_with("__ct__") || n.starts_with("__as__") || n.starts_with("__dt__")) return;
        // SpotPass: the player opted out, so titles neither ask about it nor schedule its tasks
        // (asked "not opted out", Smash prompts once online and then waits on tasks that never run).
        if (n == "GetOptoutFlag__Q3_2nn4boss5TitleCFv") {
            ctx.r[3] = 1;
            return;
        }
        // Miiverse starts and switches to online mode (as Cemu's does on Nintendo's closed service), then
        // its uploads and downloads fail like the closed service's. Connected at start, Smash waits on
        // both: a failed SwitchToOnlineMode left it on a black screen before its title.
        if (n.starts_with("Initialize__Q2_2nn3olvF") || n == "IsInitialized__Q2_2nn3olvFv" ||
            n == "SwitchToOnlineMode__Q2_2nn3olvFv") {
            ctx.r[3] = n.starts_with("Is") ? 1 : 0;
            return;
        }
        const bool query = (n.starts_with("Get") && !n.starts_with("GetResult")) || n.starts_with("Is") ||
                           n.starts_with("Test") || n.starts_with("__eq__");
        ctx.r[3] = query ? 0 : 0xA0A0CE00u;
        return;
    }
    cafe::set_current_context(&ctx);
    cafe::fatal("the game called %s:%s, which the runtime does not implement yet", module,
                name);
}

} // extern "C"
