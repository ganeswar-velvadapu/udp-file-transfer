#pragma once

// Plain-TCP file transfer used as the benchmark baseline.
//
//   client -> server : u16 name_len | name | u64 file_size | file bytes...
//   server -> client : 1 byte status ('K' = file fully written to disk)
//
// The client stops its clock only after the server's status byte, so the
// measured time is end-to-end, exactly like the UDP client waiting for FIN_ACK.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rudp {

struct TcpSendStats {
    uint64_t file_size = 0;
    double elapsed_s = 0;
    uint32_t retransmissions = 0;  // tcpi_total_retrans from TCP_INFO
    double srtt_ms = 0;

    double throughput_mbps() const { return elapsed_s > 0 ? file_size * 8.0 / 1e6 / elapsed_s : 0; }
};

TcpSendStats tcp_send_file(const std::string& host, uint16_t port, const std::string& path,
                           const std::string& remote_name, size_t io_chunk);

// Accepts connections one at a time until `stop` is set.
void tcp_serve(const std::string& host, uint16_t port, const std::filesystem::path& out_dir,
               const std::atomic<bool>& stop);

}  // namespace rudp
