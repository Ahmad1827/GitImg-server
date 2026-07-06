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
    struct stat st; memset(&st, 0, sizeof(struct stat));
    if (stat(base_dir, &st) == -1) mkdir(base_dir, 0755);

    const char* dirs[] = {"manifests", "commits", "repos", "thumbnails", "metadata", "users", "sessions"};
    for(int i=0; i<7; i++) {
        char d_path[1024]; snprintf(d_path, sizeof(d_path), "%s/%s", base_dir, dirs[i]);
        if (stat(d_path, &st) == -1) mkdir(d_path, 0755);
    }
    if (!pack_mgr->init()) return false;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) return false;
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(port_num);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return false;
    if (listen(server_fd, 10) < 0) return false;

    printf("GitImg Server listening on port %d...\n", port_num);
    return true;
}

bool ServerHub::check_repo_access(const char* owner, const char* repo, const char* auth_user, bool is_write) {
    char r_path[2048]; snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, repo);
    int fd = open(r_path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st; fstat(fd, &st);
    char* buf = (char*)malloc(st.st_size + 1);
    if(read(fd, buf, st.st_size)){} buf[st.st_size] = '\0';
    close(fd);

    char r_owner[128] = {0};
    int visibility = 0;
    char collabs[512] = {0};

    char* line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "OWNER: ", 7) == 0) strncpy(r_owner, line+7, 127);
        else if (strncmp(line, "VISIBILITY: ", 12) == 0) visibility = atoi(line+12);
        else if (strncmp(line, "COLLABS: ", 9) == 0) strncpy(collabs, line+9, 511);
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

