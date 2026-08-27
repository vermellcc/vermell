//
// Shared hardening primitives for the file-rendering family.
//
//   - read_bounded(): regular-file check + size cap (no FIFOs, no /dev,
//     no memory exhaustion).
//   - is_within():    canonical containment check for the optional jail.
//   - valid_include_name(): strict whitelist for compose() module names.
//   - escape_html():  safe reflection of names/paths in error pages.
//

#ifndef VERMELL_SECURE_RENDER_H
#define VERMELL_SECURE_RENDER_H

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace vermell::srender {

    // ---------------- bounded file reading ----------------

    enum class ReadErr { Ok, NotFound, Forbidden, TooLarge, IoError };

    struct ReadResult {
        ReadErr err = ReadErr::Ok;
        std::string data{};
    };

    [[nodiscard]] inline const char* status_of(const ReadErr err) noexcept {
        switch (err) {
            case ReadErr::NotFound:  return "404";
            case ReadErr::Forbidden: return "403";
            case ReadErr::TooLarge:  return "413";
            case ReadErr::IoError:   return "500";
            default:                 return "200";
        }
    }

    // Reads a regular file fully, refusing anything that is not a plain
    // regular file (directories, FIFOs, devices, /proc entries with a
    // lied-about size are capped by the streaming read below) and anything
    // larger than max_bytes.
    //
    // Opens with O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC and re-checks with
    // fstat(): a symlink planted between the jail check and the open, a FIFO
    // or a device can never be served (the O_NONBLOCK open of a FIFO cannot
    // block waiting for a writer, and fstat rejects anything not S_IFREG).
    [[nodiscard]] inline ReadResult read_bounded(const std::string& path, const size_t max_bytes) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return {ReadErr::NotFound, {}};

        struct stat st{};
        if (fstat(fd, &st) != 0) {
            ::close(fd);
            return {ReadErr::IoError, {}};
        }
        if (!S_ISREG(st.st_mode)) {
            ::close(fd);
            return {ReadErr::Forbidden, {}};
        }
        if (st.st_size > static_cast<off_t>(max_bytes)) {
            ::close(fd);
            return {ReadErr::TooLarge, {}};
        }

        std::string out;
        out.reserve(static_cast<size_t>(st.st_size));

        char chunk[16384];
        size_t total = 0;
        for (;;) {
            const ssize_t n = ::read(fd, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                ::close(fd);
                return {ReadErr::IoError, {}};
            }
            if (n == 0)
                break;
            total += static_cast<size_t>(n);
            if (total > max_bytes) { // grew past the cap while being read
                ::close(fd);
                return {ReadErr::TooLarge, {}};
            }
            out.append(chunk, static_cast<size_t>(n));
        }

        ::close(fd);
        return {ReadErr::Ok, std::move(out)};
    }

    // ---------------- jail containment ----------------

    // True when `target` resolves (symlinks resolved for the existing
    // prefix, '.'/'..' folded lexically) inside `base`. Both may be
    // relative; they are anchored to the CWD first.
    [[nodiscard]] inline bool is_within(const std::string& base, const std::string& target) {
        namespace fs = std::filesystem;
        if (base.empty() || target.empty())
            return false;

        std::error_code ec;
        const fs::path abs_base   = fs::absolute(fs::path(base), ec);
        if (ec) return false;
        const fs::path abs_target = fs::absolute(fs::path(target), ec);
        if (ec) return false;

        const fs::path cb = fs::weakly_canonical(abs_base, ec);
        if (ec || cb.empty()) return false;
        const fs::path ct = fs::weakly_canonical(abs_target, ec);
        if (ec || ct.empty()) return false;

        // Component-wise prefix check: base must be an ancestor of target.
        auto b = cb.begin();
        auto t = ct.begin();
        for (; b != cb.end(); ++b, ++t) {
            if (t == ct.end() || *b != *t)
                return false;
        }
        return true;
    }

    // ---------------- compose() module names ----------------

    // Module names in "#[name];" must be bare file names: this kills
    // "../../etc/passwd" style traversal regardless of the template folder.
    [[nodiscard]] inline bool valid_include_name(const std::string_view name) noexcept {
        if (name.empty() || name.size() > 255)
            return false;
        if (name == "." || name == "..")
            return false;
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok)
                return false;
        }
        return name.find("..") == std::string_view::npos;
    }

    // ---------------- HTML escaping for reflected values ----------------

    [[nodiscard]] inline std::string escape_html(const std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (const char c : in) {
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&#39;";  break;
                default:
                    // Drop C0 control chars: they have no business in a page.
                    if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t')
                        out.push_back(c);
            }
        }
        return out;
    }

} // namespace vermell::srender

#endif // VERMELL_SECURE_RENDER_H
