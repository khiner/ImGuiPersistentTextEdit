#pragma once

#include <chrono>

// Replaces std::chrono::system_clock in the test build of upstream. Uses the same time_point type
// because upstream's header declares members with it. The harness sets `Current`.
struct HarnessClock {
    using time_point = std::chrono::system_clock::time_point;

    static inline time_point Current{};
    static time_point now() { return Current; }
};
