#pragma once

#include <chrono>

namespace rudp {

// Retransmission-timeout estimator from RFC 6298 (Jacobson/Karels):
//   SRTT   <- 7/8 SRTT + 1/8 R
//   RTTVAR <- 3/4 RTTVAR + 1/4 |SRTT - R|
//   RTO     = SRTT + 4 * RTTVAR, clamped to [min, max]
// Callers must only feed samples from packets that were never retransmitted
// (Karn's algorithm), otherwise an ACK can't be matched to a transmission.
class RttEstimator {
public:
    using Micros = std::chrono::microseconds;

    RttEstimator(Micros initial_rto, Micros min_rto, Micros max_rto);

    void add_sample(Micros rtt);

    Micros rto() const { return rto_; }
    Micros srtt() const { return Micros(static_cast<long long>(srtt_)); }
    bool has_sample() const { return has_sample_; }

    // Per-packet exponential backoff: rto * 2^retries, capped at max.
    Micros backoff(unsigned retries) const;

private:
    double srtt_ = 0;
    double rttvar_ = 0;
    bool has_sample_ = false;
    Micros rto_;
    Micros min_;
    Micros max_;
};

}  // namespace rudp
