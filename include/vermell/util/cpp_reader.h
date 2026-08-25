//
// readFileX: renders ".html"-style templates with any number of embedded
// C++ blocks delimited by '$':
//
//     <body>
//       $ std::cout << "hello" << std::endl; $
//       $ for (int i = 0; i < 10; i++)
//             std::cout << "<button>" << i << "</button>"; $
//     </body>
//
// The template is split into text and code segments; the generated program
// prints each text segment verbatim (fully escaped string literals) and runs
// each code block in place, so every block's stdout lands at its position.
// A dangling '$' is treated as literal text, never as a broken template.
//
// Hardening over the legacy implementation:
//   - The parser cannot run out of bounds or touch uninitialized memory
//     (empty files, missing/loose '$' are served as plain text).
//   - Static HTML is embedded as escaped literals: no template text can
//     break out of the generated source (no injection through the markup).
//   - Compilation and execution run sandboxed: private mkdtemp workspace,
//     scrubbed environment, no inherited file descriptors, rlimits on
//     CPU/memory/output/processes and wall-clock timeouts with SIGKILL.
//   - Execution is additionally isolated by a seccomp filter that denies
//     networking and privileged/escape syscalls (inherited by every child
//     the template spawns), plus best-effort user/network namespaces; when
//     the server runs as root the template executes as the "nobody" user.
//     An unprivileged server keeps its own user's file permissions — see
//     the runtime warning and the README.
//   - The toolchain (compiler, standard, optimization, hardening, extra
//     flags and compiler resource limits) is fully configurable through
//     vermell::RenderSecurity::cpp — see router.configure({ .render = { .cpp
//     = {...} } }) or router.setCppToolchain({...}).
//   - Compiled binaries are cached under a private per-user directory keyed
//     by the SHA-256 of the generated source plus the toolchain fingerprint,
//     so steady-state requests do not pay for g++ (and a compile flood
//     cannot stall the worker pool).
//   - Compiler/program diagnostics are logged server-side; the client only
//     receives a generic error page.
//

#ifndef CPP_READER_HPP
#define CPP_READER_HPP

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#include "local_utility.h"
#include "nterminal.h"
#include "notify.h"
#include "render_security.h"
#include "secure_render.h"
#include "sysprocess.h"

using std::string;
using neosys::process;
using neosys::RunOptions;

class CppReader {
public:
    CppReader() = default;

    static std::pair<string, string> processing(const string& path,
                                                const vermell::RenderSecurity& sec = {}) {
        if (!vermell::srender::is_within(vermell::effective_root(sec), path))
            return {notify::noPath(path), "403"};

        auto read = vermell::srender::read_bounded(path, sec.max_file_bytes);
        if (read.err != vermell::srender::ReadErr::Ok)
            return {notify::noPath(path), vermell::srender::status_of(read.err)};

        const string raw_html = std::move(read.data);

        // No complete code block: serve the file verbatim instead of running
        // with uninitialized coordinates like the legacy code did. A template
        // without code carries no code-execution risk, so it is served even
        // when readFileX is disabled.
        const auto segments = split_segments(raw_html);
        if (segments.empty())
            return {raw_html, "200"};

        if (!sec.allow_readfilex)
            return {"Vermell: C++ templates are disabled on this server", "403"};

        // Honest operational note, once per process: template execution is
        // arbitrary code on the server. The sandbox blocks networking and
        // privilege escalation (seccomp) and drops to "nobody" when the
        // server runs as root, but when the server itself is unprivileged
        // the template keeps the server user's file permissions — only ever
        // enable readFileX for trusted template content.
        {
            static std::once_flag warned;
            std::call_once(warned, [] {
                std::cerr << "vermell readFileX: C++ templates are enabled. Templates run "
                             "without network access (seccomp) and drop to 'nobody' when the "
                             "server is root; an unprivileged server keeps its own file "
                             "permissions. Only serve trusted template content."
                          << std::endl;
            });
        }

        try {
            TempDir work = make_temp_dir();
            if (work.path.empty())
                return {"Vermell: cannot allocate a sandbox workspace", "500"};

            const string code = build_source(raw_html, segments);

            string error;
            const string binary = get_or_compile(code, work, sec, error);
            if (binary.empty()) {
                if (!error.empty())
                    std::cerr << "vermell readFileX compile error: " << error << std::endl;
                return {"Vermell: the template could not be compiled", "400"};
            }

            const string out_file = work.path + "/out.txt";

            RunOptions run_opts;
            run_opts.timeout         = sec.run_timeout;
            run_opts.cpu_seconds     = static_cast<rlim_t>(sec.run_timeout.count() / 1000 + 1);
            run_opts.memory_bytes    = sec.run_memory_bytes;
            run_opts.file_size_bytes = sec.max_output_bytes;
            run_opts.max_processes   = 32; // no fork bombs from a template
            run_opts.no_files        = sec.run_no_files;
            run_opts.work_dir        = work.path.c_str();
            run_opts.new_session     = true;
            run_opts.drop_privileges = true;
            run_opts.sandbox         = true; // seccomp (no network) + namespaces

            const std::vector<const char*> execute = {binary.c_str(), nullptr};
            if (process::run_command(execute, out_file, run_opts) == VER_NVALUE) {
                std::cerr << "vermell readFileX: template execution failed or timed out" << std::endl;
                return {"Vermell: the template execution failed or timed out", "400"};
            }

            auto out = vermell::srender::read_bounded(out_file, sec.max_output_bytes);
            if (out.err == vermell::srender::ReadErr::TooLarge)
                return {"Vermell: the template output exceeded the allowed size", "400"};

            // The program already interleaved static text and block output.
            return {std::move(out.data), "200"};
        } catch (const std::exception &e) {
            std::cerr << "vermell readFileX internal error: " << e.what() << std::endl;
            return {"Vermell: internal error while rendering the template", "500"};
        }
    }

private:
    // ---------------- template scanning ----------------

