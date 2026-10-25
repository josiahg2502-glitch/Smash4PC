// proc_ui: the application lifecycle. The title polls ProcUIProcessMessages
// every frame; it stays in the foreground until the user closes the window,
// then gets its exit callbacks and the Exit status.

#include "kernel.h"

#include "cafe/export.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace cafe::os {

std::atomic<bool> g_exit_requested{false};

namespace {

enum Status : uint32_t { kForeground = 0, kBackground = 1, kReleasing = 2, kExit = 3 };
enum Callback : uint32_t { kAcquireForeground, kReleaseForeground, kExitCallback, kNetIoStart,
                           kNetIoStop, kHomeButtonDenied, kCallbackCount };

struct Registered {
    uint32_t function, argument;
    int32_t priority;
};
std::vector<Registered> g_callbacks[kCallbackCount];
uint32_t g_ready_to_release = 0;
bool g_exit_sent = false;

void ProcUIInit(uint32_t ready_to_release) { g_ready_to_release = ready_to_release; }

void ProcUIRegisterCallback(uint32_t type, uint32_t function, uint32_t argument, int32_t priority) {
    if (type >= kCallbackCount) return;
    auto& list = g_callbacks[type];
    list.push_back({function, argument, priority});
    // Higher priority runs first.
    std::stable_sort(list.begin(), list.end(),
                     [](const Registered& a, const Registered& b) { return a.priority > b.priority; });
}

void ProcUIRegisterBackgroundCallback(uint32_t, uint32_t, uint64_t) {}

void ProcUIClearCallbacks() {
    for (auto& list : g_callbacks) list.clear();
}

uint32_t ProcUIProcessMessages(PPCContext& ctx, bool) {
    if (!g_exit_requested.load()) return kForeground;
    if (!g_exit_sent) {
        g_exit_sent = true;
        for (const Registered& r : g_callbacks[kExitCallback]) call_guest(ctx, r.function, {r.argument});
    }
    return kExit;
}

// The Ex forms take the save callback's argument and the core to run a
// callback on; the runtime runs callbacks on the polling thread either way.
void ProcUIInitEx(uint32_t ready_to_release, uint32_t) { ProcUIInit(ready_to_release); }
void ProcUIRegisterCallbackCore(uint32_t type, uint32_t function, uint32_t argument, int32_t priority, uint32_t) {
    ProcUIRegisterCallback(type, function, argument, priority);
}
bool ProcUIInForeground() { return !g_exit_requested.load(); }

void ProcUIShutdown() {}
void ProcUISetMEM1Storage(uint32_t, uint32_t) {}
void ProcUIDrawDoneRelease() {}

} // namespace

CAFE_EXPORT(proc_ui, ProcUIInit, ProcUIInit);
CAFE_EXPORT(proc_ui, ProcUIRegisterCallback, ProcUIRegisterCallback);
CAFE_EXPORT(proc_ui, ProcUIRegisterBackgroundCallback, ProcUIRegisterBackgroundCallback);
CAFE_EXPORT(proc_ui, ProcUIClearCallbacks, ProcUIClearCallbacks);
CAFE_EXPORT(proc_ui, ProcUIProcessMessages, ProcUIProcessMessages);
CAFE_EXPORT(proc_ui, ProcUISubProcessMessages, ProcUIProcessMessages);
CAFE_EXPORT(proc_ui, ProcUIInitEx, ProcUIInitEx);
CAFE_EXPORT(proc_ui, ProcUIRegisterCallbackCore, ProcUIRegisterCallbackCore);
CAFE_EXPORT(proc_ui, ProcUIInForeground, ProcUIInForeground);
CAFE_EXPORT(proc_ui, ProcUIShutdown, ProcUIShutdown);
CAFE_EXPORT(proc_ui, ProcUISetMEM1Storage, ProcUISetMEM1Storage);
CAFE_EXPORT(proc_ui, ProcUIDrawDoneRelease, ProcUIDrawDoneRelease);

} // namespace cafe::os
