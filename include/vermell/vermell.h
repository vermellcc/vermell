#ifndef VERMELL_H
#define VERMELL_H

#include "sockets.h"
#include "routes.hpp"
#include "config.hpp"
#include "request/router_epoll.h"
#include "util/enums.h"
#include "util/process.h"
#include "util/environment.h"
#include "util/static_files.h"
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using std::make_shared, std::make_unique;
using std::string;

// RoutesMap is a global alias from routes.hpp (transparent hashing).
using enums::neo;

template <class T>
class Vermell {

    shared_ptr<workers::RouterEpoll<T>> router_epoll;
    shared_ptr<RoutesMap> routes;
    std::shared_ptr<T> tcpControl;

    vermell::Config config_{};

    // Static directory mounts (router.staticX): the fallback layer that
    // serves files once no exact route matched. Explicit routes always win.
    std::vector<vermell::StaticMount> static_mounts_;

    void tcpInt();
    // Re-applies the network-related fields of config_ to a live tcpControl.
    void applyNetworkConfig() noexcept;

public:
    [[maybe_unused]] explicit Vermell(uint16_t port);
    explicit Vermell();

    int http_response(const string&, MiddlewareList, const string&);

    int get(const string& route,    const MiddlewareList& middlewares);
    int post(const string& route,   const MiddlewareList& middlewares);
    int put(const string& route,    const MiddlewareList& middlewares);
    int deleteX(const string& route,const MiddlewareList& middlewares);
    int patch(const string& route,  const MiddlewareList& middlewares);
    int head(const string& route,   const MiddlewareList& middlewares);
    int options(const string& route,const MiddlewareList& middlewares);
    int link(const string& route,   const MiddlewareList& middlewares);
    int unlink(const string& route, const MiddlewareList& middlewares);
    int purge(const string& route,  const MiddlewareList& middlewares);

    int use(const Route_t&);
    Vermell& staticX(const std::string& mount, const std::string& dir,
                     const vermell::StaticOptions& options = {}) noexcept;


    //   router.configure({ .max_request_size = 64UL*1024*1024, .threads = 8 });
    Vermell& configure(const vermell::Config& config) noexcept;
    [[nodiscard]] const vermell::Config& config() const noexcept { return config_; }

    Vermell& setReadTimeout(std::chrono::milliseconds timeout) noexcept;
    Vermell& setWriteTimeout(std::chrono::milliseconds timeout) noexcept;
    Vermell& setRequestTimeout(std::chrono::milliseconds timeout) noexcept;
    Vermell& setMaxRequestSize(size_t bytes) noexcept;
    Vermell& setReadChunkSize(size_t bytes) noexcept;
    Vermell& setThreads(size_t threads) noexcept;
    Vermell& setAcceptThreads(size_t accept_threads) noexcept;
    Vermell& setMaxEvents(int max_events) noexcept;
    Vermell& setMaxQueueSize(size_t max_queue_size) noexcept;
    Vermell& setMaxConnections(size_t max_connections) noexcept;
    Vermell& setBacklog(int backlog) noexcept;
    Vermell& setBufferSize(int size) noexcept;
    Vermell& setReusePort(bool reuse_port) noexcept;

    int setPort(uint16_t) noexcept;
    [[nodiscard]] [[maybe_unused]] inline uint16_t getPort() const noexcept{
        constexpr auto min_port = static_cast<uint16_t>(neo::MIN_PORT);
        constexpr auto default_port = static_cast<uint16_t>(neo::DEF_PORT);
        return config_.port >= min_port ? config_.port : default_port;
    };
    void listen();
    void listenOne();
    void setListenStatus(neo::eStatus);

};

template <class T>
[[maybe_unused]] Vermell<T>::Vermell(const uint16_t port) {
    if (port >= neo::MIN_PORT) { config_.port = port; }
    tcpInt();
}

template <class T>
Vermell<T>::Vermell() {
    tcpInt();
}

