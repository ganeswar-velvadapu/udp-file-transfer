#include "rudp/tcp_transfer.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "rudp/log.h"
#include "rudp/udp_socket.h"  // make_address / to_string

namespace rudp {

namespace fs = std::filesystem;

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

void write_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("send");
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
}

// Returns false on clean EOF before `len` bytes.
bool read_all(int fd, void* data, size_t len) {
    auto* p = static_cast<uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("recv");
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void put_be(uint8_t* p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>(v >> (8 * (bytes - 1 - i)));
}
uint64_t get_be(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}

void handle_client(int fd, const fs::path& out_dir) {
    uint8_t len_buf[2];
    if (!read_all(fd, len_buf, 2)) return;
    std::string name(get_be(len_buf, 2), '\0');
    uint8_t size_buf[8];
    if (!read_all(fd, name.data(), name.size()) || !read_all(fd, size_buf, 8)) return;
    uint64_t remaining = get_be(size_buf, 8);

    std::string base = fs::path(name).filename().string();
    if (base.empty() || base == "." || base == "..") throw std::runtime_error("bad filename");
    fs::path out_path = out_dir / ("received_" + base);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + out_path.string());

    uint64_t total = remaining;
    std::vector<char> buf(1 << 16);
    while (remaining > 0) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
        ssize_t n = ::recv(fd, buf.data(), want, 0);
        if (n == 0) throw std::runtime_error("client closed early");
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("recv");
        }
        out.write(buf.data(), n);
        remaining -= static_cast<uint64_t>(n);
    }
    out.close();
    if (out.fail()) throw std::runtime_error("write failed");
    write_all(fd, "K", 1);
    log::info("TCP received ", out_path.string(), " (", total, " bytes)");
}

}  // namespace

TcpSendStats tcp_send_file(const std::string& host, uint16_t port, const std::string& path,
                           const std::string& remote_name, size_t io_chunk) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);

    TcpSendStats st;
    st.file_size = fs::file_size(path);
    sockaddr_in addr = make_address(host, port);

    auto t0 = std::chrono::steady_clock::now();
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) throw_errno("socket");
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw_errno("connect " + to_string(addr));
    }

    std::vector<uint8_t> header(2 + remote_name.size() + 8);
    put_be(header.data(), remote_name.size(), 2);
    std::copy(remote_name.begin(), remote_name.end(), header.begin() + 2);
    put_be(header.data() + 2 + remote_name.size(), st.file_size, 8);
    write_all(fd.get(), header.data(), header.size());

    std::vector<char> buf(io_chunk);
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        write_all(fd.get(), buf.data(), static_cast<size_t>(in.gcount()));
    }

    char status = 0;
    if (!read_all(fd.get(), &status, 1) || status != 'K') throw std::runtime_error("server did not confirm");
    st.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    tcp_info info{};
    socklen_t len = sizeof(info);
    if (::getsockopt(fd.get(), IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
        st.retransmissions = info.tcpi_total_retrans;
        st.srtt_ms = info.tcpi_rtt / 1000.0;
    }
    return st;
}

void tcp_serve(const std::string& host, uint16_t port, const fs::path& out_dir, const std::atomic<bool>& stop) {
    fs::create_directories(out_dir);
    sockaddr_in addr = make_address(host, port);
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.get() < 0) throw_errno("socket");
    int one = 1;
    ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw_errno("bind " + to_string(addr));
    }
    if (::listen(listener.get(), 16) < 0) throw_errno("listen");
    log::info("TCP server listening on ", to_string(addr), ", writing to ", out_dir.string());

    while (!stop.load()) {
        pollfd pfd{listener.get(), POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;
        int c = ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        Fd client(c);
        try {
            handle_client(client.get(), out_dir);
        } catch (const std::exception& e) {
            log::info("TCP transfer failed: ", e.what());
        }
    }
}

}  // namespace rudp
