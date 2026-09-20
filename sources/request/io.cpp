#include "../../include/vermell/request/io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>

namespace {

    void send_best_effort(const int fd, const vermell::http::WireResponse& response) {
        iovec iov[2];
        iov[0].iov_base = const_cast<char*>(response.head.data());
        iov[0].iov_len = response.head.size();
        iov[1].iov_base = const_cast<char*>(response.body.data());
        iov[1].iov_len = response.body.size();

        msghdr msg{};
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        const ssize_t written = ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        (void)written;
    }

    vermell::http::WireResponse error_response(const int code, const char* error) {
        vermell::http::Response response;
        response.status(code).type("application/json").set("Connection", "close");
        response.body(std::string(R"lit({"error":")lit") + error + R"lit("})lit");

        vermell::http::WireResponse out;
        out.head = response.head();
        out.body = response.take_body();
        return out;
    }

    bool keep_alive_requested(const vermell::http::Message& message) {
        const std::string_view conn = message.header("Connection");
        if (vermell::http::detail::iequals(conn, "close"))
            return false;
        if (vermell::http::detail::iequals(conn, "keep-alive"))
            return true;
        return !message.version.starts_with("HTTP/1.0");
    }

} // namespace

RequestIO::RequestIO(const shared_ptr<vector<epoll_event>> &evs,
                     const std::shared_ptr<RoutesMap> &routes,
                     int &listener_fd,
                     int &epfd,
                     const shared_ptr<Server> &con,
                     const vermell::Config &config,
                     const std::vector<vermell::StaticMount>& static_mounts,
                     const bool allow_keep_alive,
                     const shared_ptr<threading::ThreadPool>& pool)
    : events(evs),
      routes(routes),
      file_descriptor(std::make_unique<int>(listener_fd)),
      epoll_fd(std::make_unique<int>(epfd)),
      connection(con),
      config_(std::make_shared<const vermell::Config>(config)),
      static_mounts_(static_mounts),
      allow_keep_alive_(allow_keep_alive) {

    thread_pool_ = pool;

    notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = notify_fd_;
    epoll_ctl(*epoll_fd, EPOLL_CTL_ADD, notify_fd_, &ev);
}


void RequestIO::Dispatch(const int notice) const {

    if (notice <= 0) {
        SweepStale();
        return;
    }

    for (int i = 0; i < notice; i++) {

        const int event_fd = events->operator[](i).data.fd;
        const uint32_t event_mask = events->operator[](i).events;

        if (event_fd == notify_fd_) {
            DrainCompletions();
            continue;
        }

        if (event_fd == *file_descriptor) {
            AcceptPending();
            continue;
        }

        if (event_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            if (event_mask & EPOLLIN)
                HandleReadable(event_fd);
            if (has_pending(event_fd)) {
                close(event_fd);
                drop_pending(event_fd);
                active_connections_.fetch_sub(1);
                handled_.fetch_add(1);
            }
            continue;
        }

        if (event_mask & EPOLLIN)
            HandleReadable(event_fd);
    }

    SweepStale();
}


void RequestIO::AcceptPending() const {

    for (;;) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);

        const int client_file_descriptor = accept4(*file_descriptor,
                                                   reinterpret_cast<sockaddr *>(&client_addr),
                                                   &client_addr_len,
                                                   SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client_file_descriptor == VER_NVALUE) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                terminal(VER_EPOLL_CERR, strerror(errno));
            return;
        }

        const auto cfg = config_snapshot();
        if (cfg->max_connections != 0
            && active_connections_.load() >= cfg->max_connections) {
            close(client_file_descriptor);
            continue;
        }

        epoll_event client_event{};
        client_event.events = EPOLLIN;
        if (thread_pool_)
            client_event.events |= EPOLLONESHOT;
        client_event.data.fd = client_file_descriptor;

        if (epoll_ctl(*epoll_fd, EPOLL_CTL_ADD, client_file_descriptor, &client_event) == VER_NVALUE) {
            terminal(VER_EPOLL_CERR, strerror(errno));
            close(client_file_descriptor);
            continue;
        }

        active_connections_.fetch_add(1);

        const auto now = std::chrono::steady_clock::now();
        auto& st = pending_slot(client_file_descriptor);
        st = ConnState{};
        st.start = now;
        st.last_activity = now;
    }
}


