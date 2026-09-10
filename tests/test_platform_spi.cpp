// Core/Platform-SPI tests: fake-backend substitution, Linux conformance, server smoke test.
// Portable SPI headers come first on purpose: the #error checks fail the build on OS-header leaks.

#include "../include/vermell/net/event_loop.h"
#include "../include/vermell/net/transport.h"
#include "../include/vermell/net/platform.h"

#if defined(_SYS_EPOLL_H)
#error "portable SPI leaked <sys/epoll.h>"
#endif
#if defined(_SYS_EVENTFD_H)
#error "portable SPI leaked <sys/eventfd.h>"
#endif
#if defined(_SYS_SOCKET_H)
#error "portable SPI leaked <sys/socket.h>"
#endif

#include "../include/vermell/vermell.h"
#include "../include/vermell/net/linux/event_loop.h"
#include "../include/vermell/net/linux/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vermell::net::Event;
using vermell::net::EventLoop;
using vermell::net::Interest;

// In-memory fake event loop: proves the core runs against any backend.
class FakeEventLoop final : public EventLoop {
public:
    std::vector<int> added;
    std::vector<int> modified;
    std::vector<int> removed;
    std::atomic<bool> woken{false};
    std::vector<std::vector<Event>> programmed;

    void add(const int fd, const Interest) override { added.push_back(fd); }
    void modify(const int fd, const Interest) override { modified.push_back(fd); }
    void remove(const int fd) override { removed.push_back(fd); }

    std::vector<Event> wait(const int) override {
        if (!programmed.empty()) {
            auto batch = std::move(programmed.front());
            programmed.erase(programmed.begin());
            return batch;
        }
        if (woken.exchange(false)) {
            Event wake{};
            wake.fd = -999; // sentinel, not a real connection
            wake.readable = true;
            return {wake};
        }
        return {};
    }

    void wake() override { woken.store(true); }
};

class FakeListener final : public vermell::net::TcpListener {
public:
    int fd_ = 100;
    bool bound = false;
    std::vector<int> pending_accepts;

    [[nodiscard]] int fd() const noexcept override { return bound ? fd_ : -1; }

    bool bind_listen(const std::uint16_t, const int, const bool,
                     std::string*) override {
        bound = true;
        return true;
    }

    vermell::net::Accepted accept(std::string*) override {
        if (pending_accepts.empty())
            return vermell::net::Accepted{};
        const int fd = pending_accepts.front();
        pending_accepts.erase(pending_accepts.begin());
        return vermell::net::Accepted{fd};
    }

    void close() noexcept override { bound = false; }
};

class FakeStreamOps final : public vermell::net::StreamOps {
public:
    mutable std::unordered_map<int, std::string> inbound;
    mutable std::unordered_map<int, std::string> sent;
    mutable std::vector<int> closed;

    bool set_nonblocking(const int, std::string*) const override { return true; }

    long recv_some(const int fd, char* buf, const std::size_t len) const override {
        auto it = inbound.find(fd);
        if (it == inbound.end() || it->second.empty())
            return -1;
        const std::size_t n = std::min(len, it->second.size());
        std::memcpy(buf, it->second.data(), n);
        it->second.erase(0, n);
        return static_cast<long>(n);
    }

    bool send_all(const int fd, const char* data, const std::size_t len,
                  const int, std::string*) const override {
        sent[fd].append(data, len);
        return true;
    }

    void set_tcp_nodelay(const int) const noexcept override {}

    void close_fd(const int fd) const noexcept override { closed.push_back(fd); }
};

class FakeLoopFactory final : public vermell::net::EventLoopFactory {
public:
    std::unique_ptr<EventLoop> create() const override {
        return std::make_unique<FakeEventLoop>();
    }
};

