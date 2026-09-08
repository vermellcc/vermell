<img width="2548" height="984" alt="Vermell" src="https://github.com/user-attachments/assets/781a62da-e150-4715-b525-f178dc3dc7ae" />

# Vermell

Vermell is a lightweight C++20 HTTP web framework for Linux. It provides an event-driven core built on `epoll` and a worker thread pool, designed with zero external dependencies.

Documentation: [vermell.cc](https://vermell.cc)

## Features

- **Zero dependencies**: Uses only base Linux APIs (`epoll`, sockets, pthreads).
- **C++20**: Clean modern C++ interface.
- **Event-driven**: Non-blocking I/O loop with worker thread pool for handling requests.
- **Routing**: Express-style route definitions and HTTP method handlers.
- **Static files**: Directory mounting, path traversal protection, caching headers, and SPA routing support.
- **JSON support**: Built-in RFC 8259 parser and serializer.
- **Templates**: Basic file composition (`compose`) and placeholder rendering (`render`).
- **Utilities**: Environment variable loading (`.env`) and process inspection helpers.

## Requirements

- Linux (x86_64, aarch64, armv7, WSL, Termux)
- GCC with C++20 support (`g++ >= 10`)
- CMake >= 3.16 (optional, for building the library)

> Note: macOS and Windows are not supported as Vermell relies on Linux `epoll`.

## Installation

### Building from source (CMake)

```bash
git clone https://github.com/vermellcc/vermell.git
cd vermell
cmake -B build
cmake --build build
sudo cmake --install build
```

### APT (Debian / Ubuntu)

```bash
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://vermellcc.github.io/vermell/vermell-apt-key.asc | sudo gpg --dearmor --yes -o /etc/apt/keyrings/vermell.gpg
echo "deb [signed-by=/etc/apt/keyrings/vermell.gpg] https://vermellcc.github.io/vermell stable main" | sudo tee /etc/apt/sources.list.d/vermell.list
sudo apt-get update
sudo apt-get install -y libvermell
```

### Docker

```bash
docker pull vermellcc/vermell
```

### Starter Template

```bash
npx create-vermell-static
```

## Quick Start

Create a simple `server.cpp`:

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

Compile and run:

```bash
g++ -std=c++20 server.cpp -o server -lvermell
./server
```

Test the endpoint:

```bash
curl http://localhost:8080/
```

## Routing

Register endpoints using standard HTTP methods. Handlers receive a `Query` reference.

```cpp
router.get("/users", { [](Query &web) {
    web.send("User list");
}});

router.post("/users", { [](Query &web) {
    web.send("User created");
}});

router.get("/users/:id", { [](Query &web) {
    std::string id = web.param("id");
    web.send("User ID: " + id);
}});

router.put("/users/:id", { [](Query &web) {
    web.send("User updated");
}});

// delete is a C++ keyword, so deleteX is used instead
router.deleteX("/users/:id", { [](Query &web) {
    web.send("User deleted");
}});
```

Other supported verbs include `patch`, `head`, `options`, `link`, `unlink`, and `purge`.

### Modular Routes

Routes can also be declared separately and mounted with `router.use()`:

```cpp
Route_t user_routes("/users/:id", {
    [](Query &web) {
        web.json(R"({"status":"ok"})");
    }
}, GET_TYPE);

router.use(user_routes);
```

## Configuration

Server settings can be configured via `router.configure()` or using chainable setters.

```cpp
#include <vermell/vermell.h>

int main() {
    Router router;

    router.configure({
        .read_timeout     = std::chrono::seconds{30},
        .request_timeout  = std::chrono::seconds{60},
        .write_timeout    = std::chrono::seconds{10},
        .max_request_size = 16UL * 1024 * 1024,
        .threads          = 4,
        .max_connections  = 1024,
    });

    router.setPort(8080);
    router.listen();
}
```

Equivalent using setters:

```cpp
router.setThreads(4)
      .setMaxRequestSize(16 * 1024 * 1024)
      .setReadTimeout(std::chrono::seconds{30});
```

The current configuration can be inspected with `router.config()`.

## Static Files

Mount local directories to serve assets or frontend builds:

```cpp
// Serve static assets from ./public at /assets
router.staticX("/assets", "./public");

// SPA mode: unknown routes fall back to index.html
router.staticX("/", "./dist", {
    .spa     = true,
    .max_age = std::chrono::days{30}
});
```

Directory traversal attempts (`..`, NUL bytes) are automatically blocked.

## Templates

### Compose

Assemble HTML files by referencing sub-modules with `#[module_name];`:

```cpp
router.get("/", { [](Query &web) {
    web.compose("./index.html", 2);
}});
```

### Render

Replace placeholder variables `[[var_name]]` in templates:

```cpp
router.get("/", { [](Query &web) {
    web.render("./template.html", [&](dataRender &data) {
        data("name", "Vermell");
        data("status", "running");
        return data;
    });
}});
```

## Environment & Process

Vermell includes lightweight helpers for reading process metadata and `.env` files.

```cpp
// Process inspection
std::string root = vermell::process.pwd;
pid_t pid = vermell::process.pid;
double uptime = vermell::process.uptime();

// Environment variables
std::string token = vermell::environment.get("API_TOKEN", "default_value");
int port = vermell::environment.get_as<int>("PORT", 8080);
```

## Building without root (e.g. Termux)

If you cannot install Vermell globally via `make install`, place `libvermell.a` alongside your source files and include the header directly:

```cpp
#include "include/vermell/vermell.h"

int main() {
    Router router;
    router.setPort(8080);
    router.get("/", { [](Query &http) { http.send("ok"); } });
    router.listen();
}
```

```bash
g++ -std=c++20 server.cpp -o server -L. -lvermell
```

## Examples

Check the [`examples/`](examples/README.md) directory for standalone examples:

- `examples/hello-world`: Minimal server setup
- `examples/types-routes`: Common HTTP methods and routing
- `examples/parameters-methods`: Handling query parameters and path parameters
- `examples/request-body`: Reading JSON and raw request bodies
- `examples/upload`: Handling multipart file uploads
- `examples/static`: Static asset serving and SPA fallback
- `examples/configuration`: Advanced server and thread pool configuration
- `examples/graceful-shutdown`: Handling termination signals cleanly

## Running Tests

Run the test suite using CMake and CTest:

```bash
cmake -DTESTING=ON -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Or via npm:

```bash
npm run build
npm run test
```

## Contributing

1. Fork the repository.
2. Create a feature branch (`git checkout -b feature/my-feature`).
3. Commit your changes following the guidelines in `COMMIT_FORMAT.MD`.
4. Push to your branch and open a pull request.

Please review `CODE_OF_CONDUCT.md` before contributing.

## History & Origins

Vermell originated from an early 2022 proof-of-concept repository, [Magnetar](https://github.com/scyth3-c/Magnetar) (archived). The core networking engine was later split into its own submodule, [Magnetar-core](https://github.com/scyth3-c/Magnetar-core/tree/038778f49d941c88495a51ccfa02750279b6135a), before the architecture was rewritten and consolidated into Vermell.

## License

This project is licensed under the [MIT License](LICENCE).
