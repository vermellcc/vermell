#include <vermell/vermell.h>

int main() {

    Router router;
    router.setPort(8080);

    router.configure({
        .backlog           = SOMAXCONN,
        .buffer_size       = 2048,
        .read_timeout      = std::chrono::seconds{30},
        .write_timeout     = std::chrono::seconds{10},
        .max_request_size  = 16UL * 1024UL * 1024UL,
        .read_chunk        = 32UL * 1024UL,
        .threads           = 4,
        .max_events        = 1024,
        .max_queue_size    = 512,
        .epoll_timeout     = std::chrono::milliseconds{1000},
        .render = {
            .root             = "./",
            .max_file_bytes   = 32UL * 1024UL * 1024UL,
        },
    });

    router.get("/",{[&](Query &web) {
        web.send("worker threads: " + std::to_string(router.config().threads));
    }});

    router.listen();
}