    // A slice of the template: [begin, end) inside the raw file.
    struct Segment {
        bool   code;  // true = C++ block, false = literal markup
        size_t begin;
        size_t end;
    };

    // Splits the template into alternating text/code segments. Returns an
    // empty vector when there is no complete '$'...'$' block (the file is
    // then served verbatim). A '$' without a closing partner and everything
    // after it stays literal text, so a stray currency sign or a truncated
    // template can never produce a broken translation unit.
    [[nodiscard]] static std::vector<Segment> split_segments(const string& raw) {
        std::vector<Segment> segments;
        size_t text_begin = 0;
        size_t pos        = 0;

        while (pos < raw.size()) {
            const size_t open = raw.find(CODE_LOCATE, pos);
            if (open == string::npos)
                break;
            const size_t close = raw.find(CODE_LOCATE, open + 1);
            if (close == string::npos)
                break; // dangling '$': literal text from here on

            if (open > text_begin)
                segments.push_back({false, text_begin, open});
            segments.push_back({true, open + 1, close});

            text_begin = close + 1;
            pos        = close + 1;
        }

        if (segments.empty())
            return segments; // no code at all: caller serves the raw file
        if (text_begin < raw.size())
            segments.push_back({false, text_begin, raw.size()});
        return segments;
    }

    // ---------------- source generation ----------------