void RequestIO::HandleReadable(const int fd) const {

    const auto cfg = config_snapshot();

    auto& st = pending_slot(fd);
    const auto now = std::chrono::steady_clock::now();
    if (st.start == std::chrono::steady_clock::time_point{})
        st.start = now;

    const size_t want = std::min<size_t>(cfg->read_chunk > 0 ? cfg->read_chunk : 16384,
                                         1UL << 20);
    if (read_scratch_.size() < want)
        read_scratch_.resize(want);
    char* const buf = read_scratch_.data();
    const size_t bufsz = read_scratch_.size();
    bool peer_closed = false;
    for (;;) {
        const ssize_t bytes = recv(fd, buf, bufsz, 0);
        if (bytes > 0) {
            st.buffer.append(buf, static_cast<size_t>(bytes));
            st.last_activity = std::chrono::steady_clock::now();
            if (st.buffer.size() > cfg->max_request_size) {
                Reject(fd, 413, "payload too large");
                return;
            }
            if (static_cast<size_t>(bytes) < bufsz)
                break;
            continue;
        }
        if (bytes == 0) {
            peer_closed = true;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        Reject(fd, 400, "malformed request");
        return;
    }

    if (st.buffer.empty()) {
        rearm_wait(fd);
        return;
    }

    const bool deadline_over =
        (cfg->request_timeout.count() > 0 && now - st.start > cfg->request_timeout) ||
        (cfg->read_timeout.count() > 0 && now - st.last_activity > cfg->read_timeout);

    while (!st.buffer.empty()) {
        size_t request_len = 0;

        if (st.head_known) {
            if (st.buffer.size() < st.expected) {
                if (peer_closed) {
                    Reject(fd, 400, "malformed request");
                    return;
                }
                if (deadline_over) {
                    Reject(fd, 408, "request timeout");
                    return;
                }
                rearm_wait(fd);
                return;
            }
            request_len = st.expected;
        } else {
            const auto inspection = vermell::http::Message::inspect(st.buffer);
            switch (inspection.framing) {
                case vermell::http::Message::Framing::Incomplete: {
                    if (inspection.expected > cfg->max_request_size) {
                        Reject(fd, 413, "payload too large");
                        return;
                    }
                    st.expected = inspection.expected;
                    st.head_known = inspection.expected > 0;
                    if (peer_closed) {
                        Reject(fd, 400, "malformed request");
                        return;
                    }
                    if (deadline_over) {
                        Reject(fd, 408, "request timeout");
                        return;
                    }
                    rearm_wait(fd);
                    return;
                }
                case vermell::http::Message::Framing::Complete:
                    request_len = inspection.expected;
                    break;
                case vermell::http::Message::Framing::BadRequest:
                    Reject(fd, 400, "malformed request");
                    return;
                case vermell::http::Message::Framing::TooManyHeaders:
                    Reject(fd, 431, "too many headers");
                    return;
                case vermell::http::Message::Framing::NotImplemented:
                    Reject(fd, 501, "transfer encoding not supported");
                    return;
            }
        }

        std::string raw;
        if (st.buffer.size() == request_len) {
            raw = std::move(st.buffer);
        } else {
            raw.assign(st.buffer.data(), request_len);
            st.buffer.erase(0, request_len);
        }
        st.expected = 0;
        st.head_known = false;

        if (!thread_pool_) {
            if (!serve_inline(fd, std::move(raw))) {
                close(fd);
                drop_pending(fd);
                active_connections_.fetch_sub(1);
                return;
            }
            continue;
        }

        if (!st.buffer.empty())
            st.dispatching = true;
        else
            drop_pending(fd);
        DispatchTask(fd, std::move(raw));
        return;
    }

    rearm_wait(fd);

    const auto finish = std::chrono::steady_clock::now();
    st.start = finish;
    st.last_activity = finish;
}


void RequestIO::SweepStale() const {

    const auto cfg = config_snapshot();
    if (cfg->request_timeout.count() <= 0 && cfg->read_timeout.count() <= 0)
        return;

    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < pending_.size(); ++i) {
        auto& slot = pending_[i];
        if (!slot.has_value())
            continue;
        const ConnState& st = *slot;
        if (st.dispatching)
            continue;
        const bool total_over = cfg->request_timeout.count() > 0
                                && now - st.start > cfg->request_timeout;
        const bool idle_over = cfg->read_timeout.count() > 0
                               && now - st.last_activity > cfg->read_timeout;
        if (!total_over && !idle_over)
            continue;

        const int fd = static_cast<int>(i);
        send_best_effort(fd, error_response(408, "request timeout"));
        close(fd);
        active_connections_.fetch_sub(1);
        handled_.fetch_add(1);
        drop_pending(fd);
    }
}


