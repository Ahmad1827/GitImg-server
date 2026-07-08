#ifndef SERVER_HUB_H
#define SERVER_HUB_H

#include <stdint.h>
#include <stddef.h>
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
    void handle_client(int client_fd);
    void process_post(int client_fd, const char* path, const char* auth_user, const uint8_t* body, size_t body_len, const char* real_ip);
};

#endif