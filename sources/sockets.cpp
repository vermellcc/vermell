#include <memory>
#include <poll.h>
#include <cerrno>
#include <sys/uio.h>

#include "../include/vermell/sockets.h"

Engine::Engine(const uint16_t port) : PORT(port) {}

int Server::Close() {
     try {

          if (socket_id < 0)
               return VER_SOCKET_OK; // nothing to close

          const int fd = socket_id;
          socket_id = -1; // invalidate first: a second Close() can never double-close

          if (fd >= 0 && close(fd) < 0) {
               throw std::range_error("Failed to close socket");
          }
          return VER_SOCKET_OK;
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

int Server::setNonblocking(const int& socket_id) {
        int flags = fcntl(socket_id, F_GETFL, 0);
        if (flags == -1){
            return VER_SOCKET_ERROR;
        }
        if (fcntl(socket_id, F_SETFL, flags | O_NONBLOCK) < 0){
            return VER_SOCKET_ERROR;
        }
        return VER_SOCKET_OK;
}


int Server::on() {
     try {

         // A Server can be re-used: drop any stale descriptor first so a
         // second on() never leaks the previous listening socket.
         if (socket_id >= 0) {
              close(socket_id);
              socket_id = -1;
         }

         const int fd = socket(DOMAIN, TYPE, PROTOCOL);
         if (fd < 0) {
             throw std::range_error("Failed to create domain socket");
         }
         socket_id = fd;

         if (setsockopt(socket_id,
                        SOL_SOCKET,
                        SO_REUSEADDR,
                        &*option_mame,
                        sizeof(*option_mame)) != 0x0) {
             throw std::range_error("Failed to set socket options");
         }

         // SO_REUSEPORT is strictly opt-in (Config::reuse_port): with it on,
         // any same-UID process may bind this port and intercept a share of
         // the traffic. Off by default.
         if (reuse_port_
             && setsockopt(socket_id,
                           SOL_SOCKET,
                           SO_REUSEPORT,
                           &*option_mame,
                           sizeof(*option_mame)) != 0x0) {
             throw std::range_error("Failed to set socket options");
         }

         if(setNonblocking(socket_id) == VER_SOCKET_ERROR)
             throw std::runtime_error("Failed to set nonblocking");

         address.sin_family = AF_INET;
         address.sin_addr.s_addr = INADDR_ANY;
         address.sin_port = htons(PORT);

         if (bind(socket_id, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
               throw std::range_error("Failed to bind socket");
          }
          const int backlog = (static_sessions != nullptr && *static_sessions > 0)
                                  ? *static_sessions
                                  : SOMAXCONN;
          if (listen(socket_id, backlog) < 0x0) {
               throw std::range_error("Failed to listen on socket");
           }

          return VER_SOCKET_OK;
     }
     catch (const std::exception &e) {
          // Never leave a half-open listening socket behind on failure.
          if (socket_id >= 0) {
               close(socket_id);
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

        string base;
        vector<char> buffer;
        buffer.resize(static_cast<size_t>(*buffer_size));

        const ssize_t total_bytes = read(socket_id, buffer.data(), buffer.size());
        if (total_bytes <= 0)
            throw std::range_error("response is empty");

        // Strict '<': reading buffer[total_bytes] would touch one element
        // past the payload (and run off the allocation when the read filled
        // the whole buffer).
        for (ssize_t it = 0; it < total_bytes; it++) {
            if (buffer[it] == 0)
                break;
            if(static_cast<int>(buffer[it]) == UnCATCH_ERROR_CH)
                continue;
            if (buffer[it] == 10)
                continue;
            base += buffer[it];
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

     const int fd = socket_id;
     const char* bufs[2] = { head.data(), body.data() };
     size_t lens[2] = { head.size(), body.size() };
     size_t done = 0;
     const size_t total = lens[0] + lens[1];

     while (done < total) {
          iovec iov[2];
          int count = 0;
          for (int i = 0; i < 2; ++i) {
               if (lens[i] == 0)
                    continue;
               iov[count].iov_base = const_cast<char*>(bufs[i]);
               iov[count].iov_len = lens[i];
               ++count;
          }

          msghdr msg{};
          msg.msg_iov = iov;
          msg.msg_iovlen = static_cast<size_t>(count);

          const ssize_t bytes_send = sendmsg(fd, &msg, MSG_NOSIGNAL);
          if (bytes_send > 0) {
               done += static_cast<size_t>(bytes_send);
               size_t consumed = static_cast<size_t>(bytes_send);
               for (int i = 0; i < 2 && consumed > 0; ++i) {
                    const size_t take = std::min(consumed, lens[i]);
                    lens[i] -= take;
                    bufs[i] += take;
                    consumed -= take;
               }
               continue;
          }

          if (bytes_send == -1 && errno == EINTR)
               continue;

          if (bytes_send == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
               pollfd pfd{};
               pfd.fd = fd;
               pfd.events = POLLOUT;

               if (poll(&pfd, 1, write_timeout_ms) > 0 && (pfd.revents & POLLOUT))
                    continue;
          }

          break;
     }
}

