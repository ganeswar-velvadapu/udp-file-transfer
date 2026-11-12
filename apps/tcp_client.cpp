// TCP baseline client for benchmarking.

#include <filesystem>
#include <iostream>

#include "rudp/args.h"
#include "rudp/tcp_transfer.h"

int main(int argc, char** argv) {
    try {
        rudp::Args args(argc, argv, {"json", "h", "help"});
        if (args.has("h") || args.has("help") || args.positional().size() != 1) {
            std::cerr << "usage: tcp_client <file> [--host H] [--port 9001] [--name NAME] [--json]\n";
            return 2;
        }
        const std::string path = args.positional()[0];
        auto st = rudp::tcp_send_file(args.get("host", "127.0.0.1"), static_cast<uint16_t>(args.get_int("port", 9001)),
                                      path, args.get("name", std::filesystem::path(path).filename().string()),
                                      1 << 16);
        if (args.has("json")) {
            std::cout << "{\"protocol\":\"tcp\",\"ok\":true,\"file_size\":" << st.file_size
                      << ",\"elapsed_s\":" << st.elapsed_s << ",\"throughput_mbps\":" << st.throughput_mbps()
                      << ",\"retransmissions\":" << st.retransmissions << ",\"srtt_ms\":" << st.srtt_ms << "}\n";
        } else {
            std::cout << "TCP transfer complete: " << st.file_size << " bytes in " << st.elapsed_s << " s ("
                      << st.throughput_mbps() << " Mbit/s), " << st.retransmissions << " retransmitted segments\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "tcp_client: " << e.what() << "\n";
        return 1;
    }
}
