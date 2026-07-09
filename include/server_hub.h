#ifndef SERVER_HUB_H
#define SERVER_HUB_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include "packfile.h"

class ServerHub {
public:
    ServerHub(const char* config_path);
    ~ServerHub();

    bool start();
    void run();

private:
    int server_fd;
    int port_num;
    char bind_host[128];
    char base_dir[1024];
    char public_url[512];
    
    uint64_t start_time;
    uint64_t req_count;
    
    PackfileManager* pack_mgr;

    void load_config(const char* config_path);
    bool check_repo_access(const char* owner, const char* repo, const char* auth_user, bool is_write);
    
    void log_activity(const char* user, const char* action, const char* target);
    void generate_thumbnail(uint64_t manifest_hash);
    
    std::string build_html_header(const std::string& title);
    std::string build_html_footer();
    
    void handle_client(int client_fd);
    void process_post(int client_fd, const char* path, const char* auth_user, const uint8_t* body, size_t body_len, const char* real_ip);
};

#endif