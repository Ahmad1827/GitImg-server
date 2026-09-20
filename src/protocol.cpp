#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

char Protocol::g_auth_token[256] = {0};

void Protocol::set_token(const char* token) {
    strncpy(g_auth_token, token, 255);
}

int Protocol::connect_server(const char* host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;
    
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { 
        freeaddrinfo(res); 
        return -1; 
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int conn_res = connect(sock, res->ai_addr, res->ai_addrlen);
    if (conn_res < 0) {
        if (errno == EINPROGRESS) {
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;

            if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error != 0) {
                    close(sock);
                    freeaddrinfo(res);
                    return -1;
                }
            } else {
                close(sock);
                freeaddrinfo(res);
                return -1;
            }
        } else {
            close(sock);
            freeaddrinfo(res);
            return -1;
        }
    }

    fcntl(sock, F_SETFL, flags);

    struct timeval rw_tv;
    rw_tv.tv_sec = 4;
    rw_tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rw_tv, sizeof(rw_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rw_tv, sizeof(rw_tv));

    freeaddrinfo(res);
    return sock;
}

bool Protocol::send_http_get(const char* host, int port, const char* endpoint) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[1024];
    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
             "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
             endpoint, host, port, g_auth_token);

    if (write(sock, header, strlen(header))) {}
    char response[1024];
    ssize_t bytes = read(sock, response, sizeof(response) - 1);
    close(sock);

    if (bytes > 0) {
        response[bytes] = '\0';
        if (strstr(response, "200 OK") != NULL) return true;
    }
    return false;
}

bool Protocol::fetch_http_get(const char* host, int port, const char* endpoint, char* out_buffer, size_t max_len) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[1024];
    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
             "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
             endpoint, host, port, g_auth_token);
             
    if (write(sock, header, strlen(header))) {}

    std::string response;
    char chunk[4096];
    ssize_t bytes;
    while ((bytes = read(sock, chunk, sizeof(chunk))) > 0) {
        response.append(chunk, bytes);
    }
    close(sock);

    size_t header_end = response.find("\r\n\r\n");
    if (header_end != std::string::npos && response.find("200 OK") != std::string::npos) {
        std::string body = response.substr(header_end + 4);
        strncpy(out_buffer, body.c_str(), max_len - 1);
        out_buffer[max_len - 1] = '\0';
        return true;
    }
    return false;
}

bool Protocol::fetch_to_file(const char* host, int port, const char* endpoint, const char* filepath) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[1024];
    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
             "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
             endpoint, host, port, g_auth_token);
             
    if (write(sock, header, strlen(header))) {}
    char buf[16384];
    ssize_t bytes = read(sock, buf, sizeof(buf));
    if (bytes <= 0) { close(sock); return false; }

    char* body_start = nullptr;
    for (ssize_t i = 0; i < bytes - 3; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            body_start = buf + i + 4; break;
        }
    }

    if (!body_start || strstr(buf, "200 OK") == NULL) { close(sock); return false; }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { close(sock); return false; }

    ssize_t header_len = body_start - buf;
    ssize_t body_bytes = bytes - header_len;
    if (body_bytes > 0) { if (write(fd, body_start, body_bytes)) {} }

    while ((bytes = read(sock, buf, sizeof(buf))) > 0) { if (write(fd, buf, bytes)) {} }

    close(fd); close(sock);
    return true;
}

bool Protocol::send_http_post(const char* host, int port, const char* endpoint, const uint8_t* data, size_t size, char* out_resp) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[1024];
    snprintf(header, sizeof(header),
             "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
             "Authorization: Bearer %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
             endpoint, host, port, g_auth_token, size);

    if (write(sock, header, strlen(header))) {}
    size_t total_sent = 0;
    while (total_sent < size) {
        ssize_t sent = write(sock, data + total_sent, size - total_sent);
        if (sent < 0) break;
        total_sent += sent;
    }

    char response[2048] = {0};
    if (read(sock, response, sizeof(response) - 1)) {}
    close(sock);
    
    if (out_resp) {
        char* body = strstr(response, "\r\n\r\n");
        if (body) {
            strncpy(out_resp, body + 4, 1023);
        }
    }

    return (total_sent == size && strstr(response, "200 OK") != NULL);
}

bool Protocol::login(const char* host, int port, const char* username, const char* password, char* out_token) {
    char json[512];
    snprintf(json, sizeof(json), "{\"username\":\"%s\",\"password\":\"%s\"}", username, password);
    char resp[1024] = {0};
    
    if (send_http_post(host, port, "/auth/login", (const uint8_t*)json, strlen(json), resp)) {
        char* token_start = strstr(resp, "\"token\":\"");
        if (token_start) {
            token_start += 9;
            char* token_end = strchr(token_start, '"');
            if (token_end) {
                size_t len = token_end - token_start;
                strncpy(out_token, token_start, len);
                out_token[len] = '\0';
                set_token(out_token);
                return true;
            }
        }
    }
    return false;
}

bool Protocol::logout(const char* host, int port) {
    return send_http_post(host, port, "/auth/logout", (const uint8_t*)"{}", 2);
}

bool Protocol::check_chunk(const char* host, int port, uint64_t hash) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/check/chunk/%lx", hash);
    return send_http_get(host, port, endpoint);
}

bool Protocol::push_chunk(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/push/chunk/%lx", hash);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::push_manifest(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/push/manifest/%lx", hash);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::push_commit(const char* host, int port, const char* owner, const char* repo_name, uint64_t hash, const uint8_t* data, size_t size) {
    (void)hash;
    char endpoint[512]; snprintf(endpoint, sizeof(endpoint), "/repo/%s/%s/push", owner, repo_name);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::create_repo(const char* host, int port, const char* repo_name) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/repo/create/%s", repo_name);
    return send_http_post(host, port, endpoint, (const uint8_t*)"", 0);
}

bool Protocol::push_asset_meta(const char* host, int port, const char* owner, const char* repo_name, const uint8_t* data, size_t size) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/repo/asset/%s/%s", owner, repo_name);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::fetch_commit(const char* host, int port, const char* hash_str, const char* out_path) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/get/commit/%s", hash_str);
    return fetch_to_file(host, port, endpoint, out_path);
}

bool Protocol::fetch_manifest(const char* host, int port, uint64_t hash, const char* out_path) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/get/manifest/%lx", hash);
    return fetch_to_file(host, port, endpoint, out_path);
}

bool Protocol::fetch_chunk(const char* host, int port, uint64_t hash, const char* out_path) {
    char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "/get/chunk/%lx", hash);
    return fetch_to_file(host, port, endpoint, out_path);
}