//
// Platform glue: lets the same sources build on Linux, macOS and Windows.
//
//  - Sockets:    BSD sockets everywhere; Winsock2 on Windows (with a
//                process-wide WSAStartup guard).
//  - Readiness:  epoll (Linux) / kqueue (macOS, BSD) / WSAPoll (Windows) —
//                wrapped by ../net/poller.h, not here.
//  - Wakeup:     eventfd (Linux) / self-pipe (macOS) / loopback pair
//                (Windows) — wrapped by the Waker class in ../net/poller.h.
//  - Scatter send: sendmsg (POSIX) / WSASend (Windows), with the poll-
//                while-full retry the blocking sendResponse() path needs.
//  - CRT/file shims: stat/open/read for the hardened file readers.
//
// Nothing here changes behavior on POSIX; the Windows branches only map
// the same calls onto Winsock/CRT equivalents.
//

#ifndef VERMELL_PORTABILITY_H
#define VERMELL_PORTABILITY_H

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <ws2ipdef.h>
    #include <windows.h>
    #include <basetsd.h>
    #include <sys/stat.h> // _stat64 for the hardened file readers

    // ssize_t does not exist in the MSVC CRT; MinGW-w64 provides it.
    #if !defined(_SSIZE_T_DEFINED) && !defined(SSIZE_MAX)
        typedef SSIZE_T ssize_t;
        #define _SSIZE_T_DEFINED
    #endif

    // No SIGPIPE on Windows: sends on a closed socket return WSAECONNRESET
    // instead of raising a signal, so MSG_NOSIGNAL is a no-op.
    #define VER_MSG_NOSIGNAL 0
    #define VER_MSG_DONTWAIT 0

    // shutdown() half-close of the send side (SHUT_WR on POSIX).
    #define VER_SHUT_WR SD_SEND

    // Windows headers never define INADDR_LOOPBACK (the Waker self-pair
    // uses it); the value is the well-known 127.0.0.1.
    #ifndef INADDR_LOOPBACK
        #define INADDR_LOOPBACK 0x7f000001u
    #endif

    #ifndef PATH_MAX
        #define PATH_MAX 4096
    #endif

#else // POSIX (Linux, macOS, the BSDs)

    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/uio.h>
    #include <unistd.h>

    // macOS/BSD have no MSG_NOSIGNAL; SO_NOSIGPIPE (set per socket by
    // ver_disable_sigpipe) plays the same role.
    #if defined(MSG_NOSIGNAL)
        #define VER_MSG_NOSIGNAL MSG_NOSIGNAL
    #else
        #define VER_MSG_NOSIGNAL 0
    #endif
    #if defined(MSG_DONTWAIT)
        #define VER_MSG_DONTWAIT MSG_DONTWAIT
    #else
        #define VER_MSG_DONTWAIT 0
    #endif

    #define VER_SHUT_WR SHUT_WR

    #ifndef PATH_MAX
        #define PATH_MAX 4096
    #endif

#endif // platform

// Error-code helpers: Winsock keeps its own errno space (WSAGetLastError),
// POSIX uses errno for sockets too.

#if defined(_WIN32)
    #define VER_SOCKET_ERRNO() ::WSAGetLastError()
    inline bool ver_would_block() noexcept {
        const int err = ::WSAGetLastError();
        return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
    }
    inline bool ver_interrupted() noexcept {
        return ::WSAGetLastError() == WSAEINTR;
    }
#else
    #define VER_SOCKET_ERRNO() errno
    inline bool ver_would_block() noexcept {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    inline bool ver_interrupted() noexcept {
        return errno == EINTR;
    }
#endif

inline std::string ver_strerror(const int err) {
#if defined(_WIN32)
    char buf[256] = {};
    const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(err),
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     buf, static_cast<DWORD>(sizeof(buf)), nullptr);
    if (n > 0) {
        std::string out(buf, n);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    }
    return "socket error " + std::to_string(err);
#else
    return std::string(std::strerror(err));
#endif
}

// ---- fd/socket plumbing ----------------------------------------------------

#if defined(_WIN32)
    using ver_socklen_t = int;
#else
    using ver_socklen_t = socklen_t;
#endif

inline int ver_close_socket(const int fd) noexcept {
#if defined(_WIN32)
    return ::closesocket(static_cast<SOCKET>(fd));
#else
    return ::close(fd);
#endif
}

inline int ver_set_nonblocking(const int fd) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0 ? 0 : -1;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

// setsockopt with an int value; the optval type differs (char* on Windows).
inline int ver_set_int_option(const int fd, const int level, const int optname,
                              const int value) noexcept {
#if defined(_WIN32)
    const char* raw = reinterpret_cast<const char*>(&value);
#else
    const void* raw = &value;
#endif
    return ::setsockopt(fd, level, optname, raw, sizeof(value));
}

