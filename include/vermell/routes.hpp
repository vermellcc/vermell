#pragma once
#ifndef ROUTES_DEMAND_HPP
#define ROUTES_DEMAND_HPP

#include <string>
#include <string_view>

#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <initializer_list>
#include <functional>
#include <future>
#include <unordered_map>


#include "request/request.hpp"
#include "http/message.hpp"
#include "util/mime_types.hpp"

#include "util/basic_render.h"
#include "util/ver_reader.h"
#include "util/data_render.h"
#include "util/json.hpp"
#include "util/render_security.h"

using std::string;

// Route-map key: path + separator + method. The separator is the control
// character '\x1f': no valid request target (is_valid_target rejects every
// byte <= 0x20) and no method token (RFC 9110 tchar) can contain it, so two
// distinct registrations can never map to the same key. The legacy "path +
// method" concatenation collided ("/x" with GET and "/xG" with "ET" both
// produced "/xGET"), which let one route shadow another.
[[nodiscard]] inline string route_key(const std::string_view path, const std::string_view method) {
    string key;
    key.reserve(path.size() + method.size() + 1);
    key.append(path);
    key.push_back('\x1f');
    key.append(method);
    return key;
}

// A route path is "parameterized" when it carries a named segment (":name")
// or a catch-all ("*"). Such routes are matched by segments, not by the exact
// string key above.
[[nodiscard]] inline bool is_pattern_route(const std::string_view path) noexcept {
    return path.find(':') != std::string_view::npos
        || path.find('*') != std::string_view::npos;
}

// Splits a request path into its non-empty segments: "/users/123" becomes
// {"users", "123"} and "/" becomes {} (leading/trailing and doubled slashes
// are ignored, mirroring how most routers normalize paths).
[[nodiscard]] inline std::vector<string> split_path(const std::string_view path) {
    std::vector<string> out;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string_view seg = path.substr(
            pos, slash == std::string_view::npos ? path.size() - pos : slash - pos);
        if (!seg.empty())
            out.emplace_back(seg);
        if (slash == std::string_view::npos)
            break;
        pos = slash + 1;
    }
    return out;
}

// Matches a compiled pattern (literals, ":name", "*") against the request's
// segments. On success it returns the captured parameters: ":name" yields a
// percent-decoded value, and a trailing "*" yields the joined remainder under
// the key "*". Returns std::nullopt when the pattern does not match.
[[nodiscard]] inline std::optional<std::vector<std::pair<string, string>>>
match_pattern(const std::vector<string>& pattern, const std::vector<string>& path) {
    std::vector<std::pair<string, string>> params;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const string& seg = pattern[i];
        if (seg == "*") {
            string remainder;
            for (size_t j = i; j < path.size(); ++j) {
                if (j != i)
                    remainder += '/';
                remainder += path[j];
            }
            params.emplace_back("*", vermell::http::percent_decode(remainder));
            return params;
        }
        if (i >= path.size())
            return std::nullopt;
        if (seg.size() > 1 && seg.front() == ':') {
            params.emplace_back(seg.substr(1), vermell::http::percent_decode(path[i]));
        } else if (seg != path[i]) {
            return std::nullopt;
        }
    }
    if (pattern.size() != path.size())
        return std::nullopt;
    return params;
}


struct Route {
private:
    string route_name{},
           route_type{};
    bool lock{};
    int timeline{};

public:
    [[maybe_unused]] explicit Route(string _route) : route_name(std::move(_route)), lock(false), timeline(0) {}
    Route() = default;

    inline Route& operator = (string _val) { route_name = std::move(_val); return *this; }
    inline void setType(string _type) { route_type = std::move(_type); }
    [[nodiscard]] inline string getName () const noexcept { return route_name; }
    [[nodiscard]] inline string getType () const noexcept { return route_type; }
};


template<class...P>
struct Headers_t {
    Headers_t()= default;
    [[maybe_unused]] Headers_t(std::initializer_list<P...>list): body(list) {}
    vector<string> body;
    string generate(){
        string response;
        for (auto &it : body) {
            response += it + "\n";
        }
        return response;
    }
};
using HEADERS = Headers_t<string>;


class Query {

    bool next_enable  { false};
    long time_key    {0};

    string response {"default"}, headers, guardMsg{};
    vermell::http::WireResponse last;
    // Route parameters captured by a parameterized route ("/users/:id").
    std::vector<std::pair<string, string>> route_params_{};

    public:
    Query()  = default;
    ~Query() = default;

    Request body; // property access
    Request query;

    // Security policy applied by the file-rendering methods below. The
    // router injects the server-wide value before the middlewares run.
    vermell::RenderSecurity render_sec{};

    void setRenderSecurity(const vermell::RenderSecurity& sec) noexcept { render_sec = sec; }

    // Route parameters from a parameterized route. setRouteParams() is called
    // by the router before the middleware runs; handlers read them through
    // param("id").value / param("id").as<int>() or iterate params().
    void setRouteParams(std::vector<std::pair<string, string>> p) noexcept { route_params_ = std::move(p); }
    [[nodiscard]] param_box param(const string& name) const {
        for (const auto& [k, v] : route_params_)
            if (k == name)
                return param_box(k, v);
        return param_box(name, string{});
    }
    [[nodiscard]] Param_t params() const {
        Param_t p;
        p.setContent(route_params_);
        return p;
    }

