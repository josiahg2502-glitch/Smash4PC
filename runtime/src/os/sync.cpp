// OSMutex, OSCond, OSSemaphore, OSEvent, OSMessageQueue, OSThreadQueue and
// the interrupt lock. Observable state lives in the game's structures
// (layouts as in Cemu's coreinit headers); host maps hold wake-up
// generations for objects whose state is only "who is sleeping".

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace cafe::os {
namespace {

// ------------------------------------------------------------ interrupts
// Disabling interrupts on a Cafe core stops preemption there; games use it
// as a lock against other threads on that core and against alarm callbacks.
// With every guest thread truly parallel it has to exclude all of them.
Thread* g_interrupt_owner = nullptr;

constexpr uint32_t kMutexMagic = 0x6D557458;     // 'mUtX'
constexpr uint32_t kCondMagic = 0x634E6456;      // 'cNdV'
constexpr uint32_t kSemaphoreMagic = 0x73506852; // 'sPhR'
constexpr uint32_t kEventMagic = 0x65566E54;     // 'eVnT'
constexpr uint32_t kQueueMagic = 0x6D536751;     // 'mSgQ'

// Wake-up generation per object address (conditions, thread queues).
std::unordered_map<uint32_t, uint64_t> g_generations;

// OSMutex: +0x1C owner, +0x20 lock count. OSThreadQueue at +0x0C.
uint32_t mutex_owner(uint32_t m) { return field<uint32_t>(m, 0x1C); }

void lock_mutex(KernelLock& lock, uint32_t mutex, uint32_t self) {
    wait_until(lock, mutex, [&] {
        const uint32_t owner = mutex_owner(mutex);
        return owner == 0 || owner == self;
    });
    field<uint32_t>(mutex, 0x1C) = self;
    field<int32_t>(mutex, 0x20) += 1;
}

void unlock_mutex(uint32_t mutex, uint32_t self) {
    if (mutex_owner(mutex) != self) {
        fatal("OSUnlockMutex on 0x%08X by a thread that does not own it", mutex);
    }
    be<int32_t>& count = field<int32_t>(mutex, 0x20);
    count -= 1;
    if (count == 0) {
        field<uint32_t>(mutex, 0x1C) = 0;
        wake(mutex);
    }
}

} // namespace

void acquire_interrupt_lock(KernelLock& lock, Thread* self) {
    wait_channel(kWaitInterrupts).wait(lock, [&] { return g_interrupt_owner == nullptr || g_interrupt_owner == self; });
    g_interrupt_owner = self;
    self->interrupts_disabled = true;
}

void release_interrupt_lock_for_wait(Thread* self, bool& was_held) {
    was_held = self != nullptr && g_interrupt_owner == self;
    if (was_held) {
        g_interrupt_owner = nullptr;
        self->interrupts_disabled = false;
        wake(kWaitInterrupts);
    }
}

void reacquire_interrupt_lock_after_wait(KernelLock& lock, Thread* self, bool was_held) {
    if (was_held) {
        acquire_interrupt_lock(lock, self);
    }
}

namespace {

uint32_t OSDisableInterrupts() {
    KernelLock lock(kernel_mutex());
    Thread* self = current_thread();
    if (self->interrupts_disabled) return 0;
    acquire_interrupt_lock(lock, self);
    return 1;
}

uint32_t OSRestoreInterrupts(uint32_t enable) {
    KernelLock lock(kernel_mutex());
    Thread* self = current_thread();
    const uint32_t previous = self->interrupts_disabled ? 0 : 1;
    if (enable && self->interrupts_disabled) {
        bool held = false;
        release_interrupt_lock_for_wait(self, held);
    } else if (!enable && !self->interrupts_disabled) {
        acquire_interrupt_lock(lock, self);
    }
    return previous;
}

// ------------------------------------------------------------------ mutex
void OSInitMutex(GuestAddress mutex) {
    std::memset(mutex.as<uint8_t>(), 0, 0x2C);
    field<uint32_t>(mutex.value, 0) = kMutexMagic;
}

void OSLockMutex(GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    lock_mutex(lock, mutex.value, current_thread()->guest);
}

bool OSTryLockMutex(GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    const uint32_t self = current_thread()->guest;
    const uint32_t owner = mutex_owner(mutex.value);
    if (owner != 0 && owner != self) return false;
    field<uint32_t>(mutex.value, 0x1C) = self;
    field<int32_t>(mutex.value, 0x20) += 1;
    return true;
}

void OSUnlockMutex(GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    unlock_mutex(mutex.value, current_thread()->guest);
}

// -------------------------------------------------------------- condition
void OSInitCond(GuestAddress cond) {
    std::memset(cond.as<uint8_t>(), 0, 0x1C);
    field<uint32_t>(cond.value, 0) = kCondMagic;
}

// Releases the mutex completely, waits for a signal, then restores the
// mutex with its previous recursion count.
void OSWaitCond(GuestAddress cond, GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    const uint32_t self = current_thread()->guest;
    if (mutex_owner(mutex.value) != self) {
        fatal("OSWaitCond with mutex 0x%08X not held by the caller", mutex.value);
    }
    const int32_t depth = field<int32_t>(mutex.value, 0x20);
    field<int32_t>(mutex.value, 0x20) = 0;
    field<uint32_t>(mutex.value, 0x1C) = 0;
    wake(mutex.value);
    const uint64_t generation = g_generations[cond.value];
    wait_until(lock, cond.value, [&] { return g_generations[cond.value] != generation; });
    wait_until(lock, mutex.value, [&] { return mutex_owner(mutex.value) == 0; });
    field<uint32_t>(mutex.value, 0x1C) = self;
    field<int32_t>(mutex.value, 0x20) = depth;
}

// Wakes every thread waiting on the condition.
void OSSignalCond(GuestAddress cond) {
    KernelLock lock(kernel_mutex());
    ++g_generations[cond.value];
    wake(cond.value);
}

// -------------------------------------------------------------- semaphore
// OSSemaphore: +0x0C count.
void OSInitSemaphore(GuestAddress semaphore, int32_t count) {
    std::memset(semaphore.as<uint8_t>(), 0, 0x20);
    field<uint32_t>(semaphore.value, 0) = kSemaphoreMagic;
    field<int32_t>(semaphore.value, 0x0C) = count;
}

int32_t OSWaitSemaphore(GuestAddress semaphore) {
    KernelLock lock(kernel_mutex());
    be<int32_t>& count = field<int32_t>(semaphore.value, 0x0C);
    wait_until(lock, semaphore.value, [&] { return count > 0; });
    const int32_t previous = count;
    count = previous - 1;
    return previous;
}

int32_t OSTryWaitSemaphore(GuestAddress semaphore) {
    KernelLock lock(kernel_mutex());
    be<int32_t>& count = field<int32_t>(semaphore.value, 0x0C);
    const int32_t previous = count;
    if (previous > 0) count = previous - 1;
    return previous;
}

int32_t OSSignalSemaphore(GuestAddress semaphore) {
    KernelLock lock(kernel_mutex());
    be<int32_t>& count = field<int32_t>(semaphore.value, 0x0C);
    const int32_t previous = count;
    count = previous + 1;
    wake(semaphore.value);
    return previous;
}

// ------------------------------------------------------------------ event
// OSEvent: +0x0C state (0 unsignalled, 1 signalled), +0x20 mode (0 manual
// reset, 1 auto reset). As on hardware, an auto-reset event signalled while
// threads wait hands the signal to a waiter (or, for SignalAll, to every
// waiter) and stays unsignalled; it only latches when nobody waits.
struct EventWaiters {
    uint32_t waiting = 0;
    uint32_t handoffs = 0;   // single wake-ups not yet claimed
    uint64_t generation = 0; // bumped to release every current waiter
};
std::unordered_map<uint32_t, EventWaiters> g_events;

bool event_auto(uint32_t e) { return field<uint32_t>(e, 0x20) == 1u; }

void OSInitEvent(GuestAddress event, uint32_t signalled, uint32_t mode) {
    std::memset(event.as<uint8_t>(), 0, 0x24);
    field<uint32_t>(event.value, 0) = kEventMagic;
    field<uint32_t>(event.value, 0x0C) = signalled ? 1u : 0u;
    field<uint32_t>(event.value, 0x20) = mode;
    g_events.erase(event.value);
}

// Returns whether the event was obtained (false only on timeout).
bool wait_event(KernelLock& lock, uint32_t e, const uint64_t* timeout_ns) {
    be<uint32_t>& state = field<uint32_t>(e, 0x0C);
    if (state != 0u) {
        if (event_auto(e)) state = 0u;
        return true;
    }
    if (timeout_ns != nullptr && *timeout_ns == 0) return false;
    EventWaiters& w = g_events[e];
    const uint64_t generation = w.generation;
    ++w.waiting;
    const auto ready = [&] { return state != 0u || w.handoffs > 0 || w.generation != generation; };
    bool obtained = true;
    if (timeout_ns != nullptr) {
        obtained = wait_until_for(lock, e, *timeout_ns, ready);
    } else {
        wait_until(lock, e, ready);
    }
    --w.waiting;
    if (!obtained) return false;
    if (w.generation != generation) return true;
    if (w.handoffs > 0) {
        --w.handoffs;
        return true;
    }
    if (event_auto(e)) state = 0u;
    return true;
}

// TTT2_TRACE_EVENTS=1: waits on an event over 50 ms (the event, the waiting
// guest's return address), and who signals those events.
const bool g_trace_events = std::getenv("TTT2_TRACE_EVENTS") != nullptr;
std::unordered_map<uint32_t, bool> g_slow_events;

// TTT2_TRACE_EVENTS=2 also prints, every 5 s, the time each thread spent
// waiting per event and return address, and who signalled those events.
struct WaitTotal {
    double ms = 0;
    uint32_t waits = 0;
};
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, WaitTotal> g_wait_totals; // thread, event, lr
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> g_signal_counts; // event, thread, lr
auto g_totals_since = std::chrono::steady_clock::now();

void report_event_totals() {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_totals_since < std::chrono::seconds(5)) return;
    g_totals_since = now;
    for (const auto& [k, t] : g_wait_totals) {
        if (t.ms < 200) continue;
        std::fprintf(stderr, "event: thread %08X waited %.0f ms in %u waits on 0x%08X (lr 0x%08X)\n", std::get<0>(k),
                     t.ms, t.waits, std::get<1>(k), std::get<2>(k));
        for (const auto& [s, n] : g_signal_counts) {
            if (std::get<0>(s) == std::get<1>(k)) {
                std::fprintf(stderr, "event:   signalled %u times by %08X (lr 0x%08X)\n", n, std::get<1>(s),
                             std::get<2>(s));
            }
        }
    }
    g_wait_totals.clear();
    g_signal_counts.clear();
}

void OSWaitEvent(PPCContext& ctx, GuestAddress event) {
    const auto started = std::chrono::steady_clock::now();
    KernelLock lock(kernel_mutex());
    wait_event(lock, event.value, nullptr);
    if (g_trace_events) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (std::getenv("TTT2_TRACE_EVENTS")[0] == '2') {
            WaitTotal& t = g_wait_totals[{current_thread()->guest, event.value, ctx.lr}];
            t.ms += ms;
            ++t.waits;
            g_slow_events[event.value] = true;
            report_event_totals();
        } else if (ms > 50) {
            g_slow_events[event.value] = true;
            std::fprintf(stderr, "event: thread %08X waited %.0f ms on event 0x%08X (lr 0x%08X)\n",
                         current_thread()->guest, ms, event.value, ctx.lr);
        }
    }
}

