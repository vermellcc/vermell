#ifndef MAIN_PROCESS_H
#define MAIN_PROCESS_H

#include <stdexcept>
#include <memory>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <limits>
#include <vector>
#include <thread>

#include "../util/enums.h"
#include "../util/parameter_proccess.h"
#include "../net/poller.h"

#include "../config.hpp"
#include "../routes.hpp"
#include "../util/nterminal.h"
#include "../util/static_files.h"
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
            const size_t accept_count = _listen_type == neo::WHILE
                                            ? resolve_accept_threads(config)
                                            : 1;
            // SO_REUSEPORT (and therefore one listener per accept loop) does
            // not exist on Windows: there, every loop shares the first
            // listener and the kernel serializes accept() for them.
            const bool multi_listener = vermell::net::Poller::reuse_port_supported();
            const bool reuse_port = multi_listener && (accept_count > 1 || config.reuse_port);

            const bool inline_mode = config.threads == 0;
            pool_ = inline_mode ? nullptr : make_shared<threading::ThreadPool>(config.threads, config.max_queue_size);
            configured_threads_ = inline_mode ? 0 : config.threads;

            const auto epoll_timeout_ms = std::clamp(config.epoll_timeout.count(),
                                                     std::chrono::milliseconds::rep{1},
                                                     static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max()));
            const int wait_timeout = static_cast<int>(epoll_timeout_ms);

            struct Loop {
                shared_ptr<T> listener;
                std::unique_ptr<vermell::net::Poller> poller;
                shared_ptr<vector<vermell::net::Poller::Event>> events;
                shared_ptr<RequestIO> io;
            };
            std::vector<Loop> loops;
            loops.reserve(accept_count);

            try {
                for (size_t i = 0; i < accept_count; ++i) {
                    Loop loop;

                    if (i == 0 || multi_listener) {
                        loop.listener = i == 0 ? connection : make_shared<T>();
                        loop.listener->setPort(connection->getPort());
                        loop.listener->setReusePort(reuse_port);
                        loop.listener->setSessions(config.backlog);

                        if (loop.listener->on() != VER_SOCKET_OK)
                            throw std::runtime_error(VER_MAIN_THREAD);

                        int listener_fd = loop.listener->getDescription();
                        if (listener_fd < 0) {
                            loop.listener->Close();
                            throw std::runtime_error(VER_MAIN_THREAD);
                        }
                        if (Server::setNonblocking(listener_fd) == VER_SOCKET_ERROR) {
                            loop.listener->Close();
                            throw std::runtime_error(VER_MAIN_THREAD);
                        }
                    } else {
                        // Windows: all accept loops watch the same listener.
                        loop.listener = loops[0].listener;
                    }

                    loop.poller = std::make_unique<vermell::net::Poller>();
                    if (!loop.poller->valid())
                        throw std::range_error(VER_EPOLL_RANGE);

                    const int max_events = config.max_events > 0 ? config.max_events : INIT_MAX_EVENTS;
                    loop.events = make_shared<vector<vermell::net::Poller::Event>>(static_cast<size_t>(max_events));

                    if (!loop.poller->add(loop.listener->getDescription(), vermell::net::Poller::IN))
                        throw std::range_error(VER_EPOLL_CTL);

                    loop.io = make_shared<RequestIO>(loop.events, _routes,
                                                     loop.listener->getDescription(),
                                                     *loop.poller, loop.listener, config,
                                                     static_mounts,
                                                     _listen_type == neo::WHILE, pool_);
                    requests_.push_back(loop.io);
                    loops.push_back(std::move(loop));
                }
            } catch (const std::exception& e) {
                for (auto& loop : loops) {
                    if (loop.io)
                        loop.io->shutdown();
                    loop.poller.reset();
                    loop.listener->Close();
                }
                requests_.clear();
                terminal(e.what());
                return;
            }

            const auto run_loop = [&](const Loop& loop, const bool unique) {
                if (unique) {
                    do {
                        const int notice = loop.poller->wait(loop.events->data(),
                                                             static_cast<int>(loop.events->size()),
                                                             wait_timeout);
                        if (notice == -1) {
                            if (ver_interrupted())
                                continue;
                            break;
                        }
                        loop.io->Dispatch(notice);
                    } while (loop.io->handled_connections() == 0);
                    return;
                }

                while (listen_status_.load() == neo::eStatus::START) {
                    const int notice = loop.poller->wait(loop.events->data(),
                                                         static_cast<int>(loop.events->size()),
                                                         wait_timeout);
                    if (notice == -1) {
                        if (ver_interrupted())
                            continue;
                        break;
                    }
                    loop.io->Dispatch(notice);
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
                loop.poller.reset();
                loop.listener->Close();
            }
            requests_.clear();
        }
    };
}

#endif //MAIN_PROCESS_H
