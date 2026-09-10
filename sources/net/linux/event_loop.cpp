// Linux EventLoop backend: epoll for readiness, eventfd for wakeups.

#include "vermell/net/linux/event_loop.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace vermell::net::linux_backend {
namespace {

std::uint32_t to_epoll_mask(const Interest interest) {
    switch (interest) {
        case Interest::Read: return EPOLLIN;
        case Interest::Write: return EPOLLOUT;
        case Interest::ReadWrite: return EPOLLIN | EPOLLOUT;
    }
    return EPOLLIN;
}

void ctl(int epoll_fd, int op, int fd, Interest interest) {
    epoll_event ev{};
    ev.events = to_epoll_mask(interest);
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd, op, fd, &ev) < 0) {
        // Re-adding a live fd is a modify; modifying a gone fd is an add.
        if (op == EPOLL_CTL_ADD && errno == EEXIST) {
            if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0)
                return;
        } else if (op == EPOLL_CTL_MOD && errno == ENOENT) {
            if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0)
                return;
        }
        throw std::runtime_error(std::string("epoll_ctl failed: ") + std::strerror(errno));
    }
}

} // namespace

EpollEventLoop::EpollEventLoop(const int max_events)
    : max_events_(max_events > 0 ? max_events : 1024) {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error(std::string("epoll_create1 failed: ") + std::strerror(errno));

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        const int saved = errno;
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(saved));
    }

    try {
        ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, Interest::Read);
    } catch (...) {
        ::close(wake_fd_);
        ::close(epoll_fd_);
        wake_fd_ = epoll_fd_ = -1;
        throw;
    }
}

EpollEventLoop::~EpollEventLoop() {
    if (wake_fd_ >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, wake_fd_, nullptr);
        ::close(wake_fd_);
    }
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

void EpollEventLoop::add(const int fd, const Interest interest) {
    ctl(epoll_fd_, EPOLL_CTL_ADD, fd, interest);
}

void EpollEventLoop::modify(const int fd, const Interest interest) {
    ctl(epoll_fd_, EPOLL_CTL_MOD, fd, interest);
}

void EpollEventLoop::remove(const int fd) {
    // epoll auto-drops closed fds; ENOENT/EBADF just mean "already gone".
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

std::vector<Event> EpollEventLoop::wait(const int timeout_ms) {
    std::vector<epoll_event> native(static_cast<std::size_t>(max_events_));
    const int ready = ::epoll_wait(epoll_fd_, native.data(), max_events_, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR)
            return {};
        throw std::runtime_error(std::string("epoll_wait failed: ") + std::strerror(errno));
    }

    std::vector<Event> out;
    out.reserve(static_cast<std::size_t>(ready));
    for (int i = 0; i < ready; ++i) {
        const auto& ev = native[static_cast<std::size_t>(i)];
        if (ev.data.fd == wake_fd_) {
            // Drain the wake counter, then report it.
            std::uint64_t counter = 0;
            while (::read(wake_fd_, &counter, sizeof counter) > 0) {
            }
            Event wake{};
            wake.fd = wake_fd_;
            wake.readable = true;
            out.push_back(wake);
            continue;
        }
        Event event{};
        event.fd = ev.data.fd;
        event.readable = (ev.events & EPOLLIN) != 0;
        event.writable = (ev.events & EPOLLOUT) != 0;
        event.error = (ev.events & EPOLLERR) != 0;
        event.closed = (ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0;
        out.push_back(event);
    }
    return out;
}

void EpollEventLoop::wake() {
    const std::uint64_t one = 1;
    // Non-blocking: EAGAIN is fine, the waiter fires anyway.
    const ssize_t written = ::write(wake_fd_, &one, sizeof one);
    (void)written;
}

EpollEventLoopFactory::EpollEventLoopFactory(const int max_events)
    : max_events_(max_events > 0 ? max_events : 1024) {}

std::unique_ptr<EventLoop> EpollEventLoopFactory::create() const {
    return std::make_unique<EpollEventLoop>(max_events_);
}

} // namespace vermell::net::linux_backend
