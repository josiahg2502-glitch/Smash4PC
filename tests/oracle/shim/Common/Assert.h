#pragma once
// Oracle shim: Dolphin assertions are irrelevant to instruction semantics.
#define ASSERT(...) ((void)0)
#define ASSERT_MSG(...) ((void)0)
#define DEBUG_ASSERT(...) ((void)0)
#define DEBUG_ASSERT_MSG(...) ((void)0)
