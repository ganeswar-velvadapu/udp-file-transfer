// Dependency-free unit tests for the protocol codec and the Selective Repeat
// receive window.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "rudp/crc32.h"
#include "rudp/protocol.h"
#include "rudp/receive_window.h"
#include "rudp/rtt_estimator.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

using namespace rudp;

void test_crc32() {
    const char* s = "123456789";
    CHECK(crc32(s, 9) == 0xCBF43926u);  // standard check value
    CHECK(crc32_update(crc32(s, 4), s + 4, 5) == 0xCBF43926u);
    CHECK(crc32(s, 0) == 0);
}

void test_packet_roundtrip() {
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    auto bytes = encode_packet({PacketType::Data, 0xDEADBEEF, 42}, payload);
    CHECK(bytes.size() == kHeaderSize + payload.size());

    auto pkt = decode_packet(bytes.data(), bytes.size());
    CHECK(pkt.has_value());
    CHECK(pkt->header.type == PacketType::Data);
    CHECK(pkt->header.transaction_id == 0xDEADBEEF);
    CHECK(pkt->header.seq == 42);
    CHECK(pkt->payload_len == 5);
    CHECK(std::memcmp(pkt->payload, payload.data(), 5) == 0);
}

void test_packet_rejects_corruption() {
    auto bytes = encode_packet({PacketType::Ack, 7, 9}, {0xAA, 0xBB});
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto copy = bytes;
        copy[i] ^= 0x01;  // flip one bit anywhere
        CHECK(!decode_packet(copy.data(), copy.size()).has_value());
    }
    CHECK(!decode_packet(bytes.data(), bytes.size() - 1).has_value());  // truncated
    CHECK(!decode_packet(bytes.data(), 3).has_value());
}

void test_control_payloads() {
    StartPayload s{123456789012ull, 88183, 1400, 64, "report.pdf"};
    auto enc = s.encode();
    auto dec = StartPayload::decode(enc.data(), enc.size());
    CHECK(dec && dec->file_size == s.file_size && dec->total_chunks == s.total_chunks &&
          dec->chunk_size == 1400 && dec->window == 64 && dec->filename == "report.pdf");
    CHECK(!StartPayload::decode(enc.data(), enc.size() - 1));

    auto sa = StartAckPayload{Status::Ok, 32}.encode();
    auto sad = StartAckPayload::decode(sa.data(), sa.size());
    CHECK(sad && sad->status == Status::Ok && sad->window == 32);

    auto fin = FinPayload{0xCAFEBABE, 99}.encode();
    auto find = FinPayload::decode(fin.data(), fin.size());
    CHECK(find && find->file_crc == 0xCAFEBABE && find->file_size == 99);

    CHECK(chunk_count(0, 1400) == 0);
    CHECK(chunk_count(1400, 1400) == 1);
    CHECK(chunk_count(1401, 1400) == 2);
    CHECK(chunk_length(1, 1401, 1400) == 1);
    CHECK(chunk_length(0, 1401, 1400) == 1400);
}

struct Collector {
    std::string out;
    ReceiveWindow::Sink sink() {
        return [this](const uint8_t* d, size_t n) { out.append(reinterpret_cast<const char*>(d), n); };
    }
};

ReceiveWindow::Result feed(ReceiveWindow& w, uint32_t seq) {
    uint8_t c = static_cast<uint8_t>('a' + seq);
    return w.on_data(seq, &c, 1);
}

void test_window_in_order() {
    Collector c;
    ReceiveWindow w(5, 4, c.sink());
    for (uint32_t i = 0; i < 5; ++i) {
        auto r = feed(w, i);
        CHECK(r.outcome == ReceiveWindow::Outcome::Delivered && r.send_ack);
    }
    CHECK(w.complete());
    CHECK(c.out == "abcde");
}

void test_window_reordering_and_buffering() {
    Collector c;
    ReceiveWindow w(6, 4, c.sink());
    CHECK(feed(w, 2).outcome == ReceiveWindow::Outcome::Buffered);
    CHECK(feed(w, 1).outcome == ReceiveWindow::Outcome::Buffered);
    CHECK(feed(w, 3).outcome == ReceiveWindow::Outcome::Buffered);
    CHECK(c.out.empty());
    CHECK(w.buffered() == 3);
    CHECK(feed(w, 0).outcome == ReceiveWindow::Outcome::Delivered);  // fills the gap
    CHECK(c.out == "abcd");
    CHECK(w.base() == 4 && w.buffered() == 0);
    CHECK(feed(w, 5).outcome == ReceiveWindow::Outcome::Buffered);
    CHECK(feed(w, 4).outcome == ReceiveWindow::Outcome::Delivered);
    CHECK(c.out == "abcdef" && w.complete());
    CHECK(w.max_buffered() == 3);
}

void test_window_duplicates_and_bounds() {
    Collector c;
    ReceiveWindow w(20, 4, c.sink());
    feed(w, 0);
    feed(w, 1);
    // Already delivered, within one window behind: re-ACK.
    auto r = feed(w, 0);
    CHECK(r.outcome == ReceiveWindow::Outcome::Duplicate && r.send_ack);
    // Buffered duplicate.
    CHECK(feed(w, 4).outcome == ReceiveWindow::Outcome::Buffered);
    r = feed(w, 4);
    CHECK(r.outcome == ReceiveWindow::Outcome::Duplicate && r.send_ack);
    // base = 2, window = 4 -> acceptable [2, 6); 6 is outside.
    r = feed(w, 6);
    CHECK(r.outcome == ReceiveWindow::Outcome::OutOfWindow && !r.send_ack);
    // Beyond the file.
    r = feed(w, 25);
    CHECK(r.outcome == ReceiveWindow::Outcome::OutOfWindow && !r.send_ack);
    CHECK(c.out == "ab");
}

void test_rtt_estimator() {
    using us = std::chrono::microseconds;
    RttEstimator e(us(200000), us(10000), us(2000000));
    CHECK(e.rto() == us(200000));
    e.add_sample(us(50000));
    CHECK(e.srtt() == us(50000));
    CHECK(e.rto() == us(150000));  // 50ms + 4 * 25ms
    for (int i = 0; i < 100; ++i) e.add_sample(us(100));
    CHECK(e.rto() == us(10000));  // clamped to min
    CHECK(e.backoff(1) == us(20000));
    CHECK(e.backoff(20) == us(640000));  // 2^6 cap
}

}  // namespace

int main() {
    test_crc32();
    test_packet_roundtrip();
    test_packet_rejects_corruption();
    test_control_payloads();
    test_window_in_order();
    test_window_reordering_and_buffering();
    test_window_duplicates_and_bounds();
    test_rtt_estimator();
    if (g_failures == 0) {
        std::cout << "All unit tests passed\n";
        return 0;
    }
    std::cout << g_failures << " check(s) failed\n";
    return 1;
}