    [[nodiscard]] vermell::http::WireResponse takeData() noexcept;
    [[nodiscard]] bool    getNext()    const noexcept;

    [[maybe_unused]] void    next()       noexcept;

    void    lock()        noexcept;
    [[nodiscard]] long    getTimeKey()  const noexcept;
    [[nodiscard]] string  getGuardMsg() const noexcept;

    [[maybe_unused]] void    setHeaders(HEADERS) noexcept;
    [[maybe_unused]] void    setHeaders(const string&) noexcept;
    [[maybe_unused]] void    guard(const long&, string custom_msg="") noexcept;

    //  PARAMS:  CONTENT OPTIONAL CALLBACK

    [[maybe_unused]] void    json(const string&, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    html(const string&, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    send(const string&, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    readFile(const string&,const string&, const std::function<void()>& callback=[]()->void{}) noexcept;
    // Serves a file detecting the Content-Type from its extension.
    [[maybe_unused]] void    readFile(const string&, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    file(const string&, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    compose(const string&,int, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    render(const string&, const std::function<dataRender(dataRender&)>& callback=[](dataRender&)->dataRender{ return dataRender(nullptr); }) noexcept;

    // PARAMS:  CONTENT  STATUS OPTIONAL CALLBACK
    [[maybe_unused]] void    json(const string&, int, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    html(const string&, int, const std::function<void()>& callback=[]()->void{}) noexcept;
    [[maybe_unused]] void    send(const string&, int, const std::function<void()>& callback=[]()->void{}) noexcept;

};


template <class... P>
struct Core_init_t  {

    [[maybe_unused]] Core_init_t(std::initializer_list<P...> list) :
                functions(std::move(list)) {}
    [[maybe_unused]] Core_init_t() = default;

    std::vector<P...> functions;

    [[nodiscard]] [[maybe_unused]] inline size_t size() const noexcept { return functions.size(); }

     std::pair<vermell::http::WireResponse, std::chrono::duration<double>::rep>
     execute(const vermell::http::Message &message,
             std::unique_ptr<string> &guard_msg,
             const vermell::RenderSecurity& render_sec = {},
             std::vector<std::pair<string, string>> route_params = {}) {
        Query remote_control;
        remote_control.setRenderSecurity(render_sec);
        remote_control.setRouteParams(std::move(route_params));
        remote_control.body.consume(message);

        for (size_t i = 0; i < functions.size(); i++) {
            remote_control.lock();
            functions[i](remote_control);
            if (remote_control.getNext())
                continue;
            break;
        }

        vermell::http::WireResponse response = remote_control.takeData();
        long time_key = remote_control.getTimeKey();
        if (time_key > 0)
            guard_msg = std::make_unique<string>(remote_control.getGuardMsg());

        return {std::move(response), time_key};
    }
};

using MiddlewareList = Core_init_t<std::function<void(Query&)>> ;

struct listen_routes {
    listen_routes(string _route, MiddlewareList _funcs, string _type) : middlewares(std::move(_funcs)){
        route = std::move(_route);
        route.setType(std::move(_type));
        time_key = 0;
    }

    Route route;
    MiddlewareList middlewares;
    std::chrono::duration<double>::rep time_key;
    std::chrono::time_point<std::chrono::system_clock> time_point{};
    std::unique_ptr<string> guardRouteMsg = nullptr;
    // Guards time_key / time_point / guardRouteMsg: requests are served concurrently.
    std::mutex route_mutex;
};

// ---- route table ----------------------------------------------------------
//
// The route map is keyed by "path\x1fmethod" (see route_key). Transparent
// hashing lets the hot lookup path find() with a std::string_view built in a
// stack buffer — no per-request key allocation. std::hash<std::string> and
// std::hash<std::string_view> are byte-identical on the same characters, so
// hashes computed for stored std::string keys and looked-up string_views
// always agree.
struct RouteMapHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
    [[nodiscard]] size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct RouteMapEq {
    using is_transparent = void;
    [[nodiscard]] bool operator()(const std::string& a, const std::string& b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(const std::string& a, std::string_view b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(std::string_view a, const std::string& b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

using RoutesMap = std::unordered_map<string, std::unique_ptr<listen_routes>,
                                    RouteMapHash, RouteMapEq>;

// Parameterized routes (":name" and "*") are kept separately from the exact
// string-keyed map: they are matched by segment scan in ExecuteRoute.
struct PatternRoute {
    std::vector<string> segments;         // literals, ":name" or a final "*"
    std::unique_ptr<listen_routes> impl;  // middlewares + guard state
};
using PatternRoutes = std::vector<PatternRoute>;

struct Route_t {
    Route_t(string _r, MiddlewareList _m, string _t) : route(std::move(_r)), middlewares(std::move(_m)), type(std::move(_t)) { }
    Route_t()= default;
    string route;
    MiddlewareList middlewares;
    string type;

};




#endif /*ROUTES_DEMAND_HPP */
