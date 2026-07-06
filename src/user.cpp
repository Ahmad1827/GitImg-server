#include "user.h"
#include "cdc_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

void UserManager::hash_password(const char* password, char* out_hash) {
    // NOTE: Using FNV-1a as a placeholder hash to maintain zero-dependencies.
    // In production, this MUST be replaced with bcrypt/Argon2.
    uint64_t h = CDCHasher::fnv1a_hash((const uint8_t*)password, strlen(password));
    snprintf(out_hash, 128, "%016lx", h);
}

void UserManager::generate_random_token(char* out_token, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        uint8_t buf[32];
        read(fd, buf, 32);
        close(fd);
        out_token[0] = '\0';
        for(int i = 0; i < 16 && i < (int)(len/2) - 1; i++) {
            char hex[3];
            snprintf(hex, 3, "%02x", buf[i]);
            strcat(out_token, hex);
        }
    } else {
        snprintf(out_token, len, "%016lx%016lx", (uint64_t)time(NULL), (uint64_t)rand());
    }
}

bool UserManager::create_user(const char* base_dir, const char* username, const char* email, const char* password) {
    char u_path[2048];
    snprintf(u_path, sizeof(u_path), "%s/users/%s.user", base_dir, username);
    
    if (access(u_path, F_OK) == 0) return false; // User already exists

    char hash[128];
    hash_password(password, hash);

    int fd = open(u_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    char u_id[64];
    snprintf(u_id, sizeof(u_id), "usr_%016lx", (uint64_t)time(NULL));

    char meta[1024];
    snprintf(meta, sizeof(meta), 
        "ID: %s\n"
        "USERNAME: %s\n"
        "EMAIL: %s\n"
        "HASH: %s\n"
        "CREATED: %lu\n"
        "AVATAR: default_avatar.png\n\n",
        u_id, username, email, hash, (uint64_t)time(NULL));
    
    write(fd, meta, strlen(meta));
    close(fd);
    return true;
}

bool UserManager::verify_login(const char* base_dir, const char* username, const char* password) {
    UserMetadata u;
    if (!get_user(base_dir, username, &u)) return false;

    char hash[128];
    hash_password(password, hash);

    return (strcmp(u.password_hash, hash) == 0);
}

bool UserManager::create_session(const char* base_dir, const char* username, char* out_token) {
    generate_random_token(out_token, 64);
    
    char s_path[2048];
    snprintf(s_path, sizeof(s_path), "%s/sessions/%s.session", base_dir, out_token);
    
    int fd = open(s_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    uint64_t expiry = time(NULL) + (86400 * 7); // 7 days
    char meta[512];
    snprintf(meta, sizeof(meta), "TOKEN: %s\nUSERNAME: %s\nEXPIRES: %lu\n\n", out_token, username, expiry);
    
    write(fd, meta, strlen(meta));
    close(fd);
    return true;
}

bool UserManager::validate_session(const char* base_dir, const char* token, char* out_username) {
    char s_path[2048];
    snprintf(s_path, sizeof(s_path), "%s/sessions/%s.session", base_dir, token);
    
    int fd = open(s_path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    fstat(fd, &st);
    char* buf = (char*)malloc(st.st_size + 1);
    read(fd, buf, st.st_size);
    buf[st.st_size] = '\0';
    close(fd);

    char t_usr[64] = {0};
    uint64_t expiry = 0;

    char* line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "USERNAME: ", 10) == 0) strncpy(t_usr, line + 10, 63);
        else if (strncmp(line, "EXPIRES: ", 9) == 0) expiry = strtoull(line + 9, NULL, 10);
        line = strtok(NULL, "\n");
    }
    free(buf);

    if (time(NULL) > (time_t)expiry) {
        unlink(s_path); // Expired
        return false;
    }

    if (t_usr[0] != '\0') {
        strncpy(out_username, t_usr, 63);
        return true;
    }
    return false;
}

bool UserManager::get_user(const char* base_dir, const char* username, UserMetadata* out_user) {
    char u_path[2048];
    snprintf(u_path, sizeof(u_path), "%s/users/%s.user", base_dir, username);
    
    int fd = open(u_path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    fstat(fd, &st);
    char* buf = (char*)malloc(st.st_size + 1);
    read(fd, buf, st.st_size);
    buf[st.st_size] = '\0';
    close(fd);

    char* line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "ID: ", 4) == 0) strncpy(out_user->user_id, line + 4, 63);
        else if (strncmp(line, "USERNAME: ", 10) == 0) strncpy(out_user->username, line + 10, 63);
        else if (strncmp(line, "EMAIL: ", 7) == 0) strncpy(out_user->email, line + 7, 127);
        else if (strncmp(line, "HASH: ", 6) == 0) strncpy(out_user->password_hash, line + 6, 127);
        else if (strncmp(line, "CREATED: ", 9) == 0) out_user->creation_time = strtoull(line + 9, NULL, 10);
        else if (strncmp(line, "AVATAR: ", 8) == 0) strncpy(out_user->avatar_path, line + 8, 255);
        line = strtok(NULL, "\n");
    }
    free(buf);
    return true;
}