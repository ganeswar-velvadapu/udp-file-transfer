// TCP baseline server for benchmarking.

#include <iostream>

#include "common.h"
#include "rudp/tcp_transfer.h"

int main(int argc, char** argv) {
    try {
        rudp::Args args(argc, argv, {"h", "help"});
        if (args.has("h") || args.has("help")) {
            std::cerr << "usage: tcp_server [--host 0.0.0.0] [--port 9001] [--out-dir received]\n";
            return 0;
        }
        app::install_signal_handlers();
        rudp::tcp_serve(args.get("host", "0.0.0.0"), static_cast<uint16_t>(args.get_int("port", 9001)),
                        args.get("out-dir", "received"), app::stop_flag());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "tcp_server: " << e.what() << "\n";
        return 1;
    }
}
