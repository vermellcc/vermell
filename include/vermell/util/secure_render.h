//
// Shared hardening primitives for the file-rendering family.
//
//   - read_bounded(): regular-file check + size cap (no FIFOs, no /dev,
//     no memory exhaustion).
//   - is_within():    canonical containment check for the optional jail.
//   - valid_include_name(): strict whitelist for compose() module names.
//   - escape_html():  safe reflection of names/paths in error pages.
//
// The read/stat primitives are delegated to the backend's FileOps.

#ifndef VERMELL_SECURE_RENDER_H
#define VERMELL_SECURE_RENDER_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "../net/file_ops.h"

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

    [[nodiscard]] inline ReadErr to_read_err(const vermell::net::FileReadErr err) noexcept {
        switch (err) {
            case vermell::net::FileReadErr::NotFound:  return ReadErr::NotFound;
            case vermell::net::FileReadErr::Forbidden: return ReadErr::Forbidden;
            case vermell::net::FileReadErr::TooLarge:  return ReadErr::TooLarge;
            case vermell::net::FileReadErr::IoError:   return ReadErr::IoError;
            default:                                   return ReadErr::Ok;
        }
    }

    // Reads a plain regular file through the backend FileOps (no backend -> 500).
    [[nodiscard]] inline ReadResult read_bounded(const std::string& path, const size_t max_bytes) {
        const auto ops = vermell::net::default_file_ops();
        if (ops == nullptr)
            return {ReadErr::IoError, {}};
        const auto read = ops->read_bounded(path, max_bytes);
        return {to_read_err(read.err), std::move(read.data)};
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
