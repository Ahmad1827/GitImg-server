#include "server_hub.h"
#include "repository.h"
#include "user.h"
#include "cdc_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <dirent.h>

static void extract_json_value(const char* json, const char* key, char* out_val) {
    out_val[0] = '\0';
    char search_key[64]; snprintf(search_key, sizeof(search_key), "\"%s\":\"", key);
    const char* start = strstr(json, search_key);
    if(start) {
        start += strlen(search_key);
        const char* end = strchr(start, '"');
        if(end) {
            size_t len = end - start;
            if(len > 255) len = 255;
            strncpy(out_val, start, len); out_val[len] = '\0';
        }
    }
}

ServerHub::ServerHub(const char* config_path) : server_fd(-1), start_time(time(NULL)), req_count(0) {
    port_num = 8080;
    strncpy(bind_host, "0.0.0.0", sizeof(bind_host) - 1); bind_host[sizeof(bind_host) - 1] = '\0';
    strncpy(base_dir, ".gitimgd", sizeof(base_dir) - 1); base_dir[sizeof(base_dir) - 1] = '\0';
    strncpy(public_url, "http://localhost:8080", sizeof(public_url) - 1); public_url[sizeof(public_url) - 1] = '\0';
    
    load_config(config_path);
    pack_mgr = new PackfileManager(base_dir);
}

void ServerHub::load_config(const char* config_path) {
    FILE* f = fopen(config_path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char key[256] = {0}, val[256] = {0};
            if (sscanf(line, "%255[^=]=%255[^\n]", key, val) == 2) {
                if (strcmp(key, "PORT") == 0) port_num = atoi(val);
                else if (strcmp(key, "HOST") == 0 || strcmp(key, "BIND_HOST") == 0) { strncpy(bind_host, val, 127); bind_host[127] = '\0'; }
                else if (strcmp(key, "STORAGE_DIR") == 0) { strncpy(base_dir, val, 1023); base_dir[1023] = '\0'; }
                else if (strcmp(key, "PUBLIC_URL") == 0) { strncpy(public_url, val, 511); public_url[511] = '\0'; }
            }
        }
        fclose(f);
    }
}

ServerHub::~ServerHub() {
    if (server_fd >= 0) close(server_fd);
    delete pack_mgr;
}

bool ServerHub::start() {
    struct stat st; memset(&st, 0, sizeof(struct stat));
    if (stat(base_dir, &st) == -1) mkdir(base_dir, 0755);

    const char* dirs[] = {"manifests", "commits", "repos", "thumbnails", "metadata", "users", "sessions"};
    for(int i=0; i<7; i++) {
        char d_path[2048]; snprintf(d_path, sizeof(d_path), "%s/%s", base_dir, dirs[i]);
        if (stat(d_path, &st) == -1) mkdir(d_path, 0755);
    }
    if (!pack_mgr->init()) return false;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) return false;
    
    int opt = 1; 
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in address;
    address.sin_family = AF_INET; 
    if (strcmp(bind_host, "0.0.0.0") == 0) {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        address.sin_addr.s_addr = inet_addr(bind_host);
        if (address.sin_addr.s_addr == INADDR_NONE) address.sin_addr.s_addr = INADDR_ANY; 
    }
    address.sin_port = htons(port_num);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return false;
    if (listen(server_fd, 128) < 0) return false;

    printf("GitImg Server running.\n");
    printf(" -> Bound to LAN Address: %s:%d\n", bind_host, port_num);
    printf(" -> Public URL mapping: %s\n", public_url);
    printf(" -> Storage Directory: %s\n", base_dir);
    return true;
}

