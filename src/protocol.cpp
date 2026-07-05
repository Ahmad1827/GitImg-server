#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int Protocol::connect_server(const char* host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }
    
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }
    
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    
    freeaddrinfo(res);
    return sock;
}

bool Protocol::send_http_get(const char* host, int port, const char* endpoint) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[512];
    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n\r\n",
             endpoint, host, port);

    write(sock, header, strlen(header));

    char response[1024];
    ssize_t bytes = read(sock, response, sizeof(response) - 1);
    close(sock);

    if (bytes > 0) {
        response[bytes] = '\0';
        if (strstr(response, "200 OK") != NULL) {
            return true;
        }
    }
    return false;
}

bool Protocol::fetch_http_get(const char* host, int port, const char* endpoint, char* out_buffer, size_t max_len) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[512];
    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n\r\n",
             endpoint, host, port);
    write(sock, header, strlen(header));

    char response[8192];
    ssize_t bytes = read(sock, response, sizeof(response) - 1);
    close(sock);

    if (bytes > 0) {
        response[bytes] = '\0';
        char* body = strstr(response, "\r\n\r\n");
        if (body && strstr(response, "200 OK") != NULL) {
            body += 4;
            strncpy(out_buffer, body, max_len - 1);
            out_buffer[max_len - 1] = '\0';
            return true;
        }
    }
    return false;
}

bool Protocol::fetch_to_file(const char* host, int port, const char* endpoint, const char* filepath) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[512];
    snprintf(header, sizeof(header),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n\r\n",
             endpoint, host, port);
    write(sock, header, strlen(header));

    char buf[16384];
    ssize_t bytes = read(sock, buf, sizeof(buf));
    if (bytes <= 0) {
        close(sock);
        return false;
    }

    char* body_start = nullptr;
    for (ssize_t i = 0; i < bytes - 3; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            body_start = buf + i + 4;
            break;
        }
    }

    if (!body_start || strstr(buf, "200 OK") == NULL) {
        close(sock);
        return false;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        close(sock);
        return false;
    }

    ssize_t header_len = body_start - buf;
    ssize_t body_bytes = bytes - header_len;
    if (body_bytes > 0) {
        write(fd, body_start, body_bytes);
    }

    while ((bytes = read(sock, buf, sizeof(buf))) > 0) {
        write(fd, buf, bytes);
    }

    close(fd);
    close(sock);
    return true;
}

bool Protocol::send_http_post(const char* host, int port, const char* endpoint, const uint8_t* data, size_t size) {
    int sock = connect_server(host, port);
    if (sock < 0) return false;

    char header[512];
    snprintf(header, sizeof(header),
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             endpoint, host, port, size);

    write(sock, header, strlen(header));
    
    size_t total_sent = 0;
    while (total_sent < size) {
        ssize_t sent = write(sock, data + total_sent, size - total_sent);
        if (sent < 0) break;
        total_sent += sent;
    }

    char response[1024];
    read(sock, response, sizeof(response) - 1);
    close(sock);

    return (total_sent == size);
}

bool Protocol::check_chunk(const char* host, int port, uint64_t hash) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/check/chunk/%lx", hash);
    return send_http_get(host, port, endpoint);
}

bool Protocol::push_chunk(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/push/chunk/%lx", hash);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::push_manifest(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/push/manifest/%lx", hash);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::push_commit(const char* host, int port, uint64_t hash, const uint8_t* data, size_t size) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/push/commit/%lx", hash);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::create_repo(const char* host, int port, const char* repo_name) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/repo/create/%s", repo_name);
    return send_http_post(host, port, endpoint, (const uint8_t*)"", 0);
}

bool Protocol::push_asset_meta(const char* host, int port, const char* repo_name, const uint8_t* data, size_t size) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/repo/asset/%s", repo_name);
    return send_http_post(host, port, endpoint, data, size);
}

bool Protocol::fetch_commit(const char* host, int port, const char* hash_str, const char* out_path) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/get/commit/%s", hash_str);
    return fetch_to_file(host, port, endpoint, out_path);
}

bool Protocol::fetch_manifest(const char* host, int port, uint64_t hash, const char* out_path) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/get/manifest/%lx", hash);
    return fetch_to_file(host, port, endpoint, out_path);
}

bool Protocol::fetch_chunk(const char* host, int port, uint64_t hash, const char* out_path) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/get/chunk/%lx", hash);
    return fetch_to_file(host, port, endpoint, out_path);
}