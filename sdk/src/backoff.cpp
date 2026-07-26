#include "pulsedb/sdk/backoff.hpp"

#include <algorithm>

namespace pulsedb::sdk {

std::chrono::milliseconds BackoffPolicy::delay_for(int attempt) const {
    if (attempt <= 0) {
        return std::min(initial_delay, max_delay);
    }

    double scaled = static_cast<double>(initial_delay.count());
    const double cap = static_cast<double>(max_delay.count());
    for (int i = 0; i < attempt; ++i) {
        scaled *= multiplier;
        if (scaled >= cap) {
            return max_delay;
        }
    }
    return std::min(std::chrono::milliseconds{static_cast<long long>(scaled)}, max_delay);
}

}  // namespace pulsedb::sdk
