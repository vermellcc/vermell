//
// Centralized server configuration. Every knob of the request/response
// pipeline is parametrized here; defaults preserve the legacy behavior.
//
//   router.configure({
//       .max_request_size = 64 * 1024 * 1024, // heavy uploads
//       .read_timeout      = std::chrono::seconds{30},
//       .threads           = 8,
//   });
//

#ifndef VERMELL_CONFIG_HPP
#define VERMELL_CONFIG_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

#include "util/enums.h"
#include "util/render_security.h"

namespace vermell {

    struct Config {
        // ---- network ----
        uint16_t port = 0; // listening port; 0 = default/keep current (DEF_PORT)
        int backlog = SOMAXCONN;                     // pending connections queue of listen()
        int buffer_size = enums::neo::eSize::BUFFER; // legacy socket buffer size

        // ---- request reading ----
        // Inactivity timeout between chunks: raise it for heavy uploads on
        // slow networks (e.g. a large image arriving in many TCP segments).
        std::chrono::milliseconds read_timeout{5000};
        // Hard wall-clock deadline for the whole request to arrive (headers +
        // body), regardless of how regularly the client trickles bytes. This
        // is the slowloris cure: a client may never exceed read_timeout
        // between chunks (1 byte every few seconds) yet must still finish the
        // request within request_timeout or the connection is dropped (408).
        std::chrono::milliseconds request_timeout{60000};
        // Inactivity timeout while writing the response back to the client.
        std::chrono::milliseconds write_timeout{5000};
        // Hard limit for a whole request (headers + body). Requests bigger
        // than this are rejected with 413 Payload Too Large.
        size_t max_request_size = 4UL * 1024UL * 1024UL;
        // Bytes read per recv() call.
        size_t read_chunk = 16UL * 1024UL;

        // ---- concurrency / epoll ----
        // Worker pool size. 0 = thread-per-core: each accept thread serves its
        // own requests inline (no queue). >0 = offload to a pool of N workers.
        size_t threads = 0;
        // Accept/event-loop threads. 1 = single loop (default, SO_REUSEPORT
        // off). 0 = auto (hardware_concurrency) or >1 runs that many loops,
        // each with its own SO_REUSEPORT listener for multi-core accept.
        size_t accept_threads = 1;
        int max_events = 1024;                       // epoll event batch size
        // Queued tasks before the dispatcher sheds load. 0 = auto:
        // max(1024, threads * 256), enough to absorb an epoll batch burst.
        size_t max_queue_size = 0;
        // Hard cap on simultaneously open client connections. Bounded by
        // default (1024) so a connection flood cannot exhaust memory (each
        // connection buffers up to max_request_size bytes while reading).
        // 0 = unlimited (legacy, NOT recommended).
        size_t max_connections = 1024;
        // Share the listening port with other same-UID processes via
        // SO_REUSEPORT. OFF by default: when enabled, any process running as
        // the same user can bind the same port and receive a share of the
        // incoming connections (traffic hijack). Enable it only when you
        // deliberately run several server instances side by side.
        bool reuse_port = false;
        std::chrono::milliseconds epoll_timeout{1000}; // listen loop wake-up period

        // ---- file rendering hardening (readFile / readFileX / compose / render) ----
        RenderSecurity render{};
    };

} // namespace vermell

#endif // VERMELL_CONFIG_HPP
