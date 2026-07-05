#ifndef SERVER_HUB_H
#define SERVER_HUB_H

#include "packfile.h"
#include <stdint.h>
#include <stddef.h>

class ServerHub {
public:
    ServerHub(int port, const char* storage_dir);
    ~ServerHub();

    bool start();
    void run();

private:
    int server_fd;
    int port_num;
    char base_dir[1024];
    PackfileManager* pack_mgr;

    void handle_client(int client_fd);
    void process_post(int client_fd, const char* path, const uint8_t* body, size_t body_len);
};

#endif