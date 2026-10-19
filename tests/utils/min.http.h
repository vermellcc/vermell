#pragma once
#ifndef MINVERIDIC_H
#define MINVERIDIC_H
#define CURL_STATICLIB
#include<curl/curl.h>
#include<iostream>
#include<string>
#include<utility>
#include<vector>
#include<chrono>
#include<thread>
#define PUT_TYPE "PUT"
#define POST_t "POST"
#define DELETE_TYPE "DELETE"
#define DEFAULT "00"
using std::string;using std::vector;
constexpr auto UTILS_ERROR="-[critic error]-";constexpr auto UTILS_WARNING="-[warning]-";constexpr auto UTILS_SUCCESS=0;
constexpr auto SYSTEM_DECORATOR="-[BAD SYSTEM RESPONSE]-";constexpr auto CURL_DECORATOR="-[BAD CURL RESPONSE]-";constexpr auto BAD_RESULT="-[BAD_USE]-";
constexpr auto VHTTP_ERROR=-1;constexpr auto CURL_ERROR=-2;constexpr auto HTTP_SUCCESS=0;constexpr auto UTILS_USER_ERROR=3;
template<class...C>void screen(std::ostream&o,C const&...c){((o<<c),...);}
template<typename A,class...L>struct VH{vector<L...>l;VH(std::initializer_list<L...>a):l(a){if(!l.empty()){for(auto&i:l)al.push_back(i);initial=true;}}bool initial{false};vector<A>al{};};
using VHeaders=VH<string,string>;
template<typename A,class...L>struct F{vector<L...>l;F(std::initializer_list<L...>a):l(a){}string t(){string d;size_t n=l.size();for(size_t i=0;i<n;i++){if(i)d+='&';d+=l[i];}return d;}};
typedef F<string,string>POST,PUT,GET,DELETE;
class HTTP{CURL*curl=nullptr;vector<CURLcode>pr;string URL{},r{};void prep(const string&u);CURLcode perform_with_retry();
public:
HTTP();explicit HTTP(string);~HTTP();HTTP(const HTTP&)=delete;HTTP&operator=(const HTTP&)=delete;
static size_t cb(const void*,size_t,size_t,string*);
int getSimple(const string&e=DEFAULT);int getBase(GET*f,const string&e=DEFAULT,bool h=false,const VHeaders*b=nullptr);int get(GET&f,const VHeaders&h,const string&e=DEFAULT);int getVHeaders(const VHeaders&h,const string&e=DEFAULT);
int postSimple(const string&e=DEFAULT);int postVHeaders(const VHeaders&h,const string&e=DEFAULT);int post(POST&f,const string&e=DEFAULT,const string&t=DEFAULT);int post(POST*f,const VHeaders&h,const string&e=DEFAULT,const string&t=DEFAULT);
int put(PUT&f,const string&e=DEFAULT);int put(PUT&f,const VHeaders&h,const string&e=DEFAULT);int Delete(DELETE&f,const string&e=DEFAULT);int Delete(DELETE&f,const VHeaders&h,const string&e=DEFAULT);int custom(POST&f,const string&t,const string&e=DEFAULT);int custom(POST&f,const VHeaders&h,const string&t,const string&e=DEFAULT);
static curl_slist*makeVHeaders(const vector<string>&v);void setUrl(const string&u);int setVHeaders(const VHeaders&h);[[nodiscard]]string Response()const;static string without(string t,char k);string genPerfomList();
};
inline HTTP::HTTP(){curl=curl_easy_init();}
inline HTTP::HTTP(string u):URL(std::move(u)){curl=curl_easy_init();}
inline HTTP::~HTTP(){if(curl)curl_easy_cleanup(curl);}
inline void HTTP::prep(const string&u){curl_easy_reset(curl);r.clear();curl_easy_setopt(curl,CURLOPT_URL,u.c_str());curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,cb);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&r);}
inline size_t HTTP::cb(const void*b,size_t s,size_t n,string*d){size_t x=s*n;try{d->append((const char*)b,x);}catch(std::bad_alloc&e){screen(std::clog,UTILS_ERROR,SYSTEM_DECORATOR,&e);}return x;}
inline CURLcode HTTP::perform_with_retry(){CURLcode p=curl_easy_perform(curl);for(int attempt=0;attempt<200&&p==CURLE_COULDNT_CONNECT;++attempt){std::this_thread::sleep_for(std::chrono::milliseconds(5));p=curl_easy_perform(curl);}return p;}
inline int HTTP::getSimple(const string&e){prep(e!=DEFAULT?URL+e:URL);curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);auto p=perform_with_retry();pr.push_back(p);if(p!=CURLE_OK)screen(std::clog,UTILS_WARNING,CURL_DECORATOR,p);return HTTP_SUCCESS;}
inline int HTTP::getBase(GET*f,const string&e,bool h,const VHeaders*b){string u=e!=DEFAULT?URL+e:URL;if(f)u+="?"+f->t();prep(u);curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);curl_slist*l=nullptr;if(h){l=makeVHeaders(b->al);curl_easy_setopt(curl,CURLOPT_HTTPHEADER,l);}auto p=perform_with_retry();pr.push_back(p);if(l)curl_slist_free_all(l);if(p!=CURLE_OK)screen(std::clog,UTILS_WARNING,CURL_DECORATOR,p);return HTTP_SUCCESS;}
inline int HTTP::get(GET&f,const VHeaders&h,const string&e){return getBase(&f,e,true,&h);}
inline int HTTP::getVHeaders(const VHeaders&h,const string&e){return getBase(nullptr,e,true,&h);}
inline int HTTP::post(POST&f,const string&e,const string&t){prep(e!=DEFAULT?URL+e:URL);auto d=f.t();curl_easy_setopt(curl,CURLOPT_POSTFIELDS,d.c_str());curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,t!=DEFAULT?t.c_str():(const char*)POST_t);auto p=perform_with_retry();pr.push_back(p);if(p!=CURLE_OK)screen(std::clog,UTILS_WARNING,CURL_DECORATOR,p);return HTTP_SUCCESS;}
inline int HTTP::post(POST*f,const VHeaders&h,const string&e,const string&t){prep(e!=DEFAULT?URL+e:URL);string d;if(f){d=f->t();curl_easy_setopt(curl,CURLOPT_POSTFIELDS,d.c_str());}if(t!=DEFAULT)curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,t.c_str());auto l=makeVHeaders(h.al);curl_easy_setopt(curl,CURLOPT_HTTPHEADER,l);auto p=perform_with_retry();pr.push_back(p);curl_slist_free_all(l);if(p!=CURLE_OK)screen(std::clog,UTILS_WARNING,CURL_DECORATOR,p);return HTTP_SUCCESS;}
inline int HTTP::postVHeaders(const VHeaders&h,const string&e){return post(nullptr,h,e,"POST");}
inline int HTTP::postSimple(const string&e){prep(e!=DEFAULT?URL+e:URL);curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,(const char*)POST_t);auto p=perform_with_retry();pr.push_back(p);if(p!=CURLE_OK)screen(std::clog,UTILS_WARNING,CURL_DECORATOR,p);return HTTP_SUCCESS;}
inline int HTTP::put(PUT&f,const string&e){return post(f,e!=DEFAULT?e:"",PUT_TYPE);}
inline int HTTP::put(PUT&f,const VHeaders&h,const string&e){return post(&f,h,e!=DEFAULT?e:"",PUT_TYPE);}
inline int HTTP::Delete(DELETE&f,const string&e){return post(f,e!=DEFAULT?e:"",DELETE_TYPE);}
inline int HTTP::Delete(DELETE&f,const VHeaders&h,const string&e){return post(&f,h,e!=DEFAULT?e:"",DELETE_TYPE);}
inline int HTTP::custom(POST&f,const string&t,const string&e){return post(f,e!=DEFAULT?e:"",t);}
inline int HTTP::custom(POST&f,const VHeaders&h,const string&t,const string&e){return post(&f,h,e!=DEFAULT?e:"",t);}
inline void HTTP::setUrl(const string&u){URL=u;}
inline int HTTP::setVHeaders(const VHeaders&h){return h.initial?HTTP_SUCCESS:UTILS_USER_ERROR;}
inline curl_slist*HTTP::makeVHeaders(const vector<string>&v){curl_slist*l=nullptr;for(auto&i:v)l=curl_slist_append(l,i.c_str());return l;}
inline string HTTP::Response()const{return r;}
inline string HTTP::without(string t,char k){for(auto&i:t)if(i==k)i=0;return t;}
inline string HTTP::genPerfomList(){int m{};for(auto&i:pr)m+=i;return std::to_string(m);}
class Veridic{HTTP http;string URL{};public:
Veridic()=default;explicit Veridic(string u):URL(std::move(u)){}bool setUrl(const string&u){if(u.empty())return false;URL=u;return true;}
string get(const string&e=DEFAULT);string get(const VHeaders&h,const string&e=DEFAULT);string get(GET&f,const string&e=DEFAULT);string get(GET&f,const VHeaders&h,const string&e=DEFAULT);
string post(const string&e=DEFAULT);string post(const VHeaders&h,const string&e=DEFAULT);string post(POST&f,const string&e=DEFAULT,const string&t=DEFAULT);string post(POST&f,const VHeaders&h,const string&e=DEFAULT,const string&t=DEFAULT);
string put(PUT&f,const string&e=DEFAULT);string put(PUT&f,const VHeaders&h,const string&e=DEFAULT);string Delete(DELETE&f,const string&e=DEFAULT);string Delete(DELETE&f,const VHeaders&h,const string&e=DEFAULT);string custom(POST&f,const string&t,const string&e=DEFAULT);string custom(POST&f,const VHeaders&h,const string&t,const string&e=DEFAULT);
private:string go(int);
};
inline string Veridic::go(int c){if(c!=HTTP_SUCCESS)return BAD_RESULT;return http.Response();}
inline string Veridic::get(const string&e){if(URL.empty())return BAD_RESULT;http.setUrl(URL);return go(http.getSimple(e!=DEFAULT?e:""));}
inline string Veridic::get(GET&f,const string&e){if(URL.empty())return BAD_RESULT;http.setUrl(URL);return go(http.getBase(&f,e!=DEFAULT?e:""));}
inline string Veridic::get(GET&f,const VHeaders&h,const string&e){if(URL.empty())return BAD_RESULT;http.setUrl(URL);return go(http.get(f,h,e!=DEFAULT?e:""));}
inline string Veridic::get(const VHeaders&h,const string&e){if(URL.empty())return BAD_RESULT;http.setUrl(URL);return go(http.getVHeaders(h,e!=DEFAULT?e:""));}
inline string Veridic::post(POST&f,const string&e,const string&t){if(URL.empty())return BAD_RESULT;http.setUrl(URL);return go(http.post(f,e!=DEFAULT?e:"",t.empty()?"":t));}
inline string Veridic::post(POST&f,const VHeaders&h,const string&e,const string&t){if(URL.empty()||!h.initial)return BAD_RESULT;http.setUrl(URL);return go(http.post(&f,h,e!=DEFAULT?e:"",t.empty()?"":t));}
inline string Veridic::post(const string&e){if(URL.empty())return BAD_RESULT;http.setUrl(URL);return go(http.postSimple(e!=DEFAULT?e:""));}
inline string Veridic::post(const VHeaders&h,const string&e){if(URL.empty()||!h.initial)return BAD_RESULT;http.setUrl(URL);return go(http.postVHeaders(h,e!=DEFAULT?e:""));}
inline string Veridic::put(PUT&f,const string&e){return post(f,e!=DEFAULT?e:"",PUT_TYPE);}
inline string Veridic::put(PUT&f,const VHeaders&h,const string&e){return post(f,h,e!=DEFAULT?e:"",PUT_TYPE);}
inline string Veridic::Delete(DELETE&f,const string&e){return post(f,e!=DEFAULT?e:"",DELETE_TYPE);}
inline string Veridic::Delete(DELETE&f,const VHeaders&h,const string&e){return post(f,h,e!=DEFAULT?e:"",DELETE_TYPE);}
inline string Veridic::custom(POST&f,const string&t,const string&e){return post(f,e!=DEFAULT?e:"",t);}
inline string Veridic::custom(POST&f,const VHeaders&h,const string&t,const string&e){return post(f,h,e!=DEFAULT?e:"",t);}
#endif