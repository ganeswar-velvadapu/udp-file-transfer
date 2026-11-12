#include "rudp/sr_receiver.h"

#include <algorithm>
#include <cstring>
#include <system_error>

#include "rudp/crc32.h"
#include "rudp/log.h"

namespace rudp {

namespace fs = std::filesystem;

namespace {

// Only keep the final path component so a sender can't write outside out_dir.
std::optional<std::string> sanitize_filename(const std::string& name) {
    if (name.empty() || name.find('\0') != std::string::npos) return std::nullopt;
    std::string base = fs::path(name).filename().string();
    if (base.empty() || base == "." || base == "..") return std::nullopt;
    return base;
}

}  // namespace

ReceiverServer::ReceiverServer(UdpSocket& socket, ReceiverConfig config)
    : socket_(socket), config_(std::move(config)), rx_buf_(kMaxDatagram), tx_buf_(kMaxDatagram) {
    fs::create_directories(config_.out_dir);
}

ReceiverServer::~ReceiverServer() {
    for (auto& [tid, s] : sessions_) {
        if (s->state == State::Receiving) abort_session(*s, "server shutting down");
    }
}

void ReceiverServer::run(const std::atomic<bool>& stop) {
    while (!stop.load()) {
        socket_.wait_readable(std::chrono::milliseconds(200));
        sockaddr_in from{};
        while (auto n = socket_.try_recv(rx_buf_.data(), rx_buf_.size(), from)) {
            handle_datagram(rx_buf_.data(), *n, from);
        }
        collect_garbage(Clock::now());
    }
}

void ReceiverServer::reply(const sockaddr_in& to, PacketType type, uint32_t tid, uint32_t seq,
                           const std::vector<uint8_t>& payload) {
    size_t n = encode_packet({type, tid, seq}, payload.data(), payload.size(), tx_buf_.data());
    socket_.send_to(tx_buf_.data(), n, to);
}

void ReceiverServer::handle_datagram(const uint8_t* data, size_t len, const sockaddr_in& from) {
    auto pkt = decode_packet(data, len);
    if (!pkt) {
        log::trace("Dropped corrupt/malformed datagram from ", to_string(from));
        return;  // indistinguishable from loss; sender will retransmit
    }
    const uint32_t tid = pkt->header.transaction_id;

    if (pkt->header.type == PacketType::Start) {
        on_start(*pkt, from);
        return;
    }

    auto it = sessions_.find(tid);
    if (it == sessions_.end()) {
        // e.g. server restarted mid-transfer: tell the sender to stop.
        if (pkt->header.type == PacketType::Data || pkt->header.type == PacketType::Fin) {
            reply(from, PacketType::Error, tid, 0, StatusPayload{Status::UnknownTransaction}.encode());
        }
        return;
    }
    Session& s = *it->second;
    s.last_activity = Clock::now();
    s.peer = from;  // follow the client if its address changes (NAT rebinding)

    switch (pkt->header.type) {
        case PacketType::Data: on_data(s, *pkt); break;
        case PacketType::Fin: on_fin(s, *pkt); break;
        default: break;  // ACK-type packets are not expected at the receiver
    }
}

void ReceiverServer::on_start(const PacketView& pkt, const sockaddr_in& from) {
    const uint32_t tid = pkt.header.transaction_id;

    if (auto it = sessions_.find(tid); it != sessions_.end()) {
        // Duplicate START (our START_ACK was lost): answer identically.
        Session& s = *it->second;
        reply(from, PacketType::StartAck, tid, 0, StartAckPayload{Status::Ok, s.window}.encode());
        return;
    }

    auto start = StartPayload::decode(pkt.payload, pkt.payload_len);
    auto name = start ? sanitize_filename(start->filename) : std::nullopt;
    bool valid = start && name && start->chunk_size > 0 && start->chunk_size <= kMaxPayload &&
                 start->window > 0 && start->total_chunks == chunk_count(start->file_size, start->chunk_size);
    if (!valid) {
        log::info("Rejected malformed START from ", to_string(from));
        reply(from, PacketType::StartAck, tid, 0, StartAckPayload{Status::Rejected, 0}.encode());
        return;
    }

    auto s = std::make_unique<Session>();
    s->transaction_id = tid;
    s->peer = from;
    s->info = *start;
    s->window = std::min(start->window, config_.max_window);
    s->final_path = config_.out_dir / ("received_" + *name);
    s->part_path = config_.out_dir / ("received_" + *name + "." + std::to_string(tid) + ".part");
    s->out.open(s->part_path, std::ios::binary | std::ios::trunc);
    if (!s->out) {
        log::info("Cannot create ", s->part_path.string());
        reply(from, PacketType::StartAck, tid, 0, StartAckPayload{Status::Rejected, 0}.encode());
        return;
    }
    Session* raw = s.get();
    s->rx = std::make_unique<ReceiveWindow>(start->total_chunks, s->window, [raw](const uint8_t* d, size_t n) {
        // In-order delivery: stream straight to disk and extend the file CRC.
        raw->out.write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(n));
        raw->crc = crc32_update(raw->crc, d, n);
    });
    s->started = s->last_activity = Clock::now();

