# Vermell Examples

Each folder is a self-contained server. Build any example against the
in-repo library from the repository root:

```shell
g++ -std=c++20 examples/hello-world/main.cpp -o server -I include -L build -lvermell -pthread
```

(or against an installed Vermell with `g++ -std=c++20 main.cpp -o server -lvermell -pthread`)

Every server listens on `http://localhost:8080` unless it says otherwise.

| Example | Shows |
| --- | --- |
| [hello-world](hello-world/main.cpp) | Minimal `GET` route with `web.send()`. |
| [configuration](configuration/main.cpp) | Full `router.configure({...})` reference: network, request reading, thread pool / epoll, render hardening and the chainable setters. |
| [types-routes](types-routes/main.cpp) | One route per HTTP method (`get`, `post`, `put`, `deleteX`, `patch`, `head`, `options`, `link`, `unlink`, `purge`). |
| [parameters-methods](parameters-methods/main.cpp) | Query/form parameters: `exist`, `get`, `operator[]`, typed `as<T>()` conversion, `value_or` fallbacks. |
| [request-body](request-body/main.cpp) | Raw JSON/text/binary bodies: `body.raw()`, `body.contentType()`, `body.hasBody()`. |
| [upload](upload/main.cpp) | `multipart/form-data` file uploads: `body.file(field)`, `body.files()`, `save_to()`. |
| [middlewares](middlewares/main.cpp) | Middleware chains and `web.next()`. |
| [callbacks](callbacks/main.cpp) | Post-response callbacks and per-response status codes. |
| [headers](headers/main.cpp) | Reading request headers and setting response headers (`HEADERS`, `setHeaders`). |
| [simple-json](simple-json/main.cpp) | Building and parsing JSON with the `vermell::Json` DOM (native types, escaping, strict parser); legacy `JSON_s` facade. |
| [files](files/main.cpp) | Serving files: `readFile` with explicit type and `file()` with auto-detected MIME. |
| [static](static/main.cpp) | Static directory mounts with `router.staticX()`: serve a Vue/React/Angular `dist` folder (SPA fallback, cache headers, ETag/304) plus a classic prefix mount. |
| [file-template](file-template/main.cpp) | `compose()`: HTML pages assembled from modules (`#[name];`). |
| [data-template](data-template/main.cpp) | `render()`: HTML templates with `[[variable]]` substitution. |
| [route-cooling](route-cooling/main.cpp) | Per-route cooldown with `web.guard()` and custom cooldown messages. |
| [graceful-shutdown](graceful-shutdown/main.cpp) | Stopping the server with `setListenStatus(neo::STOP)`; `listenOne()` for one-shot servers. |
| [router](router/) | Organizing routes: `Route_t` + `router.use()` and reusable `MiddlewareList`s. |
| [process](process/main.cpp) | Node.js-style process info: `vermell::process.pwd`, `exec_path`, `pid`, `arch`, `uptime()`, `memory_usage()`, ... |
| [environment](environment/main.cpp) | `.env` loaded from the executable directory: `vermell::environment.get`, typed `get_as<T>`, runtime session values with `set()`. |

## Try them

```shell
# hello-world
curl http://localhost:8080/

# parameters-methods
curl "http://localhost:8080/typed?id=21&active=true&page=3"

# request-body
curl -X POST http://localhost:8080/json -H "Content-Type: application/json" -d '{"lang":"c++"}'

# upload
curl -F "title=hello" -F "doc=@note.txt" http://localhost:8080/upload

# graceful-shutdown
curl http://localhost:8080/shutdown

# environment (copy examples/environment/.env next to the binary first;
# it listens on port 9000, taken from the .env)
curl http://localhost:9000/
```
