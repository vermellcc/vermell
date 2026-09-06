#ifndef IO_H
#define IO_H

#include <memory>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <chrono>

#include "../util/enums.h"
#include "../util/parameter_proccess.h"

#include "../config.hpp"
#include "../routes.hpp"
#include "../net/poller.h"
#include "request.hpp"
#include "../util/nterminal.h"
#include "../util/static_files.h"
#include "../sockets.h"
#include "../threading/thread_pool.h"

using std::make_shared, std::vector, std::unique_ptr;

class RequestIO {

    public:

    private:
    // One Event slot per returned readiness event, filled by the platform
    // poller (epoll / kqueue / WSAPoll) and consumed by Dispatch().
    shared_ptr<std::vector<vermell::net::Poller::Event>> events;
    shared_ptr<RoutesMap>  routes;
    unique_ptr<int> file_descriptor;
    // Non-owning: the event loop in RouterEpoll owns the Poller and outlives
    // this object.
    vermell::net::Poller* poller_ = nullptr;
    // Loop wakeup channel: eventfd (Linux), self-pipe (macOS/BSD) or a
    // loopback socket pair (Windows).
    unique_ptr<vermell::net::Waker> waker_;
    shared_ptr<Server> connection;

    mutable std::shared_mutex config_mutex_;
    std::shared_ptr<const vermell::Config> config_;

    [[nodiscard]] std::shared_ptr<const vermell::Config> config_snapshot() const {
        std::shared_lock lock(config_mutex_);
        return config_;
    }

    shared_ptr<threading::ThreadPool> thread_pool_;

    std::vector<vermell::StaticMount> static_mounts_;
    bool allow_keep_alive_ = true;

    mutable std::atomic<size_t> active_connections_{0};
    mutable std::atomic<size_t> handled_{0};

    struct ConnState {
        std::string buffer;
        std::chrono::steady_clock::time_point start{};
        std::chrono::steady_clock::time_point last_activity{};
        size_t expected = 0;
        bool head_known = false;
        bool dispatching = false;
    };

    struct Completion {
        int fd;
        bool keep_alive;
    };

    // Keyed by fd (a std::unordered_map on every platform: Windows socket
    // handles are never recycled and would make a fd-indexed vector grow
    // without bound on long-running servers).
    mutable std::unordered_map<int, ConnState> pending_;

    mutable std::vector<char> read_scratch_;

    mutable std::mutex completed_mutex_;
    mutable std::vector<Completion> completed_;

    [[nodiscard]] bool has_pending(const int fd) const noexcept {
        return pending_.find(fd) != pending_.end();
    }
    ConnState& pending_slot(const int fd) const {
        return pending_[fd];
    }
    void drop_pending(const int fd) const noexcept {
        pending_.erase(fd);
    }

    void AcceptPending() const;
    void HandleReadable(int fd) const;
    void SweepStale() const;
    void DispatchTask(int fd, std::string raw) const;
    void Reject(int fd, int code, const char* error) const;
    void ServeRequest(int fd, std::string raw) const;
    bool serve_inline(int fd, std::string raw) const;
    void DrainCompletions() const;
    void RearmConnection(int fd) const;
    void complete_connection(int fd, bool keep_alive) const;

    public:

     RequestIO(const shared_ptr<vector<vermell::net::Poller::Event>>&,
               const shared_ptr<RoutesMap> &,
               int,
               vermell::net::Poller &,
               const shared_ptr<Server>&,
               const vermell::Config &config = {},
               const std::vector<vermell::StaticMount>& static_mounts = {},
               bool allow_keep_alive = true,
               const shared_ptr<threading::ThreadPool>& pool = nullptr);

    void Dispatch(int notice) const;
    void ApplyConfig(const vermell::Config& config);
    void set_pool(const shared_ptr<threading::ThreadPool>& pool);
    void shutdown() const;

    [[nodiscard]] size_t handled_connections() const noexcept { return handled_.load(); }

    static bool TimeGuard(const RoutesMap::const_iterator & itr);
    bool ExecuteRoute(Server& instance, const shared_ptr<RoutesMap> &routes) const;
};

#endif //IO_H
