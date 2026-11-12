#pragma once

#include <csignal>
#include <atomic>

#include "rudp/args.h"
#include "rudp/udp_socket.h"

namespace app {

inline std::atomic<bool>& stop_flag() {
    static std::atomic<bool> stop{false};
    return stop;
}

inline void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = [](int) { stop_flag().store(true); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: let blocking calls return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

inline rudp::Impairment impairment_from(const rudp::Args& args) {
    rudp::Impairment imp;
    imp.loss = args.get_double("loss", 0);
    imp.dup = args.get_double("dup", 0);
    imp.reorder = args.get_double("reorder", 0);
    imp.seed = static_cast<uint64_t>(args.get_int("seed", 0));
    return imp;
}

constexpr const char* kImpairmentHelp =
    "  --loss P          drop outgoing datagrams with probability P (testing)\n"
    "  --dup P           duplicate outgoing datagrams with probability P\n"
    "  --reorder P       delay a datagram behind the next one with probability P\n"
    "  --seed N          RNG seed for the impairments\n";

}  // namespace app
