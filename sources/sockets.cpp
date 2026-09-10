// Core socket facade over the injected vermell::net transport; no <sys/*> here.

#include "../include/vermell/sockets.h"

#include <cerrno>
#include <string>

#include "../include/vermell/config.hpp"

Engine::Engine(const uint16_t port) : PORT(port) {}

Server::Server(const uint16_t Port) : Engine(Port) {
}

Server::Server() : Engine(DEFAULT_PORT) {
}

Server::Server(const uint16_t Port, std::shared_ptr<vermell::net::Platform> platform)
     : Engine(Port) {
     set_platform(std::move(platform));
}

void Server::set_platform(std::shared_ptr<vermell::net::Platform> platform) {
     if (platform != nullptr && platform->transport != nullptr) {
          listener_ = platform->transport->create_listener();
          stream_ops_ = platform->transport->stream_ops();
     } else {
          listener_.reset();
          stream_ops_.reset();
     }
}

void Server::ensure_transport() const {
     if (listener_ != nullptr && stream_ops_ != nullptr)
          return;
     try {
          if (auto platform = vermell::net::default_platform();
              platform != nullptr && platform->transport != nullptr) {
               listener_ = platform->transport->create_listener();
               stream_ops_ = platform->transport->stream_ops();
          }
     } catch (...) {
          listener_.reset();
          stream_ops_.reset();
     }
}

vermell::net::TcpListener* Server::tcp_listener() noexcept {
     return listener_.get();
}

int Server::Close() {
     try {

          if (socket_id < 0)
               return VER_SOCKET_OK; // nothing to close

          const int fd = socket_id;
          socket_id = -1; // invalidate first: a second Close() can never double-close

          ensure_transport();
          if (listener_ != nullptr && listener_->fd() == fd) {
               listener_->close();
               return VER_SOCKET_OK;
          }
          if (stream_ops_ != nullptr) {
               stream_ops_->close_fd(fd);
               return VER_SOCKET_OK;
          }
          return VER_SOCKET_ERROR;
     }
     catch (const std::exception &e) {
          std::cerr << e.what() << '\n';
          return VER_SOCKET_ERROR;
     }
}

int Engine::setPort(const uint16_t xPort) {
     try {
          if(xPort == 0) throw std::range_error("Failed to set port");
          PORT = xPort;
          return VER_SOCKET_OK;
     }
     catch (const std::exception &e) {
          std::cerr << e.what() << '\n';
          return VER_SOCKET_ERROR;
     }
}

int Engine::getPort() const {
     try {
          if (PORT > 0)  {
               return PORT;
          }
          else {
               throw std::range_error("Failed to get port");
          }
     }
     catch (const std::exception &e) {
          std::cerr << e.what() << '\n';
          return VER_SOCKET_ERROR;
     }
}



int Engine::setBuffer(int size) {
     try {
          if (size <= 0 || size > MAX_BUFFER_SIZE)
               throw std::range_error("failed to set buffer_size");
          buffer_size = std::make_shared<int>(size);
          if(*buffer_size != size) throw std::range_error("failed to set buffer_size");
          return VER_SOCKET_OK;
     }
     catch (const std::exception &e) {
          std::cerr << e.what() << '\n';
          return VER_SOCKET_ERROR;
     }
}



void Server::setSessions(int max) {
     try {
          if (max <= 0 || max > MAX_SESSIONS)
               throw std::range_error("Failed to set sessions");
          static_sessions = std::make_shared<int>(max);
          if(*static_sessions != max) throw std::range_error("Failed to set sessions");
     }
     catch (const std::exception &e) {
          std::cerr << e.what() << '\n';
     }
}

int Server::setNonblocking(const int& fd) {
     // Backend-agnostic: some fds (per-backend id namespaces, e.g. Windows)
     // are only valid through the owning StreamOps, so this may fail there.
     try {
          const auto platform = vermell::net::default_platform();
          const auto ops = (platform != nullptr && platform->transport != nullptr)
               ? platform->transport->stream_ops() : nullptr;
          if (ops == nullptr)
               return VER_SOCKET_ERROR;
          std::string err;
          return ops->set_nonblocking(fd, &err) ? VER_SOCKET_OK : VER_SOCKET_ERROR;
     }
     catch (...) {
          return VER_SOCKET_ERROR;
     }
}


