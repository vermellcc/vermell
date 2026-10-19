#include <vermell/http/message.hpp>

#include <cstdint>
#include <cstddef>
#include <string_view>

// libFuzzer entry point for the HTTP/1.1 request parser. Runs the same
// framing/parsing code the socket read loop uses (Message::inspect/parse) so
// memory-safety bugs and desync edge cases surface under -fsanitize=fuzzer.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    const std::string_view raw(reinterpret_cast<const char*>(data), size);

    const auto inspection = vermell::http::Message::inspect(raw);
    if (inspection.framing == vermell::http::Message::Framing::Complete) {
        (void)vermell::http::Message::parse(raw);
    }
    return 0;
}
