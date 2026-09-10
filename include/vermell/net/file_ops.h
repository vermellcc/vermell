// Backend-neutral file access (POSIX fds on Linux, HANDLEs on Windows).

#ifndef VERMELL_NET_FILE_OPS_H
#define VERMELL_NET_FILE_OPS_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace vermell::net {

    struct FileStat {
        bool ok = false;
        std::int64_t size = 0;
        std::int64_t mtime_sec = 0;
        bool is_dir = false;
        bool is_regular = false;
    };

    enum class FileReadErr { Ok, NotFound, Forbidden, TooLarge, IoError };

    struct FileRead {
        FileReadErr err = FileReadErr::Ok;
        std::string data{};
    };

class FileOps {
    public:
        virtual ~FileOps() = default;

        // Plain regular file only, capped at max_bytes.
        [[nodiscard]] virtual FileRead read_bounded(const std::string& path,
                                                    std::size_t max_bytes) = 0;

        // For ETag/conditional-GET and dir detection.
        [[nodiscard]] virtual FileStat stat(const std::string& path) = 0;
    };

    // FileOps of the linked backend (only one is live).
    std::shared_ptr<FileOps> default_file_ops();

} // namespace vermell::net

#endif // VERMELL_NET_FILE_OPS_H