class FakeTransportFactory final : public vermell::net::TransportFactory {
public:
    std::unique_ptr<vermell::net::TcpListener> create_listener() const override {
        return std::make_unique<FakeListener>();
    }
    std::shared_ptr<vermell::net::StreamOps> stream_ops() const override {
        if (!ops_)
            ops_ = std::make_shared<FakeStreamOps>();
        return ops_;
    }
    std::shared_ptr<FakeStreamOps> fake_ops() const {
        return std::static_pointer_cast<FakeStreamOps>(stream_ops());
    }

private:
    mutable std::shared_ptr<vermell::net::StreamOps> ops_;
};

Event readable_event(const int fd) {
    Event ev{};
    ev.fd = fd;
    ev.readable = true;
    return ev;
}

// Raw-socket HTTP client for smoke tests.
std::string raw_get(const std::uint16_t port, const std::string& target = "/") {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return {};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return {};
    }
    if (!connected) {
        if (fd >= 0)
            ::close(fd);
        return {};
    }

    const std::string req = "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            ::close(fd);
            return {};
        }
        sent += static_cast<size_t>(n);
    }

    std::string response;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return response;
}

} // namespace

// 1. Substitution proof: the core talks to the abstraction, not to Linux.
static_assert(std::is_base_of_v<EventLoop, FakeEventLoop>,
              "fakes must satisfy the EventLoop abstraction");
static_assert(std::is_base_of_v<vermell::net::TcpListener, FakeListener>);
static_assert(std::is_base_of_v<vermell::net::StreamOps, FakeStreamOps>);

TEST(PlatformSpi, FakeLoopSatisfiesAbstraction) {
    FakeEventLoop loop;
    EventLoop& generic = loop;

    generic.add(3, Interest::Read);
    generic.modify(3, Interest::ReadWrite);
    generic.wake();

    loop.programmed.push_back({readable_event(5)});

    const auto evs = generic.wait(0);
    ASSERT_EQ(evs.size(), 1UL);
    EXPECT_EQ(evs[0].fd, 5);
    EXPECT_TRUE(evs[0].readable);

    generic.remove(3);

    EXPECT_EQ(loop.added, std::vector<int>({3}));
    EXPECT_EQ(loop.modified, std::vector<int>({3}));
    EXPECT_EQ(loop.removed, std::vector<int>({3}));
    EXPECT_TRUE(loop.woken.load());
}

