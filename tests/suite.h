#ifndef HEADERS_H
#define HEADERS_H

#include "../include/vermell/vermell.h"
#include "../include/vermell/util/sysprocess.h"
#include "utils/min.http.h"
#include <future>
#include <string>
#include <mutex>
#include <set>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <gtest/gtest.h>

using std::string, std::future;
#define ISOLATE(logic) std::future<void> isolate_method = std::async(std::launch::async, ([&]() { logic }));

inline uint16_t free_port() {
    static std::mutex m;
    static std::set<uint16_t> used;
    std::lock_guard<std::mutex> lock(m);
    for (;;) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return 0;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        uint16_t port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            sockaddr_in out{};
            socklen_t len = sizeof(out);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&out), &len) == 0)
                port = ntohs(out.sin_port);
        }
        ::close(fd);
        if (port != 0 && used.insert(port).second)
            return port;
    }
}

class TestSuite : public ::testing::Test {

  protected:

    static string expected_default;

     static void SetUpTestCase(){
        expected_default = "success";
     }
};

string TestSuite::expected_default;


#endif //HEADERS_H
