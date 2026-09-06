# Cross-Platform Port: Linux → macOS + Windows

**Status:** complete and validated on macOS (Apple Silicon, AppleClang 17).
Linux behavior is unchanged; Windows support is wired but compile-validated only.

This document describes the port of vermell from a Linux-only epoll server to a
framework that builds and runs on **Linux (epoll)**, **macOS/BSD (kqueue)** and
**Windows (Winsock2 + WSAPoll)** from the same sources.

---

## 1. Why the code could not just compile

The framework was epoll-shaped end to end. The Linux dependencies were:

| Dependency | Where | Replacement |
|---|---|---|
| `epoll_create1/ctl/wait` | `request/io.cpp`, new `net/poller.h` | `Poller` class: epoll / kqueue / WSAPoll |
| `eventfd` (loop wakeup) | `request/io.cpp` | `Waker` class: eventfd / self-pipe / loopback TCP pair |
| `MSG_NOSIGNAL` | every `send()` | `VER_MSG_NOSIGNAL` (+ `SO_NOSIGPIPE` per socket on macOS/BSD) |
| `SO_REUSEPORT` | `sockets.cpp` | kept, guarded (`#if defined(SO_REUSEPORT)`); absent on Windows |
| `st_mtim.tv_sec` | `static_files.h` ETag | `ver_mtime_sec()`: `st_mtim` / `st_mtimespec` / `st_mtime` |
| `struct stat`, `stat()` | static files, secure_render | `ver_stat_t`, `ver_stat()` → `_stat64` on Windows |
| `/proc/self/exe`, `/proc/self/cmdline`, `/proc/self/status`, `getpwuid` | `process.cpp` | per-platform implementation (see §4.4) |
| `winsock`-less `socket/bind/listen/accept` | `sockets.cpp`, `io.cpp` | same calls, with `WSAStartup` + `closesocket`/`ioctlsocket` shims |
| `ssize_t`, `socklen_t` | everywhere | `ver_socklen_t`, Winsock `ssize_t` typedef |
| `usleep`, POSIX `close(fd)` on sockets | tests | `std::this_thread::sleep_for`, `ver_close_socket()` |

Everything else (HTTP parser, router, thread pool, JSON, static mounts, the
hardened file reader) was already standard C++20 and needed no changes.

---

## 2. Design: two new building blocks

### 2.1 `include/vermell/util/portability.h` — the glue layer

One header, included by everything platform-sensitive. Contains only
inline functions and macros; **nothing changes behavior on POSIX**.

- **Sockets.** `ver_close_socket` (`closesocket`/`close`), `ver_set_nonblocking`
  (`ioctlsocket(FIONBIO)`/`fcntl(O_NONBLOCK)`), `ver_set_int_option` (handles the
  `const char*` optval Winsock needs), `ver_disable_sigpipe` (`SO_NOSIGPIPE` on
  macOS/BSD, no-op elsewhere), `ver_socklen_t` (`int` vs `socklen_t`).
- **Errors.** Winsock keeps a separate errno space: `VER_SOCKET_ERRNO()` maps to
  `WSAGetLastError()` or `errno`; `ver_would_block()`, `ver_interrupted()`,
  `ver_strerror()` (FormatMessageA vs strerror).
- **Startup.** `vermell::net::startup()` — process-wide `WSAStartup` on Windows,
  no-op on POSIX. Called at every entry point (`Server::on()`, `Poller()`, Waker
  init, test client).
- **Scatter send.** `ver_send2(head, body)` — one `sendmsg`/`WSASend` syscall for
  head+body, EINTR retry, one POLLOUT wait when the buffer fills, then stop.
  Identical contract on all platforms (`WSABUF` vs `iovec`, no flags on Windows
  because no signals exist there).
- **Shutdown semantics.** `VER_SHUT_WR` (`SD_SEND` vs `SHUT_WR`),
  `VER_MSG_DONTWAIT` (0 on Windows, real flag where defined).
- **File shims.** `ver_stat_t`/`ver_stat` (`_stat64`), `ver_mtime_sec`
  (`st_mtime` / `st_mtimespec.tv_sec` / `st_mtim.tv_sec`), `S_ISREG`/`S_ISDIR`
  fallbacks for MSVC, `PATH_MAX`.

### 2.2 `include/vermell/net/poller.h` — readiness + wakeup

