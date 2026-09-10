// Core request pump over the portable vermell::net SPI (no <sys/*> here).

#include "../../include/vermell/request/io.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace {

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

    std::shared_ptr<vermell::net::StreamOps> default_stream_ops() {
        if (const auto platform = vermell::net::default_platform();
            platform != nullptr && platform->transport != nullptr)
            return platform->transport->stream_ops();
        return nullptr;
    }

} // namespace

RequestIO::RequestIO(vermell::net::EventLoop& loop,
                     const shared_ptr<RoutesMap> &routes,
                     const int listener_fd,
                     vermell::net::TcpListener& listener,
                     const vermell::Config &config,
                     const std::vector<vermell::StaticMount>& static_mounts,
                     const bool allow_keep_alive,
                     const shared_ptr<threading::ThreadPool>& pool,
                     std::shared_ptr<vermell::net::StreamOps> stream_ops)
    : loop_(&loop),
      listener_(&listener),
      listener_fd_(listener_fd),
      stream_ops_(std::move(stream_ops)),
      routes(routes),
      config_(std::make_shared<const vermell::Config>(config)),
      static_mounts_(static_mounts),
      allow_keep_alive_(allow_keep_alive) {

    thread_pool_ = pool;

    if (stream_ops_ == nullptr)
        stream_ops_ = default_stream_ops();

    if (listener_fd_ >= 0)
        loop_->add(listener_fd_, vermell::net::Interest::Read);
}


void RequestIO::dispatch(const std::vector<vermell::net::Event>& evs) const {

    DrainCompletions();

    if (evs.empty()) {
        SweepStale();
        return;
    }

    for (const auto& ev : evs) {
        const int event_fd = ev.fd;

        if (event_fd == listener_fd_) {
            AcceptPending();
            continue;
        }

        // Unknown fds (wake fd, closed races) carry no state: ignore them.
        if (!has_pending(event_fd))
            continue;

        if (ev.error || ev.closed) {
            if (ev.readable)
                HandleReadable(event_fd);
            if (has_pending(event_fd))
                remove_and_close(event_fd);
            continue;
        }

        if (ev.readable)
            HandleReadable(event_fd);
    }

    SweepStale();
}


void RequestIO::remove_and_close(const int fd) const {
    if (loop_ != nullptr)
        loop_->remove(fd);
    if (stream_ops_ != nullptr)
        stream_ops_->close_fd(fd);
    drop_pending(fd);
    active_connections_.fetch_sub(1);
    handled_.fetch_add(1);
}


void RequestIO::send_best_effort(const int fd,
                                 const vermell::http::WireResponse& response) const {
    if (stream_ops_ == nullptr)
        return;
    std::string wire;
    wire.reserve(response.head.size() + response.body.size());
    wire.append(response.head);
    wire.append(response.body);
    // timeout 0: single attempt, never stall the loop.
    stream_ops_->send_all(fd, wire.data(), wire.size(), 0, nullptr);
}


void RequestIO::AcceptPending() const {

    if (listener_ == nullptr || loop_ == nullptr || stream_ops_ == nullptr)
        return;

    for (;;) {
        std::string err;
        const vermell::net::Accepted accepted = listener_->accept(&err);

        if (accepted.fd < 0) {
            if (!err.empty())
                terminal(VER_EPOLL_CERR, err.c_str());
            return;
        }

        const int client_file_descriptor = accepted.fd;

        const auto cfg = config_snapshot();
        if (cfg->max_connections != 0
            && active_connections_.load() >= cfg->max_connections) {
            stream_ops_->close_fd(client_file_descriptor);
            continue;
        }

        std::string nb_err;
        if (!stream_ops_->set_nonblocking(client_file_descriptor, &nb_err)) {
            stream_ops_->close_fd(client_file_descriptor);
            continue;
        }

        stream_ops_->set_tcp_nodelay(client_file_descriptor);

        try {
            loop_->add(client_file_descriptor, vermell::net::Interest::Read);
        } catch (const std::exception& e) {
            terminal(VER_EPOLL_CERR, e.what());
            stream_ops_->close_fd(client_file_descriptor);
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
        const long bytes = stream_ops_->recv_some(fd, buf, bufsz);
        if (bytes > 0) {
            st.buffer.append(buf, static_cast<size_t>(bytes));
            st.last_activity = std::chrono::steady_clock::now();
            if (st.buffer.size() > cfg->max_request_size) {
                Reject(fd, 413, "payload too large");
                return;
            }
            continue;
        }
        if (bytes == 0) {
            peer_closed = true;
            break;
        }
        if (bytes == -1)
            break;
        Reject(fd, 400, "malformed request");
        return;
    }

    if (st.buffer.empty()) {
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
                // serve_inline() already counted it: close without double-counting.
                if (loop_ != nullptr)
                    loop_->remove(fd);
                if (stream_ops_ != nullptr)
                    stream_ops_->close_fd(fd);
                drop_pending(fd);
                active_connections_.fetch_sub(1);
                return;
            }
            continue;
        }

        if (loop_ != nullptr)
            loop_->remove(fd);
        if (!st.buffer.empty())
            st.dispatching = true;
        else
            drop_pending(fd);
        DispatchTask(fd, std::move(raw));
        return;
    }

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
        if (loop_ != nullptr)
            loop_->remove(fd);
        send_best_effort(fd, error_response(408, "request timeout"));
        if (stream_ops_ != nullptr)
            stream_ops_->close_fd(fd);
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
    if (stream_ops_ != nullptr)
        stream_ops_->close_fd(fd);
    drop_pending(fd);
    active_connections_.fetch_sub(1);
    handled_.fetch_add(1);
}


