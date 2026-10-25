// coreinit alarms. On hardware an alarm handler runs in interrupt context on
// the core that set it; here one alarm thread runs every handler while
// holding the interrupt lock, which keeps the same exclusion guarantees.
//
// OSAlarm (0x58 bytes, Cemu's layout): +0x00 magic 'aLrM', +0x0C handler,
// +0x10 tag, +0x18 next fire time, +0x28 period, +0x30 start, +0x38 user
// data.

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

#include <cstring>
#include <map>
#include <set>

extern "C" uint64_t cafe_ppc_timebase(void);

namespace cafe::os {

Thread* create_internal_thread(const char* name, int core);

namespace {

constexpr uint32_t kAlarmMagic = 0x614C724D; // 'aLrM'
constexpr uint64_t kTimerHz = 62156250;

std::multimap<uint64_t, uint32_t> g_schedule; // fire time (system ticks) -> alarm
std::set<uint32_t> g_armed;
Thread* g_alarm_thread = nullptr;
pthread_t g_alarm_host;

void disarm(uint32_t alarm) {
    if (!g_armed.erase(alarm)) return;
    for (auto it = g_schedule.begin(); it != g_schedule.end(); ++it) {
        if (it->second == alarm) {
            g_schedule.erase(it);
            break;
        }
    }
}

void arm(uint32_t alarm, uint64_t when) {
    disarm(alarm);
    field<uint64_t>(alarm, 0x18) = when;
    g_schedule.emplace(when, alarm);
    g_armed.insert(alarm);
    wake(kWaitAlarms);
}

void* alarm_main(void*) {
    Thread* t = g_alarm_thread;
    bind_current_thread(t);
    KernelLock lock(kernel_mutex());
    for (;;) {
        if (g_schedule.empty()) {
            wait_channel(kWaitAlarms).wait(lock);
            continue;
        }
        const uint64_t now = cafe_ppc_timebase();
        const auto next = g_schedule.begin();
        if (next->first > now) {
            const uint64_t wait = (next->first - now) * 1000000000ull / kTimerHz;
            wait_channel(kWaitAlarms).wait_for(lock, std::chrono::nanoseconds(wait));
            continue;
        }
        const uint32_t alarm = next->second;
        g_schedule.erase(next);
        g_armed.erase(alarm);
        const uint64_t period = field<uint64_t>(alarm, 0x28);
        if (period != 0) arm(alarm, field<uint64_t>(alarm, 0x18) + period);
        const uint32_t handler = field<uint32_t>(alarm, 0x0C);
        acquire_interrupt_lock(lock, t);
        lock.unlock();
        // handler(alarm, interrupted context)
        call_guest(t->ctx, handler, {alarm, t->guest});
        lock.lock();
        bool held = false;
        release_interrupt_lock_for_wait(t, held);
    }
    return nullptr;
}

void ensure_alarm_thread() {
    if (g_alarm_thread != nullptr) return;
    g_alarm_thread = create_internal_thread("cafe alarms", 0);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_create(&g_alarm_host, &attr, alarm_main, nullptr);
    pthread_attr_destroy(&attr);
    pthread_detach(g_alarm_host);
}

void OSCreateAlarm(GuestAddress alarm) {
    std::memset(alarm.as<uint8_t>(), 0, 0x58);
    field<uint32_t>(alarm.value, 0) = kAlarmMagic;
}
// The name (at 0x04) is only for debuggers.
void OSCreateAlarmEx(GuestAddress alarm, uint32_t name) {
    OSCreateAlarm(alarm);
    field<uint32_t>(alarm.value, 4) = name;
}

void set_alarm(uint32_t alarm, uint64_t when, uint64_t period, uint32_t handler) {
    ensure_alarm_thread();
    KernelLock lock(kernel_mutex());
    field<uint32_t>(alarm, 0x0C) = handler;
    field<uint64_t>(alarm, 0x28) = period;
    field<uint64_t>(alarm, 0x30) = when;
    arm(alarm, when);
}

bool OSSetAlarm(GuestAddress alarm, uint64_t delay, uint32_t handler) {
    set_alarm(alarm.value, cafe_ppc_timebase() + delay, 0, handler);
    return true;
}

// First fire at `start` (system ticks), then every `period` ticks.
bool OSSetPeriodicAlarm(GuestAddress alarm, uint64_t start, uint64_t period, uint32_t handler) {
    uint64_t when = start;
    const uint64_t now = cafe_ppc_timebase();
    if (period != 0 && when < now) when += (now - when + period - 1) / period * period;
    set_alarm(alarm.value, when, period, handler);
    return true;
}

bool OSCancelAlarm(GuestAddress alarm) {
    KernelLock lock(kernel_mutex());
    const bool was_armed = g_armed.count(alarm.value) != 0;
    disarm(alarm.value);
    return was_armed;
}

void OSCancelAlarms(uint32_t tag) {
    KernelLock lock(kernel_mutex());
    for (auto it = g_armed.begin(); it != g_armed.end();) {
        const uint32_t alarm = *it++;
        if (field<uint32_t>(alarm, 0x10) == tag) disarm(alarm);
    }
}

void OSSetAlarmUserData(GuestAddress alarm, uint32_t data) { field<uint32_t>(alarm.value, 0x38) = data; }
uint32_t OSGetAlarmUserData(GuestAddress alarm) { return field<uint32_t>(alarm.value, 0x38); }

} // namespace

CAFE_EXPORT(coreinit, OSCreateAlarm, OSCreateAlarm);
CAFE_EXPORT(coreinit, OSCreateAlarmEx, OSCreateAlarmEx);
CAFE_EXPORT(coreinit, OSSetAlarm, OSSetAlarm);
CAFE_EXPORT(coreinit, OSSetPeriodicAlarm, OSSetPeriodicAlarm);
CAFE_EXPORT(coreinit, OSCancelAlarm, OSCancelAlarm);
CAFE_EXPORT(coreinit, OSCancelAlarms, OSCancelAlarms);
CAFE_EXPORT(coreinit, OSSetAlarmUserData, OSSetAlarmUserData);
CAFE_EXPORT(coreinit, OSGetAlarmUserData, OSGetAlarmUserData);

} // namespace cafe::os
