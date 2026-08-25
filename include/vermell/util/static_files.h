//
// Static directory server (Node's express.static equivalent): mount a disk
// directory — a Vue/React/Angular dist folder, plain assets, anything — at a
// URL prefix and let Vermell resolve, jail and serve the files.
//
//   router.staticX("/", "./dist", { .spa = true });
//
// - The mount directory IS the jail: no request path can escape it, even
//   through percent-encoded ".." segments or symlinks.
// - Explicit routes always win over static mounts (static is the fallback
//   layer), and the MOST SPECIFIC matching mount answers (a "/assets" mount
//   beats a root "/" mount for "/assets/..."; ties by registration order).
// - Directories (and the mount root) serve the index file; SPA fallback is
//   opt-in so a missing asset is a 404, never silently HTML.
// - Caching is on by default: Cache-Control + ETag + conditional GET (304).
//
// "static" is a C++ keyword, hence the X suffix (same convention as
// Router::deleteX).
//

#pragma once
#ifndef VERMELL_STATIC_FILES_H
#define VERMELL_STATIC_FILES_H

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <sys/stat.h>

#include "../http/message.hpp"
#include "../http/response.hpp"
#include "mime_types.hpp"
#include "secure_render.h"

namespace vermell {

    // Tunables for one mounted static directory.
    struct StaticOptions {
        // File served for a directory path (the mount root, trailing-slash
        // paths). Vue/React/Angular builds all emit "index.html".
        std::string index = "index.html";

        // SPA fallback: any missing file under the mount answers the index
        // file instead of 404 (client-side routing of Vue/React/Angular).
        // OFF by default: a missing asset must never silently become HTML.
        bool spa = false;

        // Cache-Control + ETag + conditional GET (304 Not Modified). OFF
        // disables every caching header.
        bool cache = true;

        // Cache-Control max-age. 0 = revalidate on every request
        // (express.static default); hashed dist assets love a large value.
        std::chrono::seconds max_age{0};

        // Size cap for a single served file (same default as RenderSecurity).
        size_t max_file_bytes = 32UL * 1024UL * 1024UL;
    };

    // A single static mount: a URL prefix bound to a disk directory.
    class StaticMount {
    public:
        StaticMount() = default;

        // `mount` is the URL prefix ("/" or "/assets"), `dir` the disk
        // directory ("./dist"). An empty `dir` falls back to the working
        // directory; the mount directory is the jail for every path served.
        StaticMount(std::string mount, std::string dir, StaticOptions options = {})
            : mount_(normalize_mount(std::move(mount))),
              dir_(dir.empty() ? "." : std::move(dir)),
              options_(std::move(options)) {}

        [[nodiscard]] const std::string& mount() const noexcept { return mount_; }
        [[nodiscard]] const std::string& dir() const noexcept { return dir_; }
        [[nodiscard]] const StaticOptions& options() const noexcept { return options_; }

        // Serves `request_path` (the raw, percent-encoded request target)
        // under this mount. Returns the complete wire response, or nullopt
        // when the path is not under the mount (or the method is not
        // GET/HEAD) so the caller can try the next mount or answer 404.
        // `if_none_match` is the raw "If-None-Match" request header value.
        [[nodiscard]] std::optional<http::WireResponse> serve(const std::string_view request_path,
                                                               const std::string_view method,
                                                               const std::string_view if_none_match = {}) const;

        // True when `request_path` (raw, percent-encoded) falls under this
        // mount. Callers use it to pick the most specific mount first.
        [[nodiscard]] bool covers(const std::string_view request_path) const {
            return under_mount(vermell::http::percent_decode(request_path));
        }

    private:
        std::string mount_{"/"};
        std::string dir_{"."};
        StaticOptions options_{};

