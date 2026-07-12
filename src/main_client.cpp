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

void read_line(const char* prompt, char* buffer, size_t max_len) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, max_len, stdin)) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') buffer[len-1] = '\0';
    } else {
        buffer[0] = '\0';
    }
}

void print_usage() {
    printf("Usage:\n");
    printf("  gitimg login\n");
    printf("  gitimg push [\"message\"]\n");
    printf("  gitimg logout\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return EXIT_FAILURE; }

    load_global_config();

    if (strcmp(argv[1], "login") == 0) {
        char target_host[256] = {0};
        char username[128] = {0};
        char password[128] = {0};

        if (argc >= 5) {
            strncpy(username, argv[2], 127); username[127] = '\0';
            strncpy(password, argv[3], 127); password[127] = '\0';
            strncpy(target_host, argv[4], 255); target_host[255] = '\0';
        } else {
            printf("Connecting...\n");
            read_line("Server IP: ", target_host, sizeof(target_host));
            read_line("Username: ", username, sizeof(username));
            char* pass = getpass("Password: ");
            if (pass) {
                strncpy(password, pass, 127);
                password[127] = '\0';
            }
        }

        char h[256]; int target_port = 8080;
        if (strchr(target_host, ':')) {
            sscanf(target_host, "%255[^:]:%d", h, &target_port);
            strncpy(target_host, h, 255); target_host[255] = '\0';
        }

        printf("Authenticating...\n");

        char token[256] = {0};
        if (!Protocol::login(target_host, target_port, username, password, token)) {
            if (strcmp(target_host, "127.0.0.1") != 0 && strcmp(target_host, "localhost") != 0) {
                if (Protocol::login("127.0.0.1", target_port, username, password, token)) {
                    strncpy(target_host, "127.0.0.1", 255); target_host[255] = '\0';
                } else {
                    printf("Login failed. Check credentials or server network connection.\n");
                    return EXIT_FAILURE;
                }
            } else {
                printf("Login failed. Check credentials or server network connection.\n");
                return EXIT_FAILURE;
            }
        }
        
        save_global_config(username, token, target_host, target_port);
        printf("Successfully logged in.\n");
    }
    else if (strcmp(argv[1], "logout") == 0) {
        clear_global_config();
        printf("Logged out successfully.\n");
    }
    else if (strcmp(argv[1], "push") == 0 || strcmp(argv[1], "commit") == 0) {
        if (strlen(g_username) == 0) {
            printf("Error: Not logged in. Please run 'gitimg login' first.\n");
            return EXIT_FAILURE;
        }

        ClientRepo repo(".", g_host, g_port);
        const char* msg = (argc > 2) ? argv[2] : "Auto-commit asset sync";
        
        char folder_name[256] = {0};
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            char* slash = strrchr(cwd, '/');
            strncpy(folder_name, slash ? slash + 1 : cwd, 255); folder_name[255] = '\0';
        } else {
            strncpy(folder_name, "default_repo", 255); folder_name[255] = '\0';
        }

        char target[512];
        snprintf(target, sizeof(target), "%s/%s", g_username, folder_name);

        struct stat st;
        if (stat(".gitimg", &st) == -1) {
            repo.init(target, g_host, g_port);
            Protocol::create_repo(g_host, g_port, folder_name);
        }
        
        printf("Connecting...\n");
        printf("Repository found.\n");
        
        if (!repo.commit(msg)) {
            printf("Push failed. Run 'gitimg login' again to refresh your connection.\n");
        } else {
            printf("Done.\n");
            printf("View your repository:\n");
            printf("http://%s:%d/%s\n", g_host, g_port, target);
        }
    }
    else {
        print_usage();
    }

    return EXIT_SUCCESS;
}