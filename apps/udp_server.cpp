// Reliable file transfer server over UDP (Selective Repeat ARQ).

#include <iostream>

#include "common.h"
#include "rudp/log.h"
#include "rudp/sr_receiver.h"

namespace {

void usage() {
    std::cerr << "usage: udp_server [options]\n"
                 "  --host H          address to bind (default 0.0.0.0)\n"
                 "  --port N          port to bind (default 9000)\n"
                 "  --out-dir DIR     where received files are written (default ./received)\n"
                 "  --max-window N    largest window granted to a client (default 512)\n"
              << app::kImpairmentHelp << "  -v                trace every packet\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        rudp::Args args(argc, argv, {"v", "verbose", "h", "help"});
        if (args.has("h") || args.has("help")) {
            usage();
            return 0;
        }
        rudp::log::set_verbose(args.has("v") || args.has("verbose"));
        app::install_signal_handlers();

        rudp::ReceiverConfig cfg;
        cfg.out_dir = args.get("out-dir", "received");
        cfg.max_window = static_cast<uint16_t>(args.get_int("max-window", 512));

        rudp::UdpSocket socket;
        socket.set_buffer_sizes(4 << 20);
        socket.set_impairment(app::impairment_from(args));
        auto addr = rudp::make_address(args.get("host", "0.0.0.0"), static_cast<uint16_t>(args.get_int("port", 9000)));
        socket.bind(addr);

        rudp::log::info("UDP server listening on ", rudp::to_string(addr), ", writing to ", cfg.out_dir.string());
        rudp::ReceiverServer server(socket, cfg);
        server.run(app::stop_flag());
        rudp::log::info("Shutting down");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "udp_server: " << e.what() << "\n";
        return 1;
    }
}
