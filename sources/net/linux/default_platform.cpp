// Bundled Linux platform (epoll + Linux TCP); sole definition of default_platform().

#include "vermell/net/platform.h"

#include "vermell/net/linux/event_loop.h"
#include "vermell/net/linux/transport.h"

namespace vermell::net {

std::shared_ptr<FileOps> platform_file_ops();

std::shared_ptr<Platform> default_platform() {
    auto platform = std::make_shared<Platform>();
    platform->loops = std::make_shared<linux_backend::EpollEventLoopFactory>();
    platform->transport = std::make_shared<linux_backend::LinuxTransportFactory>();
    platform->files = platform_file_ops();
    return platform;
}

} // namespace vermell::net