    log::info("[", tid, "] START from ", to_string(from), ": '", *name, "' ", start->file_size, " bytes, ",
              start->total_chunks, " chunks of ", start->chunk_size, ", window ", s->window);
    reply(from, PacketType::StartAck, tid, 0, StartAckPayload{Status::Ok, s->window}.encode());
    sessions_.emplace(tid, std::move(s));
}

void ReceiverServer::on_data(Session& s, const PacketView& pkt) {
    const uint32_t seq = pkt.header.seq;
    s.stats.data_packets++;

    if (s.state == State::Done) {
        // Late retransmission after completion: our ACK must have been lost.
        if (seq < s.info.total_chunks) {
            reply(s.peer, PacketType::Ack, s.transaction_id, seq);
            s.stats.acks_sent++;
            s.stats.duplicates++;
        }
        return;
    }

    if (seq < s.info.total_chunks && pkt.payload_len != chunk_length(seq, s.info.file_size, s.info.chunk_size)) {
        log::trace("[", s.transaction_id, "] DATA #", seq, " has wrong length ", pkt.payload_len, ", dropped");
        return;
    }

    auto result = s.rx->on_data(seq, pkt.payload, pkt.payload_len);
    switch (result.outcome) {
        case ReceiveWindow::Outcome::Delivered:
            log::trace("[", s.transaction_id, "] DATA #", seq, " in order (base now ", s.rx->base(), ")");
            break;
        case ReceiveWindow::Outcome::Buffered:
            s.stats.out_of_order++;
            log::trace("[", s.transaction_id, "] DATA #", seq, " out of order, buffered (base ", s.rx->base(), ")");
            break;
        case ReceiveWindow::Outcome::Duplicate:
            s.stats.duplicates++;
            log::trace("[", s.transaction_id, "] Duplicate DATA #", seq);
            break;
        case ReceiveWindow::Outcome::OutOfWindow:
            s.stats.out_of_window++;
            log::trace("[", s.transaction_id, "] DATA #", seq, " outside window, dropped");
            break;
    }
    s.stats.max_buffered = s.rx->max_buffered();

    if (result.send_ack) {
        reply(s.peer, PacketType::Ack, s.transaction_id, seq);
        s.stats.acks_sent++;
    }
}

void ReceiverServer::on_fin(Session& s, const PacketView& pkt) {
    if (s.state == State::Receiving) {
        auto fin = FinPayload::decode(pkt.payload, pkt.payload_len);
        if (!fin || !s.rx->complete()) {
            // The sender only sends FIN once every chunk is ACKed, so this is
            // a stray/corrupt packet. Ignore it; a valid FIN will follow.
            return;
        }
        s.out.close();
        bool ok = !s.out.fail() && fin->file_crc == s.crc && fin->file_size == s.info.file_size;
        std::error_code ec;
        if (ok) {
            fs::rename(s.part_path, s.final_path, ec);
            ok = !ec;
        }
        if (!ok) fs::remove(s.part_path, ec);
        s.final_status = ok ? Status::Ok : Status::ChecksumMismatch;
        s.state = State::Done;

        double secs = std::chrono::duration<double>(Clock::now() - s.started).count();
        log::info("[", s.transaction_id, "] ", ok ? "COMPLETE " : "FAILED (crc mismatch) ",
                  s.final_path.string(), " in ", secs, " s | data pkts ", s.stats.data_packets, ", duplicates ",
                  s.stats.duplicates, ", out-of-order ", s.stats.out_of_order, ", max buffered ",
                  s.stats.max_buffered, ", acks ", s.stats.acks_sent);
    }
    // (Re-)acknowledge FIN, including retransmitted FINs after completion.
    reply(s.peer, PacketType::FinAck, s.transaction_id, 0, StatusPayload{s.final_status}.encode());
}

void ReceiverServer::abort_session(Session& s, const char* reason) {
    log::info("[", s.transaction_id, "] aborted: ", reason);
    s.out.close();
    std::error_code ec;
    fs::remove(s.part_path, ec);
    s.state = State::Done;
    s.final_status = Status::Rejected;
}

void ReceiverServer::collect_garbage(Clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Session& s = *it->second;
        auto idle = now - s.last_activity;
        if (s.state == State::Receiving && idle > config_.idle_timeout) {
            abort_session(s, "sender went silent");
        }
        if (s.state == State::Done && idle > config_.linger) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace rudp
