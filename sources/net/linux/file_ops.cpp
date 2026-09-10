// POSIX FileOps: the regular-file safety policy of the render layer
// (no-follow open, fstat re-check, capped streaming read) lives here.

#include "vermell/net/file_ops.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vermell::net {

namespace {

    class PosixFileOps : public FileOps {
    public:
        FileRead read_bounded(const std::string& path,
                              const std::size_t max_bytes) override {
            // O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC: no symlink following, no FIFO
            // blocking, no fd leak on exec; fstat re-checks S_IFREG below.
            const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                return {FileReadErr::NotFound, {}};

            struct stat st{};
            if (::fstat(fd, &st) != 0) {
                ::close(fd);
                return {FileReadErr::IoError, {}};
            }
            if (!S_ISREG(st.st_mode)) {
                ::close(fd);
                return {FileReadErr::Forbidden, {}};
            }
            if (st.st_size > static_cast<off_t>(max_bytes)) {
                ::close(fd);
                return {FileReadErr::TooLarge, {}};
            }

            std::string out;
            out.reserve(static_cast<std::size_t>(st.st_size));

            char chunk[16384];
            std::size_t total = 0;
            for (;;) {
                const ssize_t n = ::read(fd, chunk, sizeof(chunk));
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    ::close(fd);
                    return {FileReadErr::IoError, {}};
                }
                if (n == 0)
                    break;
                total += static_cast<std::size_t>(n);
                if (total > max_bytes) {
                    ::close(fd);
                    return {FileReadErr::TooLarge, {}};
                }
                out.append(chunk, static_cast<std::size_t>(n));
            }

            ::close(fd);
            return {FileReadErr::Ok, std::move(out)};
        }

        FileStat stat(const std::string& path) override {
            struct stat st{};
            if (::stat(path.c_str(), &st) != 0)
                return {};
            FileStat out;
            out.ok = true;
            out.size = static_cast<std::int64_t>(st.st_size);
            out.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
            out.is_dir = S_ISDIR(st.st_mode);
            out.is_regular = S_ISREG(st.st_mode);
            return out;
        }
    };

} // namespace

std::shared_ptr<FileOps> platform_file_ops() {
    static const std::shared_ptr<FileOps> instance = std::make_shared<PosixFileOps>();
    return instance;
}

} // namespace vermell::net