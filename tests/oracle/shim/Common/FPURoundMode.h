#pragma once
#include "Common/CommonTypes.h"
namespace Common::FPU {
enum class RoundMode : u32 { Nearest = 0, TowardsZero = 1, TowardsPositiveInfinity = 2, TowardsNegativeInfinity = 3 };
}
