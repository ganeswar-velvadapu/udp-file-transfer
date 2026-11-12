#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rudp {

// Receiver half of Selective Repeat, independent of sockets and files so it
// can be unit tested.
//
//   [ delivered ... | base ......... base+window ) | not yet acceptable ...
//                     ^ accept, ACK, buffer if out of order
//   [base-window, base): already delivered -> re-ACK (the old ACK was lost)
//
// In-order chunks go straight to the sink; out-of-order chunks are buffered
// in a ring of `window` slots and flushed as soon as the gap before them fills.
class ReceiveWindow {
public:
    using Sink = std::function<void(const uint8_t* data, size_t len)>;

    enum class Outcome {
        Delivered,    // seq == base: written out (plus any buffered successors)
        Buffered,     // inside window but ahead of a gap: held for reassembly
        Duplicate,    // seen before (buffered or already delivered)
        OutOfWindow,  // beyond the window or beyond the file: dropped, no ACK
    };

    struct Result {
        Outcome outcome;
        bool send_ack;
    };

    ReceiveWindow(uint32_t total_chunks, uint16_t window, Sink sink);

    Result on_data(uint32_t seq, const uint8_t* data, size_t len);

    uint32_t base() const { return base_; }
    uint32_t total() const { return total_; }
    bool complete() const { return base_ == total_; }
    size_t buffered() const { return buffered_; }
    size_t max_buffered() const { return max_buffered_; }

private:
    struct Slot {
        bool filled = false;
        std::vector<uint8_t> data;
    };

    void deliver_in_order();

    uint32_t total_;
    uint16_t window_;
    uint32_t base_ = 0;
    std::vector<Slot> slots_;
    Sink sink_;
    size_t buffered_ = 0;
    size_t max_buffered_ = 0;
};

}  // namespace rudp
