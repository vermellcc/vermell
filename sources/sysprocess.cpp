#include "../include/vermell/util/sysprocess.h"
#include "../include/vermell/util/nterminal.h"

#include <algorithm>
#include <array>
#include <fstream>

std::string neosys::process::readFile(const std::string &path, char separator) {
    std::ifstream reader(path, std::ios::binary);
    if (!reader.is_open())
        return {};

    std::string body;
    std::array<char, 16384> chunk{};
    while (reader) {
        reader.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        body.append(chunk.data(), static_cast<size_t>(reader.gcount()));
    }

    if (separator != '\0')
        std::replace(body.begin(), body.end(), '\n', separator);

    return body;
}

int neosys::process::writeFile(const std::string &path, const std::string &content) {
    std::ofstream write_stream(path, std::ios::binary);
    if (!write_stream.is_open())
        return VER_NVALUE;

    write_stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    write_stream.close();
    return write_stream.good() ? VER_OK : VER_NVALUE;
}
