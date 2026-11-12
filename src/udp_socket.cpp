#include "rudp/udp_socket.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace rudp {

namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

sockaddr_in make_address(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        throw std::runtime_error("cannot resolve host '" + host + "': " + gai_strerror(rc));
    }
    sockaddr_in addr{};
    std::memcpy(&addr, res->ai_addr, sizeof(addr));
    freeaddrinfo(res);
    addr.sin_port = htons(port);
    return addr;
}

std::string to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

UdpSocket::UdpSocket() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) throw_errno("socket");
    rng_.seed(std::random_device{}());
}

UdpSocket::~UdpSocket() {
    if (fd_ >= 0) ::close(fd_);
}

void UdpSocket::bind(const sockaddr_in& addr) {
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw_errno(("bind " + to_string(addr)).c_str());
    }
}

void UdpSocket::set_buffer_sizes(int bytes) {
    // Best effort: the kernel silently caps these at net.core.{r,w}mem_max.
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

void UdpSocket::set_impairment(const Impairment& impairment) {
    impairment_ = impairment;
    if (impairment.seed != 0) rng_.seed(impairment.seed);
}

void UdpSocket::raw_send(const uint8_t* data, size_t len, const sockaddr_in& to) {
    while (true) {
        ssize_t n = ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        if (n >= 0) break;
        if (errno == EINTR) continue;
        // A full send queue or unreachable peer is just packet loss to us;
        // the ARQ layer will retransmit.
        if (errno == ENOBUFS || errno == EAGAIN || errno == ECONNREFUSED) return;
        throw_errno("sendto");
    }
    stats_.datagrams_sent++;
    stats_.bytes_sent += len;
}

void UdpSocket::flush_held() {
    if (held_) {
        Held h = std::move(*held_);
        held_.reset();
        raw_send(h.data.data(), h.data.size(), h.to);
    }
}

void UdpSocket::send_to(const uint8_t* data, size_t len, const sockaddr_in& to) {
    if (!impairment_.active()) {
        raw_send(data, len, to);
        return;
    }
    if (coin_(rng_) < impairment_.loss) {
        stats_.impair_dropped++;
        flush_held();
        return;
    }
    if (!held_ && coin_(rng_) < impairment_.reorder) {
        // Hold this datagram back; it goes out after the next one.
        stats_.impair_reordered++;
        held_ = Held{std::vector<uint8_t>(data, data + len), to};
        return;
    }
    raw_send(data, len, to);
    if (coin_(rng_) < impairment_.dup) {
        stats_.impair_duplicated++;
        raw_send(data, len, to);
    }
    flush_held();
}

std::optional<size_t> UdpSocket::try_recv(uint8_t* buf, size_t cap, sockaddr_in& from) {
    while (true) {
        socklen_t from_len = sizeof(from);
        ssize_t n = ::recvfrom(fd_, buf, cap, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= 0) {
            stats_.datagrams_received++;
            stats_.bytes_received += static_cast<uint64_t>(n);
            return static_cast<size_t>(n);
        }
        if (errno == EINTR) continue;
        // ECONNREFUSED: ICMP port unreachable from an earlier send; ignore.
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED) return std::nullopt;
        throw_errno("recvfrom");
    }
}

bool UdpSocket::wait_readable(std::chrono::microseconds timeout) {
    flush_held();  // never leave a reordered datagram stuck while idle
    if (timeout.count() < 0) timeout = std::chrono::microseconds(0);
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(timeout.count() / 1000000);
    ts.tv_nsec = static_cast<long>((timeout.count() % 1000000) * 1000);
    pollfd pfd{fd_, POLLIN, 0};
    int rc = ::ppoll(&pfd, 1, &ts, nullptr);
    if (rc < 0) {
        if (errno == EINTR) return false;
        throw_errno("ppoll");
    }
    return rc > 0;
}

}  // namespace rudp