// Full request served end-to-end through fakes, with no socket, epoll, or thread pool.
TEST(PlatformSpi, RequestServedThroughFakeTransport) {
    FakeEventLoop loop;
    FakeListener listener;
    auto ops = std::make_shared<FakeStreamOps>();

    listener.pending_accepts.push_back(7);
    ops->inbound[7] = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

    auto routes = std::make_shared<RoutesMap>();
    MiddlewareList middlewares{[](Query& http) { http.send("hello-spi"); }};
    (*routes)[route_key("/", "GET")] =
        std::make_unique<listen_routes>("/", std::move(middlewares), "GET");

    RequestIO io(loop, routes, /*listener_fd=*/100, listener,
                 vermell::Config{}, {}, /*allow_keep_alive=*/true,
                 /*pool=*/nullptr, ops);

    io.dispatch({readable_event(100)});
    EXPECT_EQ(loop.added, std::vector<int>({100, 7}));

    io.dispatch({readable_event(7)});

    ASSERT_TRUE(ops->sent.count(7) > 0);
    EXPECT_NE(ops->sent[7].find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_TRUE(ops->sent[7].ends_with("hello-spi"));
    EXPECT_EQ(io.handled_connections(), 1UL);
    EXPECT_TRUE(std::find(ops->closed.begin(), ops->closed.end(), 7) != ops->closed.end());
}

// 2. Linux backend conformance.
static_assert(std::is_base_of_v<EventLoop, vermell::net::linux_backend::EpollEventLoop>);
static_assert(std::is_base_of_v<vermell::net::EventLoopFactory,
                                vermell::net::linux_backend::EpollEventLoopFactory>);
static_assert(std::is_base_of_v<vermell::net::TcpListener,
                                vermell::net::linux_backend::LinuxTcpListener>);
static_assert(std::is_base_of_v<vermell::net::StreamOps,
                                vermell::net::linux_backend::LinuxStreamOps>);
static_assert(std::is_base_of_v<vermell::net::TransportFactory,
                                vermell::net::linux_backend::LinuxTransportFactory>);

TEST(LinuxBackend, AddAndWaitSeesReadableSocket) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    vermell::net::linux_backend::EpollEventLoop loop;
    loop.add(pair[0], Interest::Read);

    ASSERT_EQ(::write(pair[1], "x", 1), 1);
    const auto evs = loop.wait(1000);

    bool seen = false;
    for (const auto& ev : evs)
        seen = seen || (ev.fd == pair[0] && ev.readable && !ev.error);
    EXPECT_TRUE(seen);

    loop.remove(pair[0]);
    const auto quiet = loop.wait(100);
    for (const auto& ev : quiet)
        EXPECT_NE(ev.fd, pair[0]);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST(LinuxBackend, WakeInterruptsWait) {
    vermell::net::linux_backend::EpollEventLoop loop;

    std::vector<Event> got;
    auto waiter = std::async(std::launch::async, [&] { got = loop.wait(10000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto start = std::chrono::steady_clock::now();
    loop.wake();
    waiter.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, std::chrono::seconds(5));
    ASSERT_FALSE(got.empty());
    EXPECT_EQ(got.back().fd, loop.wake_fd());
    EXPECT_TRUE(loop.wait(0).empty()); // wakeup drained
}

TEST(LinuxBackend, TransportBindListenAcceptRoundTrip) {
    vermell::net::linux_backend::LinuxTransportFactory factory;
    auto listener = factory.create_listener();
    ASSERT_NE(listener, nullptr);

    std::string err;
    ASSERT_TRUE(listener->bind_listen(0, 16, false, &err)) << err;
    ASSERT_GE(listener->fd(), 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    ASSERT_EQ(::getsockname(listener->fd(),
                            reinterpret_cast<sockaddr*>(&bound), &bound_len), 0);
    const auto port = ntohs(bound.sin_port);
    EXPECT_GT(port, 0);

    int client = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    vermell::net::Accepted accepted;
    for (int i = 0; i < 50 && accepted.fd < 0; ++i) {
        accepted = listener->accept(&err);
        if (accepted.fd < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(accepted.fd, 0) << err;

    auto ops = factory.stream_ops();
    ASSERT_NE(ops, nullptr);
    ASSERT_TRUE(ops->send_all(accepted.fd, "hello", 5, 1000, &err)) << err;

    char buf[16] = {};
    ASSERT_EQ(::recv(client, buf, 5, MSG_WAITALL), 5);
    EXPECT_EQ(std::string(buf, 5), "hello");

    ASSERT_EQ(::send(client, "world", 5, MSG_NOSIGNAL), 5);
    char back[16] = {};
    const long n = ops->recv_some(accepted.fd, back, sizeof(back));
    EXPECT_EQ(n, 5);
    EXPECT_EQ(std::string(back, 5), "world");

    ops->close_fd(accepted.fd);
    ::close(client);
    listener->close();
    EXPECT_EQ(listener->fd(), -1);
}

// 3. Platform bundle + server smoke test on the default (Linux) backend.
TEST(PlatformSpi, DefaultPlatformResolves) {
    const auto platform = vermell::net::default_platform();
    ASSERT_NE(platform, nullptr);
    ASSERT_NE(platform->loops, nullptr);
    ASSERT_NE(platform->transport, nullptr);
    EXPECT_NE(platform->loops->create(), nullptr);
    EXPECT_NE(platform->transport->create_listener(), nullptr);
    EXPECT_NE(platform->transport->stream_ops(), nullptr);
}

TEST(PlatformSpi, ServerSmokeTestOnDefaultPlatform) {
    Router router;
    router.setPort(8131);
    router.get("/", {[](Query& http) { http.send("spi-smoke"); }});

    auto server = std::async(std::launch::async, [&] { router.listenOne(); });
    const std::string res = raw_get(8131);
    server.get();

    EXPECT_NE(res.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_TRUE(res.ends_with("spi-smoke"));
}
