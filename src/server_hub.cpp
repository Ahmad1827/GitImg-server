#define _GNU_SOURCE
#include "server_hub.h"
#include "repository.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

ServerHub::ServerHub(int port, const char* storage_dir) : server_fd(-1), port_num(port) {
    strncpy(base_dir, storage_dir, sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';
    pack_mgr = new PackfileManager(base_dir);
}

ServerHub::~ServerHub() {
    if (server_fd >= 0) close(server_fd);
    delete pack_mgr;
}

bool ServerHub::start() {
    struct stat st = {0};
    if (stat(base_dir, &st) == -1) mkdir(base_dir, 0755);

    char manifest_dir[1024], commit_dir[1024], repos_dir[1024];
    snprintf(manifest_dir, sizeof(manifest_dir), "%s/manifests", base_dir);
    snprintf(commit_dir, sizeof(commit_dir), "%s/commits", base_dir);
    snprintf(repos_dir, sizeof(repos_dir), "%s/repos", base_dir);

    if (stat(manifest_dir, &st) == -1) mkdir(manifest_dir, 0755);
    if (stat(commit_dir, &st) == -1) mkdir(commit_dir, 0755);
    if (stat(repos_dir, &st) == -1) mkdir(repos_dir, 0755);

    if (!pack_mgr->init()) return false;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) return false;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_num);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return false;
    if (listen(server_fd, 10) < 0) return false;

    printf("GitImg Server listening on port %d...\n", port_num);
    return true;
}

void ServerHub::process_post(int client_fd, const char* path, const uint8_t* body, size_t body_len) {
    if (strncmp(path, "/push/chunk/", 12) == 0) {
        uint64_t hash = strtoull(path + 12, NULL, 16);
        printf("[Server] RCV CHUNK    | Hash: %016lx | Size: %zu bytes\n", hash, body_len);
        pack_mgr->append_chunk(hash, body, body_len);
    } 
    else if (strncmp(path, "/push/manifest/", 15) == 0) {
        printf("[Server] RCV MANIFEST | Hash: %s\n", path + 15);
        char m_path[2048];
        snprintf(m_path, sizeof(m_path), "%s/manifests/%s.manifest", base_dir, path + 15);
        int fd = open(m_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, body, body_len);
            close(fd);
        }
    } 
    else if (strncmp(path, "/push/commit/", 13) == 0) {
        printf("[Server] RCV COMMIT   | Hash: %s\n", path + 13);
        char c_path[2048];
        snprintf(c_path, sizeof(c_path), "%s/commits/%s.commit", base_dir, path + 13);
        int fd = open(c_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, body, body_len);
            close(fd);
        }
    }
    else if (strncmp(path, "/repo/create/", 13) == 0) {
        printf("[Server] CREATE REPO  | Name: %s\n", path + 13);
        char r_path[2048];
        snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, path + 13);
        int fd = open(r_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char meta[256];
            snprintf(meta, sizeof(meta), "ID: %s\nNAME: %s\nOWNER: artist\nCREATED: %ld\n\n", path + 13, path + 13, time(NULL));
            write(fd, meta, strlen(meta));
            close(fd);
        }
    }
    else if (strncmp(path, "/repo/asset/", 12) == 0) {
        printf("[Server] RCV ASSET    | Repo: %s\n", path + 12);
        char a_path[2048];
        snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, path + 12);
        int fd = open(a_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, body, body_len);
            close(fd);
        }
    }
    
    const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
    write(client_fd, resp, strlen(resp));
}

