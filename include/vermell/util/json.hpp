#ifndef JSON_HPP
#define JSON_HPP

//
// vermell::Json — DOM (tree) JSON value with strict parsing (RFC 8259) and
// correct, efficient serialization.
//
// Replaces the legacy string-concatenation JSON_t (kept at the bottom as a
// thin, now safe, compatibility facade): values keep their native type
// (numbers, booleans, null), strings are properly escaped, nesting is a
// real tree and every resource is owned by RAII members — no leaks, no
// out-of-bounds games.
//
//   const auto dev = vermell::Json::object({
//       {"lang",  "c++"},
//       {"level", 20},                                // number, not "20"
//       {"tags",  vermell::Json::array({"web", "http"})},
//   });
//   web.json(dev.dump());                             // serialize
//
//   const auto body = vermell::Json::parse(web.body.raw()); // parse
//   if (!body) return web.json(R"({"error":"invalid json"})", 400);
//   const vermell::Json* id = body->at("id");            // nullptr when absent
//

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vermell {

    class Json {
    public:
        using array_t  = std::vector<Json>;
        using object_t = std::vector<std::pair<std::string, Json>>; // insertion order kept

        enum class Type { Null, Bool, Int, Double, String, Array, Object };

        // ---- construction ------------------------------------------------

        Json() = default; // null
        Json(std::nullptr_t) : value_(nullptr) {}
        Json(const bool v) : value_(v) {}

        template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
        Json(const T v) : value_(static_cast<std::int64_t>(v)) {}

        template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
        Json(const T v) : value_(static_cast<double>(v)) {}

        Json(const char* v) : value_(std::string(v == nullptr ? "" : v)) {}
        Json(const std::string_view v) : value_(std::string(v)) {}
        Json(const std::string& v) : value_(v) {}
        Json(std::string&& v) noexcept : value_(std::move(v)) {}
        Json(array_t v) : value_(std::move(v)) {}
        Json(object_t v) : value_(std::move(v)) {}

        static Json array(std::initializer_list<Json> items) { return Json(array_t(items)); }
        static Json object(std::initializer_list<std::pair<std::string, Json>> items) {
            return Json(object_t(items));
        }

        // ---- inspection ----------------------------------------------------

        [[nodiscard]] Type type() const noexcept { return static_cast<Type>(value_.index()); }

        [[nodiscard]] bool is_null()   const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
        [[nodiscard]] bool is_bool()   const noexcept { return std::holds_alternative<bool>(value_); }
        [[nodiscard]] bool is_int()    const noexcept { return std::holds_alternative<std::int64_t>(value_); }
        [[nodiscard]] bool is_double() const noexcept { return std::holds_alternative<double>(value_); }
        [[nodiscard]] bool is_number() const noexcept { return is_int() || is_double(); }
        [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
        [[nodiscard]] bool is_array()  const noexcept { return std::holds_alternative<array_t>(value_); }
        [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<object_t>(value_); }

        // Typed access with fallback: the fallback is returned when the value
        // holds a different type (as_double() also promotes ints).
        [[nodiscard]] bool as_bool(const bool fallback = false) const noexcept {
            if (const auto* p = std::get_if<bool>(&value_)) return *p;
            return fallback;
        }
        [[nodiscard]] std::int64_t as_int(const std::int64_t fallback = 0) const noexcept {
            if (const auto* p = std::get_if<std::int64_t>(&value_)) return *p;
            return fallback;
        }
        [[nodiscard]] double as_double(const double fallback = 0.0) const noexcept {
            if (const auto* p = std::get_if<double>(&value_)) return *p;
            if (const auto* p = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*p);
            return fallback;
        }
        [[nodiscard]] std::string_view as_string(const std::string_view fallback = "") const noexcept {
            if (const auto* p = std::get_if<std::string>(&value_)) return *p;
            return fallback;
        }
        [[nodiscard]] const array_t& as_array() const noexcept {
            if (const auto* p = std::get_if<array_t>(&value_)) return *p;
            static const array_t empty{};
            return empty;
        }
        [[nodiscard]] const object_t& as_object() const noexcept {
            if (const auto* p = std::get_if<object_t>(&value_)) return *p;
            static const object_t empty{};
            return empty;
        }

        // Element count for arrays/objects/strings, 0 otherwise.
        [[nodiscard]] size_t size() const noexcept {
            if (const auto* p = std::get_if<array_t>(&value_)) return p->size();
            if (const auto* p = std::get_if<object_t>(&value_)) return p->size();
            if (const auto* p = std::get_if<std::string>(&value_)) return p->size();
            return 0;
        }

        // Lookup helpers: nullptr when the key/index is absent or the value
        // is not an object/array. The pointer stays valid while this Json is.
        [[nodiscard]] const Json* at(const std::string_view key) const noexcept {
            if (const auto* obj = std::get_if<object_t>(&value_))
                for (const auto& [k, v] : *obj)
                    if (k == key) return &v;
            return nullptr;
        }
        [[nodiscard]] const Json* at(const size_t index) const noexcept {
            if (const auto* arr = std::get_if<array_t>(&value_); arr != nullptr && index < arr->size())
                return &(*arr)[index];
            return nullptr;
        }

        // ---- serialization ---------------------------------------------------

        [[nodiscard]] std::string dump() const {
            std::string out;
            out.reserve(128);
            dump_to(out);
            return out;
        }

        // ---- parsing ----------------------------------------------------------

        // Strict RFC 8259 parse. std::nullopt on any syntax error.
        [[nodiscard]] static std::optional<Json> parse(std::string_view text);

    private:
        // The Type enum mirrors this alternative order.
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, array_t, object_t> value_{nullptr};

        static void escape_string(const std::string_view text, std::string& out) {
            constexpr char HEX[] = "0123456789abcdef";
            out += '"';
            for (const char ch : text) {
                const auto c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20) { // remaining control chars: \u00XX
                            out += "\\u00";
                            out += HEX[c >> 4];
                            out += HEX[c & 0x0F];
                        } else {
                            out += static_cast<char>(c); // UTF-8 passes through
                        }
                }
            }
            out += '"';
        }

        void dump_to(std::string& out) const {
            switch (type()) {
                case Type::Null:
                    out += "null";
                    return;
                case Type::Bool:
                    out += std::get<bool>(value_) ? "true" : "false";
                    return;
                case Type::Int: {
                    char buf[20]; // int64 needs at most 19 digits + sign
                    const auto res = std::to_chars(buf, buf + sizeof buf, std::get<std::int64_t>(value_));
                    out.append(buf, res.ptr);
                    return;
                }
                case Type::Double: {
                    const double d = std::get<double>(value_);
                    if (!std::isfinite(d)) { // JSON cannot express NaN/Inf
                        out += "null";
                        return;
                    }
                    char buf[32];
                    const auto res = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::general);
                    if (res.ec == std::errc{})
                        out.append(buf, res.ptr);
                    else
                        out += "null";
                    return;
                }
                case Type::String:
                    escape_string(std::get<std::string>(value_), out);
                    return;
                case Type::Array: {
                    out += '[';
                    bool first = true;
                    for (const auto& item : std::get<array_t>(value_)) {
                        if (!first) out += ',';
                        first = false;
                        item.dump_to(out);
                    }
                    out += ']';
                    return;
                }
                case Type::Object: {
                    out += '{';
                    bool first = true;
                    for (const auto& [key, item] : std::get<object_t>(value_)) {
                        if (!first) out += ',';
                        first = false;
                        escape_string(key, out);
                        out += ':';
                        item.dump_to(out);
                    }
                    out += '}';
                    return;
                }
            }
        }
    };

    namespace detail {

        // Strict RFC 8259 recursive-descent parser over a string_view.
        class JsonParser {
        public:
            explicit JsonParser(const std::string_view src) noexcept : src_(src) {}

            std::optional<Json> run() {
                skip_ws();
                auto value = parse_value(0);
                if (!value.has_value())
                    return std::nullopt;
                skip_ws();
                if (pos_ != src_.size())
                    return std::nullopt; // trailing garbage after the top-level value
                return value;
            }

        private:
            // Deep trees would overflow the stack; 256 levels is far beyond
            // anything a real API should send.
            static constexpr unsigned MAX_DEPTH = 256;

            std::string_view src_;
            size_t pos_ = 0;

            [[nodiscard]] char peek() const noexcept {
                return pos_ < src_.size() ? src_[pos_] : '\0';
            }
            [[nodiscard]] bool at_digit() const noexcept {
                return std::isdigit(static_cast<unsigned char>(peek())) != 0;
            }
            void skip_ws() noexcept {
                while (pos_ < src_.size()) {
                    const char c = src_[pos_];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                        break;
                    ++pos_;
                }
            }
            bool consume(const char c) noexcept {
                if (peek() != c) return false;
                ++pos_;
                return true;
            }
            bool consume_lit(const std::string_view lit) noexcept {
                if (src_.substr(pos_, lit.size()) != lit) return false;
                pos_ += lit.size();
                return true;
            }

            std::optional<Json> parse_value(const unsigned depth) {
                if (depth > MAX_DEPTH)
                    return std::nullopt;
                switch (peek()) {
                    case 'n':
                        if (consume_lit("null")) return Json(nullptr);
                        return std::nullopt;
                    case 't':
                        if (consume_lit("true")) return Json(true);
                        return std::nullopt;
                    case 'f':
                        if (consume_lit("false")) return Json(false);
                        return std::nullopt;
                    case '"': {
                        auto text = parse_string();
                        if (!text.has_value()) return std::nullopt;
                        return Json(std::move(*text));
                    }
                    case '[': return parse_array(depth);
                    case '{': return parse_object(depth);
                    default:  return parse_number();
                }
            }

            std::optional<Json> parse_array(const unsigned depth) {
                ++pos_; // '['
                skip_ws();
                Json::array_t items;
                if (consume(']'))
                    return Json(std::move(items));
                while (true) {
                    skip_ws();
                    auto item = parse_value(depth + 1);
                    if (!item.has_value()) return std::nullopt;
                    items.push_back(std::move(*item));
                    skip_ws();
                    if (consume(',')) continue;
                    if (consume(']')) break;
                    return std::nullopt;
                }
                return Json(std::move(items));
            }

            std::optional<Json> parse_object(const unsigned depth) {
                ++pos_; // '{'
                skip_ws();
                Json::object_t members;
                if (consume('}'))
                    return Json(std::move(members));
                while (true) {
                    skip_ws();
                    if (peek() != '"') return std::nullopt;
                    auto key = parse_string();
                    if (!key.has_value()) return std::nullopt;
                    skip_ws();
                    if (!consume(':')) return std::nullopt;
                    skip_ws();
                    auto value = parse_value(depth + 1);
                    if (!value.has_value()) return std::nullopt;
                    members.emplace_back(std::move(*key), std::move(*value));
                    skip_ws();
                    if (consume(',')) continue;
                    if (consume('}')) break;
                    return std::nullopt;
                }
                return Json(std::move(members));
            }

            std::optional<std::string> parse_string() {
                if (!consume('"')) return std::nullopt;
                std::string out;
                while (true) {
                    if (pos_ >= src_.size())
                        return std::nullopt; // unterminated string
                    const char c = src_[pos_++];
                    if (c == '"')
                        return out;
                    if (c == '\\') {
                        if (pos_ >= src_.size()) return std::nullopt;
                        switch (src_[pos_++]) {
                            case '"':  out += '"';  break;
                            case '\\': out += '\\'; break;
                            case '/':  out += '/';  break;
                            case 'b':  out += '\b'; break;
                            case 'f':  out += '\f'; break;
                            case 'n':  out += '\n'; break;
                            case 'r':  out += '\r'; break;
                            case 't':  out += '\t'; break;
                            case 'u': {
                                const auto cp = parse_codepoint();
                                if (!cp.has_value()) return std::nullopt;
                                encode_utf8(out, *cp);
                                break;
                            }
                            default: return std::nullopt; // unknown escape
                        }
                    }
                    else if (static_cast<unsigned char>(c) < 0x20) {
                        return std::nullopt; // raw control chars are invalid JSON
                    }
                    else {
                        out += c;
                    }
                }
            }

            // \uXXXX with surrogate-pair joining; result is a Unicode scalar.
            std::optional<std::uint32_t> parse_codepoint() {
                const auto high = parse_hex4();
                if (!high.has_value()) return std::nullopt;
                std::uint32_t cp = *high;
                if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate: a low one must follow
                    if (src_.substr(pos_, 2) != "\\u") return std::nullopt;
                    pos_ += 2;
                    const auto low = parse_hex4();
                    if (!low.has_value() || *low < 0xDC00 || *low > 0xDFFF)
                        return std::nullopt;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return std::nullopt; // lone low surrogate
                }
                return cp;
            }

            std::optional<std::uint32_t> parse_hex4() noexcept {
                if (pos_ + 4 > src_.size()) return std::nullopt;
                std::uint32_t value = 0;
                for (int i = 0; i < 4; ++i) {
                    const char c = src_[pos_++];
                    value <<= 4;
                    if (c >= '0' && c <= '9')      value |= static_cast<std::uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
                    else return std::nullopt;
                }
                return value;
            }

            static void encode_utf8(std::string& out, const std::uint32_t cp) {
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            // number := '-'? ('0' | [1-9][0-9]*) ('.' [0-9]+)? ([eE] [+-]? [0-9]+)?
            std::optional<Json> parse_number() {
                const size_t begin = pos_;
                consume('-');

                if (consume('0')) {
                    if (at_digit()) return std::nullopt; // leading zeros are invalid
                } else {
                    if (!at_digit()) return std::nullopt;
                    while (at_digit()) ++pos_;
                }

                bool real = false;
                if (consume('.')) {
                    real = true;
                    if (!at_digit()) return std::nullopt;
                    while (at_digit()) ++pos_;
                }
                if (peek() == 'e' || peek() == 'E') {
                    real = true;
                    ++pos_;
                    if (peek() == '+' || peek() == '-') ++pos_;
                    if (!at_digit()) return std::nullopt;
                    while (at_digit()) ++pos_;
                }

                const std::string_view token = src_.substr(begin, pos_ - begin);
                if (!real) {
                    std::int64_t v{};
                    if (std::from_chars(token.data(), token.data() + token.size(), v).ec == std::errc{})
                        return Json(v);
                    // does not fit in int64: degrade to double below
                }
                // std::from_chars for floating point is unavailable on older
                // libc++ (macOS < 26); strtod parses the same general-format
                // token (the grammar above already validated the shape) and
                // exists everywhere.
                double d{};
                {
                    const std::string tmp(token);
                    char* end = nullptr;
                    d = std::strtod(tmp.c_str(), &end);
                    if (end == nullptr
                        || static_cast<size_t>(end - tmp.c_str()) != token.size()
                        || !std::isfinite(d))
                        return std::nullopt;
                }
                return Json(d);
            }
        };

    } // namespace detail

    inline std::optional<Json> Json::parse(const std::string_view text) {
        return detail::JsonParser(text).run();
    }

} // namespace vermell