void RequestIO::DispatchTask(const int fd, std::string raw) const {

    bool accepted = false;
    try {
        accepted = thread_pool_->tryAddTask([this, fd, raw = std::move(raw)]() mutable {
            this->ServeRequest(fd, std::move(raw));
        });
    } catch (const std::exception &e) {
        terminal("THREAD POOL REJECTED TASK: ", e.what());
    }

    if (accepted)
        return;

    send_best_effort(fd, error_response(503, "server busy"));
    close(fd);
    drop_pending(fd);
    active_connections_.fetch_sub(1);
    handled_.fetch_add(1);
}


void RequestIO::Reject(const int fd, const int code, const char* error) const {
    send_best_effort(fd, error_response(code, error));
    close(fd);
    drop_pending(fd);
    active_connections_.fetch_sub(1);
    handled_.fetch_add(1);
}


void RequestIO::ServeRequest(const int fd, std::string raw) const {

    Server base;
    base.setPort(connection->getPort());
    base.setSocketId(fd);
    base.setWriteTimeout(config_snapshot()->write_timeout);
    base.setResponse(std::move(raw));

    const bool keep_alive = ExecuteRoute(base, routes);
    handled_.fetch_add(1);
    complete_connection(fd, keep_alive);
}


bool RequestIO::serve_inline(const int fd, std::string raw) const {

    Server base;
    base.setPort(connection->getPort());
    base.setSocketId(fd);
    base.setWriteTimeout(config_snapshot()->write_timeout);
    base.setResponse(std::move(raw));

    const bool keep_alive = ExecuteRoute(base, routes);
    handled_.fetch_add(1);
    return keep_alive;
}


void RequestIO::DrainCompletions() const {
    uint64_t counter;
    const ssize_t drained = ::read(notify_fd_, &counter, sizeof counter);
    (void)drained;

    pending_notify_.store(0, std::memory_order_release);

    std::vector<Completion> done;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        done.swap(completed_);
    }
    for (const Completion& c : done) {
        if (c.keep_alive)
            RearmConnection(c.fd);
        else {
            close(c.fd);
            drop_pending(c.fd);
            active_connections_.fetch_sub(1);
        }
    }
}


void RequestIO::RearmConnection(const int fd) const {
    auto& st = pending_slot(fd);
    const auto now = std::chrono::steady_clock::now();
    st.start = now;
    st.last_activity = now;
    st.expected = 0;
    st.head_known = false;
    st.dispatching = false;

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = fd;
    epoll_ctl(*epoll_fd, EPOLL_CTL_MOD, fd, &ev);

    if (!st.buffer.empty())
        HandleReadable(fd);
}


void RequestIO::rearm_wait(const int fd) const {
    if (!thread_pool_)
        return;

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = fd;
    epoll_ctl(*epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}


void RequestIO::complete_connection(const int fd, const bool keep_alive) const {
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_.push_back({fd, keep_alive});
    }
    if (pending_notify_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        const uint64_t one = 1;
        const ssize_t written = ::write(notify_fd_, &one, sizeof one);
        (void)written;
    }
}


void RequestIO::shutdown() const {
    if (thread_pool_)
        thread_pool_->kill();

    std::vector<Completion> done;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        done.swap(completed_);
    }
    for (const Completion& c : done) {
        close(c.fd);
        drop_pending(c.fd);
        active_connections_.fetch_sub(1);
    }

    for (size_t i = 0; i < pending_.size(); ++i) {
        if (!pending_[i].has_value())
            continue;
        const int fd = static_cast<int>(i);
        close(fd);
        active_connections_.fetch_sub(1);
    }
    pending_.clear();
}


