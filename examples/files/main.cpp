#include <vermell/vermell.h>

int main() {

    Router router;
    router.setPort(8080);

    router.get("/",{[&](Query &web) {
        web.readFile("./test.json", "application/json");
    }});

    router.get("/auto",{[&](Query &web) {
        web.file("./test.json");
    }});

    router.get("/stress",{[&](Query &web) {
        web.readFile("./stress.json", "application/json");
    }});

    router.configure({ .render = { .root = "public/" } });

    router.listen();
}