// ---- legacy facade -----------------------------------------------------------
//
// JSON_s keeps the old alternating key/value construction, now implemented
// safely on top of vermell::Json: proper escaping, native number/bool types,
// real nesting and no out-of-bounds reads (an odd trailing key now yields a
// null value instead of reading past the list). Prefer vermell::Json in new code.
//
//   JSON_s dev = { "name", "kevin", "age", 100, "id", someJson };
//   web.json(dev());
//
class JsonStringBuilder {
public:
    [[maybe_unused]] JsonStringBuilder(std::initializer_list<vermell::Json> items) {
        vermell::Json::object_t root;
        auto it = items.begin();
        while (it != items.end()) {
            std::string key(it->as_string());
            vermell::Json value = nullptr;
            if (++it != items.end()) {
                value = *it;
                ++it;
            }
            root.emplace_back(std::move(key), std::move(value));
        }
        root_ = vermell::Json(std::move(root));
    }

    [[nodiscard]] std::string json() const { return root_.dump(); }
    [[nodiscard]] std::string operator()() const { return root_.dump(); }
    [[nodiscard]] const vermell::Json& tree() const noexcept { return root_; }

    // Real nesting: a JSON_s can be used as a value inside another one.
    operator vermell::Json() const { return root_; }

private:
    vermell::Json root_;
};

template <class... P> using JSON_t = JsonStringBuilder; // legacy alias (params ignored)
using JSON_s = JsonStringBuilder;

#endif // ! JSON_HPP
