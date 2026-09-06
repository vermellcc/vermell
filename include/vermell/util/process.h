//
// Runtime information about the running process, captured once on first
// use. Node.js-style access through the global vermell::process object:
//
//   vermell::process.pwd        // directory containing the executable
//   vermell::process.cwd        // working directory the process was launched from
//   vermell::process.exec_path  // absolute path of the executable
//   vermell::process.pid        // process id
//   vermell::process.uptime()   // seconds since the process started
//

#ifndef VERMELL_PROCESS_H
#define VERMELL_PROCESS_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "portability.h" // ver_pid_t on every platform

namespace vermell {

    class Process {
    public:
        const std::string exec_path;  // absolute path of the running executable
        const std::string exec_name;  // executable file name
        const std::string pwd;        // directory containing the executable
        const std::string cwd;        // working directory the process was launched from
        const std::string hostname;   // machine host name
        const std::string username;   // user that owns the process
        const std::string platform;   // "linux", "darwin" or "windows"
        const std::string arch;       // "x86_64", "aarch64", "arm", "i386", ...
        const ver_pid_t pid;          // process id
        const ver_pid_t ppid;         // parent process id (0 on Windows)
        const std::vector<std::string> argv; // command line arguments

        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        // Seconds elapsed since the process started.
        [[nodiscard]] double uptime() const noexcept;
        // Resident memory of the process in bytes (0 when unavailable).
        [[nodiscard]] std::uint64_t memory_usage() const noexcept;
        // `relative` resolved against the executable directory (process.pwd);
        // an absolute `relative` is returned unchanged.
        [[nodiscard]] std::string path(const std::string& relative) const;

    private:
        Process();
        std::chrono::steady_clock::time_point started_;
        friend const Process& process_instance();
    };

    // The single instance behind vermell::process (initialized on first use,
    // safe against static-initialization-order issues).
    const Process& process_instance();

    // Node.js-style global: vermell::process.pwd
    inline const Process& process = process_instance();

} // namespace vermell

#endif // VERMELL_PROCESS_H
