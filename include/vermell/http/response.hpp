#ifndef VERMELL_HTTP_RESPONSE_HPP
#define VERMELL_HTTP_RESPONSE_HPP

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vermell::http {

    class Response {
    public:
        Response() = default;

        Response& status(const int code) noexcept {
            code_ = code;
            return *this;
        }

        Response& type(const std::string_view mime) {
            return set("Content-Type", mime);
        }

        Response& set(const std::string_view name, const std::string_view value) {
            const std::string safe_name  = sanitize(name);
            const std::string safe_value = sanitize(value);
            if (safe_name.empty())
                return *this;
            for (auto& [hname, hvalue] : headers_) {
                if (iequals(hname, safe_name)) {
                    hvalue = safe_value;
                    return *this;
                }
            }
            headers_.emplace_back(safe_name, safe_value);
            return *this;
        }

        Response& body(std::string content) {
            body_ = std::move(content);
            return *this;
        }

        [[nodiscard]] int status() const noexcept { return code_; }
        [[nodiscard]] const std::string& body() const noexcept { return body_; }
        std::string take_body() { return std::move(body_); }

        [[nodiscard]] bool has(const std::string_view name) const noexcept {
            return std::any_of(headers_.begin(), headers_.end(), [&](const auto& h) {
                return iequals(h.first, name);
            });
        }

        [[nodiscard]] std::string head() const {
            std::string out;
            out.reserve(128);

            out += "HTTP/1.1 ";
            append_number(out, code_);
            out += ' ';
            out += reason(code_);
            out += "\r\n";

            if (!has("Server"))
                out += "Server: Vermell\r\n";
            if (!has("Content-Type"))
                out += "Content-Type: text/plain\r\n";

            for (const auto& [name, value] : headers_) {
                out += name;
                out += ": ";
                out += value;
                out += "\r\n";
            }

            if (!has("Content-Length")) {
                out += "Content-Length: ";
                append_number(out, body_.size());
                out += "\r\n";
            }

            if (!has("Accept-Ranges"))
                out += "Accept-Ranges: bytes\r\n";
            if (!has("X-Content-Type-Options"))
                out += "X-Content-Type-Options: nosniff\r\n";
            if (!has("Connection"))
                out += "Connection: keep-alive\r\n";

            out += "\r\n";
            return out;
        }

        [[nodiscard]] std::string str() const {
            std::string out = head();
            out += body_;
            return out;
        }

        [[nodiscard]] static std::string_view reason(const int code) noexcept {
            switch (code) {
                case 100: return "Continue";
                case 101: return "Switching Protocols";
                case 200: return "OK";
                case 201: return "Created";
                case 202: return "Accepted";
                case 203: return "Non-Authoritative Information";
                case 204: return "No Content";
                case 205: return "Reset Content";
                case 206: return "Partial Content";
                case 300: return "Multiple Choices";
                case 301: return "Moved Permanently";
                case 302: return "Found";
                case 303: return "See Other";
                case 304: return "Not Modified";
                case 307: return "Temporary Redirect";
                case 308: return "Permanent Redirect";
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 402: return "Payment Required";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 405: return "Method Not Allowed";
                case 406: return "Not Acceptable";
                case 408: return "Request Timeout";
                case 409: return "Conflict";
                case 410: return "Gone";
                case 411: return "Length Required";
                case 413: return "Payload Too Large";
                case 414: return "URI Too Long";
                case 415: return "Unsupported Media Type";
                case 418: return "I'm a teapot";
                case 422: return "Unprocessable Content";
                case 429: return "Too Many Requests";
                case 431: return "Request Header Fields Too Large";
                case 500: return "Internal Server Error";
                case 501: return "Not Implemented";
                case 502: return "Bad Gateway";
                case 503: return "Service Unavailable";
                case 504: return "Gateway Timeout";
                default:  return "OK";
            }
        }

    private:
        static bool iequals(const std::string_view a, const std::string_view b) noexcept {
            return a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
                       return std::tolower(static_cast<unsigned char>(x))
                            == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        static std::string sanitize(const std::string_view in) {
            const auto dirty = [](const unsigned char c) { return c < 0x20 || c == 0x7f; };
            if (std::none_of(in.begin(), in.end(), [&](const char c) { return dirty(static_cast<unsigned char>(c)); }))
                return std::string(in);

            std::string out;
            out.reserve(in.size());
            for (const char c : in) {
                if (dirty(static_cast<unsigned char>(c)))
                    break;
                out.push_back(c);
            }
            return out;
        }

        template <class T>
        static void append_number(std::string& out, const T value) {
            char buf[24];
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, value);
            if (ec == std::errc{})
                out.append(buf, ptr - buf);
        }

        int code_ = 200;
        std::vector<std::pair<std::string, std::string>> headers_;
        std::string body_;
    };

    struct WireResponse {
        std::string head;
        std::string body;
    };

} // namespace vermell::http

#endif // VERMELL_HTTP_RESPONSE_HPP
