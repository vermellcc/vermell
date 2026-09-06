#ifndef PARAM_PROCESSING_HPP
#define PARAM_PROCESSING_HPP

constexpr auto GET_TYPE = "GET";
constexpr auto POST_TYPE = "POST";
constexpr auto PUT_TYPE  = "PUT";
constexpr auto DELETE_TYPE = "DELETE";
constexpr auto PATCH_TYPE = "PATCH";

constexpr auto COPY_TYPE = "COPY";
constexpr auto HEAD_TYPE = "HEAD";
constexpr auto OPTIONS_TYPE = "OPTIONS";
constexpr auto LINK_TYPE = "LINK";
constexpr auto UNLINK_TYPE = "UNLINK";
constexpr auto PURGE_TYPE = "PURGE";

constexpr auto X_WWW_FORM = "application/x-www-form-urlencoded";
constexpr auto MULTIPART_FORM = "multipart/form-data";
constexpr auto JSON_APP = "application/json";
constexpr auto PLAIN_TEXT = "text/plain";
constexpr auto RAW_ERROR = "data=null";
constexpr auto RAW_TARGET = "data=";

#define STR_ERR "-1"
#include <algorithm>
#include <memory>
#include <string>

#include "../request/request.hpp"
#include "../http/message.hpp"

using std::string;
using std::make_shared;
using std::shared_ptr;

class HTTP_QUERY
{
     [[maybe_unused]] size_t max_iterator{50};
public:
    HTTP_QUERY();
    ~HTTP_QUERY();

   [[nodiscard]] static string route_refactor_params(const string&);
   [[nodiscard]] static string route_refactor_params_get(const string&);
   [[nodiscard]] static string headers_from(const string&);
   [[nodiscard]] static string get_params(const string &);
   [[nodiscard]] static string x_www_form_urlencoded(const string &);
   [[nodiscard]] static string raw_form_encoded(const string &);
   [[nodiscard]] static string findContenType(const string&);
   [[nodiscard]] static string selectPerType(const string &,const string&, bool&);
   [[nodiscard]] static std::pair<string, string> route_refactor(const string&);
   [[nodiscard]] static string trim(string);
};

#endif //PARAM_PROCESSING_HPP
