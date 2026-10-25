#pragma once

// Guest memory owned by the OS layer: thread structures, OS-internal
// objects, strings handed to the game. Thread-safe first-fit allocator over
// layout::kSystemHeapBase.

#include <cstdint>

namespace cafe {

void init_system_heap();
uint32_t system_alloc(uint32_t size, uint32_t alignment = 16);
void system_free(uint32_t address);

} // namespace cafe
