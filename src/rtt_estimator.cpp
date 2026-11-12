#include "rudp/rtt_estimator.h"

#include <algorithm>
#include <cmath>

namespace rudp {

RttEstimator::RttEstimator(Micros initial_rto, Micros min_rto, Micros max_rto)
    : rto_(std::clamp(initial_rto, min_rto, max_rto)), min_(min_rto), max_(max_rto) {}

void RttEstimator::add_sample(Micros rtt) {
    double r = static_cast<double>(rtt.count());
    if (!has_sample_) {
        srtt_ = r;
        rttvar_ = r / 2;
        has_sample_ = true;
    } else {
        rttvar_ = 0.75 * rttvar_ + 0.25 * std::fabs(srtt_ - r);
        srtt_ = 0.875 * srtt_ + 0.125 * r;
    }
    auto rto = Micros(static_cast<long long>(srtt_ + std::max(1000.0, 4 * rttvar_)));
    rto_ = std::clamp(rto, min_, max_);
}

RttEstimator::Micros RttEstimator::backoff(unsigned retries) const {
    long long v = rto_.count() << std::min(retries, 6u);
    return std::min(Micros(v), max_);
}

}  // namespace rudp
