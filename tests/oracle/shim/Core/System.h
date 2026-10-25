#pragma once

// Oracle shim: Dolphin's System hands out emulator subsystems. Only
// system-level instructions reach them, and those effects (timers, caches,
// exceptions delivery) are outside what the oracle compares.

#include "Common/CommonTypes.h"

namespace PowerPC {
class MMU;
}

namespace Core {

class BranchWatch {
public:
    bool GetRecordingActive() const { return false; }
    template <typename... Args> void HitTrue(Args&&...) {}
    template <typename... Args> void HitFalse(Args&&...) {}
};

struct NullPowerPC {
    void CheckExceptions() {}
    void MSRUpdated() {}
    void WriteFullTimeBaseValue(u64) {}
};
struct NullTimers {
    void DecrementerSet() {}
    void TimeBaseSet() {}
    u32 GetFakeDecrementer() { return 0; }
    u64 GetFakeTimeBase() { return 0; }
};
struct NullJit {
    void InvalidateICacheLine(u32) {}
};
struct NullFifo {
    bool IsBNE() { return false; }
    void ResetGatherPipe() {}
};
struct NullMemory {};

class System {
public:
    explicit System(PowerPC::MMU& mmu) : m_mmu(mmu) {}
    PowerPC::MMU& GetMMU() { return m_mmu; }
    NullPowerPC& GetPowerPC() { return m_powerpc; }
    NullTimers& GetSystemTimers() { return m_timers; }
    NullJit& GetJitInterface() { return m_jit; }
    NullFifo& GetGPFifo() { return m_fifo; }
    NullMemory& GetMemory() { return m_memory; }

private:
    PowerPC::MMU& m_mmu;
    NullPowerPC m_powerpc;
    NullTimers m_timers;
    NullJit m_jit;
    NullFifo m_fifo;
    NullMemory m_memory;
};

} // namespace Core

class PPCSymbolDB {};