bool OSWaitEventWithTimeout(GuestAddress event, uint64_t timeout_ns) {
    KernelLock lock(kernel_mutex());
    return wait_event(lock, event.value, &timeout_ns);
}

void signal_event(uint32_t e, bool everyone) {
    be<uint32_t>& state = field<uint32_t>(e, 0x0C);
    if (state != 0u) return;
    EventWaiters& w = g_events[e];
    if (!event_auto(e)) {
        state = 1u;
        ++w.generation;
    } else if (w.waiting > w.handoffs) {
        if (everyone) {
            ++w.generation;
            w.handoffs = 0;
        } else {
            ++w.handoffs;
        }
    } else {
        state = 1u;
    }
    wake(e);
}

void OSSignalEvent(PPCContext& ctx, GuestAddress event) {
    KernelLock lock(kernel_mutex());
    if (g_trace_events && std::getenv("TTT2_TRACE_EVENTS")[0] == '2') {
        ++g_signal_counts[{event.value, current_thread()->guest, ctx.lr}];
    } else if (g_trace_events && g_slow_events.count(event.value)) {
        std::fprintf(stderr, "event: thread %08X signals 0x%08X (lr 0x%08X)\n", current_thread()->guest, event.value,
                     ctx.lr);
    }
    signal_event(event.value, false);
}