// SO_NOSIGPIPE is the macOS/BSD equivalent of MSG_NOSIGNAL; harmless no-op
// elsewhere. Applied once per socket at accept/listen time.
inline void ver_disable_sigpipe(const int fd) noexcept {
#if defined(SO_NOSIGPIPE)
    (void)ver_set_int_option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#else
    (void)fd;
#endif
}

// One-time Winsock initialization. No-op on POSIX.
namespace vermell::net {
    inline void startup() noexcept {
#if defined(_WIN32)
        static const bool ready = [] {
            WSADATA data{};
            return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        (void)ready;
#endif
    }
} // namespace vermell::net

// ---- scatter send -----------------------------------------------------------

// Sends up to two buffers (response head + body) in one syscall when the
// platform allows it, retrying while data remains. flags: extra send flags
// (use VER_MSG_DONTWAIT for pure best-effort writes). When the socket buffer
// is full: wait_writable_ms < 0 gives up immediately, otherwise waits once
// for writability and retries. Returns the total bytes sent (>= 0), or -1
// when nothing at all could be sent.
inline long long ver_send2(const int fd,
                           const char* buf1, const size_t len1,
                           const char* buf2, const size_t len2,
                           const int flags = 0,
                           const int wait_writable_ms = -1) {
    const char* bufs[2] = {buf1, buf2};
    size_t lens[2] = {len1, len2};
    long long done = 0;

    for (;;) {
        long long sent = -1;
#if defined(_WIN32)
        (void)flags; // WSASend takes no MSG_* flags; no signals exist on Windows
        WSABUF iov[2];
        int count = 0;
        for (int i = 0; i < 2; ++i) {
            if (lens[i] == 0)
                continue;
            iov[count].buf = const_cast<CHAR*>(bufs[i]);
            iov[count].len = static_cast<ULONG>(lens[i]);
            ++count;
        }
        if (count == 0)
            return done;
        DWORD written = 0;
        if (::WSASend(static_cast<SOCKET>(fd), iov, count, &written,
                      0, nullptr, nullptr) == 0)
            sent = static_cast<long long>(written);
#else
        iovec iov[2];
        int count = 0;
        for (int i = 0; i < 2; ++i) {
            if (lens[i] == 0)
                continue;
            iov[count].iov_base = const_cast<char*>(bufs[i]);
            iov[count].iov_len = lens[i];
            ++count;
        }
        if (count == 0)
            return done;
        msghdr msg{};
        msg.msg_iov = iov;
        msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(count);
        sent = static_cast<long long>(
            ::sendmsg(fd, &msg, flags | VER_MSG_NOSIGNAL));
#endif

        if (sent > 0) {
            done += sent;
            size_t consumed = static_cast<size_t>(sent);
            for (int i = 0; i < 2 && consumed > 0; ++i) {
                const size_t take = lens[i] < consumed ? lens[i] : consumed;
                lens[i] -= take;
                bufs[i] += take;
                consumed -= take;
            }
            continue;
        }

        if (ver_interrupted())
            continue;
        if (ver_would_block()) {
            if (wait_writable_ms < 0)
                return done > 0 ? done : -1;
#if defined(_WIN32)
            pollfd pfd{};
            pfd.fd = static_cast<SOCKET>(fd);
#else
            pollfd pfd{};
            pfd.fd = fd;
#endif
            pfd.events = POLLOUT;
            const int ready = ::poll(&pfd, 1, wait_writable_ms);
            if (ready > 0 && (pfd.revents & POLLOUT))
                continue;
        }
        return done > 0 ? done : -1;
    }
}

// ---- file stat shims (static_files / secure_render) -------------------------

#if defined(_WIN32)
    using ver_pid_t = unsigned long; // GetCurrentProcessId returns DWORD
#else
    using ver_pid_t = pid_t;
#endif

#if defined(_WIN32)
    using ver_stat_t = struct _stat64;
    inline int ver_stat(const char* path, ver_stat_t* out) noexcept {
        return ::_stat64(path, out);
    }
    // Modification time (seconds since epoch); field layout differs per OS.
    inline long long ver_mtime_sec(const ver_stat_t& st) noexcept {
        return static_cast<long long>(st.st_mtime);
    }
#else
    using ver_stat_t = struct stat;
    inline int ver_stat(const char* path, ver_stat_t* out) noexcept {
        return ::stat(path, out);
    }
    inline long long ver_mtime_sec(const ver_stat_t& st) noexcept {
#if defined(__APPLE__)
        return static_cast<long long>(st.st_mtimespec.tv_sec);
#else
        return static_cast<long long>(st.st_mtim.tv_sec);
#endif
    }
    #if defined(S_IFMT) && !defined(S_ISREG)
        #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
        #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
    #endif
#endif

#if defined(_WIN32) && !defined(S_ISREG)
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#endif // VERMELL_PORTABILITY_H
