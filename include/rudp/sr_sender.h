#pragma once

#include <netinet/in.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "rudp/protocol.h"
#include "rudp/rtt_estimator.h"
#include "rudp/udp_socket.h"

namespace rudp {

struct SenderConfig {
    uint16_t chunk_size = kDefaultChunkSize;
    uint16_t window = 64;  // requested; the receiver may grant less
    std::chrono::microseconds initial_rto{200'000};
    std::chrono::microseconds min_rto{10'000};
    std::chrono::microseconds max_rto{2'000'000};
    unsigned max_retries = 30;  // per packet, before giving up
};

struct SenderStats {
    uint32_t transaction_id = 0;
    uint64_t file_size = 0;
    uint32_t total_chunks = 0;
    uint16_t chunk_size = 0;
    uint16_t window = 0;  // granted window

    uint64_t data_packets_sent = 0;   // first transmissions + retransmissions
    uint64_t retransmissions = 0;     // DATA packets re-sent after a timeout
    uint64_t control_retransmissions = 0;  // START / FIN re-sent
    uint64_t acks_received = 0;       // ACKs that acknowledged something new
    uint64_t duplicate_acks = 0;      // ACKs for already-acknowledged packets
    uint64_t invalid_packets = 0;     // corrupt / foreign datagrams discarded

    uint64_t bytes_sent = 0;      // UDP payload bytes (our header + data)
    uint64_t bytes_received = 0;  // UDP payload bytes of ACKs etc.

    double elapsed_s = 0;
    double srtt_ms = 0;
    double rto_ms = 0;
    uint32_t file_crc = 0;

    double throughput_mbps() const {
        return elapsed_s > 0 ? (file_size * 8.0 / 1e6) / elapsed_s : 0;
    }
    // Extra bytes on the wire per byte of file, at the UDP payload level.
    double overhead_ratio() const {
        return file_size > 0 ? double(bytes_sent + bytes_received) / file_size - 1.0 : 0;
    }
};

// Sends one file using Selective Repeat ARQ:
//   * up to `window` unacknowledged DATA packets in flight,
//   * every packet has its own retransmission timer (adaptive RTO),
//   * only the packets that time out are retransmitted,
//   * the window base slides forward over the longest acknowledged prefix.
// Memory use is O(window * chunk_size) regardless of file size.
class SelectiveRepeatSender {
public:
    SelectiveRepeatSender(UdpSocket& socket, const sockaddr_in& peer, SenderConfig config);

    // Throws std::runtime_error if the peer stops responding or rejects the file.
    SenderStats send_file(const std::string& path, const std::string& remote_name);

private:
    using Clock = std::chrono::steady_clock;

    struct Slot {
        std::vector<uint8_t> datagram;  // encoded DATA packet, kept for retransmission
        Clock::time_point first_sent;
        Clock::time_point deadline;
        unsigned retries = 0;
        bool acked = false;
    };

    void handshake(const StartPayload& start);
    void transfer_data();
    void finish();

    // Sends a control packet until a reply of `expect` type arrives.
    std::vector<uint8_t> exchange(PacketType type, const std::vector<uint8_t>& payload, PacketType expect);

    void send_datagram(const uint8_t* data, size_t len);
    void read_chunk(uint32_t seq, std::vector<uint8_t>& out);
    void handle_ack(uint32_t seq);
    void slide_window();
    Clock::time_point earliest_deadline() const;
    void retransmit_expired(Clock::time_point now);

    UdpSocket& socket_;
    sockaddr_in peer_;
    SenderConfig config_;
    RttEstimator rtt_;

    std::ifstream file_;
    uint32_t transaction_id_ = 0;
    uint32_t total_chunks_ = 0;
    uint16_t window_ = 0;
    uint32_t base_ = 0;      // oldest unacknowledged sequence number
    uint32_t next_seq_ = 0;  // next sequence number to send for the first time
    std::vector<Slot> slots_;  // ring buffer indexed by seq % window
    uint32_t file_crc_ = 0;

    std::vector<uint8_t> chunk_buf_;
    std::vector<uint8_t> rx_buf_;
    SenderStats stats_;
};

}  // namespace rudp