void OSSignalEventAll(GuestAddress event) {
    KernelLock lock(kernel_mutex());
    signal_event(event.value, true);
}

void OSResetEvent(GuestAddress event) {
    KernelLock lock(kernel_mutex());
    field<uint32_t>(event.value, 0x0C) = 0u;
}

// ----------------------------------------------------------- message queue
// OSMessageQueue: +0x2C message array, +0x30 capacity, +0x34 first index,
// +0x38 used count. A message is four words.
constexpr uint32_t kMessageBlocking = 1;
constexpr uint32_t kMessageHighPriority = 2;

void OSInitMessageQueue(GuestAddress queue, uint32_t messages, int32_t count) {
    std::memset(queue.as<uint8_t>(), 0, 0x3C);
    field<uint32_t>(queue.value, 0) = kQueueMagic;
    field<uint32_t>(queue.value, 0x2C) = messages;
    field<uint32_t>(queue.value, 0x30) = static_cast<uint32_t>(count);
}

bool OSSendMessage(GuestAddress queue, GuestAddress message, uint32_t flags) {
    KernelLock lock(kernel_mutex());
    const uint32_t q = queue.value;
    const uint32_t capacity = field<uint32_t>(q, 0x30);
    be<uint32_t>& used = field<uint32_t>(q, 0x38);
    be<uint32_t>& first = field<uint32_t>(q, 0x34);
    if (used == capacity) {
        if (!(flags & kMessageBlocking)) return false;
        wait_until(lock, q, [&] { return used < capacity; });
    }
    uint32_t slot;
    if (flags & kMessageHighPriority) {
        // Jammed messages go in front of everything queued.
        first = (first + capacity - 1) % capacity;
        slot = first;
    } else {
        slot = (first + used) % capacity;
    }
    std::memcpy(guest_pointer(field<uint32_t>(q, 0x2C) + slot * 16), message.as<uint8_t>(), 16);
    used += 1;
    wake(q);
    return true;
}

