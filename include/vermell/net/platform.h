// Portable platform bundle: one EventLoopFactory + one TransportFactory. The core
// only talks to these two factories, so swapping backends needs no core change.
// default_platform() is declared here, defined by the backend; never include
// backend headers here (the SPI stays OS-agnostic).

#ifndef VERMELL_NET_PLATFORM_H
#define VERMELL_NET_PLATFORM_H

#include <memory>

#include "event_loop.h"
#include "file_ops.h"
#include "transport.h"

namespace vermell::net {

struct Platform {
    std::shared_ptr<EventLoopFactory> loops;
    std::shared_ptr<TransportFactory> transport;
    std::shared_ptr<FileOps> files;
};

// Backend linked into this build; the only correct backend is the one you link.
std::shared_ptr<Platform> default_platform();

} // namespace vermell::net

#endif // VERMELL_NET_PLATFORM_H
