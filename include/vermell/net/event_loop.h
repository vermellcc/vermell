// Portable event-loop SPI: semantic readiness + wakeup every backend provides.
// OS-agnostic: no <sys/*>, <netinet/*>, <arpa/*>, <unistd.h>, <fcntl.h> (spi_portable_headers test).
// Semantic API, not an epoll wrapper: backends map native readiness to Event.

#ifndef VERMELL_NET_EVENT_LOOP_H
#define VERMELL_NET_EVENT_LOOP_H

#include <memory>
#include <vector>

namespace vermell::net {

// A readiness notification for one file descriptor.
struct Event {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    bool error = false;
    // Peer closed or hangup: the fd should be drained and then removed.
    bool closed = false;
};

enum class Interest {
    Read,
    Write,
    ReadWrite,
};

class EventLoop {
public:
    virtual ~EventLoop() = default;

    // Re-adding a registered fd behaves like modify().
    virtual void add(int fd, Interest interest) = 0;
    // Adds when unknown.
    virtual void modify(int fd, Interest interest) = 0;
    // Never throws; unknown fds are ignored.
    virtual void remove(int fd) = 0;
    // Empty on timeout; wake() unblocks wait() from another thread.
    virtual std::vector<Event> wait(int timeout_ms) = 0;
    // Thread-safe.
    virtual void wake() = 0;
};

class EventLoopFactory {
public:
    virtual ~EventLoopFactory() = default;
    virtual std::unique_ptr<EventLoop> create() const = 0;
};

} // namespace vermell::net

#endif // VERMELL_NET_EVENT_LOOP_H
