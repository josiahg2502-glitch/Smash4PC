#pragma once

// Guest address-space decisions the recompiler bakes into generated code and
// the runtime must reproduce. Changing any value requires regenerating.

#include <cstdint>

namespace cafe::layout {

// Imported data objects (coreinit's _iob, MEMAllocFromDefaultHeap, ...). The
// RPX's 8-byte stub slots are too small: Tekken addresses _iob + 16. Each
// object gets its own slot here, in import order, so recompiler and runtime
// agree without a side table. The range sits in the code region above the
// game's .text, where nothing else is mapped.
constexpr uint32_t kDataImportBase = 0x0FF00000;
constexpr uint32_t kDataImportSlot = 0x1000;
constexpr uint32_t kDataImportLimit = 0x0FF80000;

// Guest-visible addresses for native functions the runtime hands to guest
// code as function pointers (e.g. the value stored in
// MEMAllocFromDefaultHeap). Indirect calls to these dispatch to host code.
constexpr uint32_t kHostThunkBase = 0x0FF80000;
constexpr uint32_t kHostThunkLimit = 0x10000000;

// The Wii U virtual memory map as titles see it (addresses from Cemu's MMU
// map and the PPC kernel). Only the ranges in use are committed.
constexpr uint32_t kSystemHeapBase = 0x01000000; // OS-owned guest objects
constexpr uint32_t kSystemHeapSize = 0x00800000;
constexpr uint32_t kCodeBase = 0x02000000;
constexpr uint32_t kMem2Base = 0x10000000;
constexpr uint32_t kMem2End = 0x50000000;
constexpr uint32_t kForegroundBucketBase = 0xE0000000;
constexpr uint32_t kForegroundBucketSize = 0x04000000;
constexpr uint32_t kMem1Base = 0xF4000000;
constexpr uint32_t kMem1Size = 0x02000000;
// Tiling apertures (GX2AllocateTilingApertureEx): linear CPU views of tiled
// surfaces, one slot each.
constexpr uint32_t kTilingApertureBase = 0xD0000000;
constexpr uint32_t kTilingApertureSlot = 0x02000000;
constexpr uint32_t kTilingApertureSlots = 8;
// Shared data the system maps for every title: the system fonts
// (OSGetSharedData), committed as they are loaded.
constexpr uint32_t kSharedDataBase = 0xF8000000;
constexpr uint32_t kSharedDataSize = 0x02000000;
// Each core's locked-cache scratchpad (LCAlloc), 16 KiB.
constexpr uint32_t kLockedCacheBase[3] = {0xFFC00000, 0xFFC40000, 0xFFC80000};
constexpr uint32_t kLockedCacheSize = 0x4000;

} // namespace cafe::layout
