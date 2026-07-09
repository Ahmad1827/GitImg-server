#include "client_repo.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

char g_username[64] = {0};
char g_host[256] = "127.0.0.1";
int g_port = 8080;

void save_global_config(const char* username, const char* token, const char* host, int port) {
    char path[1024]; snprintf(path, sizeof(path), "%s/.gitimg_credentials", getenv("HOME"));
    FILE* f = fopen(path, "w"); 
    if(f) { 
        fprintf(f, "%s\n%s\n%s\n%d\n", username, token, host, port); 
        fclose(f); 
    }
}

void load_global_config() {
    char path[1024]; snprintf(path, sizeof(path), "%s/.gitimg_credentials", getenv("HOME"));
    FILE* f = fopen(path, "r");
    if(f) { 
        char token[256] = {0};
        if (fscanf(f, "%63s\n%255s\n%255s\n%d", g_username, token, g_host, &g_port) >= 2) { 
            Protocol::set_token(token); 
        }
        fclose(f); 
    }
}

void clear_global_config() {
    char path[1024]; snprintf(path, sizeof(path), "%s/.gitimg_credentials", getenv("HOME"));
    unlink(path);
    g_username[0] = '\0';
}

void print_usage() {
    printf("Usage:\n");
    printf("  gitimg login <username> <password> [host:port]\n");
    printf("  gitimg logout\n");
    printf("  gitimg clone <owner>/<repo_name> [host:port]\n");
    printf("  gitimg push \"message\"\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return EXIT_FAILURE; }

    load_global_config();

    if (strcmp(argv[1], "login") == 0) {
        if (argc < 4) { printf("Error: Missing credentials.\n"); return EXIT_FAILURE; }
        
        char target_host[256]; strncpy(target_host, "127.0.0.1", 255);
        int target_port = 8080;
        
        if (argc >= 5) {
            char h[256]; int p;
            if (sscanf(argv[4], "%255[^:]:%d", h, &p) == 2) {
                strncpy(target_host, h, 255); target_port = p;
            } else {
                strncpy(target_host, argv[4], 255);
            }
        }

        char token[256] = {0};
        if (!Protocol::login(target_host, target_port, argv[2], argv[3], token)) {
            if (strcmp(target_host, "127.0.0.1") != 0) {
                printf("[Network] Connection to %s failed (IP may have changed). Auto-routing to 127.0.0.1...\n", target_host);
                if (Protocol::login("127.0.0.1", target_port, argv[2], argv[3], token)) {
                    strncpy(target_host, "127.0.0.1", 255);
                } else {
                    printf("Login failed. CRITICAL: Is your server daemon (gitimgd) running in another terminal?\n");
                    return EXIT_FAILURE;
                }
            } else {
                printf("Login failed. CRITICAL: Is your server daemon (gitimgd) running in another terminal?\n");
                return EXIT_FAILURE;
            }
        }
        
        save_global_config(argv[2], token, target_host, target_port);
        printf("Successfully logged into %s:%d as %s.\n", target_host, target_port, argv[2]);
    }
    else if (strcmp(argv[1], "logout") == 0) {
        clear_global_config();
        printf("Logged out successfully.\n");
    }
    else if (strcmp(argv[1], "push") == 0 || strcmp(argv[1], "commit") == 0) {
        ClientRepo repo(".", g_host, g_port);
        const char* msg = (argc > 2) ? argv[2] : "Auto-commit asset sync";
        
        struct stat st;
        if (stat(".gitimg", &st) == -1) {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                char* folder_name = strrchr(cwd, '/');
                folder_name = folder_name ? folder_name + 1 : cwd;
                
                char target[256];
                snprintf(target, sizeof(target), "%s/%s", g_username[0] ? g_username : "anonymous", folder_name);
                
                printf("Auto-initializing remote repository: %s on %s:%d\n", target, g_host, g_port);
                repo.init(target, g_host, g_port);
                Protocol::create_repo(g_host, g_port, folder_name);
            }
        }
        
        if (!repo.commit(msg)) {
            printf("Push failed. Run 'gitimg login' again to refresh your connection.\n");
        } else {
            printf("Push complete. Artwork sync successful.\n");
        }
    }
    else if (strcmp(argv[1], "clone") == 0) {
        if (argc < 3) { printf("Error: Missing <owner>/<repo>\n"); return EXIT_FAILURE; }
        
        char target_host[256]; strncpy(target_host, g_host, 255);
        int target_port = g_port;
        
        if (argc >= 4) {
            char h[256]; int p;
            if (sscanf(argv[3], "%255[^:]:%d", h, &p) == 2) {
                strncpy(target_host, h, 255); target_port = p;
            } else {
                strncpy(target_host, argv[3], 255);
            }
        }

        char owner[128]={0}, repo_name[128]={0};
        if (sscanf(argv[2], "%127[^/]/%127s", owner, repo_name) != 2) {
            printf("Error: Format must be owner/repo\n"); return EXIT_FAILURE;
        }
        
        ClientRepo repo(".", target_host, target_port);
        repo.init(argv[2], target_host, target_port);
        printf("Cloned remote repository %s/%s from %s:%d\n", owner, repo_name, target_host, target_port);
    }
    else {
        print_usage();
    }

    return EXIT_SUCCESS;
}