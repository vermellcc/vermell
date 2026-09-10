#include "vermell/util/process.h"

#include <array>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <pwd.h>
#include <unistd.h>

namespace {

    std::string exe_path() {
        std::array<char, PATH_MAX> buf{};
        const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        return len > 0 ? std::string(buf.data(), static_cast<size_t>(len)) : std::string{};
    }

    std::string dir_name(const std::string& path) {
        const auto pos = path.find_last_of('/');
        if (pos == std::string::npos)
            return {};
        return pos == 0 ? "/" : path.substr(0, pos);
    }

    std::string base_name(const std::string& path) {
        const auto pos = path.find_last_of('/');
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

    std::string current_dir() {
        std::array<char, PATH_MAX> buf{};
        return ::getcwd(buf.data(), buf.size()) != nullptr ? std::string(buf.data()) : std::string{};
    }

    std::string host_name() {
        std::array<char, 256> buf{}; // HOST_NAME_MAX + 1
        return ::gethostname(buf.data(), buf.size() - 1) == 0 ? std::string(buf.data()) : std::string{};
    }

    std::string user_name() {
        if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_name != nullptr)
            return pw->pw_name;
        if (const char* user = std::getenv("USER"); user != nullptr)
            return user;
        return {};
    }

    std::vector<std::string> command_line() {
        // /proc/self/cmdline holds argv as NUL-separated strings.
        std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
        std::vector<std::string> args;
        std::string arg;
        while (std::getline(cmdline, arg, '\0'))
            args.push_back(arg);
        return args;
    }

    const char* cpu_arch() noexcept {
    #if defined(__x86_64__)
        return "x86_64";
    #elif defined(__aarch64__)
        return "aarch64";
    #elif defined(__arm__)
        return "arm";
    #elif defined(__i386__)
        return "i386";
    #elif defined(__powerpc64__)
        return "ppc64";
    #elif defined(__riscv)
        return "riscv";
    #else
        return "unknown";
    #endif
    }

} // namespace

vermell::Process::Process()
    : exec_path(exe_path()),
      exec_name(base_name(exec_path)),
      pwd(dir_name(exec_path)),
      cwd(current_dir()),
      hostname(host_name()),
      username(user_name()),
      platform("linux"),
      arch(cpu_arch()),
      pid(::getpid()),
      ppid(::getppid()),
      argv(command_line()),
      started_(std::chrono::steady_clock::now()) {}

double vermell::Process::uptime() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

std::uint64_t vermell::Process::memory_usage() const noexcept {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            std::uint64_t kilobytes = 0;
            status >> kilobytes;
            return kilobytes * 1024;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

std::string vermell::Process::path(const std::string& relative) const {
    if (relative.empty())
        return pwd;
    if (relative.front() == '/')
        return relative; // already absolute
    return pwd + "/" + relative;
}

const vermell::Process& vermell::process_instance() {
    static const Process instance;
    return instance;
}
