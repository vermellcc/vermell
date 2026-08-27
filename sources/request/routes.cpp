#include "../../include/vermell/routes.hpp"


vermell::http::WireResponse Query::takeData() noexcept    {     return std::move(last);     }
bool    Query::getNext() const noexcept                   {     return next_enable;   }
void    Query::next()    noexcept                         {     next_enable = true;   }
void    Query::lock()    noexcept                         {     next_enable = false;  }


void Query::json(const string& _txt, const std::function<void()>& callback) noexcept {
    last = utility_t::prepare(_txt, "application/json", headers);
    callback();
}
void Query::html(const string& _txt,  const std::function<void()>& callback) noexcept {
    last = utility_t::prepare(_txt, "text/html", headers);
    callback();
}
void  Query::send(const string& _txt,  const std::function<void()>& callback) noexcept {
     last = utility_t::prepare(_txt, "text/plain", headers);
     callback();
}
void Query::json(const string& _txt, const int status, const std::function<void()>& callback) noexcept {
    last = utility_t::prepare(_txt, "application/json", headers, status);
    callback();
}
void Query::html(const string& _txt, const int status,  const std::function<void()>& callback) noexcept {
    last = utility_t::prepare(_txt, "text/html", headers, status);
    callback();
}
void  Query::send(const string& _txt,const int status,  const std::function<void()>& callback) noexcept {
     last = utility_t::prepare(_txt, "text/plain", headers, status);
     callback();
}

void  Query::readFile(const string& path,const string& type, const std::function<void()>& callback) noexcept {
    auto [data, status] = BasicRead::processing(path, render_sec);
    last = utility_t::prepare(std::move(data), type, headers, utility_t::toInt(status));
    callback();
}
void  Query::readFile(const string& path, const std::function<void()>& callback) noexcept {
    auto [data, status] = BasicRead::processing(path, render_sec);
    last = utility_t::prepare(std::move(data), vermell::mime::of(path), headers, utility_t::toInt(status));
    callback();
}
void  Query::file(const string& path, const std::function<void()>& callback) noexcept {
    auto [data, status] = BasicRead::processing(path, render_sec);
    last = utility_t::prepare(std::move(data), vermell::mime::of(path), headers, utility_t::toInt(status));
    callback();
}
void  Query::compose(const string& path, const int reserve, const std::function<void()>& callback) noexcept {
    auto [data, status] = VerReader::processing(path, reserve, render_sec);
    last = utility_t::prepare(std::move(data), vermell::mime::of(path), headers, utility_t::toInt(status));
    callback();
}

void  Query::render(const string& path, const std::function<dataRender(dataRender& data)>& callback) noexcept {
    // Stack-allocated renderer: the rendered body is a temporary and is
    // moved straight into the response (no heap, no copies, nothing to leak).
    dataRender renderer(callback);
    last = utility_t::prepare(renderer.render(path, render_sec), vermell::mime::of(path), headers);
}

void Query::setHeaders(const string& _body) noexcept {
 headers += _body + "\n";
}

void Query::setHeaders(HEADERS box) noexcept{
headers += box.generate();
}

long Query::getTimeKey() const noexcept {
    return time_key;
}
void Query::guard(const long &key, string custom_msg) noexcept {
    if(not custom_msg.empty())
      guardMsg = std::move(custom_msg);

    time_key = key;
}

string Query::getGuardMsg() const noexcept {
    return guardMsg;
}