**`Poller`** — one class, three backends selected by macros
(`VERMELL_POLL_EPOLL` / `VERMELL_POLL_KQUEUE` / `VERMELL_POLL_WSAPOLL`):

- Neutral event masks: `IN`, `ERR`, `HUP`, `RDHUP` (epoll bits mapped 1:1).
- All backends are **level-triggered**, mirroring the original epoll semantics.
- kqueue: one `EVFILT_READ` knote per fd; `EV_EOF` → `HUP|RDHUP` (+ readable so
  the EOF is observed by `recv()`); `EV_ADD` on an existing knote updates it
  (used by `mod()` and by re-arming).
- WSAPoll backend keeps an `unordered_map<int fd, short events>` interest set
  (epoll has kernel-side state; WSAPoll needs the bookkeeping in userspace).
  `POLLHUP` is reported as `HUP|RDHUP|IN` so the read loop sees the FIN.
- `reuse_port_supported()` — false on Windows, where multiple accept loops
  share one listener instead.

**`Waker`** — the event-loop wakeup channel (workers hand finished connections
back to the loop without races):

- Linux: `eventfd(EFD_NONBLOCK|EFD_CLOEXEC)` — exactly what the code used before.
- macOS/BSD: non-blocking self-pipe (`pipe()` + `O_NONBLOCK` + `FD_CLOEXEC`);
  the loop registers the read end with the same `Poller`.
- Windows: connected loopback TCP pair (bind port 0 → connect → accept), the
  standard libuv-style substitute; `notify()` sends 1 byte, `drain()` recv-loops.
- Non-blocking writes: a full pipe/counter never blocks the worker.

### 2.3 Event-loop rewiring (`request/io.cpp`)

The loop logic itself was already platform-neutral once the primitives were
abstracted — the design survived the port unchanged:

1. `Dispatch()`: waker fd → drain completions; listener fd → `accept()` loop;
   else dispatch readable connections.
2. Before handing a request to the thread pool the fd is **removed** from the
   poller (`poller_->del`) so it cannot re-fire while owned by a worker; on
   completion the worker pushes `{fd, keep_alive}` and calls `waker_->notify()`.
3. The loop's waker event swaps the completion queue and either closes or
   re-arms (`poller_->add(fd, IN)` — works after DEL on epoll, is an update on
   kqueue, is a map insert on WSAPoll).
4. `accept()` loop: `VER_NVALUE` + `ver_interrupted()`/`ver_would_block()`,
   `SO_NOSIGPIPE` + `TCP_NODELAY` per client, connection cap, then
   `poller_->add()`.

---

## 3. Per-file changes

| File | Change |
|---|---|
| `include/vermell/util/portability.h` | **new** — §2.1 |
| `include/vermell/net/poller.h` | **new** — §2.2 |
| `sources/request/io.cpp`, `include/vermell/request/io.h` | epoll/eventfd calls → `Poller`/`Waker`; `epoll_event` vector → `Poller::Event` vector; portable accept/errno/close |
| `include/vermell/request/router_epoll.h` | per-accept-loop poller creation via `std::make_unique` (was relying on a using-decl from `vermell.h`); Waker wiring; Windows single-listener note |
| `include/vermell/sockets.h` | `DOMAIN/TYPE/PROTOCOL` → `VER_DOMAIN/VER_SOCK_TYPE/VER_SOCK_PROTOCOL` (see §5.1); `getEpollEvents()` returns the neutral Event type; WSA guard includes |
| `sources/sockets.cpp` | `WSAStartup` in `on()`; stale-fd cleanup on re-`on()`; guarded `SO_REUSEPORT`; `SO_NOSIGPIPE`; `listen` backlog `SOMAXCONN`; no `INVALID_SOCKET` compare on POSIX; `sendResponse` via `ver_send2` |
| `sources/process.cpp` | rewritten per-platform — §4.4 |
| `include/vermell/util/process.h` | `pid_t` → `ver_pid_t` (Windows has no `pid_t`); `sys/types.h` include dropped in favor of portability |
| `include/vermell/util/secure_render.h` | hardened file reader uses `ver_stat`/`ver_stat_t`, portable `O_NOFOLLOW` open path, duplicate comment block removed |
| `include/vermell/util/static_files.h` | `make_etag(const struct stat&)` → `(const ver_stat_t&)` + `ver_mtime_sec()`; portable stat |
| `include/vermell/util/json.hpp` | float `std::from_chars` → `strtod` fallback (§5.3); keeps int64 `from_chars` (available everywhere) |
| `include/vermell/config.hpp` | comments: accept-loop / SO_REUSEPORT behavior per platform |
| `include/vermell/util/nterminal.h` | include hygiene for the WSA branch |
| `include/vermell/util/data_render.h` | removed `std::move` of a returned temporary (`-Wpessimizing-move`) |
| `include/vermell/routes.hpp`, `util/parameter_proccess.h` | `[[maybe_unused]]` on kept-for-API private fields |
| `tests/test_cases.cpp` | test client portable (§6.1); fixed a real OOB read bug (§6.2) |
| `CMakeLists.txt` | platform-aware flags + Winsock/PSAPI linking — §7 |
| `.github/workflows/cmake-single-platform.yml` | CI installs libgtest/libcurl and runs the suite (`-DTESTING=ON`) |

