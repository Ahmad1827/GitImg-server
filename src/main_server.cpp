#include "server_hub.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    const char* config_file = "server.conf";
    if (argc > 1) {
        config_file = argv[1];
    }

    ServerHub server(config_file);
    
    if (!server.start()) {
        printf("CRITICAL: Failed to bind port or initialize GitImg ServerHub.\n");
        return EXIT_FAILURE;
    }
    
    server.run();
    return EXIT_SUCCESS;
}