//
// Readiness poller + loop wakeup channel, portable across the three
// supported platforms:
//
//   - Linux:        epoll      (epoll_create1 / epoll_ctl / epoll_wait)
//   - macOS & BSD:  kqueue     (kqueue / kevent)
//   - Windows:      WSAPoll    (socket bookkeeping + WSAPoll)
//
// All backends are level-triggered, mirroring the semantics the epoll
// implementation relied on. Event masks use the poller-neutral constants
// below (IN / ERR / HUP / RDHUP) instead of the epoll ones.
//
// The Waker is the cross-platform stand-in for eventfd: an eventfd on
// Linux, a non-blocking self-pipe on macOS/BSD, and a connected loopback
// TCP pair on Windows. It lets worker threads hand finished connections
// back to the event loop without data races.
//

#ifndef VERMELL_NET_POLLER_H
#define VERMELL_NET_POLLER_H

#include "../util/portability.h"

#include <cstdint>
#include <ctime>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
    #define VERMELL_POLL_WSAPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #include <sys/event.h>
    #define VERMELL_POLL_KQUEUE 1
#else
    #include <sys/epoll.h>
    #define VERMELL_POLL_EPOLL 1
#endif

namespace vermell::net {

    class Poller {
    public:
        struct Event {
            uint32_t events = 0;
            int fd = -1;
        };

        static constexpr uint32_t IN = 1u << 0;
        static constexpr uint32_t ERR = 1u << 1;
        static constexpr uint32_t HUP = 1u << 2;
        static constexpr uint32_t RDHUP = 1u << 3;

        Poller() {
#if defined(VERMELL_POLL_EPOLL)
            handle_ = ::epoll_create1(0);
#elif defined(VERMELL_POLL_KQUEUE)
            handle_ = ::kqueue();
#endif
            vermell::net::startup();
        }
        ~Poller() {
#if !defined(VERMELL_POLL_WSAPOLL)
            if (handle_ >= 0)
                ver_close_socket(handle_);
#endif
        }

        Poller(const Poller&) = delete;
        Poller& operator=(const Poller&) = delete;

        [[nodiscard]] bool valid() const noexcept {
#if defined(VERMELL_POLL_WSAPOLL)
            return true;
#else
            return handle_ >= 0;
#endif
        }

        bool add(const int fd, const uint32_t events) {
#if defined(VERMELL_POLL_EPOLL)
            epoll_event ev{};
            ev.events = to_epoll(events);
            ev.data.fd = fd;
            return ::epoll_ctl(handle_, EPOLL_CTL_ADD, fd, &ev) == 0;
#elif defined(VERMELL_POLL_KQUEUE)
            // EVFILT_READ implies read-readiness; the mask is informational.
            (void)events;
            struct kevent ev{};
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD, 0, 0, nullptr);
            return ::kevent(handle_, &ev, 1, nullptr, 0, nullptr) == 0;
#else
            (void)events;
            interest_[fd] = POLLRDNORM;
            return true;
#endif
        }

        bool mod(const int fd, const uint32_t events) {
#if defined(VERMELL_POLL_EPOLL)
            epoll_event ev{};
            ev.events = to_epoll(events);
            ev.data.fd = fd;
            return ::epoll_ctl(handle_, EPOLL_CTL_MOD, fd, &ev) == 0;
#elif defined(VERMELL_POLL_KQUEUE)
            (void)events;
            // Re-adding an existing knote updates it (EV_ADD is idempotent).
            struct kevent ev{};
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
            return ::kevent(handle_, &ev, 1, nullptr, 0, nullptr) == 0;
#else
            if (interest_.find(fd) == interest_.end())
                return false;
            interest_[fd] = POLLRDNORM;
            (void)events;
            return true;
#endif
        }