void ServerHub::handle_client(int client_fd) {
    char header_buf[4096];
    ssize_t h_bytes = read(client_fd, header_buf, sizeof(header_buf) - 1);
    if (h_bytes <= 0) {
        close(client_fd);
        return;
    }
    header_buf[h_bytes] = '\0';

    char* body_ptr = strstr(header_buf, "\r\n\r\n");
    if (!body_ptr) {
        close(client_fd);
        return;
    }
    
    *body_ptr = '\0';
    body_ptr += 4;

    char method[16], path[256];
    sscanf(header_buf, "%15s %255s", method, path);

    if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/check/chunk/", 13) == 0) {
            uint64_t hash = strtoull(path + 13, NULL, 16);
            if (pack_mgr->has_chunk(hash)) {
                const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            }
        } 
        else if (strncmp(path, "/repo/info/", 11) == 0 || strncmp(path, "/repo/assets/", 13) == 0) {
            char r_path[2048];
            if (strncmp(path, "/repo/info/", 11) == 0) {
                snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, path + 11);
                printf("[Server] GET REPO     | Name: %s\n", path + 11);
            } else {
                snprintf(r_path, sizeof(r_path), "%s/repos/%s_assets.meta", base_dir, path + 13);
                printf("[Server] GET ASSETS   | Name: %s\n", path + 13);
            }
            
            int fd = open(r_path, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                fstat(fd, &st);
                uint8_t* buf = (uint8_t*)malloc(st.st_size);
                read(fd, buf, st.st_size);
                close(fd);
                
                char resp_header[512];
                snprintf(resp_header, sizeof(resp_header), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", st.st_size);
                write(client_fd, resp_header, strlen(resp_header));
                write(client_fd, buf, st.st_size);
                free(buf);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            }
        }
        else if (strncmp(path, "/get/commit/", 12) == 0) {
            char c_path[2048];
            snprintf(c_path, sizeof(c_path), "%s/commits/%s.commit", base_dir, path + 12);
            printf("[Server] GET COMMIT   | Hash: %s\n", path + 12);
            int fd = open(c_path, O_RDONLY);
            if (fd >= 0) {
                struct stat st; fstat(fd, &st);
                char resp_header[512];
                snprintf(resp_header, sizeof(resp_header), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", st.st_size);
                write(client_fd, resp_header, strlen(resp_header));
                char buf[8192];
                ssize_t b;
                while ((b = read(fd, buf, sizeof(buf))) > 0) write(client_fd, buf, b);
                close(fd);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            }
        }
        else if (strncmp(path, "/get/manifest/", 14) == 0) {
            char m_path[2048];
            snprintf(m_path, sizeof(m_path), "%s/manifests/%s.manifest", base_dir, path + 14);
            printf("[Server] GET MANIFEST | Hash: %s\n", path + 14);
            int fd = open(m_path, O_RDONLY);
            if (fd >= 0) {
                struct stat st; fstat(fd, &st);
                char resp_header[512];
                snprintf(resp_header, sizeof(resp_header), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", st.st_size);
                write(client_fd, resp_header, strlen(resp_header));
                char buf[8192];
                ssize_t b;
                while ((b = read(fd, buf, sizeof(buf))) > 0) write(client_fd, buf, b);
                close(fd);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            }
        }
        else if (strncmp(path, "/get/chunk/", 11) == 0) {
            uint64_t hash = strtoull(path + 11, NULL, 16);
            printf("[Server] GET CHUNK    | Hash: %016lx\n", hash);
            uint8_t buffer[32768];
            uint32_t out_size = 0;
            if (pack_mgr->read_chunk(hash, buffer, &out_size)) {
                char resp_header[512];
                snprintf(resp_header, sizeof(resp_header), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", out_size);
                write(client_fd, resp_header, strlen(resp_header));
                write(client_fd, buffer, out_size);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            }
        }
        else if (strncmp(path, "/view/", 6) == 0) {
            char repo_name[256];
            snprintf(repo_name, sizeof(repo_name), "%s", path + 6);
            printf("[Server] WEB VIEW     | Repo: %s\n", repo_name);
            
            char r_path[2048];
            snprintf(r_path, sizeof(r_path), "%s/repos/%s_assets.meta", base_dir, repo_name);
            int fd = open(r_path, O_RDONLY);
            if (fd >= 0) {
                struct stat st; fstat(fd, &st);
                char* meta_buf = (char*)malloc(st.st_size + 1);
                read(fd, meta_buf, st.st_size);
                meta_buf[st.st_size] = '\0';
                close(fd);

                const char* header = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                                     "<!DOCTYPE html><html><head><title>GitImg Gallery</title>"
                                     "<style>"
                                     "body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; background: #0d1117; color: #c9d1d9; padding: 40px; margin: 0; }"
                                     ".container { max-width: 1200px; margin: 0 auto; }"
                                     ".gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 24px; margin-top: 30px; }"
                                     ".card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; overflow: hidden; display: flex; flex-direction: column; transition: transform 0.2s; }"
                                     ".card:hover { transform: translateY(-4px); border-color: #8b949e; }"
                                     ".card-img { height: 200px; width: 100%; display: flex; align-items: center; justify-content: center; background: #010409; overflow: hidden; }"
                                     ".card-img img { max-width: 100%; max-height: 100%; object-fit: contain; }"
                                     ".info { padding: 16px; }"
                                     ".info strong { display: block; font-size: 16px; margin-bottom: 8px; color: #58a6ff; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }"
                                     ".info span { display: block; font-size: 12px; color: #8b949e; margin-bottom: 4px; font-family: monospace; }"
                                     "h1 { border-bottom: 1px solid #21262d; padding-bottom: 16px; margin-top: 0; font-size: 28px; }"
                                     "</style></head><body><div class=\"container\">"
                                     "<h1>Repository: ";
                write(client_fd, header, strlen(header));
                write(client_fd, repo_name, strlen(repo_name));
                const char* html_mid = "</h1><div class=\"gallery\">";
                write(client_fd, html_mid, strlen(html_mid));

                char* line = strtok(meta_buf, "\n");
                char current_file[256] = {0};
                char current_manifest[256] = {0};
                char current_commit[256] = {0};
                char current_size[256] = {0};

                while (line) {
                    if (strncmp(line, "FILE: ", 6) == 0) strncpy(current_file, line + 6, 255);
                    else if (strncmp(line, "COMMIT: ", 8) == 0) strncpy(current_commit, line + 8, 255);
                    else if (strncmp(line, "MANIFEST: ", 10) == 0) strncpy(current_manifest, line + 10, 255);
                    else if (strncmp(line, "SIZE: ", 6) == 0) {
                        strncpy(current_size, line + 6, 255);
                        if (current_file[0] != '\0') {
                            char card[2048];
                            snprintf(card, sizeof(card), 
                                "<div class=\"card\">"
                                "<div class=\"card-img\">"
                                "<img src=\"/raw/%s/%s\" alt=\"%s\" loading=\"lazy\" onerror=\"this.style.display='none';\">"
                                "</div>"
                                "<div class=\"info\">"
                                "<strong>%s</strong>"
                                "<span>Commit: %s</span>"
                                "<span>Size: %s bytes</span>"
                                "</div></div>", 
                                current_manifest, current_file, current_file, current_file, current_commit, current_size);
                            write(client_fd, card, strlen(card));
                            current_file[0] = '\0';
                        }
                    }
                    line = strtok(NULL, "\n");
                }
                
                const char* footer = "</div></div></body></html>";
                write(client_fd, footer, strlen(footer));
                free(meta_buf);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nRepository not found.";
                write(client_fd, resp, strlen(resp));
            }
        }
        else if (strncmp(path, "/raw/", 5) == 0) {
            uint64_t m_hash = 0;
            char filename[256] = {0};
            sscanf(path, "/raw/%lx/%255s", &m_hash, filename);
            printf("[Server] WEB RAW IMG  | Manifest: %016lx | File: %s\n", m_hash, filename);
            
            char m_path[2048];
            snprintf(m_path, sizeof(m_path), "%s/manifests/%lx.manifest", base_dir, m_hash);
            int m_fd = open(m_path, O_RDONLY);
            if (m_fd >= 0) {
                const char* mime = RepositoryManager::detect_mime_type(filename);
                char header[512];
                snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", mime);
                write(client_fd, header, strlen(header));

                uint64_t chunk_hash;
                uint8_t buffer[32768];
                uint32_t out_size;
                while (read(m_fd, &chunk_hash, sizeof(uint64_t)) == sizeof(uint64_t)) {
                    if (pack_mgr->read_chunk(chunk_hash, buffer, &out_size)) {
                        write(client_fd, buffer, out_size);
                    }
                }
                close(m_fd);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(client_fd, resp, strlen(resp));
            }
        }
        else {
            const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            write(client_fd, resp, strlen(resp));
        }
        close(client_fd);
        return;
    }

    if (strcmp(method, "POST") != 0) {
        const char* bad = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
        write(client_fd, bad, strlen(bad));
        close(client_fd);
        return;
    }

    size_t content_length = 0;
    char* cl_hdr = strcasestr(header_buf, "Content-Length:");
    if (cl_hdr) {
        sscanf(cl_hdr + 15, "%zu", &content_length);
    }

    if (content_length == 0) {
        close(client_fd);
        return;
    }

    uint8_t* body = (uint8_t*)malloc(content_length);
    size_t header_body_len = h_bytes - (body_ptr - header_buf);
    
    if (header_body_len > 0) {
        memcpy(body, body_ptr, header_body_len);
    }

    size_t total_read = header_body_len;
    while (total_read < content_length) {
        ssize_t r = read(client_fd, body + total_read, content_length - total_read);
        if (r <= 0) break;
        total_read += r;
    }

    process_post(client_fd, path, body, total_read);
    
    free(body);
    close(client_fd);
}

void ServerHub::run() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd >= 0) {
            handle_client(client_fd);
        }
    }
}