template <class T>

int Vermell<T>::http_response(const string &endpoint, MiddlewareList middlewareList, const string& type) {
    try {
        if (routes == nullptr)
            routes = make_shared<RoutesMap>();

        routes->operator[](route_key(endpoint, type)) = make_unique<listen_routes>( endpoint, std::move(middlewareList), type);
    }
    catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return neo::ERROR;
    }
    return neo::OK;
}

template <class T>
[[maybe_unused]] int Vermell<T>::get(const string& route,const MiddlewareList &middlewares){
    return http_response(route, middlewares, GET_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::post(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, POST_TYPE );
}
template <class T>
[[maybe_unused]] int Vermell<T>::put(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, PUT_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::deleteX(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, DELETE_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::patch(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, PATCH_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::head(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, HEAD_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::options(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, OPTIONS_TYPE);
}
template <class T>
[[maybe_unused]]  int Vermell<T>::link(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, LINK_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::unlink(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, UNLINK_TYPE);
}
template <class T>
[[maybe_unused]] int Vermell<T>::purge(const string& route,const MiddlewareList &middlewares) {
    return http_response(route, middlewares, PURGE_TYPE);
}

template <class T>
[[maybe_unused]] int Vermell<T>::use(const Route_t & route) {
    return http_response( route.route, route.middlewares, route.type);
}

template <class T>
Vermell<T>& Vermell<T>::staticX(const std::string& mount, const std::string& dir,
                                const vermell::StaticOptions& options) noexcept {
    try {
        static_mounts_.emplace_back(mount, dir, options);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
    return *this;
}


template <class T>
void Vermell<T>::listen() {
   router_epoll->getMainProcess(routes, neo::WHILE, config_, static_mounts_);
}
template <class T>
void Vermell<T>::listenOne() {
    router_epoll->getMainProcess(routes, neo::UNIQUE, config_, static_mounts_);
}

template <class T>
void Vermell<T>::setListenStatus(neo::eStatus _status) {
    router_epoll->setListenStatus(_status);
}


template <class T>
int Vermell<T>::setPort(const uint16_t _port) noexcept {
    if (_port >= neo::MIN_PORT) {
        config_.port = _port;
        if(tcpControl != nullptr) {
            tcpControl->setPort(config_.port);
            return neo::OK;
        }
    }
    return neo::ERROR;
}


template <class T>
Vermell<T>& Vermell<T>::configure(const vermell::Config& config) noexcept {
    const uint16_t previous_port = config_.port;
    config_ = config;
    if (config_.port < static_cast<uint16_t>(neo::MIN_PORT))
        config_.port = previous_port;

    static constexpr size_t MAX_READ_CHUNK = 1UL << 20; // 1 MiB per recv() call
    static constexpr int    MAX_EVENTS     = 65536;
    static constexpr size_t MAX_THREADS    = 256;
    static constexpr long long MIN_TIMEOUT_MS = 1;
    static constexpr long long MAX_TIMEOUT_MS = std::numeric_limits<int>::max();

    if (config_.read_chunk == 0 || config_.read_chunk > MAX_READ_CHUNK)
        config_.read_chunk = MAX_READ_CHUNK;
    if (config_.max_events <= 0 || config_.max_events > MAX_EVENTS)
        config_.max_events = MAX_EVENTS;
    if (config_.threads > MAX_THREADS)
        config_.threads = MAX_THREADS;
    if (config_.accept_threads > MAX_THREADS)
        config_.accept_threads = MAX_THREADS;
    if (config_.backlog <= 0 || config_.backlog > MAX_SESSIONS)
        config_.backlog = MAX_SESSIONS;

    const auto clamp_ms = [](const std::chrono::milliseconds t) {
        const long long ms = t.count();
        return std::chrono::milliseconds(ms < MIN_TIMEOUT_MS ? MIN_TIMEOUT_MS
                                    : ms > MAX_TIMEOUT_MS ? MAX_TIMEOUT_MS : ms);
    };
    config_.read_timeout    = clamp_ms(config_.read_timeout);
    config_.write_timeout   = clamp_ms(config_.write_timeout);
    config_.request_timeout = clamp_ms(config_.request_timeout);
    config_.epoll_timeout   = clamp_ms(config_.epoll_timeout);

    applyNetworkConfig();
    if (router_epoll != nullptr)
        router_epoll->applyConfig(config_);
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setReadTimeout(const std::chrono::milliseconds timeout) noexcept {

    // poll() takes an int: clamp so a 0/negative value cannot mean
    // "wait forever" (slow-client DoS) and a huge one cannot overflow.

    const long long ms = timeout.count();
    config_.read_timeout = std::chrono::milliseconds(
        ms < 1 ? 1 : (ms > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : ms));
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setWriteTimeout(const std::chrono::milliseconds timeout) noexcept {
    const long long ms = timeout.count();
    config_.write_timeout = std::chrono::milliseconds(
        ms < 1 ? 1 : (ms > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : ms));
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setRequestTimeout(const std::chrono::milliseconds timeout) noexcept {
    const long long ms = timeout.count();
    config_.request_timeout = std::chrono::milliseconds(
        ms < 1 ? 1 : (ms > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : ms));
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setMaxRequestSize(const size_t bytes) noexcept {
    config_.max_request_size = bytes;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setReadChunkSize(const size_t bytes) noexcept {
    // 1..1 MiB: a larger per-recv() buffer is a per-connection memory bomb.
    config_.read_chunk = (bytes == 0 || bytes > (1UL << 20)) ? (1UL << 20) : bytes;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setThreads(const size_t threads) noexcept {
    config_.threads = threads > 256 ? 256 : threads;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setAcceptThreads(const size_t accept_threads) noexcept {
    config_.accept_threads = accept_threads > 256 ? 256 : accept_threads;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setMaxEvents(const int max_events) noexcept {
    config_.max_events = (max_events <= 0 || max_events > 65536) ? 65536 : max_events;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setMaxQueueSize(const size_t max_queue_size) noexcept {
    config_.max_queue_size = max_queue_size;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setMaxConnections(const size_t max_connections) noexcept {
    config_.max_connections = max_connections;
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setBacklog(const int backlog) noexcept {
    config_.backlog = (backlog <= 0 || backlog > MAX_SESSIONS) ? MAX_SESSIONS : backlog;
    if (tcpControl != nullptr)
        tcpControl->setSessions(config_.backlog);
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setBufferSize(const int size) noexcept {
    config_.buffer_size = size;
    if (tcpControl != nullptr)
        tcpControl->setBuffer(size);
    return *this;
}

template <class T>
Vermell<T>& Vermell<T>::setReusePort(const bool reuse_port) noexcept {
    config_.reuse_port = reuse_port;
    if (tcpControl != nullptr)
        tcpControl->setReusePort(reuse_port);
    return *this;
}

template <class T>
void Vermell<T>::applyNetworkConfig() noexcept {
    if (tcpControl == nullptr)
        return;
    constexpr auto min_port = static_cast<uint16_t>(neo::MIN_PORT);
    constexpr auto default_port = static_cast<uint16_t>(neo::DEF_PORT);
    tcpControl->setBuffer(config_.buffer_size);
    tcpControl->setPort(config_.port >= min_port ? config_.port : default_port);
    tcpControl->setSessions(config_.backlog);
    tcpControl->setReusePort(config_.reuse_port);
}


template<class T>
void Vermell<T>::tcpInt() {

    tcpControl = make_shared<T>();
    applyNetworkConfig();

    routes = make_shared<RoutesMap>();
    router_epoll = make_shared<workers::RouterEpoll<T>>(tcpControl);
}
using Router  = Vermell<Server>;
using Convert = utility_t;


#endif // VERMELL_H