        bool del(const int fd) {
#if defined(VERMELL_POLL_EPOLL)
            return ::epoll_ctl(handle_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#elif defined(VERMELL_POLL_KQUEUE)
            struct kevent ev{};
            EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            return ::kevent(handle_, &ev, 1, nullptr, 0, nullptr) == 0;
#else
            return interest_.erase(fd) > 0;
#endif
        }

        // Returns the number of events stored into `out` (0 on timeout) or
        // -1 on error. `timeout_ms` >= 1, mirroring the epoll timeout clamp.
        int wait(Event* out, const int max_events, const int timeout_ms) {
#if defined(VERMELL_POLL_EPOLL)
            const int n = ::epoll_wait(handle_, epoll_events_.data(), max_events, timeout_ms);
            if (n <= 0)
                return n < 0 ? -1 : 0;
            for (int i = 0; i < n; ++i) {
                out[i].fd = epoll_events_[i].data.fd;
                out[i].events = from_epoll(epoll_events_[i].events);
            }
            return n;
#elif defined(VERMELL_POLL_KQUEUE)
            timespec ts{};
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
            const int n = ::kevent(handle_, nullptr, 0, kqueue_events_.data(), max_events, &ts);
            if (n <= 0)
                return n < 0 ? -1 : 0;
            for (int i = 0; i < n; ++i) {
                const struct kevent& kev = kqueue_events_[i];
                out[i].fd = static_cast<int>(kev.ident);
                uint32_t mask = IN; // EVFILT_READ fired: readable
                if (kev.flags & EV_ERROR)
                    mask |= ERR;
                if (kev.flags & EV_EOF)
                    mask |= HUP | RDHUP;
                out[i].events = mask;
            }
            return n;
#else
            if (interest_.empty())
                return 0;
            scratch_.clear();
            scratch_.reserve(interest_.size());
            for (const auto& entry : interest_)
                scratch_.push_back(pollfd{static_cast<SOCKET>(entry.first), entry.second, 0});

            const int n = ::WSAPoll(scratch_.data(),
                                    static_cast<ULONG>(scratch_.size()), timeout_ms);
            if (n <= 0)
                return n < 0 ? -1 : 0;

            int kept = 0;
            for (int i = 0; i < n && kept < max_events; ++i) {
                const short re = scratch_[i].revents;
                if (re == 0)
                    continue;
                uint32_t mask = 0;
                if (re & (POLLRDNORM | POLLRDBAND | POLLIN))
                    mask |= IN;
                if (re & POLLERR)
                    mask |= ERR;
                // POLLHUP: the peer sent FIN or reset. Report it as readable
                // too so recv() runs and observes the EOF (same contract as
                // the EPOLLRDHUP branch on Linux).
                if (re & POLLHUP)
                    mask |= HUP | RDHUP | IN;
                if (re & POLLNVAL)
                    mask |= ERR | HUP;
                if (mask == 0)
                    continue;
                out[kept].fd = static_cast<int>(scratch_[i].fd);
                out[kept].events = mask;
                ++kept;
            }
            return kept;
#endif
        }

        // SO_REUSEPORT is a Linux/macOS/BSD concept; Windows has no fair
        // port sharing, so multiple accept loops share one listener there.
        [[nodiscard]] static bool reuse_port_supported() noexcept {
#if defined(VERMELL_POLL_WSAPOLL)
            return false;
#else
            return true;
#endif
        }

    private:
#if defined(VERMELL_POLL_EPOLL)
        static uint32_t to_epoll(const uint32_t events) noexcept {
            uint32_t out = 0;
            if (events & IN)
                out |= EPOLLIN;
            if (events & ERR)
                out |= EPOLLERR;
            if (events & HUP)
                out |= EPOLLHUP;
            if (events & RDHUP)
                out |= EPOLLRDHUP;
            return out;
        }
        static uint32_t from_epoll(const uint32_t events) noexcept {
            uint32_t out = 0;
            if (events & EPOLLIN)
                out |= IN;
            if (events & EPOLLERR)
                out |= ERR;
            if (events & EPOLLHUP)
                out |= HUP;
            if (events & EPOLLRDHUP)
                out |= RDHUP;
            return out;
        }

        int handle_ = -1;
        std::vector<epoll_event> epoll_events_{64};

#elif defined(VERMELL_POLL_KQUEUE)
        int handle_ = -1;
        std::vector<struct kevent> kqueue_events_{64};

#else
        mutable std::unordered_map<int, short> interest_;
        mutable std::vector<pollfd> scratch_;
#endif
    };

    // ---------------------------------------------------------------------------------

    // Cross-platform event-loop wakeup channel (replaces Linux eventfd).
    // notify() wakes the loop; drain() empties it (called from the loop).
    class Waker {
    public:
        Waker() = default;
        ~Waker() { destroy(); }