bool ServerHub::check_repo_access(const char* owner, const char* repo, const char* auth_user, bool is_write) {
    char r_path[2048]; snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, repo);
    int fd = open(r_path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st; 
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return false; }
    
    char* buf = (char*)malloc((size_t)st.st_size + 1);
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    if(r >= 0) buf[r] = '\0'; else buf[0] = '\0';
    close(fd);

    char r_owner[128] = {0};
    int visibility = 0;
    char collabs[512] = {0};

    char* line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "OWNER: ", 7) == 0) { strncpy(r_owner, line+7, 127); r_owner[127]='\0'; }
        else if (strncmp(line, "VISIBILITY: ", 12) == 0) visibility = atoi(line+12);
        else if (strncmp(line, "COLLABS: ", 9) == 0) { strncpy(collabs, line+9, 511); collabs[511]='\0'; }
        line = strtok(NULL, "\n");
    }
    free(buf);

    if (strcmp(r_owner, owner) != 0) return false; 
    if (strcmp(auth_user, r_owner) == 0) return true;
    
    bool is_collab = (strlen(auth_user) > 0 && strstr(collabs, auth_user) != NULL);
    if (is_collab) return true;
    if (is_write) return false;
    if (visibility == 0) return true;

    return false;
}

void ServerHub::process_post(int client_fd, const char* path, const char* auth_user, const uint8_t* body, size_t body_len, const char* real_ip) {
    if (strcmp(path, "/auth/register") == 0 || strcmp(path, "/register") == 0) {
        char username[64]={0}, email[128]={0}, password[128]={0};
        extract_json_value((const char*)body, "username", username);
        extract_json_value((const char*)body, "email", email);
        extract_json_value((const char*)body, "password", password);
        
        if (username[0] && password[0] && UserManager::create_user(base_dir, username, email, password)) {
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"success\"}";
            if(write(client_fd, resp, strlen(resp))) {}
        } else {
            const char* resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"failed\"}";
            if(write(client_fd, resp, strlen(resp))) {}
        }
    }
    else if (strcmp(path, "/auth/login") == 0 || strcmp(path, "/login") == 0) {
        char username[64]={0}, password[128]={0};
        extract_json_value((const char*)body, "username", username);
        extract_json_value((const char*)body, "password", password);
        
        if (UserManager::verify_login(base_dir, username, password)) {
            char token[65]; UserManager::create_session(base_dir, username, token);
            char resp[512]; snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"token\":\"%s\"}", token);
            printf("[Auth] Login Success | User: %s | Client IP: %s\n", username, real_ip);
            if(write(client_fd, resp, strlen(resp))) {}
        } else {
            const char* resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"invalid credentials\"}";
            printf("[Auth] Login Failed | User: %s | Client IP: %s\n", username, real_ip);
            if(write(client_fd, resp, strlen(resp))) {}
        }
    }
    else if (strcmp(path, "/auth/logout") == 0) {
        const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{}";
        if(write(client_fd, resp, strlen(resp))) {}
    }
    else if (strncmp(path, "/push/chunk/", 12) == 0) {
        uint64_t hash = strtoull(path + 12, NULL, 16);
        pack_mgr->append_chunk(hash, body, body_len);
        const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
        if(write(client_fd, resp, strlen(resp))) {}
    } 
    else if (strncmp(path, "/push/manifest/", 15) == 0) {
        char m_path[2048]; snprintf(m_path, sizeof(m_path), "%s/manifests/%s.manifest", base_dir, path + 15);
        int fd = open(m_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { if(write(fd, body, body_len)) {} close(fd); }
        const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
        if(write(client_fd, resp, strlen(resp))) {}
    } 
    else if (strncmp(path, "/repo/", 6) == 0) {
        char owner[128]={0}, repo[128]={0}, action[128]={0};
        int parsed = sscanf(path, "/repo/%127[^/]/%127[^/]/%127s", owner, repo, action);
        
        if (parsed == 3 && strcmp(action, "push") == 0) {
            char r_path[2048]; snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, repo);
            if (access(r_path, F_OK) != 0) {
                if (strcmp(owner, auth_user) == 0) {
                    RepoMetadata r_meta;
                    strncpy(r_meta.id, repo, 63); r_meta.id[63]='\0';
                    strncpy(r_meta.name, repo, 127); r_meta.name[127]='\0';
                    strncpy(r_meta.owner, auth_user, 127); r_meta.owner[127]='\0';
                    r_meta.creation_time = time(NULL); r_meta.visibility = 0; r_meta.collaborators[0] = '\0';
                    int fd_r = open(r_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd_r >= 0) {
                        char meta_buf[1024]; size_t mlen = RepositoryManager::serialize_repo(&r_meta, meta_buf, sizeof(meta_buf));
                        if(write(fd_r, meta_buf, mlen)) {} close(fd_r);
                    }
                    printf("[Repo] Auto-Created: %s/%s by %s from %s\n", owner, repo, auth_user, real_ip);
                } else {
                    const char* resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
                    if(write(client_fd, resp, strlen(resp))) {} return;
                }
            } else {
                if (!check_repo_access(owner, repo, auth_user, true)) {
                    const char* resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
                    if(write(client_fd, resp, strlen(resp))) {} return;
                }
            }
            
            uint64_t c_hash = CDCHasher::fnv1a_hash((const uint8_t*)body, body_len); 
            char c_path[2048]; snprintf(c_path, sizeof(c_path), "%s/commits/%lx.commit", base_dir, c_hash);
            int fd = open(c_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { if(write(fd, body, body_len)) {} close(fd); }
            
            char rc_path[2048]; snprintf(rc_path, sizeof(rc_path), "%s/repos/%s_commits.meta", base_dir, repo);
            int rcfd = open(rc_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (rcfd >= 0) {
                char line[512]; snprintf(line, sizeof(line), "%lx\n", c_hash);
                if(write(rcfd, line, strlen(line))) {} close(rcfd);
            }
            printf("[Push] Target: %s/%s | Commit: %lx | Client IP: %s\n", owner, repo, c_hash, real_ip);
            const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
            if(write(client_fd, resp, strlen(resp))) {}
        }
        else if (strcmp(owner, "create") == 0) {
            const char* rname = repo;
            if (strlen(auth_user) == 0) {
                const char* resp = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {} return;
            }
            RepoMetadata r_meta;
            strncpy(r_meta.id, rname, 63); r_meta.id[63]='\0';
            strncpy(r_meta.name, rname, 127); r_meta.name[127]='\0';
            strncpy(r_meta.owner, auth_user, 127); r_meta.owner[127]='\0';
            r_meta.creation_time = time(NULL); r_meta.visibility = 0; r_meta.collaborators[0] = '\0';

            char r_path[2048]; snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, rname);
            int fd = open(r_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                char meta_buf[1024]; size_t mlen = RepositoryManager::serialize_repo(&r_meta, meta_buf, sizeof(meta_buf));
                if(write(fd, meta_buf, mlen)) {} close(fd);
            }
            const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
            if(write(client_fd, resp, strlen(resp))) {}
        }
        else if (strcmp(owner, "asset") == 0) {
            char sub_owner[128]={0}, sub_repo[128]={0};
            sscanf(path, "/repo/asset/%127[^/]/%127s", sub_owner, sub_repo);
            if (!check_repo_access(sub_owner, sub_repo, auth_user, true)) {
                const char* resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {} return;
            }
            char a_path[2048]; snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, sub_repo);
            int fd = open(a_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) { if(write(fd, body, body_len)) {} close(fd); }
            const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
            if(write(client_fd, resp, strlen(resp))) {}
        }
        else {
            const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            if(write(client_fd, resp, strlen(resp))) {}
        }
    }
    else {
        const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        if(write(client_fd, resp, strlen(resp))) {}
    }
}