        [[nodiscard]] static std::string normalize_mount(std::string mount);
        [[nodiscard]] bool under_mount(const std::string_view decoded) const noexcept;
        // True when any '/' separated segment of `rel` is exactly "..".
        [[nodiscard]] static bool has_parent_segment(const std::string_view rel) noexcept;
        // If-None-Match matching: comma list, "*", weak (W/) comparison.
        [[nodiscard]] static bool etag_matches(const std::string_view header,
                                               const std::string_view etag) noexcept;
        [[nodiscard]] static std::string make_etag(const struct stat& st) noexcept;
        [[nodiscard]] std::string cache_control_header() const;
        // Reads and answers one resolved file. `spa_fallback` allows the
        // index fallback on NotFound (disabled for the fallback itself, so
        // a missing index.html is a plain 404, never a loop).
        [[nodiscard]] std::optional<http::WireResponse> serve_path(const std::string& full,
                                                                   const std::string_view if_none_match,
                                                                   const bool spa_fallback) const;
        [[nodiscard]] std::optional<http::WireResponse> serve_index(const std::string_view if_none_match) const;
        [[nodiscard]] static http::WireResponse respond(const int status, std::string body,
                                                         const std::string_view mime,
                                                         const std::string_view cache_control = {},
                                                         const std::string_view etag = {});
    };

    // ------------------------------------------------------------------ //

    inline std::string StaticMount::normalize_mount(std::string mount) {
        if (mount.empty() || mount.front() != '/')
            mount.insert(mount.begin(), '/');
        while (mount.size() > 1 && mount.back() == '/')
            mount.pop_back();
        return mount;
    }

    inline bool StaticMount::under_mount(const std::string_view decoded) const noexcept {
        if (mount_ == "/")
            return true;
        if (decoded == mount_)
            return true;
        return decoded.size() > mount_.size()
            && decoded.substr(0, mount_.size()) == mount_
            && decoded[mount_.size()] == '/';
    }

