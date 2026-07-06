#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct UserMetadata {
    char user_id[64];
    char username[64];
    char email[128];
    char password_hash[128];
    uint64_t creation_time;
    char avatar_path[256];
};

struct SessionMetadata {
    char token[64];
    char username[64];
    uint64_t expires_at;
};

class UserManager {
public:
    static bool create_user(const char* base_dir, const char* username, const char* email, const char* password);
    static bool verify_login(const char* base_dir, const char* username, const char* password);
    static bool create_session(const char* base_dir, const char* username, char* out_token);
    static bool validate_session(const char* base_dir, const char* token, char* out_username);
    static bool get_user(const char* base_dir, const char* username, UserMetadata* out_user);

private:
    static void generate_random_token(char* out_token, size_t len);
    static void hash_password(const char* password, char* out_hash);
};

#endif