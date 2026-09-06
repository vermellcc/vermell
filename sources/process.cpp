#include "../include/vermell/util/process.h"

#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

#include "../include/vermell/util/portability.h"

#if defined(_WIN32)
    #include <direct.h>
    #include <psapi.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach-o/dyld.h>
    #include <pwd.h>
    #include <unistd.h>
#else
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace {

    std::string exe_path() {
#if defined(_WIN32)
        std::array<char, 32768> buf{};
        const DWORD n = ::GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        return n > 0 ? std::string(buf.data(), n) : std::string{};
#elif defined(__APPLE__)
        uint32_t size = PATH_MAX;
        std::string out(size, '\0');
        if (::_NSGetExecutablePath(out.data(), &size) == 0) {
            out.resize(std::strlen(out.c_str()));
            return out;
        }
        // Path longer than the first buffer: the API stores the needed
        // size and we retry once.
        out.resize(size);
        if (::_NSGetExecutablePath(out.data(), &size) == 0) {
            out.resize(std::strlen(out.c_str()));
            return out;
        }
        return {};
#else
        std::array<char, PATH_MAX> buf{};
        const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        return len > 0 ? std::string(buf.data(), static_cast<size_t>(len)) : std::string{};
#endif
    }

    std::string dir_name(const std::string& path) {
#if defined(_WIN32)
        auto pos = path.find_last_of("/\\");
#else
        const auto pos = path.find_last_of('/');
#endif
        if (pos == std::string::npos)
            return {};
        return pos == 0 ? std::string(1, '/') : path.substr(0, pos);
    }

    std::string base_name(const std::string& path) {
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string current_dir() {
#if defined(_WIN32)
        std::array<char, 32768> buf{};
        return ::GetCurrentDirectoryA(static_cast<DWORD>(buf.size()), buf.data()) > 0
                   ? std::string(buf.data()) : std::string{};
#else
        std::array<char, PATH_MAX> buf{};
        return ::getcwd(buf.data(), buf.size()) != nullptr ? std::string(buf.data()) : std::string{};
#endif
    }

    std::string host_name() {
#if defined(_WIN32)
        std::array<char, 256> buf{};
        DWORD size = static_cast<DWORD>(buf.size());
        return ::GetComputerNameA(buf.data(), &size) ? std::string(buf.data(), size) : std::string{};
#else
        std::array<char, 256> buf{}; // HOST_NAME_MAX + 1
        return ::gethostname(buf.data(), buf.size() - 1) == 0 ? std::string(buf.data()) : std::string{};
#endif
    }

    std::string user_name() {
#if defined(_WIN32)
        if (const char* user = std::getenv("USERNAME"); user != nullptr)
            return user;
        return {};
#else
        if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_name != nullptr)
            return pw->pw_name;
        if (const char* user = std::getenv("USER"); user != nullptr)
            return user;
        return {};
#endif
    }

    std::vector<std::string> command_line() {
#if defined(_WIN32)
        // The CRT publishes argv in globals shared by MSVC and MinGW.
        std::vector<std::string> args;
        if (const int argc = __argc; argc > 0 && __argv != nullptr) {
            for (int i = 0; i < argc; ++i)
                args.emplace_back(__argv[i]);
        }
        return args;
#elif defined(__linux__)
        // /proc/self/cmdline holds argv as NUL-separated strings.
        std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
        std::vector<std::string> args;
        std::string arg;
        while (std::getline(cmdline, arg, '\0'))
            args.push_back(arg);
        return args;
#else
        // macOS (and the other BSDs): argv is not reachable without going
        // through main(); left empty rather than guessing.
        return {};
#endif
    }

    const char* cpu_arch() noexcept {
    #if defined(__x86_64__) || defined(_M_X64)
        return "x86_64";
    #elif defined(__aarch64__) || defined(_M_ARM64)
        return "aarch64";
    #elif defined(__arm__) || defined(_M_ARM)
        return "arm";
    #elif defined(__i386__) || defined(_M_IX86)
        return "i386";
    #elif defined(__powerpc64__)
        return "ppc64";
    #elif defined(__riscv)
        return "riscv";
    #else
        return "unknown";
    #endif
    }

    const char* platform_name() noexcept {
    #if defined(_WIN32)
        return "windows";
    #elif defined(__APPLE__)
        return "darwin";
    #else
        return "linux";
    #endif
    }

    ver_pid_t pid_of() noexcept {
#if defined(_WIN32)
        return static_cast<ver_pid_t>(::GetCurrentProcessId());
#else
        return ::getpid();
#endif
    }

    // Windows has no cheap parent-pid primitive (it needs the undocumented
    // NT API or a tool-help snapshot); reported as 0 there.
    ver_pid_t ppid_of() noexcept {
#if defined(_WIN32)
        return 0;
#else
        return ::getppid();
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
      platform(platform_name()),
      arch(cpu_arch()),
      pid(pid_of()),
      ppid(ppid_of()),
      argv(command_line()),
      started_(std::chrono::steady_clock::now()) {}

double vermell::Process::uptime() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

std::uint64_t vermell::Process::memory_usage() const noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<std::uint64_t>(info.resident_size);
    return 0;
#else
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
#endif
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
