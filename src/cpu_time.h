#pragma once

#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace shogi {

// プロセス全体（全ワーカー）の消費 CPU 時間。取得できなければ負値を返す。
inline double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER kernel_ticks{}, user_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    return (static_cast<double>(kernel_ticks.QuadPart) +
            static_cast<double>(user_ticks.QuadPart)) / 10000000.0;
#else
    const auto ticks = std::clock();
    return ticks == static_cast<std::clock_t>(-1) ? -1
        : static_cast<double>(ticks) / static_cast<double>(CLOCKS_PER_SEC);
#endif
}

}  // namespace shogi
