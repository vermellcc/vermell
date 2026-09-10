// Linux event-loop backend (epoll + eventfd); only file allowed to touch them.
// linux_backend: "linux" alone would collide with GCC's predefined macro.

#ifndef VERMELL_NET_LINUX_EPOLL_EVENT_LOOP_H
#define VERMELL_NET_LINUX_EPOLL_EVENT_LOOP_H

#include "../event_loop.h"

namespace vermell::net::linux_backend {

// wake() writes an eventfd; wait() reports it as an Event on the wake fd and drains it.
class EpollEventLoop final : public EventLoop {
public:
    explicit EpollEventLoop(int max_events = 1024);
    ~EpollEventLoop() override;

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    void add(int fd, Interest interest) override;
    void modify(int fd, Interest interest) override;
    void remove(int fd) override;
    std::vector<Event> wait(int timeout_ms) override;
    void wake() override;

    // Exposed for tests.
    [[nodiscard]] int wake_fd() const noexcept { return wake_fd_; }

private:
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    int max_events_ = 1024;
};

class EpollEventLoopFactory final : public EventLoopFactory {
public:
    explicit EpollEventLoopFactory(int max_events = 1024);
    std::unique_ptr<EventLoop> create() const override;

private:
    int max_events_ = 1024;
};

} // namespace vermell::net::linux_backend

#endif // VERMELL_NET_LINUX_EPOLL_EVENT_LOOP_H
