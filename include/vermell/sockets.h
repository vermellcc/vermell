#ifndef VERMELL_SOCKETS_HPP
#define VERMELL_SOCKETS_HPP


#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <memory>
#include <stdexcept>


#include <memory>
#include <unistd.h>
#include <string>
#include <sstream>
#include <cstring>
#include <functional>

#include <iostream>
#include <vector>
#include <array>
#include <mutex>
#include <charconv>
#include <chrono>
#include <string_view>
#include <algorithm>
#include <limits>

#include "http/response.hpp"

using std::string;
using std::shared_ptr;
using std::make_shared;
using std::vector;
using std::function;

constexpr uint16_t DEFAULT_PORT = 0xBB8;

constexpr int DOMAIN = AF_INET;
constexpr int TYPE = SOCK_STREAM;
constexpr int PROTOCOL = 0;

constexpr int VER_SOCKET_ERROR = -0x1;
constexpr int VER_SOCKET_OK = 0x0;
[[maybe_unused]] constexpr int VER_SOCKET_CONFUSED = 0x1;

constexpr int DEF_BUFFER_SIZE = 0x400;
// Upper bounds for the user-tunable socket knobs: a huge buffer or backlog
// is a memory/DoS foot-gun, so out-of-range values are rejected.
constexpr int MAX_BUFFER_SIZE = 16 * 1024 * 1024;
constexpr int MAX_SESSIONS = 65535;
constexpr int UnCATCH_ERROR_CH = -0x42;

[[maybe_unused]] constexpr auto SOCK_ERR = "_ERROR";

class Engine {

   protected:

        std::mutex lock_guard;
        std::mutex response_guard;
        uint16_t PORT;
        // Plain int: the hot path (ServeRequest) assigns an accepted fd to a
        // fresh stack Server per request, so a shared_ptr<int> would cost a
        // heap allocation per request for one integer. -1 = no socket.
        int socket_id = -1;
        shared_ptr
                   <int>
                         state_receptor = nullptr,
                         address_len = make_shared<int>(static_cast<int>(sizeof(address))),
                         option_mame = make_shared<int>(0x1),
                         buffer_size = make_shared<int>(DEF_BUFFER_SIZE);
    public:

        struct sockaddr_in address{};
        explicit Engine(uint16_t);
        virtual ~Engine() = default;


    [[maybe_unused]] int
             setBuffer(int),
             setPort(uint16_t);

    [[nodiscard]] int getPort() const;

        virtual int on() = 0;
        virtual void getResponseProcessing() = 0;

        [[maybe_unused]] virtual int Close() = 0;
        [[nodiscard]] virtual string getResponse() const = 0;
};

struct SendData {
    int socket;
    std::string data;
};

class Server final : public Engine {
  private:

     shared_ptr<string> buffereOd_data;
     shared_ptr
               <int> static_sessions = make_shared<int>(10);
     int epoll_fd, notices;

     int write_timeout_ms = 5000;

     // SO_REUSEPORT is opt-in (Config::reuse_port): when off, no other
     // same-UID process can bind the same port and intercept traffic.
     bool reuse_port_ = false;

     std::vector<epoll_event> events;

  public:

     explicit Server(uint16_t const Port) : Engine(Port), epoll_fd(-1), notices(-1) {}
     Server() : Engine(DEFAULT_PORT), epoll_fd(-1), notices(-1){}

    void getResponseProcessing() override;
     int on() override;
     int Close() override;

     [[maybe_unused]] [[nodiscard]] inline int getDescription() const {
          return socket_id;
     }
     // Kept for API compatibility; allocating here is fine (never on the
     // request hot path, which reads getDescription()/sendResponse()).
     [[maybe_unused]] inline shared_ptr<int> getSocketId() { return std::make_shared<int>(socket_id); }
     [[maybe_unused]] inline void setSocketId(int const identity) { socket_id = identity; }

     void setSessions(int);
     // Inactivity timeout while writing the response to the client.
     inline void setWriteTimeout(const std::chrono::milliseconds timeout) noexcept {
          // poll() takes an int timeout: clamp so an absurd config cannot
          // overflow the conversion (negative = wait forever = slow-client DoS).
          write_timeout_ms = std::clamp(static_cast<long long>(timeout.count()),
                                        1LL,
                                        static_cast<long long>(std::numeric_limits<int>::max()));
     }
     inline void setReusePort(const bool enable) noexcept { reuse_port_ = enable; }
     void sendResponse(const string& head, const string& body) const;
     void setResponse(const std::array<char, DEF_BUFFER_SIZE> &buffer);
     void setResponse(const string &data);
     // Move overload: the request body is handed to the worker by value, so
     // this avoids copying the whole body into the response storage.
     void setResponse(string &&data);

     inline void setEpollEvents(std::vector<epoll_event> const &e){events = e;}
     inline void setEpollfd(int const arg) noexcept { epoll_fd = arg; }
     inline void setNotices(int const arg) noexcept { notices = arg;  }

     [[nodiscard]] inline std::vector<epoll_event> getEpollEvents() const {return events; }
     [[nodiscard]] inline int getEpollfd() const {return epoll_fd;}
     [[nodiscard]] inline int getNotices() const {return notices;}


     static int setNonblocking(const int&);

     [[nodiscard]] inline string getResponse()  const override {
            return buffereOd_data != nullptr ? *buffereOd_data : string{};
       }
};


#endif // !VERMELL_SOCKETS_HPP
