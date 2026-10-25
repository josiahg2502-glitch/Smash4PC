#pragma once
// Oracle shim: Dolphin's high-level emulation hooks are never installed.
namespace HLE {
template <typename... Args>
void Execute(Args&&...) {}
} // namespace HLE
