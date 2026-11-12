#pragma once

#include <iostream>
#include <sstream>

namespace rudp::log {

inline bool& verbose_flag() {
    static bool verbose = false;
    return verbose;
}

inline void set_verbose(bool v) { verbose_flag() = v; }

template <typename... Args>
void info(const Args&... args) {
    std::ostringstream os;
    (os << ... << args);
    std::cerr << os.str() << '\n';
}

// Per-packet tracing, only printed with -v.
template <typename... Args>
void trace(const Args&... args) {
    if (verbose_flag()) info(args...);
}

}  // namespace rudp::log