    // Appends a text segment as a fully escaped C++ string literal. Every
    // byte that could terminate or alter the literal ('"', '\', control
    // characters) is escaped, so markup can never inject code into the
    // generated translation unit.
    static void emit_text(string& out, const std::string_view text) {
        if (text.empty())
            return;
        out += "std::cout << \"";
        for (const char c : text) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20 || c == '\x7f') {
                        // Fixed-width octal escape: valid C++ and immune to
                        // digit-run ambiguity with the following character.
                        const auto byte = static_cast<unsigned char>(c);
                        out += '\\';
                        out += static_cast<char>('0' + ((byte >> 6) & 0x7));
                        out += static_cast<char>('0' + ((byte >> 3) & 0x7));
                        out += static_cast<char>('0' + (byte & 0x7));
                    } else {
                        out += c; // printable ASCII and UTF-8 bytes pass through
                    }
            }
        }
        out += "\";\n";
    }

    // Builds the translation unit: one main() that prints every text
    // segment and executes every code block at its position in the page.
    [[nodiscard]] static string build_source(const string& raw,
                                             const std::vector<Segment>& segments) {
        string code;
        code.reserve(raw.size() + raw.size() / 2 + 64);
        code += BASE; // #include <iostream> + int main() {
        for (const auto& seg : segments) {
            const std::string_view piece(raw.data() + seg.begin, seg.end - seg.begin);
            if (seg.code)
                    code += piece; // verbatim C++ block
            else
                emit_text(code, piece);
            code += '\n';
        }
        code += CODE_END;
        return code;
    }

    // ---------------- private workspace ----------------

    struct TempDir {
        string path{};
        TempDir() = default;
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;
        TempDir(TempDir&& other) noexcept : path(std::move(other.path)) { other.path.clear(); }
        TempDir& operator=(TempDir&&) = delete;
        ~TempDir() {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        }
    };

    [[nodiscard]] static string tmp_base() {
        if (const char* env = std::getenv("TMPDIR"); env != nullptr && env[0] != '\0')
            return env;
        return "/libvermell1";
    }

    [[nodiscard]] static TempDir make_temp_dir() {
        TempDir dir;
        string pattern = tmp_base() + "/vermell-x-XXXXXX";
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data()) != nullptr) { // mkdtemp creates it with mode 0700
            dir.path = buf.data();
            // o+x so a dropped-privilege template can traverse to its binary;
            // no group/other read or write is ever granted.
            chmod(dir.path.c_str(), 0711);
        }
        return dir;
    }

    // ---------------- compiler location ----------------

    // Legacy auto-detection: the first g++ found in the usual install dirs.
    [[nodiscard]] static string detect_compiler() {
        static const string found = [] {
            std::error_code ec;
            for (const char* candidate : {"/usr/bin/g++", "/bin/g++", "/usr/local/bin/g++"}) {
                if (std::filesystem::is_regular_file(candidate, ec) && !ec)
                    return string(candidate);
            }
            return string{};
        }();
        return found;
    }

    // Resolves the configured toolchain compiler to an absolute path (the
    // sandboxed execve does not search PATH): absolute paths are used
    // as-is, bare names are searched in the usual install dirs and an empty
    // value falls back to g++ auto-detection.
    [[nodiscard]] static string resolve_compiler(const vermell::CppToolchain& tc) {
        if (tc.compiler.empty())
            return detect_compiler();

        const auto usable = [](const string& candidate) {
            std::error_code ec;
            return std::filesystem::is_regular_file(candidate, ec) && !ec
                   && access(candidate.c_str(), X_OK) == 0;
        };

        if (tc.compiler.find('/') != string::npos)
            return usable(tc.compiler) ? tc.compiler : string{};

        for (const char* dir : {"/usr/bin", "/bin", "/usr/local/bin"}) {
            const string candidate = string(dir) + "/" + tc.compiler;
            if (usable(candidate))
                return candidate;
        }
        return {};
    }

    // ---------------- binary cache ----------------

    struct Cache {
        std::mutex mtx;
        std::unordered_map<string, string> entries; // sha256 -> binary path
        string dir{};
        bool usable = false;

        Cache() {
            const string candidate = tmp_base() + "/vermell-cx-" + std::to_string(getuid());
            std::error_code ec;
            std::filesystem::create_directory(candidate, ec); // ignores EEXIST
            struct stat st{};
            // lstat(): a symlink planted at the candidate path is rejected,
            // never followed (and never chmod'ed into usability).
            if (lstat(candidate.c_str(), &st) == 0
                && S_ISDIR(st.st_mode)
                && st.st_uid == getuid()
                && chmod(candidate.c_str(), 0711) == 0) {
                // 0711: owner full access, others may only traverse, so a
                // dropped-privilege template can reach its cached binary.
                dir = candidate;
                usable = true;
            }
        }
    };

    [[nodiscard]] static Cache& cache() {
        static Cache instance;
        return instance;
    }

    // Builds the compiler argv from the configured toolchain. The storage
    // vector owns every string so the c_str() pointers stay valid until
    // run_command() returns.
    [[nodiscard]] static std::vector<string>
    build_compile_args(const vermell::CppToolchain& tc, const string& cxx,
                       const string& source, const string& binary) {
        std::vector<string> args;
        args.reserve(12 + tc.flags.size());

        args.push_back(cxx);
        if (!tc.standard.empty())
            args.push_back("-std=" + tc.standard);
        if (!tc.optimize.empty())
            args.push_back(tc.optimize);
        if (tc.hardening) {
            args.emplace_back("-fstack-protector-strong");
            args.emplace_back("-D_FORTIFY_SOURCE=2");
            args.emplace_back("-s");
        }
        if (tc.suppress_warnings)
            args.emplace_back("-w");
        // User flags go last: in gcc/clang later options win (-O, -std, ...).
        for (const auto& flag : tc.flags)
            args.push_back(flag);

        args.push_back(source);
        args.emplace_back("-o");
        args.push_back(binary);
        return args;
    }

    // Returns the path of a ready-to-run binary for `code`, or an empty
    // string on failure (diagnostics land in `error`).
    [[nodiscard]] static string get_or_compile(const string& code,
                                               const TempDir& work,
                                               const vermell::RenderSecurity& sec,
                                               string& error) {
        const vermell::CppToolchain& tc = sec.cpp;

        const string cxx = resolve_compiler(tc);
        if (cxx.empty()) {
            error = "no usable C++ compiler found (configured: '"
                    + (tc.compiler.empty() ? string("auto-detect") : tc.compiler) + "')";
            return {};
        }

        // The toolchain is part of the identity of the binary: the same
        // source built with another compiler/flag set is another entry.
        const string key = vermell::srender::sha256_hex(
            code + '\x1f' + cxx + '\x1f' + tc.fingerprint());

        Cache& store = cache();
        {
            std::lock_guard<std::mutex> lock(store.mtx);
            if (const auto it = store.entries.find(key);
                it != store.entries.end() && std::filesystem::exists(it->second))
                return it->second;
        }

        const bool cacheable = store.usable
                               && sec.compile_cache_entries != 0
                               && store.entries.size() < sec.compile_cache_entries;
        const string binary = cacheable
                                  ? work.path + "/tpl.bin"   // renamed into the cache on success
                                  : work.path + "/once.bin"; // cache full/off: ephemeral
        const string source = work.path + "/tpl.cpp";
        const string log    = work.path + "/compile.log";

        {
            std::ofstream out(source, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                error = "cannot write the translation unit";
                return {};
            }
            out << code;
        }

        const std::vector<string> storage = build_compile_args(tc, cxx, source, binary);
        std::vector<const char*> compile;
        compile.reserve(storage.size() + 1);
        for (const auto& arg : storage)
            compile.push_back(arg.c_str());
        compile.push_back(nullptr);

        RunOptions compile_opts;
        compile_opts.timeout         = sec.compile_timeout;
        compile_opts.cpu_seconds     = static_cast<rlim_t>(sec.compile_timeout.count() / 1000 + 1);
        compile_opts.memory_bytes    = tc.memory_bytes;
        compile_opts.file_size_bytes = tc.file_size_bytes;
        compile_opts.work_dir        = work.path.c_str();
        compile_opts.new_session     = true;

        if (process::run_command(compile, log, compile_opts) == VER_NVALUE) {
            auto details = vermell::srender::read_bounded(log, 4UL * 1024UL);
            error = std::move(details.data);
            return {};
        }

        // 0755: the executing template runs as "nobody" (dropped privileges)
        // when the server is root. A dynamically-linked ELF requires READ
        // access for ld.so even with execute permission, so the binary must
        // stay world-readable; only the sandbox (rlimits, timeouts, private
        // 0711 workspace, scrubbed environment) protects its execution.
        chmod(binary.c_str(), 0755);

        if (!cacheable)
            return binary;

        // Publish atomically; a concurrent request compiling the same code
        // wins the race and its binary is reused.
        const string cached_path = store.dir + "/" + key;
        std::lock_guard<std::mutex> lock(store.mtx);
        std::error_code ec;
        if (!std::filesystem::exists(cached_path, ec)) {
            std::filesystem::rename(binary, cached_path, ec);
            if (ec) {
                std::filesystem::remove(cached_path, ec);
                ec.clear();
                std::filesystem::rename(binary, cached_path, ec);
                if (ec)
                    return binary; // keep the ephemeral copy, just uncached
            }
            chmod(cached_path.c_str(), 0755);
        } else {
            std::filesystem::remove(binary, ec);
        }
        store.entries[key] = cached_path;
        return cached_path;
    }
};

#endif // ! CPP_READER_HPP