bool OSReceiveMessage(GuestAddress queue, GuestAddress message, uint32_t flags) {
    KernelLock lock(kernel_mutex());
    const uint32_t q = queue.value;
    const uint32_t capacity = field<uint32_t>(q, 0x30);
    be<uint32_t>& used = field<uint32_t>(q, 0x38);
    be<uint32_t>& first = field<uint32_t>(q, 0x34);
    if (used == 0u) {
        if (!(flags & kMessageBlocking)) return false;
        wait_until(lock, q, [&] { return used != 0u; });
    }
    std::memcpy(message.as<uint8_t>(), guest_pointer(field<uint32_t>(q, 0x2C) + first * 16), 16);
    first = (first + 1) % capacity;
    used -= 1;
    wake(q);
    return true;
}

// ----------------------------------------------------------- thread queue
void OSInitThreadQueue(GuestAddress queue) { std::memset(queue.as<uint8_t>(), 0, 0x10); }

void OSSleepThread(GuestAddress queue) {
    KernelLock lock(kernel_mutex());
    const uint64_t generation = g_generations[queue.value];
    wait_until(lock, queue.value, [&] { return g_generations[queue.value] != generation; });
}

void OSWakeupThread(GuestAddress queue) {
    KernelLock lock(kernel_mutex());
    ++g_generations[queue.value];
    wake(queue.value);
}

void OSInitEventEx(GuestAddress event, uint32_t signalled, uint32_t mode, uint32_t) {
    OSInitEvent(event, signalled, mode);
}

// The Green Hills C library's lock hooks: *slot holds an OSMutex.
void ghs_mtx_init(GuestAddress slot) {
    const uint32_t mutex = system_alloc(0x2C, 8);
    OSInitMutex(GuestAddress{mutex});
    *slot.as<be<uint32_t>>() = mutex;
}
void ghs_mtx_dst(GuestAddress) {}
void ghs_mtx_lock(GuestAddress slot) { OSLockMutex(GuestAddress{*slot.as<be<uint32_t>>()}); }
void ghs_mtx_unlock(GuestAddress slot) { OSUnlockMutex(GuestAddress{*slot.as<be<uint32_t>>()}); }

// shortcut: OSFastMutex is an OSMutex in the same 0x2C bytes; replace if the game reads its fields.
void OSFastMutex_Init(GuestAddress mutex, uint32_t) { OSInitMutex(mutex); }

