#ifndef NTERMINAL_H
#define NTERMINAL_H
#include <iostream>

constexpr auto VER_NVALUE = -1;
constexpr auto VER_OK = 0;

constexpr auto VER_EPOLL_RANGE =  "AN ERROR OCCURRED WHEN CREATION OF THE POLLER FILE DESCRIPTOR";
constexpr auto VER_EPOLL_CTL =    "AN ERROR OCCURRED WHEN REGISTERING A FILE DESCRIPTOR WITH THE POLLER";
constexpr auto VER_EPOLL_CERR =   "THE FOLLOWING ERROR WAS FOUND WHEN EXECUTING THE POLLER METHOD: ";

constexpr auto VER_SOCKET_FAIL =  "A PROBLEM OCCURRED WHEN READING THE SOCKET REQUEST, CHECK THE CONNECTION";
constexpr auto VER_SOCKET_CLOSE = "A PROBLEM OCCURRED WHEN TRYING TO CLOSE THE CONNECTION WITH THE SOCKET: normally it is due to an error in the previous code";

constexpr auto VER_MAIN_THREAD =  "AN ERROR OCCURRED IN THE MAIN PROCESSING THREAD";


template<class...P>
auto terminal(P const&... args) {
    ((std::cerr<<"[ "<<args<<"]"<<std::endl),...);
}




#endif //NTERMINAL_H
