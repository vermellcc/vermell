// Historical name kept so existing code still compiles; drives any EventLoop + TransportFactory Platform.
#ifndef MAIN_PROCESS_H
#define MAIN_PROCESS_H

#include <stdexcept>
#include <memory>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <limits>
#include <vector>
#include <thread>

#include "../util/enums.h"
#include "../util/parameter_proccess.h"

#include "../config.hpp"
#include "../routes.hpp"
#include "../util/nterminal.h"
#include "../util/static_files.h"
#include "../net/platform.h"
#include "../threading/thread_pool.h"
#include "io.h"

namespace workers {

    using enums::neo;

    constexpr int BUFFER = neo::eSize::BUFFER;
    constexpr int SESSION = neo::eSize::SESSION;
    constexpr int INIT_MAX_EVENTS = 1024;
    constexpr int EPOLL_TIMEOUT_MS = 1000;

    inline size_t resolve_accept_threads(const vermell::Config& config) {
        size_t n = config.accept_threads;
        if (n == 0) {
            const unsigned int cores = std::thread::hardware_concurrency();
            n = cores == 0 ? 1 : cores;
        }
        return n < 1 ? 1 : n;
    }

    template<class T>
    class RouterEpoll {

        shared_ptr<T> connection;
        std::atomic<neo::eStatus> listen_status_;
        std::vector<std::thread> accept_threads_;
        std::vector<shared_ptr<RequestIO>> requests_;
        shared_ptr<threading::ThreadPool> pool_;
        size_t configured_threads_ = 0;

    public:

        explicit RouterEpoll(const shared_ptr<T> &conn)
            : connection(conn),
              listen_status_(enums::neo::eStatus::START) {}

        void setListenStatus(const neo::eStatus _status) {
            this->listen_status_.store(_status);
        }

        void applyConfig(const vermell::Config& config) {
            if (pool_ && config.threads > 0 && config.threads != configured_threads_) {
                pool_ = make_shared<threading::ThreadPool>(config.threads, config.max_queue_size);
                configured_threads_ = config.threads;
                for (const auto& request : requests_)
                    request->set_pool(pool_);
            }
            for (const auto& request : requests_)
                request->ApplyConfig(config);
        }

        void getMainProcess(const shared_ptr<RoutesMap> &_routes,
                            const neo::LISTEN_TYPE _listen_type = neo::WHILE,
                            const vermell::Config &config = {},
                            const std::vector<vermell::StaticMount>& static_mounts = {}) {
            const auto backend = vermell::net::default_platform();
            if (backend == nullptr || backend->loops == nullptr || backend->transport == nullptr) {
                terminal("NO PLATFORM BACKEND: link a backend library (e.g. vermell-windows)");
                return;
            }

            const size_t accept_count = _listen_type == neo::WHILE
                                            ? resolve_accept_threads(config)
                                            : 1;
            const bool reuse_port = accept_count > 1 || config.reuse_port;

            const bool inline_mode = config.threads == 0;
            pool_ = inline_mode ? nullptr : make_shared<threading::ThreadPool>(config.threads, config.max_queue_size);
            configured_threads_ = inline_mode ? 0 : config.threads;

            const auto loop_timeout_ms = std::clamp(config.loop_timeout().count(),
                                                     std::chrono::milliseconds::rep{1},
                                                     static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max()));
            const int wait_timeout = static_cast<int>(loop_timeout_ms);

            struct Loop {
                shared_ptr<T> listener;
                std::unique_ptr<vermell::net::EventLoop> loop;
                shared_ptr<RequestIO> io;
            };
            std::vector<Loop> loops;
            loops.reserve(accept_count);

            try {
                for (size_t i = 0; i < accept_count; ++i) {
                    Loop loop;
                    loop.listener = i == 0 ? connection : make_shared<T>();
                    // Binds the default backend to every accept-thread listener.
                    loop.listener->set_platform(backend);
                    loop.listener->setPort(connection->getPort());
                    loop.listener->setReusePort(reuse_port);
                    loop.listener->setSessions(config.backlog);

                    if (loop.listener->on() != VER_SOCKET_OK)
                        throw std::runtime_error(VER_MAIN_THREAD);

                    if (loop.listener->tcp_listener() == nullptr) {
                        loop.listener->Close();
                        throw std::runtime_error(VER_MAIN_THREAD);
                    }

                    const int listener_fd = loop.listener->getDescription();
                    if (listener_fd < 0) {
                        loop.listener->Close();
                        throw std::runtime_error(VER_MAIN_THREAD);
                    }
                    std::string nb_err;
                    if (!backend->transport->stream_ops()->set_nonblocking(listener_fd, &nb_err)) {
                        loop.listener->Close();
                        throw std::runtime_error(VER_MAIN_THREAD);
                    }

                    loop.loop = backend->loops->create();

                    loop.io = make_shared<RequestIO>(*loop.loop, _routes, listener_fd,
                                                     *loop.listener->tcp_listener(),
                                                     config, static_mounts,
                                                     _listen_type == neo::WHILE, pool_,
                                                     backend->transport->stream_ops());
                    requests_.push_back(loop.io);
                    loops.push_back(std::move(loop));
                }
            } catch (const std::exception& e) {
                for (auto& loop : loops) {
                    if (loop.io)
                        loop.io->shutdown();
                    if (loop.listener != nullptr && loop.loop != nullptr)
                        loop.loop->remove(loop.listener->getDescription());
                    if (loop.listener != nullptr)
                        loop.listener->Close();
                }
                requests_.clear();
                terminal(e.what());
                return;
            }

            const auto run_loop = [&](const Loop& loop, const bool unique) {
                if (unique) {
                    do {
                        loop.io->dispatch(loop.loop->wait(wait_timeout));
                    } while (loop.io->handled_connections() == 0);
                    return;
                }

                while (listen_status_.load() == neo::eStatus::START) {
                    loop.io->dispatch(loop.loop->wait(wait_timeout));
                }
            };

            for (size_t i = 0; i + 1 < accept_count; ++i)
                accept_threads_.emplace_back([&, i] { run_loop(loops[i], false); });

            run_loop(loops.back(), _listen_type == neo::UNIQUE);

            for (auto& thread : accept_threads_)
                if (thread.joinable())
                    thread.join();
            accept_threads_.clear();

            for (auto& loop : loops) {
                loop.io->shutdown();
                loop.loop->remove(loop.listener->getDescription());
                loop.listener->Close();
            }
            requests_.clear();
        }
    };
}

#endif //MAIN_PROCESS_H
