#include "rudp/receive_window.h"

#include <algorithm>
#include <stdexcept>

namespace rudp {

ReceiveWindow::ReceiveWindow(uint32_t total_chunks, uint16_t window, Sink sink)
    : total_(total_chunks), window_(window), slots_(window), sink_(std::move(sink)) {
    if (window == 0) throw std::invalid_argument("window must be >= 1");
}

ReceiveWindow::Result ReceiveWindow::on_data(uint32_t seq, const uint8_t* data, size_t len) {
    if (seq >= total_) return {Outcome::OutOfWindow, false};

    if (seq < base_) {
        // Already delivered. If it is within one window behind, the sender
        // may still be waiting for its ACK (ours was lost), so ACK again.
        bool recent = base_ - seq <= window_;
        return {recent ? Outcome::Duplicate : Outcome::OutOfWindow, recent};
    }
    if (seq - base_ >= window_) return {Outcome::OutOfWindow, false};

    Slot& slot = slots_[seq % window_];
    if (slot.filled) return {Outcome::Duplicate, true};

    if (seq == base_) {
        sink_(data, len);
        ++base_;
        deliver_in_order();
        return {Outcome::Delivered, true};
    }

    slot.filled = true;
    slot.data.assign(data, data + len);
    ++buffered_;
    max_buffered_ = std::max(max_buffered_, buffered_);
    return {Outcome::Buffered, true};
}

void ReceiveWindow::deliver_in_order() {
    while (base_ < total_) {
        Slot& slot = slots_[base_ % window_];
        if (!slot.filled) break;
        sink_(slot.data.data(), slot.data.size());
        slot.filled = false;
        --buffered_;
        ++base_;
    }
}

}  // namespace rudp