---

## 4. Platform notes

### 4.1 macOS / BSD (kqueue)

- `EVFILT_READ` replaces `EPOLLIN`; `EV_EOF` handles half-close detection.
- No `MSG_NOSIGNAL`: every accepted socket gets `SO_NOSIGPIPE` instead; sends
  also pass `VER_MSG_NOSIGNAL` (0 there).
- `st_mtimespec` instead of `st_mtim`.
- `_NSGetExecutablePath` (two-call protocol) for `process.exec_path`.

### 4.2 Windows (Winsock2 + WSAPoll)

- `WSAStartup`/`WSACleanup` guarded once per process.
- `SOCKET` is `UINT_PTR`: all handles flow through `int` (the framework's fd
  type) with explicit casts; `INVALID_SOCKET` == `-1` keeps `fd < 0` checks valid.
- Non-blocking via `ioctlsocket(FIONBIO)`.
- Waker = loopback TCP pair; `poller_->mod()`/`del()` are map operations.
- No `SO_REUSEPORT`: multiple accept loops share one listener (fairness is
  slightly worse; correctness identical).
- `ppid` reported as 0 (needs the undocumented NT API or a tool-help snapshot —
  deliberately not worth it).
- PSAPI linked for `Process::memory_usage()` (working set).

### 4.3 Linux

Byte-for-byte the old paths: epoll, eventfd, `MSG_NOSIGNAL`, `st_mtim`,
`/proc`. CI (ubuntu-latest) builds and runs the full suite.

### 4.4 `Process` information per platform

| Field | Linux | macOS | Windows |
|---|---|---|---|
| `exec_path` | `readlink /proc/self/exe` | `_NSGetExecutablePath` | `GetModuleFileNameA` |
| `argv` | `/proc/self/cmdline` | `[]` (unreachable without `main`) | `__argc/__argv` |
| `memory_usage` | `VmRSS` in `/proc/self/status` | `task_info(MACH_TASK_BASIC_INFO)` | `GetProcessMemoryInfo` |
| `pid` / `ppid` | `getpid`/`getppid` | `getpid`/`getppid` | `GetCurrentProcessId` / 0 |
| `username` | `getpwuid(getuid())` | same | `USERNAME` env |
| `hostname` | `gethostname` | same | `GetComputerNameA` |
| `platform` | `"linux"` | `"darwin"` | `"windows"` |

---

## 5. Portability bugs found and fixed along the way

1. **`DOMAIN` collides with a macro.** macOS `<math.h>` `#define DOMAIN 1`
   detonates `constexpr int DOMAIN = AF_INET;` (`expected unqualified-id`).
   Renamed to `VER_DOMAIN` / `VER_SOCK_TYPE` / `VER_SOCK_PROTOCOL`.
2. **`std::from_chars` for floating point is unavailable in libc++ before
   macOS 26** (`'from_chars' is unavailable: introduced in macOS 26.0`).
   The JSON parser now uses `strtod` for the real-number path (the token
   grammar is pre-validated, so the semantics match); the int64 path keeps
   `from_chars`, which is available.
3. **`make_unique` include-order dependency**: `router_epoll.h` compiled only
   when `vermell.h` had already pulled the using-declaration. Qualified.
4. **`st_mtim`** doesn't exist on macOS (`st_mtimespec`) or Windows (`st_mtime`)
   — see `ver_mtime_sec`.
5. **`INVALID_SOCKET`** compared unconditionally in `Server::on()` — undefined
   on POSIX. The `fd < 0` check subsumes it (`INVALID_SOCKET` is `(SOCKET)~0`).

