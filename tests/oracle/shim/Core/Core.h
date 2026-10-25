#pragma once
// Oracle shim: Dolphin's CPU-thread guard; the oracle has a single thread.
#include "Core/System.h"
namespace Core {
struct CPUThreadGuard {
    explicit CPUThreadGuard(System&) {}
};
} // namespace Core
