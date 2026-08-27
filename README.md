
<img width="2548" height="984" alt="image" src="https://github.com/user-attachments/assets/781a62da-e150-4715-b525-f178dc3dc7ae" />


**A minimal, zero-bloat web framework designed for modern C++ environments. Fast, structural, and strictly typed.**

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square&logo=cplusplus&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat-square&logo=linux&logoColor=black)
![epoll](https://img.shields.io/badge/event%20loop-epoll-critical?style=flat-square)
![Zero dependencies](https://img.shields.io/badge/dependencies-zero-success?style=flat-square)
![CMake](https://img.shields.io/badge/CMake-%23008FBA.svg?style=flat-square&logo=cmake&logoColor=white)
![Docker](https://img.shields.io/badge/Docker-2496ED?style=flat-square&logo=docker&logoColor=fff)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg?style=flat-square)

Vermell is a web framework for **modern C++ environments**: one header to include, one static library to link, and nothing else. No runtime, no garbage collector, no framework-specific DSL, no vendored dependencies — what you write is C++, and what runs is C++.

Under the hood it is an event-driven engine: a non-blocking **epoll** loop reads requests and hands work to a pool of worker threads. That split is what makes Vermell fast under load and resilient against slow clients.

- **Zero dependencies** — only base Linux APIs (sockets, epoll, pthreads, fork/exec).
- **One command to build** — `g++ -std=c++20 server.cpp -o exe -lvermell`.
- **Any Linux with g++** — x86_64, ARM (aarch64, armv7), Android via Termux, WSL, Raspberry Pi, containers.
- **Hardened by default** — timeouts, request caps, connection limits and a render jail are on out of the box.
- **In-tree JSON DOM** — strict RFC 8259 parser and serializer, typed parameters, raw bodies, multipart uploads.
- **C++ templates** — `compose()` modules and `render()` variables.
- **Fluent configuration** — one `configure({...})` call or chainable setters, readable at runtime.

> 📚 **Full documentation:** [vermell.cc](https://vermell.cc) — bilingual (EN/ES) manual covering every section of this README with examples and diagrams.

## Table of Contents
1. [Installation](#installation)
   - [CMake](#cmake)
   - [npx](#npx)
   - [Docker](#docker)
2. [Quick Start](#quick-start)
3. [Compile](#compile)
4. [Routing & Handlers](#routing--handlers)
   - [Lambda captures](#lambda-captures)
5. [Server Configuration](#server-configuration)
6. [MIME Types & File Rendering](#mime-types--file-rendering)
7. [Static Directories](#static-directories)
8. [Templates: compose & render](#templates-compose--render)
9. [Render Security](#render-security)
10. [Process & Environment](#process--environment)
11. [Examples](#examples)
12. [Support](#support)
13. [Testing](#testing)
14. [Contribution](#contribution)
15. [License](#license)

## Installation

### CMake

<img alt="CMake" src="https://img.shields.io/badge/CMake-%23008FBA.svg?style=for-the-badge&logo=cmake&logoColor=white"/>

```shell
$ git clone https://github.com/vermellcc/vermell.git
$ cd Vermell
$ cmake .
$ cmake --build .
$ make install
```

### npx

<img alt="NodeJS" src="https://img.shields.io/badge/node.js-%2343853D.svg?style=for-the-badge&logo=node-dot-js&logoColor=white"/>

Ready-to-use scaffold:

```shell
$ npx create-vermell-static
```

### Docker

[![Docker](https://img.shields.io/badge/Docker-2496ED?logo=docker&logoColor=fff)](#)

```shell
$ docker pull vermellcc/vermell
```

### APT (Debian/Ubuntu)

[![Debian](https://img.shields.io/badge/Debian-A81D33?style=for-the-badge&logo=debian&logoColor=white)](#)

Packages for `amd64`, `arm64` and `armhf` live on GitHub Pages, signed and ready to add:

```shell
$ sudo install -d -m 0755 /etc/apt/keyrings
$ curl -fsSL https://vermellcc.github.io/vermell/vermell-apt-key.asc | sudo gpg --dearmor --yes -o /etc/apt/keyrings/vermell.gpg
$ echo "deb [signed-by=/etc/apt/keyrings/vermell.gpg] https://vermellcc.github.io/vermell stable main" | sudo tee /etc/apt/sources.list.d/vermell.list
$ sudo apt-get update
$ sudo apt-get install -y libvermell
```

> Key fingerprint: `022D 56AA 7A6B 2028 B005  3629 F616 54D8 8AD1 C323`

## Quick Start

A Vermell server is a `Router`: register a handler for a route, choose a port, and call `listen()`.

```cpp
#include <vermell/vermell.h>

int main() {
    Router router;
    router.setPort(8080);

    router.get("/", { [](Query &http) {
        http.send("Hello from Vermell");
    }});

    router.listen();
}
```

> **The `{ ... }` around the handler matter.** The second argument of `router.get(...)` is a `MiddlewareList`, so handlers are always passed as a braced list: `router.get("/", { [](Query &http) { ... } })`.

Compile and run:

```shell
$ g++ -std=c++20 server.cpp -o exe -lvermell
$ ./exe
```

Then point your browser or `curl` at it:

```shell
$ curl http://localhost:8080/
Hello from Vermell
```

`router.listen()` blocks and serves forever. `listenOne()` serves a single request and returns — handy for tests and one-shot servers.

## Compile

A single `g++` invocation compiles and links everything — no extra flags, no link order games:

```bash
$ g++ -std=c++20 server.cpp -o exe -lvermell
```

For larger projects use CMake, but a server is always one command away.

> **Portability.** Vermell has no dependencies, so anything that derives from Linux and has a C++20 `g++` can build it: x86_64, **ARM** (aarch64, armv7), **Android via Termux**, WSL, Raspberry Pi, containers. macOS and Windows are not supported targets (epoll).
>
> **No root? No problem.** On Termux (or any system without root) you cannot `make install` into `/usr/local`. Include the header by relative path (`#include "../include/vermell/vermell.h"`) and link the static library directly — copy `libvermell.a` next to your sources and compile with `-L. -lvermell`:

```cpp
// Termux / no-root build: header referenced by relative path
#include "../include/vermell/vermell.h"

int main() {
    Router router;
    router.setPort(8080);

    router.get("/", { [](Query &http) {
        http.send("hi from termux");
    }});

    router.listen();
}
```

```shell
$ cp libvermell.a .          # static library next to the sources
$ g++ -std=c++20 server.cpp -o exe -L. -lvermell
$ ./exe
```

## Routing & Handlers

The router exposes one registration method per HTTP verb. Static routes dispatch in O(1) through a transparent-hash route map.

```cpp
router.get("/users",     { [](Query &web) { web.send("list"); } });
router.post("/users",    { [](Query &web) { web.send("create"); } });
router.put("/users/:id", { [](Query &web) { web.send("update"); } });
router.deleteX("/users/:id", { [](Query &web) { web.send("delete"); } });
router.patch("/users/:id",   { [](Query &web) { web.send("patch"); } });
router.head("/status",   { [](Query &web) { web.send("head"); } });
router.options("/ping",  { [](Query &web) { web.send("options"); } });
router.link("/rel",      { [](Query &web) { web.send("link"); } });
router.unlink("/unlink", { [](Query &web) { web.send("unlink"); } });
router.purge("/cache",   { [](Query &web) { web.send("purge"); } });
```

Note the `deleteX()` name: `delete` is a C++ keyword. For larger applications, declare routes separately and mount them with `router.use()`:

```cpp
// routes.cpp — separated declaration
Route_t users_routes("/users/:id", {
    [](Query &web) { web.json(R"({"op":"get"})"); }
}, GET_TYPE);

// main.cpp — mounting
router.use(users_routes);
router.use(admin_routes);
```

### Lambda captures

Every handler is a C++ lambda `void(Query&)`. The capture list between `[` and `]` decides how outside state reaches it:

```cpp
string app_name = "vermell-demo";
int   port     = 8080;

// [] — nothing captured: the handler only sees the Query
router.get("/ping", { [](Query &web) {
    web.json(R"({"pong":true})");
}});

// [=] — outside values arrive BY COPY: a private snapshot
router.get("/name", { [=](Query &web) {
    web.send(app_name);                 // reads a copy made at registration
}});

// [&] — outside variables arrive BY REFERENCE: a live view
router.get("/info", { [&](Query &web) {
    web.send(app_name + ":" + std::to_string(port));
}});

// named captures — only what you need:
// [port]      -> copy of port         [&port]  -> reference to port
// [this]      -> enclosing object     [=, &port] -> all by copy, port by ref
```

| Capture | Meaning |
| --- | --- |
| `[]` | No capture — the handler only receives the `Query`. |
| `[=]` | Every used outside variable **by copy** (snapshot at creation). |
| `[&]` | Every used outside variable **by reference** (live aliases). |
| `[x]` / `[&x]` | Named capture: copy of `x`, or reference to `x`. |
| `[this]` | Capture the enclosing class (members by reference). |
| `[=, &x]` | Everything by copy, except `x` by reference. |

> **Thread safety.** Handlers run on **worker threads** and live for the whole server lifetime. `[&]` captures are references to the registering scope: fine for variables that outlive `listen()`, but never capture stack locals that die earlier — that is a dangling reference. Because requests run concurrently, shared mutable state captured by reference needs a mutex; prefer `[=]` for immutable snapshots.

## Server Configuration

Every knob of the request/response pipeline lives in `vermell::Config` (`include/vermell/config.hpp`). Pass it whole with `router.configure({...})` (defaults preserve the legacy behavior):

```cpp
router.configure({
    // network
    .backlog           = SOMAXCONN, // pending connections queue of listen()
    .reuse_port        = false,     // SO_REUSEPORT: OFF by default (a same-UID
                                    // process could otherwise bind the port and
                                    // intercept a share of the traffic)

    // request reading
    .read_timeout      = std::chrono::seconds{30}, // inactivity between chunks
    .request_timeout   = std::chrono::seconds{60}, // total deadline for the whole
                                                   // request to arrive (slowloris cure)
    .write_timeout     = std::chrono::seconds{10}, // inactivity while responding
    .max_request_size  = 16UL * 1024UL * 1024UL,   // bigger => 413 Payload Too Large
    .read_chunk        = 32UL * 1024UL,            // bytes read per recv() call

    // concurrency / epoll
    .threads           = 4,    // worker threads; 0 = auto (hardware_concurrency)
    .max_events        = 1024, // epoll event batch size
    .max_queue_size    = 512,  // queued tasks before the dispatcher sheds load
    .max_connections   = 1024, // hard cap on open connections; 0 = unlimited
    .epoll_timeout     = std::chrono::milliseconds{1000},
});
```

> **Hardening defaults:** `read_chunk` is clamped to `[1, 1 MiB]`, `max_events` to `[1, 65536]`, `threads` to `[0, 256]` and every timeout to `[1ms, INT_MAX ms]` — absurd values are a memory/DoS foot-gun, not a feature. Requests to HTTP/1.1 (or newer) without exactly one `Host` header are rejected with 400 (RFC 9112 §3.2, proxy desync / request-smuggling vector); HTTP/1.0 legacy clients keep working. `max_connections` is bounded by default (1024) so a connection flood cannot exhaust memory.
>
> **Slowloris is not a DoS anymore:** request bytes are read on the event loop (non-blocking), so a trickling client occupies an epoll fd — bounded by `max_connections` and the `read_timeout`/`request_timeout` deadlines — never a worker thread. A client that sends 1 byte every few seconds for hours is dropped with 408 as soon as the whole request exceeds `request_timeout`. When the task queue is full the dispatcher sheds the connection (503) instead of stalling the accept loop.

Or use the chainable setters:

```cpp
router.setThreads(4)
      .setMaxRequestSize(16UL * 1024UL * 1024UL)
      .setReadTimeout(std::chrono::seconds{30});
// setWriteTimeout, setRequestTimeout, setReadChunkSize, setMaxEvents,
// setMaxQueueSize, setMaxConnections, setBacklog, setBufferSize,
// setPort, setReusePort
```

> **`configure()` replaces the WHOLE configuration** (designated initializers recommended): settings made earlier with the setters are discarded, so pass everything in one call. `configure()` also applies to a **running** server — timeouts, limits and the thread count are picked up live by the event loop and the worker pool (`RequestIO::ApplyConfig`); only the network-side knobs (`port`, `backlog`, `reuse_port`) need a restart.

The active configuration is readable at runtime with `router.config()`. A full annotated example lives in [`examples/configuration`](examples/configuration/main.cpp).

## MIME Types & File Rendering

Vermell detects the `Content-Type` from the final extension of a file. This means `kevin.txt.html` is served as `text/html`, and matching is case-insensitive. Query strings and fragments are ignored when determining the type. Unknown extensions use `application/octet-stream`.

```cpp
web.readFile("public/data.json");       // application/json
web.file("public/assets/app.js");       // application/javascript

web.send("{}", vermell::mime::json);     // reusable common MIME constants
```

An explicit type passed to `readFile` always takes precedence. The registry includes common text, data, document, image, audio, video, font, archive and executable formats.

## Static Directories

Node's `express.static` as a Vermell mount: bind a disk directory — a Vue, React or Angular `dist` folder, plain assets, anything — to a URL prefix with one call. `static` is a C++ keyword, hence the `X` suffix (same convention as `deleteX`).

```cpp
// SPA dist at the site root: deep links and refreshes fall back to index.html
router.staticX("/", "./dist", {
    .spa     = true,
    .max_age = std::chrono::days{30},   // Cache-Control: public, max-age=2592000
});

// classic mount: only ./public/assets is served at /assets
router.staticX("/assets", "./public/assets");
```

- **Routes always win.** Static mounts are the fallback layer: an exact route like `/api/health` is never shadowed, and the **most specific** mount answers (a `/assets` mount beats a root `/` mount for `/assets/...`; ties go to the first registered).
- **The mount directory IS the jail.** No request path can escape it — percent-encoded `..` (`/%2e%2e/...`), NUL bytes and symlinked escapes are rejected with 403. Files are served with the same hardening as `readFile`: regular files only, `O_NOFOLLOW`, size cap (`StaticOptions::max_file_bytes`).
- **Directories answer their index file** (`index.html` by default). With `.spa = true` any missing file answers the index instead of 404, so Vue/React/Angular client-side routes work on refresh and deep links. `.spa` is OFF by default: a missing asset is a 404, never silently HTML.
- **Caching on by default:** `Cache-Control: public, max-age=N` (`N = StaticOptions::max_age`; `0` = revalidate every request, the express default), a strong `ETag` (size + mtime) and conditional `GET` → `304 Not Modified`. `.cache = false` disables every caching header.
- **GET/HEAD only.** Other methods keep the generic 404 semantics.

Full options live in `vermell::StaticOptions` (`include/vermell/util/static_files.h`). A complete example — SPA dist + classic mount + a JSON API side by side — is in [`examples/static`](examples/static/main.cpp).

## Templates: compose & render

**`compose()`** assembles an HTML page from modules referenced as `#[name];` inside the template. A page that (transitively) includes itself answers 413 instead of exhausting memory:

```cpp
// index.html: <body> #[header]; #[main]; </body>
router.get("/", { [](Query &web) {
    web.compose("./index.html", 2);   // 2 module passes
}});
```

**`render()`** fills `[[variable]]` placeholders in an HTML template through a `dataRender` callback:

```cpp
// data.html: <h1>[[name]]</h1> <p>age: [[age]]</p>
router.get("/", { [](Query &web) {
    web.render("./data.html", [&](dataRender &Data) {
        Data("name", "kevin");   // [[name]] in the html file
        Data("age",  "21");      // [[age]]
        return Data;
    });
}});
```

## Render Security

The file-rendering methods (`readFile`, `file`, `compose`, `render`) are hardened through `Config::render`:

```cpp
router.configure({
    .render = {
        .root             = "public/", // jail: no path escapes this directory
        .max_file_bytes   = 32UL * 1024 * 1024,
    },
});
```

- All readers serve **regular files only** (no FIFOs/devices, symlinks are rejected via `O_NOFOLLOW`), cap the size in memory, and never leak internal errors to the client.
- **The jail is ON even without `.root`:** an empty `render.root` falls back to the working directory, so a server that never configured a root can still not serve files from outside its launch directory (no more open-by-default Local File Inclusion). Set `.root` to a dedicated `public/` directory in production.
- `compose()` module names (`#[name];`) are restricted to bare file names, so `#[../../etc/passwd];` is rejected, and the composed page is capped at `max_file_bytes` per pass — a module that (transitively) includes itself answers 413 instead of exhausting memory.
## Process & Environment

Node.js-style runtime information and configuration, available just by including `vermell/vermell.h`.

`vermell::process` captures the process data once (first use):

```cpp
vermell::process.pwd        // directory containing the executable
vermell::process.cwd        // working directory it was launched from
vermell::process.exec_path  // absolute path of the executable
vermell::process.pid        // process id (also ppid, argv, hostname,
                         // username, platform, arch)
vermell::process.uptime()        // seconds since the process started
vermell::process.memory_usage()  // resident memory in bytes
vermell::process.path(".env")    // path resolved against the executable directory
```

`vermell::environment` loads the `.env` file sitting **next to the executable** automatically, and also holds runtime "session" values. Values from the file and `set()` take precedence over the OS environment; every method is thread-safe.

```cpp
vermell::environment.get("TOKEN")              // .env / set(), else OS env, else ""
vermell::environment.get("TOKEN", "fallback")
vermell::environment.get_as<int>("PORT", 8080) // typed: arithmetic, bool, string
vermell::environment["TOKEN"]

vermell::environment.set("request_count", "1") // runtime session value
vermell::environment.reload()                  // re-read the .env file
vermell::environment.load("config/.env")       // or load another file
```

The `.env` syntax supports `#` comments, `export KEY=VALUE`, quoted values and trailing comments. See [`examples/process`](examples/process/main.cpp) and [`examples/environment`](examples/environment/main.cpp).

## Examples

In the [`examples/`](examples/README.md) folder you'll find self-contained servers for the different use cases:

- **basics**: [hello-world](examples/hello-world/main.cpp), [types-routes](examples/types-routes/main.cpp) (all HTTP methods), [callbacks](examples/callbacks/main.cpp)
- **requests**: [parameters-methods](examples/parameters-methods/main.cpp) (typed `as<T>()`, fallbacks), [request-body](examples/request-body/main.cpp) (raw JSON/text bodies), [upload](examples/upload/main.cpp) (multipart files), [headers](examples/headers/main.cpp)
- **responses**: [simple-json](examples/simple-json/main.cpp), [files](examples/files/main.cpp) (auto-MIME with `file()`), [static](examples/static/main.cpp) (dist folders with `router.staticX`: SPA fallback, cache, ETag/304), [file-template](examples/file-template/main.cpp) (`compose`), [data-template](examples/data-template/main.cpp) (`render`)
- **server**: [configuration](examples/configuration/main.cpp) (`router.configure`, thread pool, timeouts), [route-cooling](examples/route-cooling/main.cpp) (`web.guard`), [graceful-shutdown](examples/graceful-shutdown/main.cpp), [middlewares](examples/middlewares/main.cpp), [router](examples/router/) (route separation with `Route_t` + `use`), [process](examples/process/main.cpp) (`vermell::process` runtime info), [environment](examples/environment/main.cpp) (`.env` + session values with `vermell::environment`)

## Support

<img alt="Linux" src="https://img.shields.io/badge/Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black">

## Testing

CMake / CTest:

```shell
$ cmake -DTESTING=ON -S. -B build
$ cmake --build build/
$ cd build
$ ctest
```

with NPM:

```shell
$ npm run build
$ npm run test
```

### Debug

`tests/debug.cpp` is a scratchpad for testing Vermell against the **installed**
library — it is deliberately not part of the CMake build. Copy `libvermell.a`
next to it and compile it by hand:

```shell
$ g++ -std=c++20 tests/debug.cpp -o debug -L. -lvermell -pthread
```

and edit the file `tests/debug.cpp` freely.

## Contribution

Contributions are welcome! If you want to contribute to Vermell, please follow these guidelines:

- Fork the repository.
- Create a branch for your new feature (`git checkout -b feature/new-feature`).
- Make your changes and commit meaningful messages (see `COMMIT_FORMAT.MD`).
- Push your branch (`git push origin feature/new-feature`).
- Create a pull request.

Please read `CODE_OF_CONDUCT.md` before contributing, and use the issue templates in `.github/ISSUE_TEMPLATE` for bug reports and feature requests.

## License

This project is licensed under the [MIT License](LICENCE).
