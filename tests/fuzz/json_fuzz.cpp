#include <vermell/util/json.hpp>

#include <cstdint>
#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    const std::string_view raw(reinterpret_cast<const char*>(data), size);

    const auto value = vermell::Json::parse(raw);
    if (value.has_value()) {
        const std::string serialized = value->dump();
        (void)vermell::Json::parse(serialized);
    }
    return 0;
}