void RequestIO::Reject(const int fd, const int code, const char* error) const {
    if (loop_ != nullptr)
        loop_->remove(fd);
    send_best_effort(fd, error_response(code, error));
    if (stream_ops_ != nullptr)
        stream_ops_->close_fd(fd);
    drop_pending(fd);
    active_connections_.fetch_sub(1);
    handled_.fetch_add(1);
}


void RequestIO::ServeRequest(const int fd, std::string raw) const {

    // The listening port is not observable from request handlers (Query is
    // built from the Message only), so the per-request Server keeps the default.
    Server base;
    base.setPort(DEFAULT_PORT);
    base.set_stream_ops(stream_ops_);
    base.setSocketId(fd);
    base.setWriteTimeout(config_snapshot()->write_timeout);
    base.setResponse(std::move(raw));

    const bool keep_alive = ExecuteRoute(base, routes);
    handled_.fetch_add(1);
    complete_connection(fd, keep_alive);
}


bool RequestIO::serve_inline(const int fd, std::string raw) const {

    // Same as ServeRequest above: the port is not handler-observable.
    Server base;
    base.setPort(DEFAULT_PORT);
    base.set_stream_ops(stream_ops_);
    base.setSocketId(fd);
    base.setWriteTimeout(config_snapshot()->write_timeout);
    base.setResponse(std::move(raw));

    const bool keep_alive = ExecuteRoute(base, routes);
    handled_.fetch_add(1);
    return keep_alive;
}


void RequestIO::DrainCompletions() const {
    std::vector<Completion> done;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        done.swap(completed_);
    }
    for (const Completion& c : done) {
        if (c.keep_alive)
            RearmConnection(c.fd);
        else {
            if (stream_ops_ != nullptr)
                stream_ops_->close_fd(c.fd);
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

    if (loop_ != nullptr) {
        try {
            loop_->add(fd, vermell::net::Interest::Read);
        } catch (const std::exception& e) {
            terminal(VER_EPOLL_CERR, e.what());
            remove_and_close(fd);
            return;
        }
    }

    if (!st.buffer.empty())
        HandleReadable(fd);
}


void RequestIO::complete_connection(const int fd, const bool keep_alive) const {
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_.push_back({fd, keep_alive});
    }
    // Wake the loop so the completion is picked up promptly.
    if (loop_ != nullptr)
        loop_->wake();
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
        if (stream_ops_ != nullptr)
            stream_ops_->close_fd(c.fd);
        drop_pending(c.fd);
        active_connections_.fetch_sub(1);
    }

    for (size_t i = 0; i < pending_.size(); ++i) {
        if (!pending_[i].has_value())
            continue;
        const int fd = static_cast<int>(i);
        if (loop_ != nullptr)
            loop_->remove(fd);
        if (stream_ops_ != nullptr)
            stream_ops_->close_fd(fd);
        active_connections_.fetch_sub(1);
    }
    pending_.clear();

    if (loop_ != nullptr)
        loop_->wake();
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
