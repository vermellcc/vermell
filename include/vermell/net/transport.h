// Portable TCP transport SPI: listening sockets + byte-stream helpers.
// OS-agnostic: no <sys/*>, <netinet/*>, <arpa/*>, <unistd.h>, <fcntl.h> (spi_portable_headers test).
// fds are plain ints owned by the creator; recv_some codes: >0 bytes, 0 = peer
// shutdown, -1 = no data right now, -2 = hard error; accept() returns Accepted{-1}
// with empty err when nothing is pending.

#ifndef VERMELL_NET_TRANSPORT_H
#define VERMELL_NET_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vermell::net {

struct Accepted {
    int fd = -1;
};

class TcpListener {
public:
    virtual ~TcpListener() = default;

    // -1 when not bound.
    [[nodiscard]] virtual int fd() const noexcept = 0;
    // Non-blocking bind + listen; SO_REUSEPORT only when reuse_port is true.
    // Returns false and fills *err (when non-null) on failure.
    virtual bool bind_listen(std::uint16_t port, int backlog, bool reuse_port,
                             std::string* err) = 0;
    // Accepted{-1} with empty *err means "nothing pending, try later".
    virtual Accepted accept(std::string* err) = 0;
    // Never throws; idempotent.
    virtual void close() noexcept = 0;
};

class StreamOps {
public:
    virtual ~StreamOps() = default;

    virtual bool set_nonblocking(int fd, std::string* err) const = 0;
    // NOTE: long (not ssize_t) keeps this header free of <sys/types.h>.
    virtual long recv_some(int fd, char* buf, std::size_t len) const = 0;
    // Writes the whole buffer within timeout_ms; false + *err on failure/timeout.
    virtual bool send_all(int fd, const char* data, std::size_t len,
                          int timeout_ms, std::string* err) const = 0;
    virtual void set_tcp_nodelay(int fd) const noexcept = 0;
    virtual void close_fd(int fd) const noexcept = 0;
};

struct TransportFactory {
    virtual ~TransportFactory() = default;
    virtual std::unique_ptr<TcpListener> create_listener() const = 0;
    virtual std::shared_ptr<StreamOps> stream_ops() const = 0;
};

} // namespace vermell::net

#endif // VERMELL_NET_TRANSPORT_H
