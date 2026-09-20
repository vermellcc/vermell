#ifndef IO_H
#define IO_H

#include <memory>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>

#include "../util/enums.h"
#include "../util/parameter_proccess.h"

#include "../config.hpp"
#include "../routes.hpp"
#include "request.hpp"
#include "../util/nterminal.h"
#include "../util/static_files.h"
#include "../sockets.h"
#include "../threading/thread_pool.h"

using std::make_shared, std::vector, std::unique_ptr;

class RequestIO {

    public:

    private:
    shared_ptr<std::vector<epoll_event>> events;
    shared_ptr<RoutesMap>  routes;
    unique_ptr<int> file_descriptor;
    unique_ptr<int> epoll_fd;
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

    mutable std::vector<std::optional<ConnState>> pending_;

    mutable std::vector<char> read_scratch_;

    mutable int notify_fd_ = -1;
    mutable std::mutex completed_mutex_;
    mutable std::vector<Completion> completed_;
    mutable std::atomic<uint64_t> pending_notify_{0};

    [[nodiscard]] bool has_pending(const int fd) const noexcept {
        return fd >= 0 && static_cast<size_t>(fd) < pending_.size()
            && pending_[static_cast<size_t>(fd)].has_value();
    }
    ConnState& pending_slot(const int fd) const {
        const size_t u = static_cast<size_t>(fd);
        if (u >= pending_.size())
            pending_.resize(u + 1);
        auto& slot = pending_[u];
        if (!slot.has_value())
            slot.emplace();
        return *slot;
    }
    void drop_pending(const int fd) const noexcept {
        if (fd >= 0 && static_cast<size_t>(fd) < pending_.size())
            pending_[static_cast<size_t>(fd)].reset();
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
    void rearm_wait(int fd) const;
    void complete_connection(int fd, bool keep_alive) const;

    public:

     RequestIO(const shared_ptr<vector<epoll_event>>&,
               const shared_ptr<RoutesMap> &,
               int &,
               int &,
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