void ServerHub::process_post(int client_fd, const char* path, const char* auth_user, const uint8_t* body, size_t body_len) {
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
            if(write(client_fd, resp, strlen(resp))) {}
        } else {
            const char* resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"invalid credentials\"}";
            if(write(client_fd, resp, strlen(resp))) {}
        }
    }
    else if (strcmp(path, "/auth/logout") == 0) {
        const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{}";
        if(write(client_fd, resp, strlen(resp))) {}
    }
    else if (strncmp(path, "/push/chunk/", 12) == 0) {
        uint64_t hash = strtoull(path + 12, NULL, 16);
        printf("[Server] RCV CHUNK    | Hash: %016lx | Size: %zu bytes\n", hash, body_len);
        pack_mgr->append_chunk(hash, body, body_len);
        const char* resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK";
        if(write(client_fd, resp, strlen(resp))) {}
    } 
    else if (strncmp(path, "/push/manifest/", 15) == 0) {
        printf("[Server] RCV MANIFEST | Hash: %s\n", path + 15);
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
            printf("[Server] AUTH PUSH    | Target: %s/%s | Auth User: %s\n", owner, repo, auth_user[0] ? auth_user : "none");
            if (!check_repo_access(owner, repo, auth_user, true)) {
                printf("  -> Rejected: 403 Forbidden\n");
                const char* resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
                if(write(client_fd, resp, strlen(resp))) {} return;
            }
            
            uint64_t c_hash = CDCHasher::fnv1a_hash((const uint8_t*)body, body_len); 
            printf("  -> Accepted Commit %lx\n", c_hash);
            
            char c_path[2048]; snprintf(c_path, sizeof(c_path), "%s/commits/%lx.commit", base_dir, c_hash);
            int fd = open(c_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { if(write(fd, body, body_len)) {} close(fd); }
            
            char rc_path[2048]; snprintf(rc_path, sizeof(rc_path), "%s/repos/%s_commits.meta", base_dir, repo);
            int rcfd = open(rc_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (rcfd >= 0) {
                char line[512]; snprintf(line, sizeof(line), "%lx\n", c_hash);
                if(write(rcfd, line, strlen(line))) {} close(rcfd);
            }
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
            strncpy(r_meta.id, rname, 63); strncpy(r_meta.name, rname, 127); strncpy(r_meta.owner, auth_user, 127);
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
    char header_buf[4096];
    ssize_t h_bytes = read(client_fd, header_buf, sizeof(header_buf) - 1);
    if (h_bytes <= 0) { close(client_fd); return; }
    header_buf[h_bytes] = '\0';

    char* body_ptr = strstr(header_buf, "\r\n\r\n");
    if (!body_ptr) { close(client_fd); return; }
    *body_ptr = '\0'; body_ptr += 4;

    char method[16], path[512];
    sscanf(header_buf, "%15s %511s", method, path);

    char auth_user[64] = {0};
    char* auth_hdr = strcasestr(header_buf, "Authorization: Bearer ");
    if (auth_hdr) {
        char token[65];
        if (sscanf(auth_hdr + 22, "%64s", token) == 1) {
            UserManager::validate_session(base_dir, token, auth_user);
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/check/chunk/", 13) == 0) {
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
                char header[512]; snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", mime);
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
                strncpy(p1, p, s1 - p);
                const char* p_next = s1 + 1;
                const char* s2 = strchr(p_next, '/');
                if (s2) { strncpy(p2, p_next, s2 - p_next); strcpy(p3, s2 + 1); } 
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
                
                char header[2048];
                snprintf(header, sizeof(header), 
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                    "<!DOCTYPE html><html><head><title>%s/%s - GitImg</title>"
                    "<style>"
                    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#0d1117;color:#c9d1d9;margin:0;padding:30px;}"
                    ".container{max-width:1200px;margin:0 auto;}"
                    ".header{border-bottom:1px solid #30363d;padding-bottom:20px;margin-bottom:20px;}"
                    ".header h1{margin:0 0 10px 0;font-size:24px;font-weight:600;}"
                    ".header h1 a{color:#58a6ff;text-decoration:none;}"
                    ".stats{font-size:14px;color:#8b949e;display:flex;gap:20px;}"
                    ".layout{display:flex;gap:30px;}"
                    ".main{flex:3;}"
                    ".sidebar{flex:1;background:#161b22;padding:20px;border-radius:8px;border:1px solid #30363d;height:fit-content;}"
                    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:16px;}"
                    ".card{background:#010409;border:1px solid #30363d;border-radius:6px;overflow:hidden;}"
                    ".card:hover{border-color:#8b949e;}"
                    ".card img{width:100%%;height:160px;object-fit:cover;display:block;background:#000;}"
                    ".card-info{padding:12px;}"
                    ".card-info strong{display:block;margin-bottom:6px;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
                    ".card-info strong a{color:#c9d1d9;text-decoration:none;}"
                    ".card-info strong a:hover{color:#58a6ff;}"
                    ".card-info span{font-size:12px;color:#8b949e;line-height:1.4;display:block;}"
                    "h2{margin-top:0;font-size:16px;border-bottom:1px solid #30363d;padding-bottom:10px;}"
                    ".commit{padding:10px 0;border-bottom:1px solid #30363d;font-family:monospace;font-size:13px;}"
                    ".commit:last-child{border-bottom:none;padding-bottom:0;}"
                    ".commit strong{color:#58a6ff;}"
                    "</style></head><body><div class=\"container\">"
                    "<div class=\"header\">"
                    "<h1><a href=\"/%s\">%s</a> / <a href=\"/%s/%s\"><b>%s</b></a></h1>"
                    "<div class=\"stats\"><span>Repository Viewer</span></div>"
                    "</div><div class=\"layout\"><div class=\"main\"><h2>Assets Gallery</h2><div class=\"grid\">",
                    p1, p2, p1, p1, p1, p2, p2);
                if(write(client_fd, header, strlen(header))) {}

                char a_path[2048]; snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, p2);
                int afd = open(a_path, O_RDONLY);
                if (afd >= 0) {
                    struct stat ast; fstat(afd, &ast);
                    char* abuf = (char*)malloc(ast.st_size + 1);
                    if(read(afd, abuf, ast.st_size)) {} abuf[ast.st_size] = '\0';
                    close(afd);
                    
                    char* aline = strtok(abuf, "\n");
                    char c_file[256]={0}, c_man[256]={0}, c_size[256]={0}, c_commit[256]={0};
                    while(aline) {
                        if (strncmp(aline, "FILE: ", 6) == 0) strncpy(c_file, aline+6, 255);
                        else if (strncmp(aline, "COMMIT: ", 8) == 0) strncpy(c_commit, aline+8, 255);
                        else if (strncmp(aline, "MANIFEST: ", 10) == 0) strncpy(c_man, aline+10, 255);
                        else if (strncmp(aline, "SIZE: ", 6) == 0) strncpy(c_size, aline+6, 255);
                        else if (strncmp(aline, "HEIGHT: ", 8) == 0) {
                            if (c_file[0] != '\0') {
                                char card[2048];
                                snprintf(card, sizeof(card),
                                    "<div class=\"card\">"
                                    "<a href=\"/raw/%s/%s\"><img src=\"/image/%s\" alt=\"%s\" loading=\"lazy\"></a>"
                                    "<div class=\"card-info\">"
                                    "<strong><a href=\"/raw/%s/%s\">%s</a></strong>"
                                    "<span>Size: %s bytes<br>Commit: %s</span>"
                                    "</div></div>", c_man, c_file, c_man, c_file, c_man, c_file, c_file, c_size, c_commit);
                                if(write(client_fd, card, strlen(card))) {}
                                c_file[0] = '\0';
                            }
                        }
                        aline = strtok(NULL, "\n");
                    }
                    free(abuf);
                }

                const char* mid = "</div></div><div class=\"sidebar\"><h2>Commit Feed</h2><div class=\"commit-feed\">";
                if(write(client_fd, mid, strlen(mid))) {}

                char c_path[2048]; snprintf(c_path, sizeof(c_path), "%s/repos/%s_commits.meta", base_dir, p2);
                int cfd = open(c_path, O_RDONLY);
                if (cfd >= 0) {
                    struct stat cst; fstat(cfd, &cst);
                    char* cbuf = (char*)malloc(cst.st_size + 1);
                    if(read(cfd, cbuf, cst.st_size)) {} cbuf[cst.st_size] = '\0';
                    close(cfd);
                    char* cline = strtok(cbuf, "\n");
                    while(cline) {
                        char cdiv[512]; snprintf(cdiv, sizeof(cdiv), "<div class=\"commit\">commit <strong>%s</strong></div>", cline);
                        if(write(client_fd, cdiv, strlen(cdiv))) {}
                        cline = strtok(NULL, "\n");
                    }
                    free(cbuf);
                } else {
                    const char* no_com = "<div class=\"commit\">No commits yet.</div>";
                    if(write(client_fd, no_com, strlen(no_com))) {}
                }

                const char* footer = "</div></div></div></div></body></html>";
                if(write(client_fd, footer, strlen(footer))) {}
            } else if (strlen(p1) > 0 && strlen(p2) == 0) {
                UserMetadata u;
                if (UserManager::get_user(base_dir, p1, &u)) {
                    char header[1024];
                    snprintf(header, sizeof(header), 
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><title>%s - GitImg</title>"
                        "<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;margin:40px;}"
                        ".card{background:#161b22;padding:20px;border-radius:8px;border:1px solid #30363d;max-width:800px;}</style></head>"
                        "<body><div class=\"card\"><h1>@%s</h1><p>User ID: %s</p><p>Member since: %lu</p>"
                        "<h2>Public Repositories</h2><ul>", u.username, u.username, u.user_id, u.creation_time);
                    if(write(client_fd, header, strlen(header))) {}

                    DIR* dir;
                    struct dirent* ent;
                    char repos_dir[1024];
                    snprintf(repos_dir, sizeof(repos_dir), "%s/repos", base_dir);
                    if ((dir = opendir(repos_dir)) != NULL) {
                        while ((ent = readdir(dir)) != NULL) {
                            if (strstr(ent->d_name, ".repo")) {
                                char rp[2048];
                                snprintf(rp, sizeof(rp), "%s/%s", repos_dir, ent->d_name);
                                int fd = open(rp, O_RDONLY);
                                if (fd >= 0) {
                                    struct stat st; fstat(fd, &st);
                                    char* buf = (char*)malloc(st.st_size+1);
                                    if(read(fd, buf, st.st_size)) {} 
                                    buf[st.st_size]='\0';
                                    close(fd);
                                    
                                    char o_check[128];
                                    snprintf(o_check, sizeof(o_check), "OWNER: %s", u.username);
                                    if (strstr(buf, o_check)) {
                                        char* name_pos = strstr(buf, "NAME: ");
                                        if(name_pos) {
                                            char rname[128];
                                            sscanf(name_pos + 6, "%127[^\n]", rname);
                                            char li[512];
                                            snprintf(li, sizeof(li), "<li><a href=\"/%s/%s\" style=\"color:#58a6ff;\">%s</a></li>", u.username, rname, rname);
                                            if(write(client_fd, li, strlen(li))) {}
                                        }
                                    }
                                    free(buf);
                                }
                            }
                        }
                        closedir(dir);
                    }
                    if(write(client_fd, "</ul></div></body></html>", 25)) {}
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
            if (r <= 0) break; total_read += r;
        }
        body[total_read] = '\0';
    }

    process_post(client_fd, path, auth_user, body, total_read);
    
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