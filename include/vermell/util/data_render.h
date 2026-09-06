#ifndef DATA_RENDER_HPP
#define DATA_RENDER_HPP

#include <functional>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "notify.h"
#include "local_utility.h"
#include "render_security.h"
#include "secure_render.h"

using std::string;

class dataRender {

        std::vector<std::tuple<std::string, std::string>> variables = {};

public:

    explicit dataRender(const std::function<dataRender(dataRender &data)>& _parser) {
        const dataRender temp_render = _parser(*this);
        variables = temp_render.getVariables(); // returns by value: copy elision, no std::move
    }
    ~dataRender() = default;

    [[nodiscard]] inline std::vector<std::tuple<std::string, std::string>> getVariables() const
    { return variables; }

     void operator()(const string& nombre, const string& valor) {
        variables.emplace_back(nombre,valor);
    }

     std::string render(const string& path, const vermell::RenderSecurity& sec = {})  {

        if(!vermell::srender::is_within(vermell::effective_root(sec), path))
            return notify_html::noFIle(path);

        auto read = vermell::srender::read_bounded(path, sec.max_file_bytes);
        if (read.err != vermell::srender::ReadErr::Ok)
            return notify_html::noFIle(path);

        string body = std::move(read.data);

        for (size_t iterator = 0; iterator < variables.size(); iterator++)
            body = body_tratament(body);
        return body;
    }

private:
     // Replaces the first "[[name]]" marker with its variable. A body with
     // no markers is returned unchanged (the legacy code read and wrote
     // through uninitialized coordinates in that case).
     string body_tratament(const string &body) {

         size_t open = string::npos;
         size_t close = string::npos;

        for (size_t iter = 0; iter + 1 < body.length(); iter++) {
            if (body[iter] == OPEN_DATA[0] && body[iter + 1] == OPEN_DATA[1])
                open = iter;
            else if (body[iter] == CLOSE_DATA[0] && body[iter + 1] == CLOSE_DATA[1]) {
                close = iter;
                break;
            }
        }

        if (open == string::npos && close == string::npos)
            return body;
        if (open == string::npos || close == string::npos || close < open)
            return notify_html::noSafeData();

         const string name = body.substr(open + 2, close - open - 2);

         string data = "error";
         for (auto & variable : variables) {
             if (std::get<0>(variable) == name) {
                 data = std::get<1>(variable);
                 break;
             }
         }

         // The value is HTML-escaped on substitution: a request-derived
         // variable can never inject markup/scripts into the rendered page.
        return body.substr(0, open) + vermell::srender::escape_html(data) + body.substr(close + 2);
    }
};

#endif // ! DATA_RENDER_HPP
