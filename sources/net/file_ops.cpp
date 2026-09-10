// Reads Platform::files from the linked backend's default_platform().

#include "vermell/net/file_ops.h"

#include "vermell/net/platform.h"

namespace vermell::net {

std::shared_ptr<FileOps> default_file_ops() {
    const auto platform = default_platform();
    return platform != nullptr ? platform->files : nullptr;
}

} // namespace vermell::net