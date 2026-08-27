#ifndef VERMELL_SYSPROCESS_H
#define VERMELL_SYSPROCESS_H

#include <string>

namespace neosys {

    class process {
    public:
        [[maybe_unused]] static std::string readFile(const std::string &path, char separator = '\0');
        [[maybe_unused]] static int writeFile(const std::string &path, const std::string &content);
    };

}

#endif //VERMELL_SYSPROCESS_H
