// Reliable file transfer client over UDP (Selective Repeat ARQ).

#include <filesystem>
#include <iostream>

#include "common.h"
#include "rudp/log.h"
#include "rudp/sr_sender.h"

namespace {

void usage() {
    std::cerr << "usage: udp_client <file> [options]\n"
                 "  --host H          server host (default 127.0.0.1)\n"
                 "  --port N          server port (default 9000)\n"
                 "  --name NAME       name to store the file under (default: basename)\n"
                 "  --chunk N         payload bytes per DATA packet (default 1400)\n"
                 "  --window N        Selective Repeat window in packets (default 64)\n"
                 "  --min-rto-ms N    lower bound for the retransmission timeout (default 10)\n"
                 "  --max-retries N   retransmissions per packet before giving up (default 30)\n"
              << app::kImpairmentHelp
              << "  --json            print stats as one JSON object on stdout\n"
                 "  -v                trace every packet\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        rudp::Args args(argc, argv, {"json", "v", "verbose", "h", "help"});
        if (args.has("h") || args.has("help") || args.positional().size() != 1) {
            usage();
            return args.positional().size() == 1 ? 0 : 2;
        }
        rudp::log::set_verbose(args.has("v") || args.has("verbose"));

        const std::string path = args.positional()[0];
        const std::string name = args.get("name", std::filesystem::path(path).filename().string());

        rudp::SenderConfig cfg;
        cfg.chunk_size = static_cast<uint16_t>(args.get_int("chunk", rudp::kDefaultChunkSize));
        cfg.window = static_cast<uint16_t>(args.get_int("window", 64));
        cfg.min_rto = std::chrono::milliseconds(args.get_int("min-rto-ms", 10));
        cfg.max_retries = static_cast<unsigned>(args.get_int("max-retries", 30));

        rudp::UdpSocket socket;
        socket.set_buffer_sizes(4 << 20);
        socket.set_impairment(app::impairment_from(args));
        auto server = rudp::make_address(args.get("host", "127.0.0.1"), static_cast<uint16_t>(args.get_int("port", 9000)));

        rudp::SelectiveRepeatSender sender(socket, server, cfg);
        rudp::SenderStats st = sender.send_file(path, name);
        const auto& sock = socket.stats();

        if (args.has("json")) {
            std::cout << "{\"protocol\":\"udp\",\"ok\":true"
                      << ",\"file_size\":" << st.file_size << ",\"elapsed_s\":" << st.elapsed_s
                      << ",\"throughput_mbps\":" << st.throughput_mbps() << ",\"chunks\":" << st.total_chunks
                      << ",\"window\":" << st.window << ",\"data_packets_sent\":" << st.data_packets_sent
                      << ",\"retransmissions\":" << st.retransmissions
                      << ",\"control_retransmissions\":" << st.control_retransmissions
                      << ",\"acks_received\":" << st.acks_received << ",\"duplicate_acks\":" << st.duplicate_acks
                      << ",\"bytes_sent\":" << st.bytes_sent << ",\"bytes_received\":" << st.bytes_received
                      << ",\"overhead_ratio\":" << st.overhead_ratio() << ",\"srtt_ms\":" << st.srtt_ms
                      << ",\"rto_ms\":" << st.rto_ms << "}\n";
        } else {
            std::cout << "Transfer complete: " << path << " -> " << rudp::to_string(server) << " as '" << name
                      << "'\n"
                      << "  transaction id     " << st.transaction_id << "\n"
                      << "  size               " << st.file_size << " bytes in " << st.total_chunks
                      << " chunks of " << st.chunk_size << "\n"
                      << "  window (granted)   " << st.window << " packets\n"
                      << "  time               " << st.elapsed_s << " s\n"
                      << "  throughput         " << st.throughput_mbps() << " Mbit/s\n"
                      << "  DATA packets sent  " << st.data_packets_sent << " (" << st.retransmissions
                      << " retransmissions)\n"
                      << "  ACKs               " << st.acks_received << " new, " << st.duplicate_acks
                      << " duplicate\n"
                      << "  protocol overhead  " << st.overhead_ratio() * 100 << " % (headers + ACKs + retx)\n"
                      << "  SRTT / RTO         " << st.srtt_ms << " ms / " << st.rto_ms << " ms\n"
                      << "  file CRC32         " << std::hex << st.file_crc << std::dec << "\n";
            if (sock.impair_dropped || sock.impair_duplicated || sock.impair_reordered) {
                std::cout << "  impairment         dropped " << sock.impair_dropped << ", duplicated "
                          << sock.impair_duplicated << ", reordered " << sock.impair_reordered << "\n";
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "udp_client: " << e.what() << "\n";
        return 1;
    }
}