    inline bool StaticMount::has_parent_segment(const std::string_view rel) noexcept {
        size_t pos = 0;
        while (pos <= rel.size()) {
            const size_t slash = rel.find('/', pos);
            const std::string_view segment = rel.substr(
                pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
            if (segment == "..")
                return true;
            if (slash == std::string_view::npos)
                break;
            pos = slash + 1;
        }
        return false;
    }

    inline bool StaticMount::etag_matches(const std::string_view header,
                                          const std::string_view etag) noexcept {
        if (header.empty() || etag.empty())
            return false;
        size_t pos = 0;
        for (;;) {
            const size_t comma = header.find(',', pos);
            std::string_view item = header.substr(
                pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.remove_suffix(1);
            if (item == "*")
                return true;
            // Weak comparison (RFC 9110 §8.8.3.1): a W/ prefix on the
            // client side still matches our strong ETag for GET/HEAD.
            if (item.size() >= 2 && item[0] == 'W' && item[1] == '/')
                item.remove_prefix(2);
            if (item == etag)
                return true;
            if (comma == std::string_view::npos)
                break;
            pos = comma + 1;
        }
        return false;
    }

    inline std::string StaticMount::make_etag(const struct stat& st) noexcept {
        // Strong opaque tag from size + mtime (seconds): cheap, stable for
        // the whole life of an unchanged build artifact.
        return "\"" + std::to_string(static_cast<long long>(st.st_size)) + "-"
                    + std::to_string(static_cast<long long>(st.st_mtim.tv_sec)) + "\"";
    }

    inline std::string StaticMount::cache_control_header() const {
        if (!options_.cache)
            return {};
        return "public, max-age=" + std::to_string(options_.max_age.count());
    }

    inline http::WireResponse StaticMount::respond(const int status, std::string body,
                                                   const std::string_view mime,
                                                   const std::string_view cache_control,
                                                   const std::string_view etag) {
        vermell::http::Response response;
        response.status(status).type(mime).body(std::move(body));
        if (!cache_control.empty())
            response.set("Cache-Control", cache_control);
        if (!etag.empty())
            response.set("ETag", etag);

        http::WireResponse out;
        out.head = response.head();
        out.body = response.take_body();
        return out;
    }

    inline std::optional<http::WireResponse> StaticMount::serve_path(
        const std::string& full, const std::string_view if_none_match,
        const bool spa_fallback) const {

        auto read = vermell::srender::read_bounded(full, options_.max_file_bytes);
        switch (read.err) {
            case vermell::srender::ReadErr::Ok:
                break;
            case vermell::srender::ReadErr::NotFound:
                if (spa_fallback && options_.spa)
                    return serve_index(if_none_match);
                return respond(404, "Vermell: 404 Not Found", vermell::mime::plain);
            case vermell::srender::ReadErr::Forbidden:
                return respond(403, "Vermell: forbidden path", vermell::mime::plain);
            case vermell::srender::ReadErr::TooLarge:
                return respond(413, "Vermell: file too large", vermell::mime::plain);
            default:
                return respond(500, "Vermell: internal error", vermell::mime::plain);
        }

        const std::string cache_control = cache_control_header();
        if (options_.cache) {
            struct stat mst{};
            std::string etag;
            if (::stat(full.c_str(), &mst) == 0 && S_ISREG(mst.st_mode))
                etag = make_etag(mst);
            if (!etag.empty() && etag_matches(if_none_match, etag))
                return respond(304, {}, vermell::mime::of(full), cache_control, etag);
            return respond(200, std::move(read.data), vermell::mime::of(full),
                           cache_control, etag);
        }
        return respond(200, std::move(read.data), vermell::mime::of(full), cache_control);
    }

    inline std::optional<http::WireResponse> StaticMount::serve_index(
        const std::string_view if_none_match) const {

        std::string full = dir_.empty() ? "." : dir_;
        if (full.back() != '/')
            full.push_back('/');
        full += options_.index;

        // Same jail as any other file under the mount.
        if (!vermell::srender::is_within(dir_.empty() ? "." : dir_, full))
            return respond(403, "Vermell: forbidden path", vermell::mime::plain);

        // spa_fallback = false: a missing index.html is a 404, never a loop.
        return serve_path(full, if_none_match, false);
    }

    inline std::optional<http::WireResponse> StaticMount::serve(
        const std::string_view request_path, const std::string_view method,
        const std::string_view if_none_match) const {

        // Static mounts answer GET/HEAD only; other methods fall through so
        // the generic 404 keeps its semantics.
        if (method != "GET" && method != "HEAD")
            return std::nullopt;

        // Path semantics: %XX escapes decoded, '+' stays a literal plus
        // (url_decode() would turn a file "a+b.js" into "a b.js").
        const std::string decoded = vermell::http::percent_decode(request_path);
        if (!under_mount(decoded))
            return std::nullopt;

        // Relative path below the mount ("" for the mount root itself).
        std::string rel = mount_ == "/" ? decoded : decoded.substr(mount_.size());
        if (const size_t first = rel.find_first_not_of('/'); first != std::string::npos)
            rel.erase(0, first);
        else
            rel.clear();
        if (const size_t last = rel.find_last_not_of('/'); last != std::string::npos)
            rel.erase(last + 1);
        else
            rel.clear();

        // The decoded path is attacker-controlled: NUL bytes and ".."
        // segments never reach the filesystem (the jail below is the second
        // wall).
        if (rel.find('\0') != std::string::npos || has_parent_segment(rel))
            return respond(403, "Vermell: forbidden path", vermell::mime::plain);

        std::string full = dir_.empty() ? "." : dir_;
        if (full.back() != '/')
            full.push_back('/');
        full += rel;

        // Directory (or the mount root): serve the index file instead.
        struct stat st{};
        const bool is_dir = ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        if (is_dir || rel.empty()) {
            if (full.back() != '/')
                full.push_back('/');
            full += options_.index;
        }

        // Jail: the mount directory is the root. Symlinks are resolved for
        // the containment check, then read_bounded refuses symlinked final
        // components (O_NOFOLLOW) — the same policy as readFile/file.
        if (!vermell::srender::is_within(dir_.empty() ? "." : dir_, full))
            return respond(403, "Vermell: forbidden path", vermell::mime::plain);

        return serve_path(full, if_none_match, true);
    }

} // namespace vermell

#endif // VERMELL_STATIC_FILES_H
