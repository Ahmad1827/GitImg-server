#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

class Protocol {
public:
    static bool check_chunk(const char* host, int port, uint64_t hash);
    static bool push_chunk(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size);
    static bool push_manifest(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size);
    static bool push_commit(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size);
    
    static bool create_repo(const char* host, int port, const char* repo_name);
    static bool push_asset_meta(const char* host, int port, const char* repo_name, const uint8_t* data, size_t size);
    static bool fetch_http_get(const char* host, int port, const char* endpoint, char* out_buffer, size_t max_len);

    static bool fetch_commit(const char* host, int port, const char* hash_str, const char* out_path);
    static bool fetch_manifest(const char* host, int port, uint64_t hash, const char* out_path);
    static bool fetch_chunk(const char* host, int port, uint64_t hash, const char* out_path);

private:
    static int connect_server(const char* host, int port);
    static bool send_http_get(const char* host, int port, const char* endpoint);
    static bool send_http_post(const char* host, int port, const char* endpoint, const uint8_t* data, size_t size);
    static bool fetch_to_file(const char* host, int port, const char* endpoint, const char* filepath);
};

#endif