void ServerHub::handle_client(int client_fd) {
    req_count++;
    
    char header_buf[8192];
    ssize_t h_bytes = read(client_fd, header_buf, sizeof(header_buf) - 1);
    if (h_bytes <= 0) { close(client_fd); return; }
    header_buf[h_bytes] = '\0';

    char* body_ptr = strstr(header_buf, "\r\n\r\n");
    if (!body_ptr) { close(client_fd); return; }
    *body_ptr = '\0'; body_ptr += 4;

    char method[16], path[512];
    sscanf(header_buf, "%15s %511s", method, path);

    char real_ip[64] = {0};
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(client_fd, (struct sockaddr*)&peer, &peer_len) == 0) {
        strncpy(real_ip, inet_ntoa(peer.sin_addr), 63); real_ip[63]='\0';
    }
    char* xff = strcasestr(header_buf, "X-Forwarded-For: ");
    if (xff) { sscanf(xff + 17, "%63[^ \r\n]", real_ip); }

    char auth_user[64] = {0};
    char* auth_hdr = strcasestr(header_buf, "Authorization: Bearer ");
    if (auth_hdr) {
        char token[65];
        if (sscanf(auth_hdr + 22, "%64s", token) == 1) {
            UserManager::validate_session(base_dir, token, auth_user);
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/health") == 0) {
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"healthy\"}";
            if(write(client_fd, resp, strlen(resp))) {}
        }
        else if (strcmp(path, "/version") == 0) {
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"version\":\"1.0.0-lan\",\"build\":\"gitimg-core\"}";
            if(write(client_fd, resp, strlen(resp))) {}
        }
        else if (strcmp(path, "/metrics") == 0) {
            uint64_t uptime = time(NULL) - start_time;
            char resp[512]; snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"requests\":%lu,\"uptime_sec\":%lu}", req_count, uptime);
            if(write(client_fd, resp, strlen(resp))) {}
        }
        else if (strncmp(path, "/check/chunk/", 13) == 0) {
            uint64_t hash = strtoull(path + 13, NULL, 16);
            if (pack_mgr->has_chunk(hash)) {
                const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {}
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {}
            }
        } 
        else if (strncmp(path, "/image/", 7) == 0 || strncmp(path, "/raw/", 5) == 0) {
            uint64_t m_hash = 0; char filename[256] = {0};
            if (strncmp(path, "/image/", 7) == 0) m_hash = strtoull(path + 7, NULL, 16);
            else sscanf(path, "/raw/%lx/%255s", &m_hash, filename);
            
            char m_path[2048]; snprintf(m_path, sizeof(m_path), "%s/manifests/%lx.manifest", base_dir, m_hash);
            int m_fd = open(m_path, O_RDONLY);
            if (m_fd >= 0) {
                const char* mime = (filename[0] != '\0') ? RepositoryManager::detect_mime_type(filename) : "image/jpeg";
                char header[512]; snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: public, max-age=31536000, immutable\r\nConnection: close\r\n\r\n", mime);
                if(write(client_fd, header, strlen(header))) {}

                uint64_t chunk_hash; uint8_t buffer[32768]; uint32_t out_size;
                while (read(m_fd, &chunk_hash, sizeof(uint64_t)) == sizeof(uint64_t)) {
                    if (pack_mgr->read_chunk(chunk_hash, buffer, &out_size)) {
                        if(write(client_fd, buffer, out_size)) {}
                    }
                }
                close(m_fd);
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {}
            }
        }
        else {
            char p1[128]={0}, p2[128]={0}, p3[128]={0};
            const char* p = path; if (p[0] == '/') p++;
            const char* s1 = strchr(p, '/');
            if (s1) {
                strncpy(p1, p, s1 - p); p1[s1-p]='\0';
                const char* p_next = s1 + 1;
                const char* s2 = strchr(p_next, '/');
                if (s2) { 
                    strncpy(p2, p_next, s2 - p_next); p2[s2-p_next]='\0';
                    strcpy(p3, s2 + 1); 
                } 
                else { strcpy(p2, p_next); }
            } else { strcpy(p1, p); }

            if (strlen(p1) > 0 && strlen(p2) > 0) {
                if (!check_repo_access(p1, p2, auth_user, false)) {
                    const char* resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
                    if(write(client_fd, resp, strlen(resp))) {} return;
                }
                
                if (strcmp(p3, "pull") == 0) {
                    const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"status\":\"pull_ready\"}";
                    if(write(client_fd, resp, strlen(resp))) {} return;
                }
                
                char latest_commit[256] = {0};
                int commit_count = 0;
                char c_path[2048]; snprintf(c_path, sizeof(c_path), "%s/repos/%s_commits.meta", base_dir, p2);
                int cfd = open(c_path, O_RDONLY);
                if (cfd >= 0) {
                    struct stat cst; 
                    if (fstat(cfd, &cst) == 0 && cst.st_size > 0) {
                        char* cbuf = (char*)malloc((size_t)cst.st_size + 1);
                        ssize_t rb = read(cfd, cbuf, (size_t)cst.st_size);
                        if(rb >= 0) {
                            cbuf[rb] = '\0';
                            char* cline = strtok(cbuf, "\n");
                            while(cline) { strncpy(latest_commit, cline, 255); commit_count++; cline = strtok(NULL, "\n"); }
                        }
                        free(cbuf);
                    }
                    close(cfd);
                }

                char header[8192];
                snprintf(header, sizeof(header), 
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                    "<!DOCTYPE html><html><head><title>%s/%s - GitImg LAN</title>"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                    "<style>"
                    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#0d1117;color:#c9d1d9;margin:0;padding:0;}"
                    ".navbar{background:#161b22;padding:16px 20px;border-bottom:1px solid #30363d;display:flex;align-items:center;}"
                    ".navbar .logo{font-weight:bold;font-size:20px;color:#fff;text-decoration:none;}"
                    ".container{max-width:1200px;margin:30px auto;padding:0 20px;}"
                    ".header{border-bottom:1px solid #30363d;padding-bottom:20px;margin-bottom:20px;}"
                    ".header h1{margin:0 0 10px 0;font-size:24px;font-weight:600;}"
                    ".header h1 a{color:#58a6ff;text-decoration:none;}"
                    ".stats{font-size:14px;color:#8b949e;display:flex;gap:20px;}"
                    ".layout{display:flex;flex-wrap:wrap;gap:30px;}"
                    ".main{flex:3;min-width:300px;}"
                    ".sidebar{flex:1;min-width:250px;background:#161b22;padding:20px;border-radius:8px;border:1px solid #30363d;height:fit-content;}"
                    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:16px;}"
                    ".card{background:#010409;border:1px solid #30363d;border-radius:6px;overflow:hidden;}"
                    ".card:hover{border-color:#8b949e;}"
                    ".card img{width:100%%;height:160px;object-fit:cover;display:block;background:#000;}"
                    ".card-info{padding:12px;}"
                    ".card-info strong{display:block;margin-bottom:6px;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
                    ".card-info strong a{color:#c9d1d9;text-decoration:none;}"
                    ".card-info strong a:hover{color:#58a6ff;}"
                    ".card-info span{font-size:12px;color:#8b949e;line-height:1.4;display:block;}"
                    "h2{margin-top:0;font-size:16px;border-bottom:1px solid #30363d;padding-bottom:10px;}"
                    ".commit{padding:10px 0;border-bottom:1px solid #30363d;font-family:monospace;font-size:13px;word-break:break-all;}"
                    ".commit:last-child{border-bottom:none;padding-bottom:0;}"
                    ".commit strong{color:#58a6ff;}"
                    "</style></head><body>"
                    "<div class=\"navbar\"><a href=\"/\" class=\"logo\">GitImg Studio</a></div>"
                    "<div class=\"container\">"
                    "<div class=\"header\">"
                    "<h1><a href=\"/%s\">%s</a> / <a href=\"/%s/%s\"><b>%s</b></a></h1>"
                    "<div class=\"stats\"><span>%d Commits</span></div>"
                    "</div><div class=\"layout\"><div class=\"main\"><h2>Assets Gallery</h2><div class=\"grid\">",
                    p1, p2, p1, p1, p1, p1, p2, p2, commit_count);
                if(write(client_fd, header, strlen(header))) {}

                char a_path[2048]; snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, p2);
                int afd = open(a_path, O_RDONLY);
                if (afd >= 0) {
                    struct stat ast; 
                    if (fstat(afd, &ast) == 0 && ast.st_size > 0) {
                        char* abuf = (char*)malloc((size_t)ast.st_size + 1);
                        ssize_t rb = read(afd, abuf, (size_t)ast.st_size);
                        if(rb >= 0) {
                            abuf[rb] = '\0';
                            char* aline = strtok(abuf, "\n");
                            char c_file[256]={0}, c_man[256]={0}, c_size[256]={0}, c_commit[256]={0}, c_time[256]={0};
                            while(aline) {
                                if (strncmp(aline, "FILE: ", 6) == 0) strncpy(c_file, aline+6, 255);
                                else if (strncmp(aline, "COMMIT: ", 8) == 0) strncpy(c_commit, aline+8, 255);
                                else if (strncmp(aline, "MANIFEST: ", 10) == 0) strncpy(c_man, aline+10, 255);
                                else if (strncmp(aline, "SIZE: ", 6) == 0) strncpy(c_size, aline+6, 255);
                                else if (strncmp(aline, "TIME: ", 6) == 0) strncpy(c_time, aline+6, 255);
                                else if (strncmp(aline, "HEIGHT: ", 8) == 0) {
                                    if (c_file[0] != '\0' && strcmp(c_commit, latest_commit) == 0) {
                                        char card[2048];
                                        snprintf(card, sizeof(card),
                                            "<div class=\"card\">"
                                            "<a href=\"/raw/%s/%s\"><img src=\"/image/%s\" alt=\"%s\" loading=\"lazy\"></a>"
                                            "<div class=\"card-info\">"
                                            "<strong><a href=\"/raw/%s/%s\">%s</a></strong>"
                                            "<span>Size: %s bytes<br>Uploaded: <span class=\"ts\">%s</span></span>"
                                            "</div></div>", c_man, c_file, c_man, c_file, c_man, c_file, c_file, c_size, c_time);
                                        if(write(client_fd, card, strlen(card))) {}
                                        c_file[0] = '\0';
                                    }
                                }
                                aline = strtok(NULL, "\n");
                            }
                        }
                        free(abuf);
                    }
                    close(afd);
                }

                const char* mid = "</div></div><div class=\"sidebar\"><h2>Timeline</h2><div class=\"commit-feed\">";
                if(write(client_fd, mid, strlen(mid))) {}

                cfd = open(c_path, O_RDONLY);
                if (cfd >= 0) {
                    struct stat cst; 
                    if (fstat(cfd, &cst) == 0 && cst.st_size > 0) {
                        char* cbuf = (char*)malloc((size_t)cst.st_size + 1);
                        ssize_t rb = read(cfd, cbuf, (size_t)cst.st_size);
                        if (rb >= 0) {
                            cbuf[rb] = '\0';
                            char* cline = strtok(cbuf, "\n");
                            while(cline) {
                                char cdiv[512]; snprintf(cdiv, sizeof(cdiv), "<div class=\"commit\">commit <strong>%s</strong></div>", cline);
                                if(write(client_fd, cdiv, strlen(cdiv))) {}
                                cline = strtok(NULL, "\n");
                            }
                        }
                        free(cbuf);
                    }
                    close(cfd);
                } else {
                    const char* no_com = "<div class=\"commit\">No activity yet.</div>";
                    if(write(client_fd, no_com, strlen(no_com))) {}
                }

                const char* footer = "</div></div></div></div><script>"
                                     "document.querySelectorAll('.ts').forEach(el => {"
                                     "const d = new Date(parseInt(el.innerText) * 1000);"
                                     "el.innerText = d.toLocaleDateString() + ' ' + d.toLocaleTimeString();"
                                     "});</script></body></html>";
                if(write(client_fd, footer, strlen(footer))) {}
            } else if (strlen(p1) > 0 && strlen(p2) == 0) {
                UserMetadata u;
                if (UserManager::get_user(base_dir, p1, &u)) {
                    int total_repos = 0;
                    char repo_list_html[8192] = {0};
                    
                    DIR* dir;
                    struct dirent* ent;
                    char repos_dir[2048];
                    snprintf(repos_dir, sizeof(repos_dir), "%s/repos", base_dir);
                    if ((dir = opendir(repos_dir)) != NULL) {
                        while ((ent = readdir(dir)) != NULL) {
                            if (strstr(ent->d_name, ".repo")) {
                                char rp[4096];
                                snprintf(rp, sizeof(rp), "%s/%s", repos_dir, ent->d_name);
                                int fd = open(rp, O_RDONLY);
                                if (fd >= 0) {
                                    struct stat st; 
                                    if (fstat(fd, &st) == 0 && st.st_size > 0) {
                                        char* buf = (char*)malloc((size_t)st.st_size+1);
                                        ssize_t rb = read(fd, buf, (size_t)st.st_size);
                                        if(rb >= 0) {
                                            buf[rb]='\0';
                                            char o_check[128]; snprintf(o_check, sizeof(o_check), "OWNER: %s", u.username);
                                            if (strstr(buf, o_check)) {
                                                char* name_pos = strstr(buf, "NAME: ");
                                                if(name_pos) {
                                                    char rname[128];
                                                    sscanf(name_pos + 6, "%127[^\n]", rname);
                                                    
                                                    int rc = 0;
                                                    char rc_path[2048]; snprintf(rc_path, sizeof(rc_path), "%s/repos/%s_commits.meta", base_dir, rname);
                                                    int rcfd = open(rc_path, O_RDONLY);
                                                    if (rcfd >= 0) {
                                                        struct stat rcst; 
                                                        if (fstat(rcfd, &rcst) == 0 && rcst.st_size > 0) {
                                                            char* rcbuf = (char*)malloc((size_t)rcst.st_size+1);
                                                            ssize_t rcb = read(rcfd, rcbuf, (size_t)rcst.st_size);
                                                            if (rcb >= 0) {
                                                                rcbuf[rcb]='\0';
                                                                for(off_t i=0; i<rcst.st_size; i++) if(rcbuf[i]=='\n') rc++;
                                                            }
                                                            free(rcbuf);
                                                        }
                                                        close(rcfd);
                                                    }

                                                    char card[512];
                                                    snprintf(card, sizeof(card), 
                                                        "<div class=\"repo-card\"><h3><a href=\"/%s/%s\">%s</a></h3>"
                                                        "<span style=\"color:#8b949e;font-size:12px;\">%d Commits</span></div>", 
                                                        u.username, rname, rname, rc);
                                                    strncat(repo_list_html, card, sizeof(repo_list_html) - strlen(repo_list_html) - 1);
                                                    total_repos++;
                                                }
                                            }
                                        }
                                        free(buf);
                                    }
                                    close(fd);
                                }
                            }
                        }
                        closedir(dir);
                    }

                    char header[8192];
                    snprintf(header, sizeof(header), 
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><title>%s - GitImg LAN</title>"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                        "<style>body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;margin:0;}"
                        ".navbar{background:#161b22;padding:16px 20px;border-bottom:1px solid #30363d;}"
                        ".navbar .logo{font-weight:bold;font-size:20px;color:#fff;text-decoration:none;}"
                        ".container{max-width:1000px;margin:30px auto;display:flex;flex-wrap:wrap;gap:40px;padding:0 20px;}"
                        ".sidebar{width:280px;min-width:250px;}"
                        ".avatar{width:100%%;aspect-ratio:1/1;border-radius:50%%;border:1px solid #30363d;background:#21262d;margin-bottom:16px;display:flex;align-items:center;justify-content:center;font-size:64px;color:#8b949e;}"
                        ".username{font-size:24px;font-weight:600;color:#c9d1d9;margin-bottom:4px;}"
                        ".stats{display:flex;gap:16px;margin-top:16px;border-top:1px solid #30363d;padding-top:16px;}"
                        ".stat-box{display:flex;flex-direction:column;}"
                        ".stat-num{font-size:20px;font-weight:bold;color:#c9d1d9;}"
                        ".stat-label{font-size:12px;color:#8b949e;}"
                        ".main{flex:1;min-width:300px;}"
                        ".repo-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:16px;}"
                        ".repo-card{border:1px solid #30363d;padding:16px;border-radius:6px;background:#0d1117;}"
                        ".repo-card h3{margin:0 0 8px 0;}"
                        ".repo-card a{color:#58a6ff;text-decoration:none;font-size:18px;font-weight:600;}"
                        "</style></head><body>"
                        "<div class=\"navbar\"><a href=\"/\" class=\"logo\">GitImg Studio</a></div>"
                        "<div class=\"container\"><div class=\"sidebar\">"
                        "<div class=\"avatar\">%c</div>"
                        "<div class=\"username\">%s</div>"
                        "<div class=\"stats\">"
                        "<div class=\"stat-box\"><span class=\"stat-num\">%d</span><span class=\"stat-label\">Repositories</span></div>"
                        "</div></div><div class=\"main\">"
                        "<h2 style=\"border-bottom:1px solid #30363d;padding-bottom:8px;margin-top:0;\">Artwork Portfolios</h2>"
                        "<div class=\"repo-grid\">", u.username, u.username[0] >= 'a' ? u.username[0] - 32 : u.username[0], u.username, total_repos);
                    
                    if(write(client_fd, header, strlen(header))) {}
                    if(write(client_fd, repo_list_html, strlen(repo_list_html))) {}
                    if(write(client_fd, "</div></div></div></body></html>", 32)) {}

                } else {
                    const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                    if(write(client_fd, resp, strlen(resp))) {}
                }
            } else {
                const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {}
            }
        }
        close(client_fd);
        return;
    }

    if (strcmp(method, "POST") != 0) {
        const char* bad = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
        if(write(client_fd, bad, strlen(bad))) {} close(client_fd); return;
    }

    size_t content_length = 0;
    char* cl_hdr = strcasestr(header_buf, "Content-Length:");
    if (cl_hdr) sscanf(cl_hdr + 15, "%zu", &content_length);

    uint8_t* body = nullptr; size_t total_read = 0;
    
    if (content_length > 0) {
        body = (uint8_t*)malloc(content_length + 1);
        size_t header_body_len = h_bytes - (body_ptr - header_buf);
        if (header_body_len > 0) {
            size_t copy_len = (header_body_len < content_length) ? header_body_len : content_length;
            memcpy(body, body_ptr, copy_len); total_read = copy_len;
        }
        while (total_read < content_length) {
            ssize_t r = read(client_fd, body + total_read, content_length - total_read);
            if (r <= 0) { break; } 
            total_read += r;
        }
        body[total_read] = '\0';
    }

    process_post(client_fd, path, auth_user, body, total_read, real_ip);
    
    if (body) free(body);
    close(client_fd);
}

void ServerHub::run() {
    while (true) {
        struct sockaddr_in client_addr; socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd >= 0) handle_client(client_fd);
    }
}