int Server::on() {
     try {
          ensure_transport();
          if (listener_ == nullptr || stream_ops_ == nullptr)
               throw std::range_error("Failed to create socket: no platform transport");

           // Re-used Server: drop any stale descriptor so on() never leaks it.
           if (socket_id >= 0) {
               if (listener_->fd() == socket_id)
                    listener_->close();
               else
                    stream_ops_->close_fd(socket_id);
               socket_id = -1;
          }

          const int backlog = (static_sessions != nullptr && *static_sessions > 0)
                                  ? *static_sessions
                                  : vermell::kDefaultBacklog;
          std::string err;
          if (!listener_->bind_listen(PORT, backlog, reuse_port_, &err))
               throw std::range_error(std::string("Failed to bind socket") + (err.empty() ? "" : ": " + err));
          socket_id = listener_->fd();

          return VER_SOCKET_OK;
     }
     catch (const std::exception &e) {
          // Never leave a half-open listening socket behind on failure.
          if (listener_ != nullptr && socket_id >= 0 && listener_->fd() == socket_id) {
               listener_->close();
               socket_id = -1;
          } else if (socket_id >= 0) {
               if (stream_ops_ != nullptr)
                    stream_ops_->close_fd(socket_id);
               socket_id = -1;
          }
          std::cerr << e.what() << '\n';
          return VER_SOCKET_ERROR;
     }
}

void Server::getResponseProcessing() {
    try {
        if (socket_id < 0 || buffer_size == nullptr || *buffer_size <= 0)
            throw std::range_error("response is empty");
        if (stream_ops_ == nullptr) {
            ensure_transport();
            if (stream_ops_ == nullptr)
                throw std::range_error("response is empty");
        }

        string base;
        vector<char> buffer;
        buffer.resize(static_cast<size_t>(*buffer_size));

        const long total_bytes = stream_ops_->recv_some(socket_id, buffer.data(), buffer.size());
        if (total_bytes <= 0)
            throw std::range_error("response is empty");

        // Strict '<': reading buffer[total_bytes] would touch one element
        // past the payload (and run off the allocation when the read filled
        // the whole buffer).
        for (long it = 0; it < total_bytes; it++) {
            if (buffer[static_cast<size_t>(it)] == 0)
                break;
            if(static_cast<int>(buffer[static_cast<size_t>(it)]) == UnCATCH_ERROR_CH)
                continue;
            if (buffer[static_cast<size_t>(it)] == 10)
                continue;
            base += buffer[static_cast<size_t>(it)];
        }
        if(base.empty()) throw std::range_error("response is empty");
        buffereOd_data = make_shared<string>(base);
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
}

void Server::setResponse(const std::array<char,DEF_BUFFER_SIZE> &buffer) {
     if( std::string raw(buffer.data(), buffer.size()) ; !raw.empty()) {
          buffereOd_data = make_shared<string>(raw);
     }
}

void Server::setResponse(const string &data) {
     if (!data.empty()) {
          buffereOd_data = make_shared<string>(data);
     }
}

void Server::setResponse(string &&data) {
     if (!data.empty()) {
          buffereOd_data = make_shared<string>(std::move(data));
     }
}

void Server::sendResponse(const string& head, const string& body) const {
     if (socket_id < 0)
          return;
     if (stream_ops_ == nullptr) {
          ensure_transport();
          if (stream_ops_ == nullptr)
               return;
     }

     // Single-buffer write: join once instead of two partial writes.
     string wire;
     wire.reserve(head.size() + body.size());
     wire.append(head);
     wire.append(body);
     stream_ops_->send_all(socket_id, wire.data(), wire.size(), write_timeout_ms, nullptr);
}
