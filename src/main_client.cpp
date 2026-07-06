#include "client_repo.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void save_global_token(const char* token) {
    char path[1024]; snprintf(path, sizeof(path), "%s/.gitimg_credentials", getenv("HOME"));
    FILE* f = fopen(path, "w"); if(f) { fprintf(f, "%s\n", token); fclose(f); }
}

void load_global_token() {
    char path[1024]; snprintf(path, sizeof(path), "%s/.gitimg_credentials", getenv("HOME"));
    FILE* f = fopen(path, "r");
    if(f) { 
        char token[256] = {0};
        if (fscanf(f, "%255s", token) == 1) { Protocol::set_token(token); }
        fclose(f); 
    }
}

void clear_global_token() {
    char path[1024]; snprintf(path, sizeof(path), "%s/.gitimg_credentials", getenv("HOME"));
    unlink(path);
}

void print_usage() {
    printf("Usage:\n");
    printf("  gitimg login <username> <password> [host:port]\n");
    printf("  gitimg logout\n");
    printf("  gitimg init <repo_name>\n");
    printf("  gitimg clone <owner>/<repo_name> [host:port]\n");
    printf("  gitimg push \"message\"\n");
    printf("  gitimg watch\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return EXIT_FAILURE; }

    load_global_token();

    if (strcmp(argv[1], "login") == 0) {
        if (argc < 4) { printf("Error: Missing credentials.\n"); return EXIT_FAILURE; }
        char token[256] = {0};
        const char* host = "127.0.0.1"; int port = 8080;
        if (Protocol::login(host, port, argv[2], argv[3], token)) {
            save_global_token(token);
            printf("Successfully logged in as %s.\n", argv[2]);
        } else {
            printf("Login failed. Check credentials.\n");
        }
    }
    else if (strcmp(argv[1], "logout") == 0) {
        clear_global_token();
        printf("Logged out successfully.\n");
    }
    else if (strcmp(argv[1], "init") == 0) {
        ClientRepo repo(".", "127.0.0.1", 8080);
        const char* repo_name = (argc > 2) ? argv[2] : "default_repo";
        repo.init(repo_name, "127.0.0.1", 8080);
        if (Protocol::create_repo("127.0.0.1", 8080, repo_name)) {
            printf("Created remote repository '%s'\n", repo_name);
        } else {
            printf("Failed to contact server to create repo (Are you logged in?).\n");
        }
    } 
    else if (strcmp(argv[1], "push") == 0 || strcmp(argv[1], "commit") == 0) {
        ClientRepo repo(".", "127.0.0.1", 8080);
        const char* msg = (argc > 2) ? argv[2] : "Auto-commit";
        if (!repo.commit(msg)) {
            printf("Push failed. Check your access permissions.\n");
        }
    }
    else if (strcmp(argv[1], "clone") == 0) {
        if (argc < 3) { printf("Error: Missing <owner>/<repo>\n"); return EXIT_FAILURE; }
        char owner[128]={0}, repo_name[128]={0};
        if (sscanf(argv[2], "%127[^/]/%127s", owner, repo_name) != 2) {
            printf("Error: Format must be owner/repo\n"); return EXIT_FAILURE;
        }
        ClientRepo repo(".", "127.0.0.1", 8080);
        repo.init(argv[2], "127.0.0.1", 8080); // FIXED: Passes full owner/repo string
        printf("Cloned remote repository %s/%s\n", owner, repo_name);
    }
    else {
        print_usage();
    }

    return EXIT_SUCCESS;
}