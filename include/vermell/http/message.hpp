//
// Modern HTTP/1.1 request parsing (C++20): zero-copy scanning with
// std::string_view, percent-decoding, query/form parsing and
// multipart/form-data (file upload) support.
//

#ifndef VERMELL_HTTP_MESSAGE_HPP
#define VERMELL_HTTP_MESSAGE_HPP

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vermell::http {

    inline constexpr std::string_view TYPE_FORM_URLENCODED = "application/x-www-form-urlencoded";
    inline constexpr std::string_view TYPE_JSON            = "application/json";
    inline constexpr std::string_view TYPE_MULTIPART       = "multipart/form-data";
    inline constexpr std::string_view TYPE_TEXT            = "text/plain";

    // An uploaded file extracted from a multipart/form-data body.
    struct UploadedFile {
        std::string field;        // form field name (Content-Disposition name="...")
        std::string filename;     // original file name sent by the client
        std::string mime;         // part Content-Type (application/octet-stream by default)
        std::string content;      // raw bytes

        [[nodiscard]] size_t size()  const noexcept { return content.size(); }
        [[nodiscard]] bool   empty() const noexcept { return content.empty(); }

        // Persists the file. Refuses absolute paths (POSIX and Windows drive
        // letters / UNC), any ".." traversal segment and NUL bytes, so even
        // a caller that forgets to sanitize the filename cannot write outside
        // its chosen directory.
        bool save_to(const std::string& path) const {
            if (path.empty() || path.front() == '/' || path.front() == '\\')
                return false;
            // Windows drive-letter ("C:\...", "C:/...") and UNC ("\\server\...")
            // absolute paths.
            if (path.size() >= 2 && path[1] == ':' &&
                ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
                return false;

            size_t pos = 0;
            while (pos < path.size()) {
                const size_t next = path.find_first_of("/\\", pos);
                const std::string_view segment(path.data() + pos,
                                               (next == std::string::npos ? path.size() : next) - pos);
                if (segment == "..")
                    return false;
                if (segment.find('\0') != std::string_view::npos)
                    return false; // a NUL would silently truncate the path
                if (next == std::string::npos)
                    break;
                pos = next + 1;
            }

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            return out.good();
        }
    };

    namespace detail {

        [[nodiscard]] inline char ascii_lower(const char c) noexcept {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        [[nodiscard]] inline bool iequals(const std::string_view a, const std::string_view b) noexcept {
            return a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
                       return ascii_lower(x) == ascii_lower(y);
                   });
        }

        [[nodiscard]] inline std::string_view trim(const std::string_view sv) noexcept {
            constexpr std::string_view WS = " \t\r\n";
            const size_t first = sv.find_first_not_of(WS);
            if (first == std::string_view::npos)
                return {};
            return sv.substr(first, sv.find_last_not_of(WS) - first + 1);
        }

        [[nodiscard]] inline std::string lower(const std::string_view sv) {
            std::string out;
            out.reserve(sv.size());
            for (const char c : sv)
                out.push_back(ascii_lower(c));
            return out;
        }

        [[nodiscard]] inline int hex_value(const char c) noexcept {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

    } // namespace detail

    // Decodes application/x-www-form-urlencoded escapes: %XX and '+'.
    [[nodiscard]] inline std::string url_decode(const std::string_view in) {
        std::string out;
        out.reserve(in.size());

        for (size_t i = 0; i < in.size(); ++i) {
            const char c = in[i];
            if (c == '%' && i + 2 < in.size()) { // in[i+1] and in[i+2] are in bounds
                const int hi = detail::hex_value(in[i + 1]);
                const int lo = detail::hex_value(in[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
                out.push_back(c); // malformed escape: keep literally
            }
            else if (c == '+') {
                out.push_back(' ');
            }
            else {
                out.push_back(c);
            }
        }
        return out;
    }

    // Decodes %XX escapes only; '+' stays a literal plus. This is the
    // URL-PATH semantics (a file named "a+b.js" must resolve to "a+b.js"),
    // as opposed to url_decode() which also maps '+' to space for query
    // strings and urlencoded form bodies.
    [[nodiscard]] inline std::string percent_decode(const std::string_view in) {
        std::string out;
        out.reserve(in.size());

        for (size_t i = 0; i < in.size(); ++i) {
            const char c = in[i];
            if (c == '%' && i + 2 < in.size()) { // in[i+1] and in[i+2] are in bounds
                const int hi = detail::hex_value(in[i + 1]);
                const int lo = detail::hex_value(in[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
                out.push_back(c); // malformed escape: keep literally
            }
            else {
                out.push_back(c);
            }
        }
        return out;
    }

    // Parses "a=1&b=two" (query strings and urlencoded bodies) with decoding.
    [[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
    parse_query(const std::string_view raw) {
        std::vector<std::pair<std::string, std::string>> out;

        size_t pos = 0;
        while (pos <= raw.size()) {
            const size_t amp = raw.find('&', pos);
            const std::string_view segment = raw.substr(pos, amp == std::string_view::npos ? amp : amp - pos);

            if (!segment.empty()) {
                if (const size_t eq = segment.find('='); eq != std::string_view::npos)
                    out.emplace_back(url_decode(segment.substr(0, eq)), url_decode(segment.substr(eq + 1)));
                else
                    out.emplace_back(url_decode(segment), std::string{});
            }

            if (amp == std::string_view::npos)
                break;
            pos = amp + 1;
        }
        return out;
    }

    // A fully parsed HTTP/1.1 request message.
    //
    // Hardening rules (RFC 9112):
    //   - request line is strictly METHOD SP TARGET SP HTTP/DIGIT.DIGIT
    //   - a request without Content-Length has NO body: extra bytes after
    //     the head are ignored (no pipelining: the connection is closed)
    //   - the body is exactly Content-Length bytes; less is an error
    //   - duplicated Content-Length is accepted only when every value
    //     matches; conflicting or non-numeric values are rejected (400)
    //   - Transfer-Encoding: chunked is supported (de-chunked on parse); any
    //     other coding is rejected with 501 instead of being mis-parsed
    //   - at most MAX_HEADERS header fields (431)
    class Message {
    public:
        // Hard cap on header fields: far beyond any browser or API client,
        // and a wall against header-flood abuse.
        static constexpr size_t MAX_HEADERS = 100;
        // Hard cap on a single header line (request line included): a 64 KiB
        // field is beyond any real client and bounds per-line memory no
        // matter how large max_request_size is configured.
        static constexpr size_t MAX_HEADER_LINE = 64 * 1024;

        std::string method;        // GET, POST, ...
        std::string target;        // raw request target, e.g. /index?id=2
        std::string path;          // target without query string or trailing '/'
        std::string query;         // raw query string (still percent-encoded)
        std::string version;       // e.g. HTTP/1.1
        std::string body;          // message body (exactly Content-Length bytes)

        // Header list preserving wire order and original name casing.
        std::vector<std::pair<std::string, std::string>> headers;

        [[nodiscard]] std::string_view header(const std::string_view name) const noexcept {
            for (const auto& [hname, hvalue] : headers)
                if (detail::iequals(hname, name))
                    return hvalue;
            return {};
        }

        [[nodiscard]] bool has_header(const std::string_view name) const noexcept {
            return std::any_of(headers.begin(), headers.end(), [&](const auto& h) {
                return detail::iequals(h.first, name);
            });
        }

        // Lowercased media type without parameters: "text/html; charset=x" -> "text/html".
        [[nodiscard]] std::string_view content_type() const noexcept { return content_type_; }

        // Value of a Content-Type parameter, e.g. content_type_param("boundary").
        [[nodiscard]] std::optional<std::string_view> content_type_param(const std::string_view key) const noexcept {
            for (const auto& [k, v] : content_type_params_)
                if (k == key)
                    return v;
            return std::nullopt;
        }

        // Validated Content-Length (0 when absent). Consistent by
        // construction: parse() rejects anything ambiguous.
        [[nodiscard]] size_t content_length() const noexcept { return content_length_; }

        // ---- framing inspection -------------------------------------------

        // How a raw (possibly partial) request buffer is framed.
        enum class Framing {
            Complete,       // ready for parse()
            Incomplete,     // keep reading from the socket
            BadRequest,     // 400: invalid or conflicting Content-Length
            TooManyHeaders, // 431: more than MAX_HEADERS header fields
            NotImplemented  // 501: Transfer-Encoding is not supported
        };

        struct Inspection {
            Framing framing = Framing::Incomplete;
            // Total request bytes once the head is known (head + declared
            // body). 0 while the head has not fully arrived.
            size_t  expected = 0;
        };

        // Inspects raw bytes WITHOUT building a Message. This is the single
        // source of truth for "is the request complete?" shared by the
        // socket read loop and parse() itself.
        [[nodiscard]] static Inspection inspect(const std::string_view raw) noexcept {
            const auto [head_end, separator] = head_bounds(raw);

            if (head_end == std::string_view::npos) {
                // Head still on the wire, but a flood of lines or a giant
                // line is already answer enough: reject without waiting for
                // the terminator.
                size_t lines = 0;
                size_t prev = 0;
                for (size_t p = raw.find('\n'); p != std::string_view::npos; p = raw.find('\n', p + 1)) {
                    if (++lines > MAX_HEADERS + 1) // +1: the request line
                        return {Framing::TooManyHeaders, 0};
                    if (p - prev > MAX_HEADER_LINE) // a single line out of control
                        return {Framing::BadRequest, 0};
                    prev = p + 1;
                }
                if (raw.size() - prev > MAX_HEADER_LINE) // trailing partial line
                    return {Framing::BadRequest, 0};
                return {Framing::Incomplete, 0};
            }

            const HeadScan scan = scan_head(raw.substr(0, head_end));
            if (scan.too_many)
                return {Framing::TooManyHeaders, 0};
            if (scan.bad)
                return {Framing::BadRequest, 0};
            if (scan.transfer_encoding)
                return {Framing::NotImplemented, 0};
            // RFC 9112 §3.5: HTTP/1.1+ requests must use CRLF terminators;
            // bare-LF or mixed line endings are a desync vector behind a
            // proxy that normalizes differently.
            if (!line_endings_ok(raw.substr(0, head_end), separator))
                return {Framing::BadRequest, 0};

            const size_t base = head_end + separator;

            if (scan.chunked) {
                const auto result = scan_chunked(raw.substr(base));
                if (result.state == ChunkState::Complete)
                    return {Framing::Complete, base + result.consumed};
                if (result.state == ChunkState::Malformed)
                    return {Framing::BadRequest, 0};
                return {Framing::Incomplete, 0};
            }

            size_t total = base;
            if (scan.content_length > std::string_view::npos - base)
                total = std::string_view::npos; // saturated: the caller's size cap rejects it
            else
                total = base + scan.content_length;

            return {raw.size() >= total ? Framing::Complete : Framing::Incomplete, total};
        }

        // ---- parsing --------------------------------------------------------

        // Parses a COMPLETE request (see inspect()). Malformed input yields
        // nullopt; the transport layer answers 400/431/501 as appropriate.
        [[nodiscard]] static std::optional<Message> parse(const std::string_view raw) {
            const auto [head_end, separator] = head_bounds(raw);
            if (head_end == std::string_view::npos)
                return std::nullopt; // incomplete head: nothing trustworthy to parse

            const std::string_view head = raw.substr(0, head_end);
            const HeadScan scan = scan_head(head);
            if (scan.bad || scan.transfer_encoding || scan.too_many)
                return std::nullopt; // inspect() already pinpointed the reason
            if (!line_endings_ok(head, separator))
                return std::nullopt; // RFC 9112 §3.5: bare-LF/mixed endings for HTTP/1.1+

            Message msg;

            // ---- request line: METHOD SP TARGET SP HTTP/DIGIT.DIGIT ----
            const size_t line_end = head.find('\n');
            const std::string_view request_line = head.substr(0, line_end);

            const size_t sp1 = request_line.find(' ');
            if (sp1 == std::string_view::npos || sp1 == 0)
                return std::nullopt;
            const size_t sp2 = request_line.find(' ', sp1 + 1);
            if (sp2 == std::string_view::npos || sp2 == sp1 + 1)
                return std::nullopt; // empty target or missing version
            if (request_line.find(' ', sp2 + 1) != std::string_view::npos)
                return std::nullopt; // exactly three parts, no more

            const std::string_view method_view = request_line.substr(0, sp1);
            const std::string_view target_view = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string_view version_view = request_line.substr(sp2 + 1);
            if (!version_view.empty() && version_view.back() == '\r')
                version_view.remove_suffix(1);

            if (!is_token(method_view) || !is_valid_target(target_view) || !is_valid_version(version_view))
                return std::nullopt;

            msg.method  = std::string(method_view);
            msg.target  = std::string(target_view);
            msg.version = std::string(version_view);

            // path / query split; strip a single trailing '/' like the legacy router did
            if (const size_t q = msg.target.find('?'); q != std::string::npos) {
                msg.path  = msg.target.substr(0, q);
                msg.query = msg.target.substr(q + 1);
            } else {
                msg.path = msg.target;
            }
            if (msg.path.size() > 1 && msg.path.back() == '/')
                msg.path.pop_back();

            // ---- header block (the head is already validated by scan_head) ----
            if (line_end != std::string_view::npos) {
                size_t pos = line_end + 1;
                while (pos < head.size()) {
                    const size_t eol = head.find('\n', pos);
                    std::string_view line = head.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
                    pos = eol == std::string_view::npos ? head.size() : eol + 1;

                    if (!line.empty() && line.back() == '\r')
                        line.remove_suffix(1);
                    if (line.empty())
                        continue;

                    // RFC 9112 §5.2: a line starting with SP/HTAB is an
                    // obsolete fold of the previous field. It MUST be
                    // rejected, never reinterpreted as a new header (behind
                    // a proxy that folds differently it is a desync vector).
                    if (line.front() == ' ' || line.front() == '\t')
                        return std::nullopt;

                    const size_t colon = line.find(':');
                    if (colon == std::string_view::npos || colon == 0)
                        return std::nullopt; // malformed field line

                    const std::string_view name = detail::trim(line.substr(0, colon));
                    if (!is_token(name))
                        return std::nullopt; // spaces/control chars in the name

                    msg.headers.emplace_back(std::string(name),
                                             std::string(detail::trim(line.substr(colon + 1))));
                }
            }

            // RFC 9112 §3.2/§6.2: an HTTP/1.1 (or newer) request must carry
            // exactly one Host header. Duplicate Hosts are a smuggling /
            // cache-desync vector in ANY version (RFC 9112 §6.2: "more than
            // one Host header field" => 400); only the *absence* of Host is
            // tolerated for HTTP/1.0 legacy clients.
            size_t host_count = 0;
            for (const auto& header : msg.headers) {
                if (!detail::iequals(header.first, "Host"))
                    continue;
                if (++host_count > 1)
                    return std::nullopt;
            }
            if (host_count == 0 && !version_view.starts_with("HTTP/1.0"))
                return std::nullopt;

            // ---- body: exactly Content-Length bytes, chunked, or none ----
            msg.content_length_ = scan.content_length;
            const size_t body_begin = head_end + separator;
            if (scan.has_content_length && scan.content_length > 0) {
                if (raw.size() - body_begin < scan.content_length)
                    return std::nullopt; // promised bytes never arrived
                msg.body = std::string(raw.substr(body_begin, scan.content_length));
            }
            else if (scan.chunked) {
                auto result = scan_chunked(raw.substr(body_begin));
                if (result.state != ChunkState::Complete)
                    return std::nullopt;
                msg.body = std::move(result.data);
                msg.content_length_ = msg.body.size();
            }

            msg.parse_content_type();
            return msg;
        }

    private:
        size_t content_length_ = 0;
        // Owning member: a string_view pointing into a sibling string member
        // would dangle when a Message is moved (SSO buffers are copied inline).
        std::string content_type_;
        std::vector<std::pair<std::string, std::string>> content_type_params_;

        // Result of scanning the header block for framing purposes.
        struct HeadScan {
            size_t content_length     = 0;
            bool   has_content_length = false;
            bool   bad                = false; // CL non-numeric or conflicting duplicates
            bool   transfer_encoding  = false; // a Transfer-Encoding other than "chunked"
            bool   chunked            = false; // Transfer-Encoding: chunked
            bool   too_many           = false; // > MAX_HEADERS fields
        };

        // Returns {head_bytes, separator_size} for the head terminator
        // ("\r\n\r\n", or bare "\n\n" for legacy clients), or {npos, 0}
        // when the head has not fully arrived yet.
        static std::pair<size_t, size_t> head_bounds(const std::string_view raw) noexcept {
            const size_t crlf = raw.find("\r\n\r\n");
            const size_t lf   = raw.find("\n\n");
            if (crlf == std::string_view::npos)
                return lf == std::string_view::npos ? std::pair{lf, size_t{0}} : std::pair{lf, size_t{2}};
            if (lf == std::string_view::npos)
                return {crlf, size_t{4}};
            return lf < crlf ? std::pair{lf, size_t{2}} : std::pair{crlf, size_t{4}};
        }

        // Scans the header block line by line, validating only what framing
        // needs: Content-Length and Transfer-Encoding. Zero allocations.
        static HeadScan scan_head(const std::string_view head) noexcept {
            HeadScan scan;

            // Skip the request line, but a giant one is still a giant line.
            const size_t first_eol = head.find('\n');
            if (first_eol == std::string_view::npos)
                return scan;
            if (first_eol > MAX_HEADER_LINE) {
                scan.bad = true;
                return scan;
            }
            size_t pos = first_eol + 1;

            size_t count = 0;
            while (pos < head.size()) {
                const size_t eol = head.find('\n', pos);
                std::string_view line = head.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
                pos = eol == std::string_view::npos ? head.size() : eol + 1;

                if (line.size() > MAX_HEADER_LINE) {
                    scan.bad = true;
                    return scan;
                }
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                if (line.empty())
                    continue;
                if (++count > MAX_HEADERS) {
                    scan.too_many = true;
                    return scan;
                }

                // obs-fold: parse() rejects it with 400; framing must agree.
                if (line.front() == ' ' || line.front() == '\t') {
                    scan.bad = true;
                    return scan;
                }

                const size_t colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0)
                    continue; // malformed line: parse() rejects it; framing ignores it

                const std::string_view name  = detail::trim(line.substr(0, colon));
                const std::string_view value = detail::trim(line.substr(colon + 1));

                if (detail::iequals(name, "content-length")) {
                    if (scan.chunked || scan.transfer_encoding) {
                        scan.bad = true;
                        return scan;
                    }
                    size_t parsed = 0;
                    const char* first = value.data();
                    const char* last  = first + value.size();
                    const auto [ptr, ec] = std::from_chars(first, last, parsed);
                    // Whole value must be digits: no junk, no overflow.
                    if (ec != std::errc{} || ptr != last) {
                        scan.bad = true;
                        return scan;
                    }
                    // Duplicates are legal only when every value matches
                    // (RFC 9112 §6.3); a mismatch is a smuggling attempt.
                    if (scan.has_content_length && scan.content_length != parsed) {
                        scan.bad = true;
                        return scan;
                    }
                    scan.has_content_length = true;
                    scan.content_length     = parsed;
                } else if (detail::iequals(name, "transfer-encoding")) {
                    if (scan.has_content_length || scan.chunked || scan.transfer_encoding) {
                        scan.bad = true;
                        return scan;
                    }
                    if (detail::iequals(detail::trim(value), "chunked"))
                        scan.chunked = true;
                    else
                        scan.transfer_encoding = true;
                }
            }
            return scan;
        }

        enum class ChunkState { Complete, Incomplete, Malformed };

        struct ChunkScanResult {
            ChunkState state = ChunkState::Incomplete;
            std::string data;
            size_t consumed = 0;
        };

        static ChunkScanResult scan_chunked(const std::string_view body) noexcept {
            ChunkScanResult result;
            size_t pos = 0;
            for (;;) {
                const size_t line_end = body.find("\r\n", pos);
                if (line_end == std::string_view::npos) {
                    result.state = ChunkState::Incomplete;
                    return result;
                }
                const std::string_view size_line = body.substr(pos, line_end - pos);

                size_t size = 0;
                size_t i = 0;
                bool any = false;
                while (i < size_line.size()) {
                    const int d = detail::hex_value(size_line[i]);
                    if (d < 0)
                        break;
                    any = true;
                    if (size > (std::string_view::npos - static_cast<size_t>(d)) / 16) {
                        result.state = ChunkState::Malformed;
                        return result;
                    }
                    size = size * 16 + static_cast<size_t>(d);
                    ++i;
                }
                if (!any) {
                    result.state = ChunkState::Malformed;
                    return result;
                }
                if (i < size_line.size() && size_line[i] != ';') {
                    result.state = ChunkState::Malformed;
                    return result;
                }

                pos = line_end + 2;

                if (size == 0) {
                    for (;;) {
                        const size_t t_end = body.find("\r\n", pos);
                        if (t_end == std::string_view::npos) {
                            result.state = ChunkState::Incomplete;
                            return result;
                        }
                        if (t_end == pos) {
                            result.state = ChunkState::Complete;
                            result.consumed = t_end + 2;
                            return result;
                        }
                        pos = t_end + 2;
                    }
                }

                if (body.size() - pos < size) {
                    result.state = ChunkState::Incomplete;
                    return result;
                }
                result.data.append(body.data() + pos, size);
                pos += size;
                if (pos + 2 > body.size()) {
                    result.state = ChunkState::Incomplete;
                    return result;
                }
                if (body[pos] != '\r' || body[pos + 1] != '\n') {
                    result.state = ChunkState::Malformed;
                    return result;
                }
                pos += 2;
            }
        }

        // RFC 9110 tchar: method and header names are tokens.
        static bool is_token(const std::string_view s) noexcept {
            if (s.empty())
                return false;
            for (const char c : s) {
                const auto u = static_cast<unsigned char>(c);
                if (!(std::isalnum(u) || c == '!' || c == '#' || c == '$' || c == '%'
                      || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-'
                      || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~'))
                    return false;
            }
            return true;
        }

        // No spaces or control characters in the request target.
        static bool is_valid_target(const std::string_view target) noexcept {
            if (target.empty())
                return false;
            for (const char c : target)
                if (const auto u = static_cast<unsigned char>(c); u <= 0x20 || u == 0x7f)
                    return false;
            return true;
        }

        static bool is_valid_version(const std::string_view v) noexcept {
            return v.size() == 8 && v.starts_with("HTTP/")
                && std::isdigit(static_cast<unsigned char>(v[5]))
                && v[6] == '.'
                && std::isdigit(static_cast<unsigned char>(v[7]));
        }

        // True when the request line declares HTTP/1.1 or newer. Malformed
        // lines report false: parse() rejects them anyway, and inspect() only
        // uses this to decide how strict the line-ending policy must be.
        static bool version_at_least_11(const std::string_view request_line) noexcept {
            std::string_view v = request_line;
            if (!v.empty() && v.back() == '\r')
                v.remove_suffix(1);
            const size_t h = v.find("HTTP/");
            if (h == std::string_view::npos || v.size() - h != 8)
                return false;
            const char major = v[h + 5];
            const char minor = v[h + 7];
            if (!std::isdigit(static_cast<unsigned char>(major)) || v[h + 6] != '.'
                || !std::isdigit(static_cast<unsigned char>(minor)))
                return false;
            // Character comparisons only: keeps -Wstrict-overflow quiet and
            // is equivalent to (major,minor) >= (1,1) for digit chars.
            if (major < '1')
                return false; // HTTP/0.x
            if (major > '1')
                return true;  // HTTP/2+
            return minor >= '1'; // HTTP/1.1+
        }

        // RFC 9112 §3.5 line-ending policy: HTTP/1.1+ requires a CRLFCRLF
        // head terminator and a trailing '\r' on every line. Bare-LF heads
        // are tolerated only for HTTP/1.0 legacy clients; mixed endings
        // inside one head are always rejected for HTTP/1.1+ (behind a proxy
        // that folds differently they are a request-smuggling/desync vector).
        static bool line_endings_ok(const std::string_view head,
                                    const size_t separator) noexcept {
            // A head with no headers at all ("GET / HTTP/1.1\r\n\r\n") has no
            // internal '\n'; the whole head is the request line.
            const size_t le = head.find('\n');
            const std::string_view rl = le == std::string_view::npos ? head : head.substr(0, le);
            if (!version_at_least_11(rl))
                return true; // HTTP/1.0 legacy clients stay lenient
            if (separator != 4)
                return false; // bare-LF head terminator
            if (rl.empty() || rl.back() != '\r')
                return false; // the request line itself is not CRLF
            if (le == std::string_view::npos)
                return true; // no header lines to check
            size_t pos = le + 1;
            while (pos < head.size()) {
                const size_t eol = head.find('\n', pos);
                const std::string_view line = head.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
                // Every non-final line must be CRLF-terminated. The final
                // line's CR is the first byte of the CRLFCRLF separator
                // (already guaranteed by `separator == 4`).
                if (eol != std::string_view::npos && !line.empty() && line.back() != '\r')
                    return false; // a bare-LF header line inside a CRLF head
                if (eol == std::string_view::npos)
                    break;
                pos = eol + 1;
            }
            return true;
        }

        void parse_content_type() {
            const std::string_view value = header("Content-Type");
            if (value.empty())
                return;

            const size_t semi = value.find(';');
            content_type_ = detail::lower(detail::trim(value.substr(0, semi)));

            // parse "; key=value" parameters (boundary, charset, ...)
            size_t pos = semi == std::string_view::npos ? value.size() : semi + 1;
            while (pos < value.size()) {
                const size_t next = value.find(';', pos);
                const std::string_view pair = value.substr(pos, next == std::string_view::npos ? next : next - pos);
                if (const size_t eq = pair.find('='); eq != std::string_view::npos) {
                    std::string_view val = detail::trim(pair.substr(eq + 1));
                    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                        val = val.substr(1, val.size() - 2);
                    content_type_params_.emplace_back(detail::lower(detail::trim(pair.substr(0, eq))),
                                                      std::string(val));
                }
                if (next == std::string_view::npos)
                    break;
                pos = next + 1;
            }
        }
    };

    // Result of parsing a multipart/form-data body.
    struct Multipart {
        std::vector<std::pair<std::string, std::string>> fields;
        std::vector<UploadedFile> files;
    };

    namespace detail {

        // Reduces a client-supplied filename to a bare, safe file name:
        // strips any directory prefix (both '/' and '\'), and returns "" for
        // ".", "..", empty names or names containing control characters.
        // An empty result means "do not trust this name".
        [[nodiscard]] inline std::string sanitize_filename(std::string name) {
            if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos)
                name = name.substr(slash + 1);
            if (name.empty() || name == "." || name == "..")
                return {};
            for (const char c : name)
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                    return {};
            return name;
        }

        // Extracts a quoted token from a Content-Disposition header value:
        // disposition_param(R"(form-data; name="doc"; filename="a.txt")", "name") -> "doc"
        //
        // The key is matched only at a parameter boundary (the start of the
        // value or right after a ';'): a plain substring search would find
        // "name=" inside "filename=" when filename is sent first.
        [[nodiscard]] inline std::string disposition_param(const std::string_view disposition,
                                                           const std::string_view key) {
            size_t pos = 0;
            while (pos <= disposition.size()) {
                const size_t semi = disposition.find(';', pos);
                const std::string_view param = trim(disposition.substr(
                    pos, semi == std::string_view::npos ? semi : semi - pos));

                if (param.size() >= key.size() + 2
                    && param.substr(0, key.size()) == key
                    && param[key.size()] == '='
                    && param[key.size() + 1] == '"') {
                    const size_t value_begin = key.size() + 2;
                    const size_t value_end = param.find('"', value_begin);
                    if (value_end == std::string_view::npos)
                        return {};
                    return std::string(param.substr(value_begin, value_end - value_begin));
                }

                if (semi == std::string_view::npos)
                    break;
                pos = semi + 1;
            }
            return {};
        }

    } // namespace detail

    // Splits a multipart/form-data body into plain fields and uploaded files.
    [[nodiscard]] inline Multipart parse_multipart(const std::string_view body,
                                                   const std::string_view boundary) {
        Multipart result;
        if (boundary.empty())
            return result;

        const std::string delimiter = "--" + std::string(boundary);
        size_t pos = body.find(delimiter);
        if (pos == std::string_view::npos)
            return result;
        pos += delimiter.size();

        while (pos <= body.size()) {
            // Final delimiter ("--boundary--") ends the message.
            if (body.substr(pos, 2) == "--")
                break;
            if (body.substr(pos, 2) == "\r\n")
                pos += 2;
            else if (body.substr(pos, 1) == "\n")
                pos += 1;

            const size_t next = body.find(delimiter, pos);
            if (next == std::string_view::npos)
                break;

            std::string_view part = body.substr(pos, next - pos);
            pos = next + delimiter.size();

            // Strip the CRLF that precedes the next delimiter.
            if (part.size() >= 2 && part.substr(part.size() - 2) == "\r\n")
                part.remove_suffix(2);
            else if (!part.empty() && part.back() == '\n')
                part.remove_suffix(1);

            const size_t split = part.find("\r\n\r\n");
            const std::string_view part_headers = split == std::string_view::npos ? part : part.substr(0, split);
            const std::string_view content = split == std::string_view::npos
                                                 ? std::string_view{}
                                                 : part.substr(split + 4);

            std::string disposition, part_mime;
            size_t hpos = 0;
            while (hpos <= part_headers.size()) {
                const size_t eol = part_headers.find("\r\n", hpos);
                const std::string_view line = part_headers.substr(hpos, eol == std::string_view::npos ? eol : eol - hpos);
                if (const size_t colon = line.find(':'); colon != std::string_view::npos) {
                    const std::string_view hname = detail::trim(line.substr(0, colon));
                    const std::string_view hvalue = detail::trim(line.substr(colon + 1));
                    if (detail::iequals(hname, "Content-Disposition"))
                        disposition = std::string(hvalue);
                    else if (detail::iequals(hname, "Content-Type"))
                        part_mime = std::string(hvalue);
                }
                if (eol == std::string_view::npos)
                    break;
                hpos = eol + 2;
            }

            std::string name = detail::disposition_param(disposition, "name");
            if (name.empty())
                continue;

            if (std::string filename = detail::sanitize_filename(detail::disposition_param(disposition, "filename")); !filename.empty()) {
                UploadedFile file;
                file.field    = std::move(name);
                file.filename = std::move(filename);
                file.mime     = part_mime.empty() ? "application/octet-stream" : std::move(part_mime);
                file.content  = std::string(content);
                result.files.push_back(std::move(file));
            } else {
                result.fields.emplace_back(std::move(name), std::string(content));
            }
        }
        return result;
    }

} // namespace vermell::http

#endif // VERMELL_HTTP_MESSAGE_HPP
