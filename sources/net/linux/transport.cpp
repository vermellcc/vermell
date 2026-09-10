// Linux TCP transport backend: sockets, fcntl, tcp options, send/recv.

#include "vermell/net/linux/transport.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace vermell::net::linux_backend {
namespace {

void set_err(std::string* err, const char* what) {
    if (err != nullptr) {
        *err = std::string(what) + ": " + std::strerror(errno);
    }
}

bool make_nonblocking(const int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

LinuxTcpListener::~LinuxTcpListener() {
    close();
}

int LinuxTcpListener::fd() const noexcept {
    return fd_;
}

bool LinuxTcpListener::bind_listen(const std::uint16_t port, const int backlog,
                                   const bool reuse_port, std::string* err) {
    close();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err(err, "socket failed");
        return false;
    }

    const int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0) {
        set_err(err, "setsockopt(SO_REUSEADDR) failed");
        ::close(fd);
        return false;
    }
    // SO_REUSEPORT stays opt-in: same-UID processes could hijack the port.
    if (reuse_port
        && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0) {
        set_err(err, "setsockopt(SO_REUSEPORT) failed");
        ::close(fd);
        return false;
    }

    if (!make_nonblocking(fd)) {
        set_err(err, "fcntl(O_NONBLOCK) failed");
        ::close(fd);
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) < 0) {
        set_err(err, "bind failed");
        ::close(fd);
        return false;
    }
    if (::listen(fd, backlog > 0 ? backlog : SOMAXCONN) < 0) {
        set_err(err, "listen failed");
        ::close(fd);
        return false;
    }

    fd_ = fd;
    port_ = port;
    backlog_ = backlog;
    return true;
}

Accepted LinuxTcpListener::accept(std::string* err) {
    if (fd_ < 0) {
        if (err != nullptr)
            *err = "listener is not bound";
        return Accepted{};
    }
    for (;;) {
        // accept4 returns a non-blocking, close-on-exec fd directly.
        const int client = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0)
            return Accepted{client};
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Accepted{};
        set_err(err, "accept failed");
        return Accepted{};
    }
}

void LinuxTcpListener::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LinuxStreamOps::set_nonblocking(const int fd, std::string* err) const {
    if (!make_nonblocking(fd)) {
        set_err(err, "fcntl(O_NONBLOCK) failed");
        return false;
    }
    return true;
}

long LinuxStreamOps::recv_some(const int fd, char* buf, const std::size_t len) const {
    if (buf == nullptr || len == 0)
        return -1;
    for (;;) {
        const ssize_t bytes = ::recv(fd, buf, len, 0);
        if (bytes > 0)
            return static_cast<long>(bytes);
        if (bytes == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;
        return -2;
    }
}

bool LinuxStreamOps::send_all(const int fd, const char* data, const std::size_t len,
                              const int timeout_ms, std::string* err) const {
    if (fd < 0 || (data == nullptr && len > 0))
        return false;
    std::size_t done = 0;
    const int wait_ms = timeout_ms < 0 ? 0 : timeout_ms;
    while (done < len) {
        iovec iov{};
        iov.iov_base = const_cast<char*>(data + done);
        iov.iov_len = len - done;
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        const ssize_t sent = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (sent > 0) {
            done += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            // timeout 0 = single best-effort attempt.
            if (::poll(&pfd, 1, wait_ms) > 0 && (pfd.revents & POLLOUT) != 0)
                continue;
        }
        set_err(err, "send failed");
        return false;
    }
    return true;
}

void LinuxStreamOps::set_tcp_nodelay(const int fd) const noexcept {
    const int on = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

void LinuxStreamOps::close_fd(const int fd) const noexcept {
    if (fd >= 0)
        (void)::close(fd);
}

std::unique_ptr<TcpListener> LinuxTransportFactory::create_listener() const {
    return std::make_unique<LinuxTcpListener>();
}

std::shared_ptr<StreamOps> LinuxTransportFactory::stream_ops() const {
    if (!ops_)
        ops_ = std::make_shared<LinuxStreamOps>();
    return ops_;
}

} // namespace vermell::net::linux_backend
