#pragma once

// Offset of local time from UTC at `when`, in seconds (daylight saving included).

#include <ctime>
#include <cstdint>

namespace cafe::host {

inline int64_t utc_offset_seconds(time_t when) {
#ifdef _WIN32
    struct tm local;
    localtime_s(&local, &when);
    return static_cast<int64_t>(_mkgmtime(&local) - when);
#else
    struct tm local;
    localtime_r(&when, &local);
    return local.tm_gmtoff;
#endif
}

} // namespace cafe::host
