#include "pulsedb/query/system_stats.hpp"

#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>

#include <cstdio>
#include <cstring>
#endif

namespace pulsedb::query {
namespace {

std::uint64_t wall_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#ifdef _WIN32

std::uint64_t process_cpu_ns() {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto to_ns = [](const FILETIME& ft) -> std::uint64_t {
        ULARGE_INTEGER li;
        li.LowPart = ft.dwLowDateTime;
        li.HighPart = ft.dwHighDateTime;
        return static_cast<std::uint64_t>(li.QuadPart) * 100ULL;  // 100ns units -> ns
    };
    return to_ns(kernel) + to_ns(user);
}

#else

std::uint64_t process_cpu_ns() {
    std::FILE* f = std::fopen("/proc/self/stat", "r");
    if (!f) {
        return 0;
    }
    char buffer[2048];
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    buffer[n] = '\0';

    // Field 2 (comm) is parenthesised and may contain spaces; parse after
    // the final ')'. utime/stime are fields 14/15 (12th/13th after ')').
    const char* p = std::strrchr(buffer, ')');
    if (!p) {
        return 0;
    }
    // The skipped conversions carry no length modifier: with assignment
    // suppression there is no destination for one to describe, and GCC warns on
    // the combination (-Wformat). Parsing is unaffected -- the digits of
    // minflt/cminflt/majflt/cmajflt are consumed either way.
    unsigned long long utime = 0, stime = 0;
    if (std::sscanf(p + 1,
                    " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                    &utime, &stime) != 2) {
        return 0;
    }
    const long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        return 0;
    }
    return (static_cast<std::uint64_t>(utime) + stime) * 1'000'000'000ULL /
           static_cast<std::uint64_t>(hz);
}

#endif

}  // namespace

double SystemStats::cpu_percent() {
    const std::uint64_t cpu = process_cpu_ns();
    const std::uint64_t wall = wall_ns();
    if (last_wall_ns_ == 0) {
        last_cpu_ns_ = cpu;
        last_wall_ns_ = wall;
        return 0.0;
    }
    const double d_cpu = static_cast<double>(cpu - last_cpu_ns_);
    const double d_wall = static_cast<double>(wall - last_wall_ns_);
    last_cpu_ns_ = cpu;
    last_wall_ns_ = wall;
    if (d_wall <= 0.0) {
        return 0.0;
    }
    return 100.0 * d_cpu / d_wall;
}

#ifdef _WIN32

std::size_t SystemStats::memory_bytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<std::size_t>(pmc.WorkingSetSize);
    }
    return 0;
}

#else

std::size_t SystemStats::memory_bytes() {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }
    unsigned long long total_pages = 0, resident_pages = 0;
    const int matched = std::fscanf(f, "%llu %llu", &total_pages, &resident_pages);
    std::fclose(f);
    if (matched != 2) {
        return 0;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    return static_cast<std::size_t>(resident_pages) * static_cast<std::size_t>(page_size);
}

#endif

}  // namespace pulsedb::query