bool RequestIO::ExecuteRoute(Server& instance, const shared_ptr<RoutesMap> &routes) const {
    bool head_only = false;
    bool keep_alive = false;
    bool parsed = false;
    bool responded = false;
    vermell::http::WireResponse send_target;

    try {
        const string socket_response = instance.getResponse();

        if (const auto message = vermell::http::Message::parse(socket_response)) {
            parsed = true;
            head_only = (message->method == "HEAD");
            keep_alive = keep_alive_requested(*message);

            const std::string_view path_view = message->path;
            const std::string_view method_view = message->method;
            const size_t key_len = path_view.size() + 1 + method_view.size();
            std::array<char, 64> key_buf;
            std::string key_heap;
            char* const key = key_len <= key_buf.size()
                                  ? key_buf.data()
                                  : (key_heap.resize(key_len), key_heap.data());
            std::memcpy(key, path_view.data(), path_view.size());
            key[path_view.size()] = '\x1f';
            std::memcpy(key + path_view.size() + 1, method_view.data(), method_view.size());

            if (const auto itr = routes->find(std::string_view(key, key_len)); itr != routes->end()) {
                bool guarded;
                {
                    std::lock_guard<std::mutex> lock(itr->second->route_mutex);
                    guarded = TimeGuard(itr);
                    if (guarded)
                        send_target = itr->second->guardRouteMsg != nullptr
                                          ? utility_t::guard_route(itr->second->time_key, *itr->second->guardRouteMsg)
                                          : utility_t::guard_route(itr->second->time_key);
                }

                if (guarded) {
                    responded = true;
                } else {
                    std::unique_ptr<string> guard_msg;
                    auto [data, time_key] = itr->second->middlewares.execute(*message, guard_msg, config_snapshot()->render);

                    if (time_key > VER_OK) {
                        std::lock_guard<std::mutex> lock(itr->second->route_mutex);
                        itr->second->time_key = time_key;
                        itr->second->time_point = std::chrono::system_clock::now();
                        itr->second->guardRouteMsg = std::move(guard_msg);
                    }
                    send_target = std::move(data);
                    responded = true;
                }
            }
            else if (!static_mounts_.empty()) {
                const vermell::StaticMount* best = nullptr;
                size_t best_len = 0;
                for (const auto& mount : static_mounts_) {
                    if (mount.covers(message->path) && mount.mount().size() > best_len) {
                        best = &mount;
                        best_len = mount.mount().size();
                    }
                }
                if (best != nullptr) {
                    if (auto served = best->serve(message->path, message->method,
                                                  message->header("If-None-Match"));
                        served.has_value()) {
                        send_target = std::move(*served);
                        responded = true;
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        terminal("REQUEST PROCESSING ERROR: ", e.what());
    }

    if (!responded) {
        vermell::http::Response response;
        response.status(parsed ? 404 : 400).type("application/json");
        response.body(parsed ? R"lit({"error":"this route is not defined"})lit"
                             : R"lit({"error":"malformed request"})lit");
        send_target.head = response.head();
        send_target.body = response.take_body();
    }

    if (head_only)
        send_target.body.clear();

    if (!allow_keep_alive_)
        keep_alive = false;
    if (send_target.head.find("Connection: close") != string::npos)
        keep_alive = false;
    if (!keep_alive) {
        const size_t pos = send_target.head.find("Connection: keep-alive");
        if (pos != string::npos)
            send_target.head.replace(pos + 12, 10, "close");
    }

    instance.sendResponse(send_target.head, send_target.body);
    return keep_alive;
}


bool RequestIO::TimeGuard(const RoutesMap::const_iterator &itr) {
    if (itr->second->time_key <= 0)
        return false;
    if (itr->second->time_point == std::chrono::time_point<std::chrono::system_clock>())
        return false;

    const auto now = std::chrono::system_clock::now();

    const std::chrono::duration<double> distance = now - itr->second->time_point;
    return distance.count() < itr->second->time_key;
}


void RequestIO::set_pool(const shared_ptr<threading::ThreadPool>& pool) {
    thread_pool_ = pool;
}


void RequestIO::ApplyConfig(const vermell::Config& config) {
    {
        std::unique_lock lock(config_mutex_);
        config_ = std::make_shared<const vermell::Config>(config);
    }
}
