#include "client_repo.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_usage() {
    printf("Usage:\n");
    printf("  gitimg init <repo_name> [server_ip] [port]\n");
    printf("  gitimg commit \"message\"\n");
    printf("  gitimg checkout <commit_hash>\n");
    printf("  gitimg watch\n");
    printf("  gitimg info <repo_name>\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return EXIT_FAILURE;
    }

    ClientRepo repo(".", "127.0.0.1", 8080);

    if (strcmp(argv[1], "init") == 0) {
        const char* repo_name = (argc > 2) ? argv[2] : "default_repo";
        const char* host = (argc > 3) ? argv[3] : "127.0.0.1";
        int port = (argc > 4) ? atoi(argv[4]) : 8080;
        
        repo.init(repo_name, host, port);
        if (Protocol::create_repo(host, port, repo_name)) {
            printf("Successfully created remote repository '%s' on %s:%d\n", repo_name, host, port);
        } else {
            printf("Failed to contact server at %s:%d to create repo.\n", host, port);
        }
    } 
    else if (strcmp(argv[1], "commit") == 0) {
        if (argc < 3) {
            printf("Error: Missing commit message.\n");
            return EXIT_FAILURE;
        }
        repo.commit(argv[2]);
    } 
    else if (strcmp(argv[1], "checkout") == 0) {
        if (argc < 3) {
            printf("Error: Missing commit hash.\n");
            return EXIT_FAILURE;
        }
        repo.checkout(argv[2]);
    }
    else if (strcmp(argv[1], "watch") == 0) {
        repo.watch();
    } 
    else if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            printf("Error: Missing repo name.\n");
            return EXIT_FAILURE;
        }
        char buffer[4096];
        char endpoint[256];
        snprintf(endpoint, sizeof(endpoint), "/repo/info/%s", argv[2]);
        if (Protocol::fetch_http_get("127.0.0.1", 8080, endpoint, buffer, sizeof(buffer))) {
            printf("=== REPOSITORY INFO ===\n%s\n", buffer);
        }
        
        snprintf(endpoint, sizeof(endpoint), "/repo/assets/%s", argv[2]);
        if (Protocol::fetch_http_get("127.0.0.1", 8080, endpoint, buffer, sizeof(buffer))) {
            printf("=== ASSET FEED ===\n%s\n", buffer);
        }
    }
    else {
        print_usage();
    }

    return EXIT_SUCCESS;
}