// OSSpinLock: owning thread at 0x0, recursion count at 0x8. Guest threads
// are host threads, so a spin with yields is a real spin lock.
void OSInitSpinLock(GuestAddress spin) { std::memset(spin.as<uint8_t>(), 0, 0x10); }
void OSAcquireSpinLock(GuestAddress spin) {
    auto* owner = spin.as<uint32_t>();
    const uint32_t self = __builtin_bswap32(current_thread()->guest);
    if (__atomic_load_n(owner, __ATOMIC_ACQUIRE) == self) {
        field<uint32_t>(spin.value, 8) = field<uint32_t>(spin.value, 8) + 1;
        return;
    }
    for (uint32_t expected = 0; !__atomic_compare_exchange_n(owner, &expected, self, false, __ATOMIC_ACQUIRE,
                                                             __ATOMIC_RELAXED);
         expected = 0) {
        std::this_thread::yield();
    }
}
void OSReleaseSpinLock(GuestAddress spin) {
    if (field<uint32_t>(spin.value, 8) != 0) {
        field<uint32_t>(spin.value, 8) = field<uint32_t>(spin.value, 8) - 1;
        return;
    }
    __atomic_store_n(spin.as<uint32_t>(), 0u, __ATOMIC_RELEASE);
}

} // namespace

CAFE_EXPORT(coreinit, OSInitEventEx, OSInitEventEx);
CAFE_EXPORT(coreinit, __ghs_mtx_init, ghs_mtx_init);
CAFE_EXPORT(coreinit, __ghs_mtx_dst, ghs_mtx_dst);
CAFE_EXPORT(coreinit, __ghs_mtx_lock, ghs_mtx_lock);
CAFE_EXPORT(coreinit, __ghs_mtx_unlock, ghs_mtx_unlock);
CAFE_EXPORT(coreinit, OSFastMutex_Init, OSFastMutex_Init);
CAFE_EXPORT(coreinit, OSFastMutex_Lock, OSLockMutex);
CAFE_EXPORT(coreinit, OSFastMutex_Unlock, OSUnlockMutex);
CAFE_EXPORT(coreinit, OSInitSpinLock, OSInitSpinLock);
CAFE_EXPORT(coreinit, OSAcquireSpinLock, OSAcquireSpinLock);
CAFE_EXPORT(coreinit, OSReleaseSpinLock, OSReleaseSpinLock);

CAFE_EXPORT(coreinit, OSDisableInterrupts, OSDisableInterrupts);
CAFE_EXPORT(coreinit, OSRestoreInterrupts, OSRestoreInterrupts);
CAFE_EXPORT(coreinit, OSInitMutex, OSInitMutex);
CAFE_EXPORT(coreinit, OSLockMutex, OSLockMutex);
CAFE_EXPORT(coreinit, OSTryLockMutex, OSTryLockMutex);
CAFE_EXPORT(coreinit, OSUnlockMutex, OSUnlockMutex);
CAFE_EXPORT(coreinit, OSInitCond, OSInitCond);
CAFE_EXPORT(coreinit, OSWaitCond, OSWaitCond);
CAFE_EXPORT(coreinit, OSSignalCond, OSSignalCond);
CAFE_EXPORT(coreinit, OSInitSemaphore, OSInitSemaphore);
CAFE_EXPORT(coreinit, OSWaitSemaphore, OSWaitSemaphore);
CAFE_EXPORT(coreinit, OSTryWaitSemaphore, OSTryWaitSemaphore);
CAFE_EXPORT(coreinit, OSSignalSemaphore, OSSignalSemaphore);
CAFE_EXPORT(coreinit, OSInitEvent, OSInitEvent);
CAFE_EXPORT(coreinit, OSWaitEvent, OSWaitEvent);
CAFE_EXPORT(coreinit, OSWaitEventWithTimeout, OSWaitEventWithTimeout);
CAFE_EXPORT(coreinit, OSSignalEvent, OSSignalEvent);
CAFE_EXPORT(coreinit, OSSignalEventAll, OSSignalEventAll);
CAFE_EXPORT(coreinit, OSResetEvent, OSResetEvent);
CAFE_EXPORT(coreinit, OSInitMessageQueue, OSInitMessageQueue);
bool send_message(uint32_t queue, uint32_t message, uint32_t flags) {
    return OSSendMessage(GuestAddress{queue}, GuestAddress{message}, flags);
}

CAFE_EXPORT(coreinit, OSSendMessage, OSSendMessage);
CAFE_EXPORT(coreinit, OSReceiveMessage, OSReceiveMessage);
CAFE_EXPORT(coreinit, OSInitThreadQueue, OSInitThreadQueue);
CAFE_EXPORT(coreinit, OSSleepThread, OSSleepThread);
CAFE_EXPORT(coreinit, OSWakeupThread, OSWakeupThread);

} // namespace cafe::os
