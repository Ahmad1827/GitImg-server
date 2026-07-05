#include "server_hub.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    int port = 8080;
    const char* storage = ".gitimgd";

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    ServerHub hub(port, storage);

    if (!hub.start()) {
        fprintf(stderr, "Fatal: Failed to start Server Hub on port %d\n", port);
        return EXIT_FAILURE;
    }

    hub.run();

    return EXIT_SUCCESS;
}