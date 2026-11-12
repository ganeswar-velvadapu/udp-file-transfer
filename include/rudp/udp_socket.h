#pragma once

#include <netinet/in.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace rudp {

// Optional in-process network impairment applied to outgoing datagrams.
// Lets you exercise loss / duplication / reordering handling without root
// (the benchmark uses Linux netem instead, which affects TCP equally).
struct Impairment {
    double loss = 0.0;     // probability a datagram is silently dropped
    double dup = 0.0;      // probability a datagram is sent twice
    double reorder = 0.0;  // probability a datagram is held back behind the next one
    uint64_t seed = 0;     // 0 = random seed

    bool active() const { return loss > 0 || dup > 0 || reorder > 0; }
};

struct SocketStats {
    uint64_t datagrams_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t datagrams_received = 0;
    uint64_t bytes_received = 0;
    uint64_t impair_dropped = 0;
    uint64_t impair_duplicated = 0;
    uint64_t impair_reordered = 0;
};

sockaddr_in make_address(const std::string& host, uint16_t port);
std::string to_string(const sockaddr_in& addr);

// RAII wrapper around a UDP socket. Sends block; receives never do.
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void bind(const sockaddr_in& addr);
    void set_buffer_sizes(int bytes);
    void set_impairment(const Impairment& impairment);

    void send_to(const uint8_t* data, size_t len, const sockaddr_in& to);

    // Returns the datagram length, or nullopt if nothing is queued.
    std::optional<size_t> try_recv(uint8_t* buf, size_t cap, sockaddr_in& from);

    // Waits until a datagram is readable or the timeout expires.
    bool wait_readable(std::chrono::microseconds timeout);

    const SocketStats& stats() const { return stats_; }
    int fd() const { return fd_; }

private:
    void raw_send(const uint8_t* data, size_t len, const sockaddr_in& to);
    void flush_held();

    int fd_ = -1;
    Impairment impairment_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> coin_{0.0, 1.0};

    struct Held {
        std::vector<uint8_t> data;
        sockaddr_in to;
    };
    std::optional<Held> held_;
    SocketStats stats_;
};

}  // namespace rudp