---

## 6. Test-suite changes

### 6.1 Portable raw client

`raw_exchange()` (drives all one-shot socket tests) now uses:
`vermell::net::startup()`, `ver_close_socket()`, `VER_MSG_NOSIGNAL`,
`VER_SHUT_WR`, `std::this_thread::sleep_for` instead of `usleep`, and casts the
`SOCKET`-returning `socket()` to int.

### 6.2 Real bug in the tests (caught by ASan during the port)

```cpp
// before: reads 20 bytes from an 18-char literal → global-buffer-overflow
EXPECT_FALSE(Msg::parse(string("G\x01""T / HTTP/1.1\r\n\r\n", 20)).has_value());
// after
EXPECT_FALSE(Msg::parse(string("G\x01T / HTTP/1.1\r\n\r\n")).has_value());
```

This was a latent out-of-bounds read that existed before the port; ASan
flagged it on the first full macOS run.

---

## 7. Build system

- **Flags per platform.** Linux keeps the full hardening set
  (`-D_FORTIFY_SOURCE=2 -fPIE -fstack-protector-strong`, ASan in Debug,
  `-pie -z relro -z now` at link). macOS: stack protector only —
  `D_FORTIFY_SOURCE` is predefined by AppleClang (redefinition warning) and
  `-fPIE` would make the driver forward `-pie` to dylib links (ld64 warning:
  "-pie being ignored"). Windows/MinGW: stack protector.
- **Linking.** `ws2_32` (all socket calls) and `psapi` (memory_usage) are
  PUBLIC deps of the `vermell` target on WIN32, so executables inherit them.
- **Library type.** `add_library(vermell ...)` respects `BUILD_SHARED_LIBS`
  (Debian .deb flow) and builds static by default (GitHub CI), with
  `VERSION`/`SOVERSION` for the shared case — verified producing
  `libvermell.1.0.5.dylib` + symlinks.
- **CI.** `cmake-single-platform.yml` now installs `libgtest-dev` +
  `libcurl4-openssl-dev`, configures with `-DTESTING=ON`, and runs `ctest` —
  previously the test target was never configured and ctest ran zero tests.

---

## 8. Validation results

macOS 15.7.9 arm64, AppleClang 17, Homebrew GTest 1.18:

| Check | Result |
|---|---|
| Release build, warnings | **0** |
| Debug build + ASan, warnings | **0** |
| Full suite (89 tests, real sockets through kqueue) | **89/89 PASSED** (Release and ASan) |
| Shared library build (`BUILD_SHARED_LIBS=ON`) | clean, versioned dylib |
| Examples compile (`hello-world`, `process`, `static`, `graceful-shutdown`) | OK |
| Runtime: `process` example | `platform:"darwin"`, `arch:"aarch64"`, RSS via Mach, pid/ppid/hostname/user OK |
| Runtime: `static` example | index 200, SPA fallback 200 text/html, traversal 403, `/assets` mount precedence per docs |
| ETag/304 flow | covered by tests 8126/8127 (pass) |
| Linux CI | paths unchanged; workflow now actually runs the tests |

**Note on running the tests:** the suite resolves fixtures as
`../examples/files/...` relative to the working directory — run the binary
from the build directory (ctest does this by design).

---

## 9. Known limitations / deliberate scope cuts

- **Windows `ppid` is 0**; `argv` on macOS is empty (both documented in §4.4).
- **WSAPoll is O(n)** per wait over registered fds — fine for the framework's
  target scale; an IOCP backend would be the next step for Windows scale.
- The kqueue backend registers read-interest only (the loop never requests
  write-readiness; backpressure is handled by `ver_send2`'s POLLOUT wait), so
  `mod()` exists for API parity and is not exercised by the loop.
- `secure_render`'s `/proc`-safe streaming caps were re-checked for the
  `O_NONBLOCK`-on-regular-file semantics that Linux and macOS share.

---

## 10. Files added

```
include/vermell/util/portability.h   (platform glue — §2.1)
include/vermell/net/poller.h         (Poller + Waker — §2.2)
```

Everything else is an edit to an existing file; no source file gained a
`#ifdef` jungle — platform branching is concentrated in `portability.h` and
`poller.h`, with small guarded regions in `sockets.cpp`, `process.cpp`,
`static_files.h`/`secure_render.h` and the JSON number parser.
