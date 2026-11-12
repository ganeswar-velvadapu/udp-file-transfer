#pragma once

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

#include "rudp/protocol.h"
#include "rudp/receive_window.h"
#include "rudp/udp_socket.h"

namespace rudp {

struct ReceiverConfig {
    std::filesystem::path out_dir = "received";
    uint16_t max_window = 512;  // largest window granted to a sender
    std::chrono::seconds idle_timeout{30};  // abort transfers that go silent
    std::chrono::seconds linger{10};        // keep finished transfers to re-ACK late packets
};

struct ReceiverTransferStats {
    uint64_t data_packets = 0;
    uint64_t duplicates = 0;
    uint64_t out_of_order = 0;  // buffered ahead of a gap
    uint64_t out_of_window = 0;
    uint64_t acks_sent = 0;
    size_t max_buffered = 0;
};

// Single-threaded event loop that serves any number of concurrent transfers,
// demultiplexed by transaction ID.
class ReceiverServer {
public:
    ReceiverServer(UdpSocket& socket, ReceiverConfig config);
    ~ReceiverServer();

    void run(const std::atomic<bool>& stop);

    // Exposed for tests: handle a single datagram.
    void handle_datagram(const uint8_t* data, size_t len, const sockaddr_in& from);
    void collect_garbage(std::chrono::steady_clock::time_point now);
    size_t active_sessions() const { return sessions_.size(); }

private:
    using Clock = std::chrono::steady_clock;
    enum class State { Receiving, Done };

    struct Session {
        uint32_t transaction_id = 0;
        sockaddr_in peer{};
        StartPayload info;
        uint16_t window = 0;
        std::filesystem::path part_path;
        std::filesystem::path final_path;
        std::ofstream out;
        std::unique_ptr<ReceiveWindow> rx;
        uint32_t crc = 0;
        State state = State::Receiving;
        Status final_status = Status::Ok;
        Clock::time_point started;
        Clock::time_point last_activity;
        ReceiverTransferStats stats;
    };

    void on_start(const PacketView& pkt, const sockaddr_in& from);
    void on_data(Session& s, const PacketView& pkt);
    void on_fin(Session& s, const PacketView& pkt);
    void reply(const sockaddr_in& to, PacketType type, uint32_t tid, uint32_t seq,
               const std::vector<uint8_t>& payload = {});
    void abort_session(Session& s, const char* reason);

    UdpSocket& socket_;
    ReceiverConfig config_;
    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions_;
    std::vector<uint8_t> rx_buf_;
    std::vector<uint8_t> tx_buf_;
};

}  // namespace rudp
