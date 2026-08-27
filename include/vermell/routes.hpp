#pragma once
#ifndef ROUTES_DEMAND_HPP
#define ROUTES_DEMAND_HPP

#include <string>
#include <string_view>

#include <memory>
#include <mutex>
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

    public:
    Query()  = default;
    ~Query() = default;

    Request body; // property access
    Request query;

    // Security policy applied by the file-rendering methods below. The
    // router injects the server-wide value before the middlewares run.
    vermell::RenderSecurity render_sec{};

    void setRenderSecurity(const vermell::RenderSecurity& sec) noexcept { render_sec = sec; }

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
             const vermell::RenderSecurity& render_sec = {}) {
        Query remote_control;
        remote_control.setRenderSecurity(render_sec);
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

struct Route_t {
    Route_t(string _r, MiddlewareList _m, string _t) : route(std::move(_r)), middlewares(std::move(_m)), type(std::move(_t)) { }
    Route_t()= default;
    string route;
    MiddlewareList middlewares;
    string type;

};




#endif /*ROUTES_DEMAND_HPP */
