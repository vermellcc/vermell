#include <vermell/http/message.hpp>

#include <cstdint>
#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    const std::string_view raw(reinterpret_cast<const char*>(data), size);

    const std::size_t nl = raw.find('\n');
    const std::string_view boundary = nl == std::string_view::npos ? raw : raw.substr(0, nl);
    const std::string_view body     = nl == std::string_view::npos ? std::string_view{} : raw.substr(nl + 1);

    const auto parsed = vermell::http::parse_multipart(body, boundary);
    (void)parsed.files.size();
    (void)parsed.fields.size();
    return 0;
}
