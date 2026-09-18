#ifndef CLIENT_REPO_H
#define CLIENT_REPO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string>
#include <vector>

struct CommitEntry {
    std::string hash;
    uint64_t timestamp;
    std::string message;
};

class ClientRepo {
public:
    ClientRepo(const char* dir, const char* default_host, int default_port);
    ~ClientRepo();
    
    bool init(const char* repo_target, const char* host, int port);
    bool commit(const char* message);
    bool checkout(const char* commit_hash_str);
    bool log(bool json_format);
    std::vector<CommitEntry> get_commit_list();
    void watch();

private:
    char base_dir[1024];
    char repo_dir[1024];
    char current_owner[128];
    char current_repo[128];
    char srv_host[256];
    int srv_port;
    int inotify_fd;
    int watch_fd;

    bool is_tracked_file(const char* filename);
    uint64_t chunk_and_push(const char* filepath, uint64_t* out_size, uint32_t* skipped_chunks, uint32_t* new_chunks);
    void handle_events(int fd);
};

#endif