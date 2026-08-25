#ifndef PARAMETERS_HPP
#define PARAMETERS_HPP

#include <charconv>
#include <chrono>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "../http/response.hpp"

using std::string;
using std::vector;

class param_box {
    std::pair<string, string> _body;

public:
    string name;
    string value;

    param_box(string, string);
    explicit param_box(std::pair<string, string> content) : _body(content) {
        name  = std::move(content.first);
        value = std::move(content.second);
    }

    // Typed conversion: params.get("id").as<int>() / as<double>() / as<bool>() ...
    template <class T>
    [[nodiscard]] T as(T fallback = {}) const {
        if constexpr (std::is_same_v<T, string>) {
            return value;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return value;
        } else if constexpr (std::is_same_v<T, const char*>) {
            return value.c_str();
        } else if constexpr (std::is_same_v<T, bool>) {
            return value == "1" || value == "true" || value == "on" || value == "yes"
                    ? true
                    : (value == "0" || value == "false" || value == "off" || value == "no" ? false : fallback);
        } else if constexpr (std::is_arithmetic_v<T>) {
            T out{};
            const char* first = value.data();
            const char* last  = first + value.size();
            if (const auto [ptr, ec] = std::from_chars(first, last, out);
                ec == std::errc{} && ptr == last)
                return out;
            return fallback;
        } else {
            static_assert(std::is_arithmetic_v<T>, "param_box::as<T> supports string, string_view and arithmetic types");
        }
    }

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};


class Param_t {
    vector<std::pair<string, string>> _list;

public:
    Param_t() = default;
    ~Param_t() { _list.clear(); }

    [[maybe_unused]] explicit Param_t(vector<std::pair<string, string>> list) : _list(std::move(list)) {}

    param_box operator[](int);

    void setContent(const vector<std::pair<string, string>>&);

    [[maybe_unused]] inline void clear() { _list.clear(); }
    [[maybe_unused]] [[nodiscard]] inline bool empty() const { return _list.empty(); }

    [[maybe_unused]] bool exist(const string&);
    [[nodiscard]] bool exist(std::string_view name) const;

    [[maybe_unused]] param_box get(const string&);

    // Value of `name` or `fallback` when missing.
    [[maybe_unused]] [[nodiscard]] string value_or(std::string_view name, string fallback) const;

    [[nodiscard]] inline vector<std::pair<string, string>> toArray() const noexcept { return _list; }
    [[nodiscard]] inline size_t size() const noexcept { return _list.size(); }
};


struct utility_t {
    // Builds the HTTP/1.1 wire response delivered to the client.
    //
    //   - `body` travels by value and is moved into the response: renders
    //     and file reads reach the socket with zero extra copies.
    //   - `type`/`headers` are views: string literals and vermell::mime::of()
    //     results never materialize a temporary std::string.
    //   - `status` is a plain int: no string round-trips at the call sites.
    //
    // Every header is forwarded to Response::set, which strips CR/LF, so a
    // tainted value can never split the response (header injection).
    [[nodiscard]] static vermell::http::WireResponse prepare(string body,
                                                             const std::string_view type,
                                                             const std::string_view headers,
                                                             const int status = 200) {
        vermell::http::Response response;
        response.status(status).type(type);
        apply_headers(response, headers);
        response.body(std::move(body));

        vermell::http::WireResponse out;
        out.head = response.head();
        out.body = response.take_body();
        return out;
    }

    [[maybe_unused]] static vermell::http::WireResponse guard_route(
        const std::chrono::duration<double>::rep seconds, string msg = "") {

        string body = R"lit({"message":")lit"
            + string(not msg.empty()
                         ? json_escape(msg)
                         : "wait, this route has a " + std::to_string(static_cast<int>(seconds)) + " second cooldown")
            + R"lit("})lit";

        return prepare(std::move(body), "application/json", "", 401);
    }

    // Bridge for the file-rendering family, which still reports its status
    // as a string ("200", "404", ...). Unparseable input yields 0.
    [[maybe_unused]] static int toInt(const string& data) {
        int result = 0;
        const char* first = data.data();
        const char* last  = first + data.size();
        if (const auto [ptr, ec] = std::from_chars(first, last, result);
            ec == std::errc{} && ptr == last)
            return result;
        return 0;
    }

private:
    // Escapes a value for embedding inside a JSON string literal: a custom
    // guard message can carry '"' or control characters and must never
    // break out of the {"message":"..."} envelope.
    static std::string json_escape(const std::string_view in) {
        constexpr char HEX[] = "0123456789abcdef";
        std::string out;
        out.reserve(in.size());
        for (const char ch : in) {
            switch (ch) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (const auto c = static_cast<unsigned char>(ch); c < 0x20) {
                        out += "\\u00";
                        out += HEX[c >> 4];
                        out += HEX[c & 0x0F];
                    } else {
                        out += ch;
                    }
            }
        }
        return out;
    }

    // The legacy API hands over headers as a raw "Name: value\n..." block;
    // it is parsed in place with string_views: no per-line allocations.
    static void apply_headers(vermell::http::Response& response, const std::string_view headers) {
        size_t pos = 0;
        while (pos < headers.size()) {
            const size_t eol = headers.find('\n', pos);
            const std::string_view line = headers.substr(
                pos, eol == std::string_view::npos ? eol : eol - pos);

            if (const size_t colon = line.find(':'); colon != std::string_view::npos && colon > 0) {
                std::string_view value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ')
                    value.remove_prefix(1);
                if (!value.empty() && value.back() == '\r')
                    value.remove_suffix(1);
                response.set(line.substr(0, colon), value);
            }

            if (eol == std::string_view::npos)
                break;
            pos = eol + 1;
        }
    }
};


#endif // PARAMETERS_HPP
