// Linux TCP transport backend; only file allowed to touch sockets/fcntl/tcp options.
// linux_backend: "linux" alone would collide with GCC's predefined macro.

#ifndef VERMELL_NET_LINUX_TRANSPORT_H
#define VERMELL_NET_LINUX_TRANSPORT_H

#include <cstdint>

#include "../transport.h"

namespace vermell::net::linux_backend {

// IPv4 listener over accept4 (O_NONBLOCK); SO_REUSEPORT opt-in.
class LinuxTcpListener final : public TcpListener {
public:
    LinuxTcpListener() = default;
    ~LinuxTcpListener() override;

    LinuxTcpListener(const LinuxTcpListener&) = delete;
    LinuxTcpListener& operator=(const LinuxTcpListener&) = delete;

    [[nodiscard]] int fd() const noexcept override;
    bool bind_listen(std::uint16_t port, int backlog, bool reuse_port,
                     std::string* err) override;
    Accepted accept(std::string* err) override;
    void close() noexcept override;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int backlog() const noexcept { return backlog_; }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    int backlog_ = 0;
};

class LinuxStreamOps final : public StreamOps {
public:
    bool set_nonblocking(int fd, std::string* err) const override;
    long recv_some(int fd, char* buf, std::size_t len) const override;
    bool send_all(int fd, const char* data, std::size_t len,
                  int timeout_ms, std::string* err) const override;
    void set_tcp_nodelay(int fd) const noexcept override;
    void close_fd(int fd) const noexcept override;
};

class LinuxTransportFactory final : public TransportFactory {
public:
    std::unique_ptr<TcpListener> create_listener() const override;
    std::shared_ptr<StreamOps> stream_ops() const override;

private:
    mutable std::shared_ptr<StreamOps> ops_;
};

} // namespace vermell::net::linux_backend

#endif // VERMELL_NET_LINUX_TRANSPORT_H