        Waker(const Waker&) = delete;
        Waker& operator=(const Waker&) = delete;

        bool init() {
            destroy();
#if defined(_WIN32)
            return init_loopback_pair();
#elif defined(VERMELL_POLL_EPOLL)
            // eventfd: a single 8-byte counter fd, exactly what the loop
            // always used on Linux.
            fd_read_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            return fd_read_ >= 0;
#else
            int fds[2] = {-1, -1};
            if (::pipe(fds) != 0)
                return false;
            for (const int fd : fds) {
                if (ver_set_nonblocking(fd) != 0) {
                    ::close(fds[0]);
                    ::close(fds[1]);
                    return false;
                }
                (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            fd_read_ = fds[0];
            fd_write_ = fds[1];
            return true;
#endif
        }

        // The fd the event loop watches for read-readiness.
        [[nodiscard]] int fd() const noexcept { return fd_read_; }

        void notify() const noexcept {
#if defined(_WIN32)
            if (fd_write_ < 0)
                return;
            const char one = 1;
            (void)::send(static_cast<SOCKET>(fd_write_), &one, 1, 0);
#elif defined(VERMELL_POLL_EPOLL)
            if (fd_read_ < 0)
                return;
            const uint64_t one = 1;
            const ssize_t written = ::write(fd_read_, &one, sizeof(one));
            (void)written;
#else
            if (fd_write_ < 0)
                return;
            const char one = 1;
            while (::write(fd_write_, &one, 1) < 0 && errno == EINTR) {}
#endif
        }

        void drain() const noexcept {
#if defined(_WIN32)
            char buf[64];
            while (::recv(static_cast<SOCKET>(fd_read_), buf, sizeof(buf), 0) > 0) {}
#else
            uint64_t counter;
            while (::read(fd_read_, &counter, sizeof(counter)) > 0) {}
#endif
        }

        void destroy() noexcept {
#if defined(_WIN32)
            if (fd_read_ >= 0)
                ver_close_socket(fd_read_);
            if (fd_write_ >= 0)
                ver_close_socket(fd_write_);
#else
            if (fd_read_ >= 0)
                ::close(fd_read_);
            if (fd_write_ >= 0 && fd_write_ != fd_read_)
                ::close(fd_write_);
#endif
            fd_read_ = -1;
            fd_write_ = -1;
        }

    private:
#if defined(_WIN32)
        // Windows has neither eventfd nor pipe(); a connected loopback TCP
        // pair is the standard stand-in (the same trick libuv uses).
        bool init_loopback_pair() {
            vermell::net::startup();

            const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET)
                return false;

            SOCKET client = INVALID_SOCKET;
            SOCKET server = INVALID_SOCKET;
            bool ok = false;

            do {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = 0;

                if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
                    break;
                if (::listen(listener, 1) != 0)
                    break;

                // Resolve the ephemeral port the kernel picked for connect().
                ver_socklen_t addr_len = sizeof(addr);
                if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0)
                    break;

                client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (client == INVALID_SOCKET)
                    break;
                if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
                    break;

                server = ::accept(listener, nullptr, nullptr);
                if (server == INVALID_SOCKET)
                    break;

                if (ver_set_nonblocking(static_cast<int>(server)) != 0)
                    break;
                if (ver_set_nonblocking(static_cast<int>(client)) != 0)
                    break;
                (void)ver_set_int_option(static_cast<int>(server), IPPROTO_TCP, TCP_NODELAY, 1);
                (void)ver_set_int_option(static_cast<int>(client), IPPROTO_TCP, TCP_NODELAY, 1);

                ok = true;
            } while (false);

            ::closesocket(listener);
            if (!ok) {
                if (client != INVALID_SOCKET)
                    ::closesocket(client);
                if (server != INVALID_SOCKET)
                    ::closesocket(server);
                return false;
            }

            // server: the end the loop reads from; client: workers write here.
            fd_read_ = static_cast<int>(server);
            fd_write_ = static_cast<int>(client);
            return true;
        }
#endif

        int fd_read_ = -1;
        int fd_write_ = -1;
    };

} // namespace vermell::net

#endif // VERMELL_NET_POLLER_H
