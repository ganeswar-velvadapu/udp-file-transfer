#include "rudp/sr_sender.h"

#include <algorithm>
#include <filesystem>
#include <random>
#include <stdexcept>

#include "rudp/crc32.h"
#include "rudp/log.h"

namespace rudp {

namespace fs = std::filesystem;
using std::chrono::duration_cast;
using std::chrono::microseconds;

SelectiveRepeatSender::SelectiveRepeatSender(UdpSocket& socket, const sockaddr_in& peer, SenderConfig config)
    : socket_(socket),
      peer_(peer),
      config_(config),
      rtt_(config.initial_rto, config.min_rto, config.max_rto),
      rx_buf_(kMaxDatagram) {
    if (config_.chunk_size == 0 || config_.chunk_size > kMaxPayload) {
        throw std::invalid_argument("chunk size must be in [1, " + std::to_string(kMaxPayload) + "]");
    }
    if (config_.window == 0) throw std::invalid_argument("window must be >= 1");
}

SenderStats SelectiveRepeatSender::send_file(const std::string& path, const std::string& remote_name) {
    file_.open(path, std::ios::binary);
    if (!file_) throw std::runtime_error("cannot open " + path);

    std::random_device rd;
    do {
        transaction_id_ = rd();
    } while (transaction_id_ == 0);

    StartPayload start;
    start.file_size = fs::file_size(path);
    start.chunk_size = config_.chunk_size;
    start.total_chunks = chunk_count(start.file_size, start.chunk_size);
    start.window = config_.window;
    start.filename = remote_name;

    stats_ = SenderStats{};
    stats_.transaction_id = transaction_id_;
    stats_.file_size = start.file_size;
    stats_.total_chunks = start.total_chunks;
    stats_.chunk_size = start.chunk_size;
    total_chunks_ = start.total_chunks;

    auto t0 = Clock::now();
    handshake(start);
    transfer_data();
    finish();
    stats_.elapsed_s = std::chrono::duration<double>(Clock::now() - t0).count();

    stats_.srtt_ms = rtt_.srtt().count() / 1000.0;
    stats_.rto_ms = rtt_.rto().count() / 1000.0;
    stats_.file_crc = file_crc_;
    stats_.bytes_sent = socket_.stats().bytes_sent;
    stats_.bytes_received = socket_.stats().bytes_received;
    return stats_;
}

void SelectiveRepeatSender::send_datagram(const uint8_t* data, size_t len) {
    socket_.send_to(data, len, peer_);
}

std::vector<uint8_t> SelectiveRepeatSender::exchange(PacketType type, const std::vector<uint8_t>& payload,
                                                     PacketType expect) {
    auto packet = encode_packet({type, transaction_id_, 0}, payload);

    for (unsigned attempt = 0; attempt <= config_.max_retries; ++attempt) {
        if (attempt > 0) {
            stats_.control_retransmissions++;
            log::trace("TIMEOUT ", to_string(type), " -> retransmitting (attempt ", attempt + 1, ")");
        }
        auto sent_at = Clock::now();
        send_datagram(packet.data(), packet.size());
        log::trace("Sent ", to_string(type));

        auto deadline = sent_at + rtt_.backoff(attempt);
        while (true) {
            auto now = Clock::now();
            if (now >= deadline) break;
            socket_.wait_readable(duration_cast<microseconds>(deadline - now));

            sockaddr_in from{};
            while (auto n = socket_.try_recv(rx_buf_.data(), rx_buf_.size(), from)) {
                auto pkt = decode_packet(rx_buf_.data(), *n);
                if (!pkt || pkt->header.transaction_id != transaction_id_) {
                    stats_.invalid_packets++;
                    continue;
                }
                if (pkt->header.type == PacketType::Error) {
                    auto st = StatusPayload::decode(pkt->payload, pkt->payload_len);
                    throw std::runtime_error(std::string("server error: ") +
                                             (st ? to_string(st->status) : "unknown"));
                }
                if (pkt->header.type != expect) continue;  // e.g. stray late ACKs
                if (attempt == 0) {
                    rtt_.add_sample(duration_cast<microseconds>(Clock::now() - sent_at));
                }
                log::trace("Received ", to_string(expect));
                return {pkt->payload, pkt->payload + pkt->payload_len};
            }
        }
    }
    throw std::runtime_error(std::string("no ") + to_string(expect) + " from " + to_string(peer_) + " after " +
                             std::to_string(config_.max_retries + 1) + " attempts");
}

void SelectiveRepeatSender::handshake(const StartPayload& start) {
    auto reply = exchange(PacketType::Start, start.encode(), PacketType::StartAck);
    auto ack = StartAckPayload::decode(reply.data(), reply.size());
    if (!ack) throw std::runtime_error("malformed START_ACK");
    if (ack->status != Status::Ok) {
        throw std::runtime_error(std::string("server rejected transfer: ") + to_string(ack->status));
    }
    // Flow control: never have more in flight than the receiver can buffer.
    window_ = std::max<uint16_t>(1, std::min(config_.window, ack->window));
    stats_.window = window_;
    slots_.assign(window_, Slot{});
    log::trace("Handshake done: tid=", transaction_id_, " window=", window_, " chunks=", total_chunks_);
}

void SelectiveRepeatSender::read_chunk(uint32_t seq, std::vector<uint8_t>& out) {
    size_t len = chunk_length(seq, stats_.file_size, config_.chunk_size);
    out.resize(len);
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
    if (static_cast<size_t>(file_.gcount()) != len) {
        throw std::runtime_error("short read on chunk " + std::to_string(seq) + " (file changed?)");
    }
    // Chunks are read strictly in order, so the whole-file CRC is built incrementally.
    file_crc_ = crc32_update(file_crc_, out.data(), len);
}

void SelectiveRepeatSender::handle_ack(uint32_t seq) {
    if (seq < base_ || seq >= next_seq_) {
        stats_.duplicate_acks++;  // ACK for something already slid past (its first ACK was late)
        return;
    }
    Slot& slot = slots_[seq % window_];
    if (slot.acked) {
        stats_.duplicate_acks++;
        return;
    }
    slot.acked = true;
    stats_.acks_received++;
    if (slot.retries == 0) {  // Karn's algorithm
        rtt_.add_sample(duration_cast<microseconds>(Clock::now() - slot.first_sent));
    }
    log::trace("Received ACK #", seq);
}

void SelectiveRepeatSender::slide_window() {
    while (base_ < next_seq_ && slots_[base_ % window_].acked) {
        slots_[base_ % window_].acked = false;
        ++base_;
    }
}

SelectiveRepeatSender::Clock::time_point SelectiveRepeatSender::earliest_deadline() const {
    auto earliest = Clock::time_point::max();
    for (uint32_t seq = base_; seq < next_seq_; ++seq) {
        const Slot& slot = slots_[seq % window_];
        if (!slot.acked) earliest = std::min(earliest, slot.deadline);
    }
    return earliest;
}

void SelectiveRepeatSender::retransmit_expired(Clock::time_point now) {
    for (uint32_t seq = base_; seq < next_seq_; ++seq) {
        Slot& slot = slots_[seq % window_];
        if (slot.acked || now < slot.deadline) continue;
        if (slot.retries >= config_.max_retries) {
            throw std::runtime_error("packet " + std::to_string(seq) + " unacknowledged after " +
                                     std::to_string(slot.retries) + " retransmissions; giving up");
        }
        ++slot.retries;
        send_datagram(slot.datagram.data(), slot.datagram.size());
        slot.deadline = now + rtt_.backoff(slot.retries);
        stats_.data_packets_sent++;
        stats_.retransmissions++;
        log::trace("TIMEOUT DATA #", seq, " -> retransmitting (retry ", slot.retries, ")");
    }
}

void SelectiveRepeatSender::transfer_data() {
    base_ = next_seq_ = 0;

    while (base_ < total_chunks_) {
        // 1. Fill the window with new packets.
        while (next_seq_ < total_chunks_ && next_seq_ - base_ < window_) {
            read_chunk(next_seq_, chunk_buf_);
            Slot& slot = slots_[next_seq_ % window_];
            slot.datagram.resize(kHeaderSize + chunk_buf_.size());
            encode_packet({PacketType::Data, transaction_id_, next_seq_}, chunk_buf_.data(), chunk_buf_.size(),
                          slot.datagram.data());
            slot.acked = false;
            slot.retries = 0;
            slot.first_sent = Clock::now();
            slot.deadline = slot.first_sent + rtt_.rto();
            send_datagram(slot.datagram.data(), slot.datagram.size());
            stats_.data_packets_sent++;
            log::trace("Sent DATA #", next_seq_);
            ++next_seq_;
        }

        // 2. Sleep until an ACK arrives or the earliest per-packet timer fires.
        auto wait = earliest_deadline() - Clock::now();
        socket_.wait_readable(duration_cast<microseconds>(std::max(wait, Clock::duration::zero())));

        // 3. Drain every queued ACK.
        sockaddr_in from{};
        while (auto n = socket_.try_recv(rx_buf_.data(), rx_buf_.size(), from)) {
            auto pkt = decode_packet(rx_buf_.data(), *n);
            if (!pkt || pkt->header.transaction_id != transaction_id_) {
                stats_.invalid_packets++;
                continue;
            }
            if (pkt->header.type == PacketType::Ack) {
                handle_ack(pkt->header.seq);
            } else if (pkt->header.type == PacketType::Error) {
                auto st = StatusPayload::decode(pkt->payload, pkt->payload_len);
                throw std::runtime_error(std::string("server error: ") + (st ? to_string(st->status) : "unknown"));
            }
            // Duplicate START_ACKs are harmless and ignored.
        }

        // 4. Slide the window over the acknowledged prefix.
        slide_window();

        // 5. Selectively retransmit only the packets whose timer expired.
        retransmit_expired(Clock::now());
    }
}

void SelectiveRepeatSender::finish() {
    FinPayload fin{file_crc_, stats_.file_size};
    auto reply = exchange(PacketType::Fin, fin.encode(), PacketType::FinAck);
    auto status = StatusPayload::decode(reply.data(), reply.size());
    if (!status) throw std::runtime_error("malformed FIN_ACK");
    if (status->status != Status::Ok) {
        throw std::runtime_error(std::string("transfer failed: ") + to_string(status->status));
    }
}

}  // namespace rudp
