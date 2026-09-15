#include "server_hub.h"
#include "repository.h"
#include "user.h"
#include "cdc_hash.h"
#include "manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <sstream>

static void extract_json_value(const char* json, const char* key, char* out_val) {
    out_val[0] = '\0';
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char* start = strstr(json, search_key);
    if (start) {
        start += strlen(search_key);
        while (*start == ' ' || *start == ':' || *start == '\t') start++;
        if (*start == '"') {
            start++;
            const char* end = strchr(start, '"');
            if (end) {
                size_t len = end - start;
                if (len > 255) len = 255;
                strncpy(out_val, start, len);
                out_val[len] = '\0';
            }
        }
    }
}

ServerHub::ServerHub(const char* config_path) : server_fd(-1), start_time(time(NULL)), req_count(0) {
    port_num = 8080;
    strncpy(bind_host, "0.0.0.0", sizeof(bind_host) - 1);
    bind_host[sizeof(bind_host) - 1] = '\0';
    strncpy(base_dir, ".gitimgd", sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';
    strncpy(public_url, "http://localhost:8080", sizeof(public_url) - 1);
    public_url[sizeof(public_url) - 1] = '\0';
    
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
                else if (strcmp(key, "HOST") == 0 || strcmp(key, "BIND_HOST") == 0) {
                    strncpy(bind_host, val, 127);
                    bind_host[127] = '\0';
                }
                else if (strcmp(key, "STORAGE_DIR") == 0) {
                    strncpy(base_dir, val, 1023);
                    base_dir[1023] = '\0';
                }
                else if (strcmp(key, "PUBLIC_URL") == 0) {
                    strncpy(public_url, val, 511);
                    public_url[511] = '\0';
                }
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
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    if (stat(base_dir, &st) == -1) mkdir(base_dir, 0755);

    const char* dirs[] = {"manifests", "commits", "repos", "thumbnails", "metadata", "users", "sessions", "activities"};
    for (int i = 0; i < 8; i++) {
        char d_path[2048];
        snprintf(d_path, sizeof(d_path), "%s/%s", base_dir, dirs[i]);
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
    memset(&address, 0, sizeof(address));
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

void ServerHub::run() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd >= 0) handle_client(client_fd);
    }
}

void ServerHub::log_activity(const char* user, const char* action, const char* target) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/activities/global.log", base_dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char line[2048];
        snprintf(line, sizeof(line), "%ld|%s|%s|%s\n", time(NULL), user, action, target);
        if (write(fd, line, strlen(line))) {}
        close(fd);
    }
}

void ServerHub::generate_thumbnail(uint64_t m_hash) {
    char m_path[2048];
    snprintf(m_path, sizeof(m_path), "%s/manifests/%lx.manifest", base_dir, m_hash);
    int m_fd = open(m_path, O_RDONLY);
    if (m_fd < 0) return;
    
    char tmp_orig[1024];
    snprintf(tmp_orig, sizeof(tmp_orig), "/tmp/gitimg_%lx.orig", m_hash);
    int t_fd = open(tmp_orig, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (t_fd >= 0) {
        uint64_t c_hash;
        uint8_t buf[32768];
        uint32_t c_size;
        while (read(m_fd, &c_hash, sizeof(uint64_t)) == sizeof(uint64_t)) {
            if (pack_mgr->read_chunk(c_hash, buf, &c_size)) {
                if (write(t_fd, buf, c_size)) {}
            }
        }
        close(t_fd);
    }
    close(m_fd);

    char tmp_thumb[1024];
    snprintf(tmp_thumb, sizeof(tmp_thumb), "/tmp/gitimg_%lx.thumb", m_hash);
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "convert \"%s[0]\" -auto-orient -thumbnail 400x400^ -gravity center -extent 400x400 \"%s\" 2>/dev/null", tmp_orig, tmp_thumb);
    int ret = system(cmd);
    
    char thumb_path[2048];
    snprintf(thumb_path, sizeof(thumb_path), "%s/thumbnails/%lx.thumb", base_dir, m_hash);
    
    if (ret == 0 && access(tmp_thumb, F_OK) == 0) {
        int src = open(tmp_thumb, O_RDONLY);
        int dst = open(thumb_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (src >= 0 && dst >= 0) {
            char buf[32768];
            ssize_t b;
            while ((b = read(src, buf, sizeof(buf))) > 0) {
                if (write(dst, buf, b)) {}
            }
        }
        if (src >= 0) close(src);
        if (dst >= 0) close(dst);
    } else {
        int src = open(tmp_orig, O_RDONLY);
        int dst = open(thumb_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (src >= 0 && dst >= 0) {
            char buf[32768];
            ssize_t b;
            while ((b = read(src, buf, sizeof(buf))) > 0) {
                if (write(dst, buf, b)) {}
            }
        }
        if (src >= 0) close(src);
        if (dst >= 0) close(dst);
    }
    unlink(tmp_orig); 
    unlink(tmp_thumb);
}

bool ServerHub::check_repo_access(const char* owner, const char* repo, const char* auth_user, bool is_write) {
    char r_path[2048];
    snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, repo);
    int fd = open(r_path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st; 
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }
    
    char* buf = (char*)malloc((size_t)st.st_size + 1);
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    if (r >= 0) buf[r] = '\0'; else buf[0] = '\0';
    close(fd);

    char r_owner[128] = {0};
    int visibility = 0;
    char collabs[512] = {0};

    char* line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "OWNER: ", 7) == 0) {
            strncpy(r_owner, line + 7, 127);
            r_owner[127] = '\0';
        }
        else if (strncmp(line, "VISIBILITY: ", 12) == 0) visibility = atoi(line + 12);
        else if (strncmp(line, "COLLABS: ", 9) == 0) {
            strncpy(collabs, line + 9, 511);
            collabs[511] = '\0';
        }
        line = strtok(NULL, "\n");
    }
    free(buf);

    if (!is_write && visibility == 0) return true;
    if (strcmp(r_owner, owner) != 0) return false; 
    if (strcmp(auth_user, r_owner) == 0) return true;
    
    bool is_collab = (strlen(auth_user) > 0 && strstr(collabs, auth_user) != NULL);
    if (is_collab) return true;
    if (is_write) return false;

    return false;
}

void ServerHub::process_post(int client_fd, const char* path, const char* auth_user, const uint8_t* body, size_t body_len, const char* real_ip) {
    (void)real_ip;
    if (strncmp(path, "/api/push/", 10) == 0) {
        if (strlen(auth_user) == 0) {
            std::string resp = "HTTP/1.1 401 Unauthorized\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"error\":\"unauthorized\"}";
            if (write(client_fd, resp.c_str(), resp.length())) {}
            return;
        }

        char repo[128] = {0};
        char filename[256] = {0};
        sscanf(path + 10, "%127[^/]/%255s", repo, filename);
        char* q = strchr(filename, '?');
        char commit_msg[256] = "Pushed from WisdomPark";
        if (q) {
            *q = '\0';
            const char* m_param = strstr(q + 1, "msg=");
            if (m_param) {
                sscanf(m_param + 4, "%255[^&]", commit_msg);
            }
        }

        if (repo[0] == '\0' || filename[0] == '\0' || body_len == 0) {
            std::string resp = "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"error\":\"missing_data\"}";
            if (write(client_fd, resp.c_str(), resp.length())) {}
            return;
        }

        char r_path[2048];
        snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, repo);
        if (access(r_path, F_OK) != 0) {
            RepoMetadata r_meta;
            strncpy(r_meta.id, repo, 63);
            r_meta.id[63] = '\0';
            strncpy(r_meta.name, repo, 127);
            r_meta.name[127] = '\0';
            strncpy(r_meta.owner, auth_user, 127);
            r_meta.owner[127] = '\0';
            r_meta.creation_time = time(NULL);
            r_meta.visibility = 0;
            r_meta.collaborators[0] = '\0';
            int fd_r = open(r_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_r >= 0) {
                char meta_buf[1024];
                size_t mlen = RepositoryManager::serialize_repo(&r_meta, meta_buf, sizeof(meta_buf));
                if (write(fd_r, meta_buf, mlen)) {}
                close(fd_r);
            }
            char target[256];
            snprintf(target, sizeof(target), "%s/%s", auth_user, repo);
            log_activity(auth_user, "created repository", target);
        } else if (!check_repo_access(auth_user, repo, auth_user, true)) {
            std::string resp = "HTTP/1.1 403 Forbidden\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
            if (write(client_fd, resp.c_str(), resp.length())) {}
            return;
        }

        Manifest manifest;
        size_t offset = 0;
        while (offset < body_len) {
            size_t rem = body_len - offset;
            size_t c_size = CDCHasher::find_chunk_boundary(body + offset, rem);
            uint64_t c_hash = CDCHasher::fnv1a_hash(body + offset, c_size);
            pack_mgr->append_chunk(c_hash, body + offset, c_size);
            manifest.add_chunk(c_hash);
            offset += c_size;
        }

        uint64_t m_hash = CDCHasher::fnv1a_hash(body, body_len);
        char m_path[2048];
        snprintf(m_path, sizeof(m_path), "%s/manifests/%lx.manifest", base_dir, m_hash);
        manifest.save_to_file(m_path);

        time_t now = time(NULL);
        char commit_buf[4096];
        int c_pos = snprintf(commit_buf, sizeof(commit_buf), "TIME: %ld\nMSG: %s\n\n%lx %s\n", now, commit_msg, m_hash, filename);
        uint64_t commit_hash = CDCHasher::fnv1a_hash((const uint8_t*)commit_buf, c_pos);
        char c_path[2048];
        snprintf(c_path, sizeof(c_path), "%s/commits/%lx.commit", base_dir, commit_hash);
        int cfd = open(c_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cfd >= 0) {
            if (write(cfd, commit_buf, c_pos)) {}
            close(cfd);
        }

        char rc_path[2048];
        snprintf(rc_path, sizeof(rc_path), "%s/repos/%s_commits.meta", base_dir, repo);
        int rcfd = open(rc_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (rcfd >= 0) {
            char line[512];
            snprintf(line, sizeof(line), "%lx|%ld|%s\n", commit_hash, now, commit_msg);
            if (write(rcfd, line, strlen(line))) {}
            close(rcfd);
        }

        AssetMetadata meta;
        RepositoryManager::generate_asset_id(filename, commit_hash, meta.asset_id);
        strncpy(meta.filename, filename, 255);
        meta.filename[255] = '\0';
        strncpy(meta.mime_type, RepositoryManager::detect_mime_type(filename), 63);
        meta.mime_type[63] = '\0';
        meta.upload_time = now;
        meta.commit_hash = commit_hash;
        meta.manifest_hash = m_hash;
        meta.file_size = body_len;
        meta.width = 0;
        meta.height = 0;
        strncpy(meta.thumbnail_path, "pending_gen", 255);
        meta.thumbnail_path[255] = '\0';

        char a_path[2048];
        snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, repo);
        int afd = open(a_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (afd >= 0) {
            char meta_buf[1024];
            size_t mlen = RepositoryManager::serialize_asset(&meta, meta_buf, sizeof(meta_buf));
            if (write(afd, meta_buf, mlen)) {}
            close(afd);
        }

        generate_thumbnail(m_hash);

        char act_target[256];
        snprintf(act_target, sizeof(act_target), "%s in %s/%s", filename, auth_user, repo);
        log_activity(auth_user, "uploaded artwork", act_target);

        char resp_body[1024];
        snprintf(resp_body, sizeof(resp_body), "{\"status\":\"success\",\"repo\":\"%s/%s\",\"file\":\"%s\",\"manifest\":\"%lx\",\"url\":\"/%s/%s/asset/%lx/%s\"}", auth_user, repo, filename, m_hash, auth_user, repo, m_hash, filename);
        char resp[2048];
        snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n%s", resp_body);
        if (write(client_fd, resp, strlen(resp))) {}
        return;
    }

    if (strcmp(path, "/auth/register") == 0 || strcmp(path, "/register") == 0) {
        char username[64] = {0}, email[128] = {0}, password[128] = {0};
        extract_json_value((const char*)body, "username", username);
        extract_json_value((const char*)body, "email", email);
        extract_json_value((const char*)body, "password", password);
        
        if (username[0] && password[0] && UserManager::create_user(base_dir, username, email, password)) {
            char target[256];
            snprintf(target, sizeof(target), "Joined the platform");
            log_activity(username, "registered", target);
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"success\"}";
            if (write(client_fd, resp.c_str(), resp.length())) {}
        } else {
            std::string resp = "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"failed\"}";
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
    }
    else if (strcmp(path, "/auth/login") == 0 || strcmp(path, "/login") == 0) {
        char username[64] = {0}, password[128] = {0};
        extract_json_value((const char*)body, "username", username);
        extract_json_value((const char*)body, "password", password);
        
        if (UserManager::verify_login(base_dir, username, password)) {
            char token[65];
            UserManager::create_session(base_dir, username, token);
            char resp[512];
            snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"token\":\"%s\"}", token);
            if (write(client_fd, resp, strlen(resp))) {}
        } else {
            UserMetadata u;
            if (!UserManager::get_user(base_dir, username, &u)) {
                UserManager::create_user(base_dir, username, "artist@lan.local", password);
                char token[65];
                UserManager::create_session(base_dir, username, token);
                char target[256];
                snprintf(target, sizeof(target), "Joined the platform");
                log_activity(username, "registered", target);
                char resp[512];
                snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"token\":\"%s\"}", token);
                if (write(client_fd, resp, strlen(resp))) {}
            } else {
                std::string resp = "HTTP/1.1 401 Unauthorized\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"invalid credentials\"}";
                if (write(client_fd, resp.c_str(), resp.length())) {}
            }
        }
    }
    else if (strcmp(path, "/auth/logout") == 0) {
        std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}";
        if (write(client_fd, resp.c_str(), resp.length())) {}
    }
    else if (strncmp(path, "/push/chunk/", 12) == 0) {
        uint64_t hash = strtoull(path + 12, NULL, 16);
        pack_mgr->append_chunk(hash, body, body_len);
        std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK";
        if (write(client_fd, resp.c_str(), resp.length())) {}
    } 
    else if (strncmp(path, "/push/manifest/", 15) == 0) {
        char m_path[2048];
        snprintf(m_path, sizeof(m_path), "%s/manifests/%s.manifest", base_dir, path + 15);
        int fd = open(m_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (write(fd, body, body_len)) {}
            close(fd);
        }
        std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK";
        if (write(client_fd, resp.c_str(), resp.length())) {}
    } 
    else if (strncmp(path, "/repo/", 6) == 0) {
        char owner[128] = {0}, repo[128] = {0}, action[128] = {0};
        int parsed = sscanf(path, "/repo/%127[^/]/%127[^/]/%127s", owner, repo, action);
        
        if (parsed == 3 && strcmp(action, "push") == 0) {
            char r_path[2048];
            snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, repo);
            if (access(r_path, F_OK) != 0) {
                if (strcmp(owner, auth_user) == 0) {
                    RepoMetadata r_meta;
                    strncpy(r_meta.id, repo, 63);
                    r_meta.id[63] = '\0';
                    strncpy(r_meta.name, repo, 127);
                    r_meta.name[127] = '\0';
                    strncpy(r_meta.owner, auth_user, 127);
                    r_meta.owner[127] = '\0';
                    r_meta.creation_time = time(NULL);
                    r_meta.visibility = 0;
                    r_meta.collaborators[0] = '\0';
                    int fd_r = open(r_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd_r >= 0) {
                        char meta_buf[1024];
                        size_t mlen = RepositoryManager::serialize_repo(&r_meta, meta_buf, sizeof(meta_buf));
                        if (write(fd_r, meta_buf, mlen)) {}
                        close(fd_r);
                    }
                    char target[256];
                    snprintf(target, sizeof(target), "%s/%s", owner, repo);
                    log_activity(auth_user, "created repository", target);
                } else {
                    std::string resp = "HTTP/1.1 403 Forbidden\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                    if (write(client_fd, resp.c_str(), resp.length())) {}
                    return;
                }
            } else if (!check_repo_access(owner, repo, auth_user, true)) {
                std::string resp = "HTTP/1.1 403 Forbidden\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                if (write(client_fd, resp.c_str(), resp.length())) {}
                return;
            }
            
            uint64_t c_hash = CDCHasher::fnv1a_hash((const uint8_t*)body, body_len); 
            char c_path[2048];
            snprintf(c_path, sizeof(c_path), "%s/commits/%lx.commit", base_dir, c_hash);
            int fd = open(c_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                if (write(fd, body, body_len)) {}
                close(fd);
            }
            
            time_t now = time(NULL);
            char rc_path[2048];
            snprintf(rc_path, sizeof(rc_path), "%s/repos/%s_commits.meta", base_dir, repo);
            int rcfd = open(rc_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (rcfd >= 0) {
                char line[512];
                snprintf(line, sizeof(line), "%lx|%ld|Sync via CLI\n", c_hash, now);
                if (write(rcfd, line, strlen(line))) {}
                close(rcfd);
            }
            char target[256];
            snprintf(target, sizeof(target), "%s/%s", owner, repo);
            log_activity(auth_user, "pushed a commit to", target);
            
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK";
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
        else if (strcmp(owner, "create") == 0) {
            const char* rname = repo;
            if (strlen(auth_user) == 0) {
                std::string resp = "HTTP/1.1 401 Unauthorized\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                if (write(client_fd, resp.c_str(), resp.length())) {}
                return;
            }
            RepoMetadata r_meta;
            strncpy(r_meta.id, rname, 63);
            r_meta.id[63] = '\0';
            strncpy(r_meta.name, rname, 127);
            r_meta.name[127] = '\0';
            strncpy(r_meta.owner, auth_user, 127);
            r_meta.owner[127] = '\0';
            r_meta.creation_time = time(NULL);
            r_meta.visibility = 0;
            r_meta.collaborators[0] = '\0';

            char r_path[2048];
            snprintf(r_path, sizeof(r_path), "%s/repos/%s.repo", base_dir, rname);
            int fd = open(r_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                char meta_buf[1024];
                size_t mlen = RepositoryManager::serialize_repo(&r_meta, meta_buf, sizeof(meta_buf));
                if (write(fd, meta_buf, mlen)) {}
                close(fd);
            }
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK";
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
        else if (strcmp(owner, "asset") == 0) {
            char sub_owner[128] = {0}, sub_repo[128] = {0};
            sscanf(path, "/repo/asset/%127[^/]/%127s", sub_owner, sub_repo);
            if (!check_repo_access(sub_owner, sub_repo, auth_user, true)) {
                std::string resp = "HTTP/1.1 403 Forbidden\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                if (write(client_fd, resp.c_str(), resp.length())) {}
                return;
            }
            char a_path[2048];
            snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, sub_repo);
            int fd = open(a_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                if (write(fd, body, body_len)) {}
                close(fd);
            }
            
            char body_str[8192];
            size_t c_len = body_len < 8191 ? body_len : 8191;
            strncpy(body_str, (const char*)body, c_len);
            body_str[c_len] = '\0';
            
            char* m_ptr = strstr(body_str, "MANIFEST: ");
            char* f_ptr = strstr(body_str, "FILE: ");
            if (m_ptr && f_ptr) {
                uint64_t m_hash = 0;
                char fname[256] = {0};
                sscanf(m_ptr + 10, "%lx", &m_hash);
                sscanf(f_ptr + 6, "%255[^\n]", fname);
                generate_thumbnail(m_hash);
                
                char target[256];
                snprintf(target, sizeof(target), "%s in %s/%s", fname, sub_owner, sub_repo);
                log_activity(auth_user, "uploaded artwork", target);
            }

            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK";
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
    }
}

std::string ServerHub::build_html_header(const std::string& title) {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
    oss << "<!DOCTYPE html><html><head><title>" << title << " - GitImg</title>";
    oss << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
    oss << "<style>";
    oss << "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#0d1117;color:#c9d1d9;margin:0;padding:0;}";
    oss << "a{color:#58a6ff;text-decoration:none;} a:hover{text-decoration:underline;}";
    oss << ".navbar{background:#161b22;padding:16px 24px;border-bottom:1px solid #30363d;display:flex;align-items:center;justify-content:space-between;}";
    oss << ".navbar .logo{font-weight:800;font-size:22px;color:#fff;}";
    oss << ".navbar .nav-links{display:flex;gap:20px;font-size:14px;font-weight:600;}";
    oss << ".search-bar{background:#0d1117;border:1px solid #30363d;padding:6px 12px;border-radius:6px;color:#c9d1d9;width:250px;}";
    oss << ".container{max-width:1280px;margin:30px auto;padding:0 20px;}";
    oss << ".card{background:#0d1117;border:1px solid #30363d;border-radius:8px;overflow:hidden;transition:border-color 0.2s;box-shadow:0 1px 3px rgba(0,0,0,0.12);}";
    oss << ".card:hover{border-color:#8b949e;}";
    oss << ".btn{background:#238636;color:#ffffff;border:1px solid rgba(240,246,252,0.1);padding:5px 16px;border-radius:6px;font-weight:500;cursor:pointer;}";
    oss << ".btn-outline{background:#21262d;border:1px solid #363b42;color:#c9d1d9;padding:5px 16px;border-radius:6px;}";
    oss << ".stat-row{display:flex;gap:20px;border-bottom:1px solid #30363d;padding-bottom:16px;margin-bottom:24px;flex-wrap:wrap;}";
    oss << ".stat-box{display:flex;flex-direction:column;}";
    oss << ".stat-num{font-size:20px;font-weight:bold;color:#c9d1d9;}";
    oss << ".stat-label{font-size:12px;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px;}";
    oss << ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:20px;}";
    oss << ".asset-card img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#010409;border-bottom:1px solid #30363d;}";
    oss << ".asset-info{padding:16px;}";
    oss << ".asset-info h4{margin:0 0 8px 0;font-size:16px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}";
    oss << ".asset-info p{margin:4px 0;font-size:13px;color:#8b949e;}";
    oss << ".feed-item{padding:16px 0;border-bottom:1px solid #30363d;font-size:14px;}";
    oss << ".feed-item:last-child{border-bottom:none;}";
    oss << ".avatar{width:100%;aspect-ratio:1/1;border-radius:50%;border:1px solid #30363d;background:#21262d;display:flex;align-items:center;justify-content:center;font-size:72px;color:#8b949e;margin-bottom:16px;}";
    oss << ".layout-sidebar{display:flex;flex-wrap:wrap;gap:40px;}";
    oss << ".sidebar{width:280px;flex-shrink:0;}";
    oss << ".main-content{flex:1;min-width:300px;}";
    oss << ".viewer-container{background:#010409;border:1px solid #30363d;border-radius:8px;padding:20px;text-align:center;margin-bottom:30px;}";
    oss << ".viewer-container img{max-width:100%;max-height:70vh;object-fit:contain;border-radius:4px;box-shadow:0 4px 12px rgba(0,0,0,0.5);}";
    oss << "@media (max-width: 768px){ .layout-sidebar{flex-direction:column;} .sidebar{width:100%;} .search-bar{width:150px;} }";
    oss << "</style></head><body>";
    oss << "<div class=\"navbar\">";
    oss << "<a href=\"/\" class=\"logo\">GitImg</a>";
    oss << "<div class=\"nav-links\"><form action=\"/search\" method=\"GET\"><input type=\"text\" name=\"q\" class=\"search-bar\" placeholder=\"Search artists...\"></form></div>";
    oss << "</div><div class=\"container\">";
    return oss.str();
}

std::string ServerHub::build_html_footer() {
    return "</div><script>document.querySelectorAll('.ts').forEach(el=>{const d=new Date(parseInt(el.innerText)*1000);el.innerText=d.toLocaleDateString()+' '+d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});});</script></body></html>";
}

void ServerHub::handle_client(int client_fd) {
    struct timeval tv;
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    req_count++;
    std::vector<uint8_t> header_data;
    header_data.resize(8192);
    ssize_t h_bytes = read(client_fd, header_data.data(), 8191);
    if (h_bytes <= 0) {
        close(client_fd);
        return;
    }
    header_data[h_bytes] = '\0';

    char* delim = strstr((char*)header_data.data(), "\r\n\r\n");
    if (!delim) {
        close(client_fd);
        return;
    }

    size_t header_len = (uint8_t*)delim - header_data.data() + 4;
    std::string header_str((char*)header_data.data(), header_len);

    char method[16] = {0};
    char path[1024] = {0};
    sscanf(header_str.c_str(), "%15s %1023s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        std::string resp = "HTTP/1.1 204 No Content\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type, Authorization, X-Repo-Name, X-Filename, X-Commit-Msg\r\n"
                           "Access-Control-Max-Age: 86400\r\n"
                           "Connection: close\r\n\r\n";
        if (write(client_fd, resp.c_str(), resp.length())) {}
        close(client_fd);
        return;
    }

    char real_ip[64] = {0};
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(client_fd, (struct sockaddr*)&peer, &peer_len) == 0) {
        strncpy(real_ip, inet_ntoa(peer.sin_addr), 63);
        real_ip[63] = '\0';
    }
    const char* xff = strcasestr(header_str.c_str(), "X-Forwarded-For: ");
    if (xff) sscanf(xff + 17, "%63[^ \r\n]", real_ip);

    char auth_user[64] = {0};
    const char* auth_hdr = strcasestr(header_str.c_str(), "Authorization: Bearer ");
    if (auth_hdr) {
        char token[65];
        if (sscanf(auth_hdr + 22, "%64s", token) == 1) {
            UserManager::validate_session(base_dir, token, auth_user);
        }
    }

    if (strcmp(method, "POST") == 0) {
        size_t content_len = 0;
        const char* cl_hdr = strcasestr(header_str.c_str(), "Content-Length: ");
        if (cl_hdr) content_len = (size_t)strtoull(cl_hdr + 16, NULL, 10);

        size_t body_already_read = h_bytes - header_len;
        std::vector<uint8_t> body_buf;
        if (content_len > 0) {
            body_buf.resize(content_len);
            size_t to_copy = (body_already_read < content_len) ? body_already_read : content_len;
            if (to_copy > 0) {
                memcpy(body_buf.data(), header_data.data() + header_len, to_copy);
            }
            size_t total_body_read = to_copy;
            while (total_body_read < content_len) {
                ssize_t b = read(client_fd, body_buf.data() + total_body_read, content_len - total_body_read);
                if (b <= 0) break;
                total_body_read += b;
            }
            process_post(client_fd, path, auth_user, body_buf.data(), total_body_read, real_ip);
        } else {
            process_post(client_fd, path, auth_user, header_data.data() + header_len, body_already_read, real_ip);
        }
        close(client_fd);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/health") == 0) {
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"status\":\"healthy\"}";
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
        else if (strncmp(path, "/api/repos/", 11) == 0) {
            char target_user[128] = {0};
            sscanf(path + 11, "%127s", target_user);
            
            DIR* dir;
            struct dirent* ent;
            char repos_dir[2048];
            snprintf(repos_dir, sizeof(repos_dir), "%s/repos", base_dir);
            std::string json = "[";
            bool first = true;
            
            if ((dir = opendir(repos_dir)) != NULL) {
                while ((ent = readdir(dir)) != NULL) {
                    if (strstr(ent->d_name, ".repo")) {
                        char rp[4096];
                        snprintf(rp, sizeof(rp), "%s/%s", repos_dir, ent->d_name);
                        int fd = open(rp, O_RDONLY);
                        if (fd >= 0) {
                            struct stat st;
                            if (fstat(fd, &st) == 0 && st.st_size > 0) {
                                char* buf = (char*)malloc((size_t)st.st_size + 1);
                                ssize_t rb = read(fd, buf, (size_t)st.st_size);
                                if (rb >= 0) {
                                    buf[rb] = '\0';
                                    char o_check[128];
                                    snprintf(o_check, sizeof(o_check), "OWNER: %s", target_user);
                                    if (strstr(buf, o_check)) {
                                        char* name_pos = strstr(buf, "NAME: ");
                                        if (name_pos) {
                                            char rname[128];
                                            sscanf(name_pos + 6, "%127[^\n]", rname);
                                            if (!first) json += ",";
                                            json += "{\"name\":\"" + std::string(rname) + "\"}";
                                            first = false;
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
            json += "]";
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + json;
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
        else if (strncmp(path, "/api/commits/", 13) == 0) {
            char p1[128] = {0}, p2[128] = {0};
            sscanf(path + 13, "%127[^/]/%127s", p1, p2);
            char c_path[2048];
            snprintf(c_path, sizeof(c_path), "%s/repos/%s_commits.meta", base_dir, p2);
            int cfd = open(c_path, O_RDONLY);
            std::string json = "[]";
            if (cfd >= 0) {
                struct stat cst;
                if (fstat(cfd, &cst) == 0 && cst.st_size > 0) {
                    char* cbuf = (char*)malloc((size_t)cst.st_size + 1);
                    ssize_t rb = read(cfd, cbuf, (size_t)cst.st_size);
                    if (rb >= 0) {
                        cbuf[rb] = '\0';
                        json = "[";
                        char* cline = strtok(cbuf, "\n");
                        bool first = true;
                        std::vector<std::string> lines;
                        while (cline) {
                            lines.push_back(cline);
                            cline = strtok(NULL, "\n");
                        }
                        for (int i = (int)lines.size() - 1; i >= 0; i--) {
                            char chash[128] = {0}, ctime[128] = {0}, cmsg[256] = {0};
                            int p_count = sscanf(lines[i].c_str(), "%127[^|]|%127[^|]|%255[^\n]", chash, ctime, cmsg);
                            if (p_count < 3) {
                                strncpy(cmsg, "Artwork sync", 255);
                                snprintf(ctime, sizeof(ctime), "%ld", time(NULL));
                            }
                            if (!first) json += ",";
                            json += "{\"hash\":\"" + std::string(chash) + "\",\"time\":" + std::string(ctime) + ",\"msg\":\"" + std::string(cmsg) + "\"}";
                            first = false;
                        }
                        json += "]";
                    }
                    free(cbuf);
                }
                close(cfd);
            }
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + json;
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
        else if (strncmp(path, "/api/repo/", 10) == 0) {
            char p1[128] = {0}, p2[128] = {0};
            sscanf(path + 10, "%127[^/]/%127s", p1, p2);
            char a_path[2048];
            snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, p2);
            int afd = open(a_path, O_RDONLY);
            std::string json = "[]";
            if (afd >= 0) {
                struct stat ast;
                if (fstat(afd, &ast) == 0 && ast.st_size > 0) {
                    char* abuf = (char*)malloc((size_t)ast.st_size + 1);
                    ssize_t rb = read(afd, abuf, (size_t)ast.st_size);
                    if (rb >= 0) {
                        abuf[rb] = '\0';
                        json = "[";
                        char* aline = strtok(abuf, "\n");
                        char c_file[256] = {0}, c_man[256] = {0}, c_size[256] = {0}, c_time[256] = {0};
                        bool first = true;
                        while (aline) {
                            if (strncmp(aline, "FILE: ", 6) == 0) strncpy(c_file, aline + 6, 255);
                            else if (strncmp(aline, "MANIFEST: ", 10) == 0) strncpy(c_man, aline + 10, 255);
                            else if (strncmp(aline, "SIZE: ", 6) == 0) strncpy(c_size, aline + 6, 255);
                            else if (strncmp(aline, "TIME: ", 6) == 0) strncpy(c_time, aline + 6, 255);
                            else if (strncmp(aline, "HEIGHT: ", 8) == 0) {
                                if (c_file[0] != '\0') {
                                    if (!first) json += ",";
                                    json += "{\"file\":\"" + std::string(c_file) + "\",\"manifest\":\"" + std::string(c_man) + "\",\"size\":" + std::string(c_size) + ",\"time\":" + std::string(c_time) + ",\"thumb_url\":\"/thumb/" + std::string(c_man) + "\",\"raw_url\":\"/raw/" + std::string(c_man) + "/" + std::string(c_file) + "\"}";
                                    first = false;
                                    c_file[0] = '\0';
                                }
                            }
                            aline = strtok(NULL, "\n");
                        }
                        json += "]";
                    }
                    free(abuf);
                }
                close(afd);
            }
            std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + json;
            if (write(client_fd, resp.c_str(), resp.length())) {}
        }
        else if (strncmp(path, "/check/chunk/", 13) == 0) {
            uint64_t hash = strtoull(path + 13, NULL, 16);
            if (pack_mgr->has_chunk(hash)) {
                std::string resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                if (write(client_fd, resp.c_str(), resp.length())) {}
            } else {
                std::string resp = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                if (write(client_fd, resp.c_str(), resp.length())) {}
            }
        } 
        else if (strncmp(path, "/thumb/", 7) == 0 || strncmp(path, "/raw/", 5) == 0 || strncmp(path, "/image/", 7) == 0) {
            uint64_t m_hash = 0;
            char filename[256] = {0};
            bool is_thumb = (strncmp(path, "/thumb/", 7) == 0);
            
            if (is_thumb) m_hash = strtoull(path + 7, NULL, 16);
            else if (strncmp(path, "/image/", 7) == 0) m_hash = strtoull(path + 7, NULL, 16);
            else sscanf(path, "/raw/%lx/%255s", &m_hash, filename);
            
            if (is_thumb) {
                char thumb_path[2048];
                snprintf(thumb_path, sizeof(thumb_path), "%s/thumbnails/%lx.thumb", base_dir, m_hash);
                int fd = open(thumb_path, O_RDONLY);
                if (fd >= 0) {
                    std::string header = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: image/jpeg\r\nCache-Control: public, max-age=31536000, immutable\r\nConnection: close\r\n\r\n";
                    if (write(client_fd, header.c_str(), header.length())) {}
                    char buf[32768];
                    ssize_t b;
                    while ((b = read(fd, buf, sizeof(buf))) > 0) {
                        if (write(client_fd, buf, b)) {}
                    }
                    close(fd);
                } else {
                    is_thumb = false; 
                }
            }
            
            if (!is_thumb) {
                char m_path[2048];
                snprintf(m_path, sizeof(m_path), "%s/manifests/%lx.manifest", base_dir, m_hash);
                int m_fd = open(m_path, O_RDONLY);
                if (m_fd >= 0) {
                    const char* mime = (filename[0] != '\0') ? RepositoryManager::detect_mime_type(filename) : "image/jpeg";
                    char header[512];
                    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: %s\r\nCache-Control: public, max-age=31536000, immutable\r\nConnection: close\r\n\r\n", mime);
                    if (write(client_fd, header, strlen(header))) {}

                    uint64_t chunk_hash;
                    uint8_t buffer[32768];
                    uint32_t out_size;
                    while (read(m_fd, &chunk_hash, sizeof(uint64_t)) == sizeof(uint64_t)) {
                        if (pack_mgr->read_chunk(chunk_hash, buffer, &out_size)) {
                            if (write(client_fd, buffer, out_size)) {}
                        }
                    }
                    close(m_fd);
                } else {
                    std::string resp = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                    if (write(client_fd, resp.c_str(), resp.length())) {}
                }
            }
        }
        else if (strcmp(path, "/") == 0) {
            std::string html = build_html_header("Home");
            html += "<h2>Recent Activity</h2><div class=\"card\" style=\"padding:0 20px;\">";
            
            char a_path[2048];
            snprintf(a_path, sizeof(a_path), "%s/activities/global.log", base_dir);
            int fd = open(a_path, O_RDONLY);
            if (fd >= 0) {
                struct stat st; 
                if (fstat(fd, &st) == 0 && st.st_size > 0) {
                    char* buf = (char*)malloc((size_t)st.st_size + 1);
                    ssize_t rb = read(fd, buf, (size_t)st.st_size);
                    if (rb >= 0) {
                        buf[rb] = '\0';
                        std::vector<std::string> lines;
                        char* line = strtok(buf, "\n");
                        while (line) {
                            lines.push_back(line);
                            line = strtok(NULL, "\n");
                        }
                        int start_idx = lines.size() > 10 ? lines.size() - 10 : 0;
                        for (int i = lines.size() - 1; i >= start_idx; i--) {
                            char ts[64] = {0}, user[64] = {0}, act[64] = {0}, tgt[256] = {0};
                            sscanf(lines[i].c_str(), "%63[^|]|%63[^|]|%63[^|]|%255[^\n]", ts, user, act, tgt);
                            html += "<div class=\"feed-item\"><strong><a href=\"/" + std::string(user) + "\">" + user + "</a></strong> " + act + " <em>" + tgt + "</em> <span style=\"color:#8b949e;float:right;\" class=\"ts\">" + ts + "</span></div>";
                        }
                    }
                    free(buf);
                }
                close(fd);
            } else {
                html += "<div class=\"feed-item\">Welcome to GitImg! No activity yet.</div>";
            }
            html += "</div>" + build_html_footer();
            if (write(client_fd, html.c_str(), html.length())) {}
        }
        else if (strncmp(path, "/search", 7) == 0) {
            char query[256] = {0};
            const char* q_ptr = strstr(path, "?q=");
            if (q_ptr) sscanf(q_ptr + 3, "%255s", query);
            
            std::string html = build_html_header("Search Results");
            html += "<h2>Search Results for '" + std::string(query) + "'</h2><div class=\"grid\">";
            
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
                                char* buf = (char*)malloc((size_t)st.st_size + 1);
                                ssize_t rb = read(fd, buf, (size_t)st.st_size);
                                if (rb >= 0) {
                                    buf[rb] = '\0';
                                    if (strlen(query) == 0 || strcasestr(buf, query)) {
                                        char rname[128] = {0}, rowner[128] = {0};
                                        char* np = strstr(buf, "NAME: ");
                                        if (np) sscanf(np + 6, "%127[^\n]", rname);
                                        char* op = strstr(buf, "OWNER: ");
                                        if (op) sscanf(op + 7, "%127[^\n]", rowner);
                                        html += "<div class=\"card asset-info\"><h4><a href=\"/" + std::string(rowner) + "/" + std::string(rname) + "\">" + rname + "</a></h4><p>Owned by " + rowner + "</p></div>";
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
            html += "</div>" + build_html_footer();
            if (write(client_fd, html.c_str(), html.length())) {}
        }
        else {
            char p1[128] = {0}, p2[128] = {0}, p3[128] = {0}, p4[128] = {0}, p5[256] = {0};
            const char* p = path;
            if (p[0] == '/') p++;
            const char* s1 = strchr(p, '/');
            if (s1) {
                strncpy(p1, p, s1 - p);
                p1[s1 - p] = '\0';
                const char* p_next = s1 + 1;
                const char* s2 = strchr(p_next, '/');
                if (s2) { 
                    strncpy(p2, p_next, s2 - p_next);
                    p2[s2 - p_next] = '\0';
                    const char* p_next2 = s2 + 1;
                    const char* s3 = strchr(p_next2, '/');
                    if (s3) {
                        strncpy(p3, p_next2, s3 - p_next2);
                        p3[s3 - p_next2] = '\0';
                        const char* p_next3 = s3 + 1;
                        const char* s4 = strchr(p_next3, '/');
                        if (s4) {
                            strncpy(p4, p_next3, s4 - p_next3);
                            p4[s4 - p_next3] = '\0';
                            strcpy(p5, s4 + 1);
                        } else {
                            strcpy(p4, p_next3);
                        }
                    } else {
                        strcpy(p3, p_next2);
                    }
                } else {
                    strcpy(p2, p_next);
                }
            } else {
                strcpy(p1, p);
            }

            if (strlen(p1) > 0 && strlen(p2) > 0 && strcmp(p3, "asset") == 0 && strlen(p4) > 0) {
                std::string html = build_html_header("Asset Viewer");
                html += "<div style=\"margin-bottom:20px;\"><a href=\"/" + std::string(p1) + "/" + std::string(p2) + "\" class=\"btn-outline\">&larr; Back to Repository</a></div>";
                html += "<div class=\"viewer-container\"><img src=\"/raw/" + std::string(p4) + "/" + std::string(p5) + "\" alt=\"Asset\"></div>";
                html += "<div class=\"card asset-info\"><h2>Asset Details</h2><p>Manifest Hash: <span style=\"font-family:monospace;\">" + std::string(p4) + "</span></p></div>";
                html += build_html_footer();
                if (write(client_fd, html.c_str(), html.length())) {}
            }
            else if (strlen(p1) > 0 && strlen(p2) > 0) {
                if (!check_repo_access(p1, p2, auth_user, false)) {
                    std::string resp = "HTTP/1.1 403 Forbidden\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                    if (write(client_fd, resp.c_str(), resp.length())) {}
                    close(client_fd);
                    return;
                }
                
                int commit_count = 0;
                uint64_t total_size = 0;
                int asset_count = 0;
                char c_path[2048];
                snprintf(c_path, sizeof(c_path), "%s/repos/%s_commits.meta", base_dir, p2);
                
                std::string timeline_html = "";
                int cfd = open(c_path, O_RDONLY);
                if (cfd >= 0) {
                    struct stat cst; 
                    if (fstat(cfd, &cst) == 0 && cst.st_size > 0) {
                        char* cbuf = (char*)malloc((size_t)cst.st_size + 1);
                        ssize_t rb = read(cfd, cbuf, (size_t)cst.st_size);
                        if (rb >= 0) {
                            cbuf[rb] = '\0';
                            char* cline = strtok(cbuf, "\n");
                            std::vector<std::string> rev_commits;
                            while (cline) {
                                rev_commits.push_back(cline);
                                commit_count++;
                                cline = strtok(NULL, "\n");
                            }
                            for (int i = rev_commits.size() - 1; i >= 0; i--) {
                                char chash[128] = {0}, ctime[128] = {0}, cmsg[256] = {0};
                                int p_cnt = sscanf(rev_commits[i].c_str(), "%127[^|]|%127[^|]|%255[^\n]", chash, ctime, cmsg);
                                if (p_cnt < 3) strncpy(chash, rev_commits[i].c_str(), 127);
                                timeline_html += "<div class=\"feed-item\">commit <strong style=\"font-family:monospace;\">" + std::string(chash) + "</strong></div>";
                            }
                        }
                        free(cbuf);
                    }
                    close(cfd);
                } else {
                    timeline_html = "<div class=\"feed-item\">No activity yet.</div>";
                }

                std::string html = build_html_header(std::string(p1) + "/" + std::string(p2));
                html += "<div style=\"margin-bottom:24px;\"><h1 style=\"margin:0;\"><a href=\"/" + std::string(p1) + "\">" + std::string(p1) + "</a> / <b>" + std::string(p2) + "</b></h1></div>";
                
                std::string gallery_html = "<div class=\"grid\">";
                char a_path[2048];
                snprintf(a_path, sizeof(a_path), "%s/repos/%s_assets.meta", base_dir, p2);
                int afd = open(a_path, O_RDONLY);
                if (afd >= 0) {
                    struct stat ast; 
                    if (fstat(afd, &ast) == 0 && ast.st_size > 0) {
                        char* abuf = (char*)malloc((size_t)ast.st_size + 1);
                        ssize_t rb = read(afd, abuf, (size_t)ast.st_size);
                        if (rb >= 0) {
                            abuf[rb] = '\0';
                            char* aline = strtok(abuf, "\n");
                            char c_file[256] = {0}, c_man[256] = {0}, c_size[256] = {0}, c_time[256] = {0};
                            while (aline) {
                                if (strncmp(aline, "FILE: ", 6) == 0) strncpy(c_file, aline + 6, 255);
                                else if (strncmp(aline, "MANIFEST: ", 10) == 0) strncpy(c_man, aline + 10, 255);
                                else if (strncmp(aline, "SIZE: ", 6) == 0) {
                                    strncpy(c_size, aline + 6, 255);
                                    total_size += atoll(c_size);
                                }
                                else if (strncmp(aline, "TIME: ", 6) == 0) strncpy(c_time, aline + 6, 255);
                                else if (strncmp(aline, "HEIGHT: ", 8) == 0) {
                                    if (c_file[0] != '\0') {
                                        asset_count++;
                                        gallery_html += "<div class=\"card asset-card\">";
                                        gallery_html += "<a href=\"/" + std::string(p1) + "/" + std::string(p2) + "/asset/" + c_man + "/" + c_file + "\">";
                                        gallery_html += "<img src=\"/thumb/" + std::string(c_man) + "\" alt=\"Thumbnail\" loading=\"lazy\"></a>";
                                        gallery_html += "<div class=\"asset-info\"><h4><a href=\"/" + std::string(p1) + "/" + std::string(p2) + "/asset/" + c_man + "/" + c_file + "\">" + c_file + "</a></h4>";
                                        double mb = atoll(c_size) / 1048576.0;
                                        char size_fmt[32];
                                        snprintf(size_fmt, sizeof(size_fmt), "%.2f MB", mb);
                                        gallery_html += "<p>" + std::string(size_fmt) + " &bull; <span class=\"ts\">" + c_time + "</span></p>";
                                        gallery_html += "</div></div>";
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
                gallery_html += "</div>";

                html += "<div class=\"stat-row\">";
                html += "<div class=\"stat-box\"><span class=\"stat-num\">" + std::to_string(commit_count) + "</span><span class=\"stat-label\">Commits</span></div>";
                html += "<div class=\"stat-box\"><span class=\"stat-num\">" + std::to_string(asset_count) + "</span><span class=\"stat-label\">Assets</span></div>";
                char sz[64];
                snprintf(sz, sizeof(sz), "%.2f MB", total_size / 1048576.0);
                html += "<div class=\"stat-box\"><span class=\"stat-num\">" + std::string(sz) + "</span><span class=\"stat-label\">Raw Storage</span></div>";
                html += "</div>";

                html += "<div class=\"layout-sidebar\"><div class=\"main-content\"><h2>Gallery</h2>" + gallery_html + "</div>";
                html += "<div class=\"sidebar\"><div class=\"card\" style=\"padding:0 16px;\"><h2 style=\"padding-top:16px;\">Timeline</h2>" + timeline_html + "</div></div></div>";
                html += build_html_footer();

                if (write(client_fd, html.c_str(), html.length())) {}
            } else if (strlen(p1) > 0 && strlen(p2) == 0) {
                UserMetadata u;
                if (UserManager::get_user(base_dir, p1, &u)) {
                    std::string html = build_html_header(std::string(p1));
                    html += "<div class=\"layout-sidebar\"><div class=\"sidebar\">";
                    char initial = u.username[0] >= 'a' ? u.username[0] - 32 : u.username[0];
                    html += "<div class=\"avatar\">" + std::string(1, initial) + "</div>";
                    html += "<h1 style=\"margin:0;\">" + std::string(u.username) + "</h1>";
                    html += "<p style=\"color:#8b949e;margin-top:4px;\">GitImg Artist</p>";
                    html += "<button class=\"btn\" style=\"width:100%;margin-top:16px;\">Follow</button>";
                    html += "</div><div class=\"main-content\">";
                    html += "<h2 style=\"border-bottom:1px solid #30363d;padding-bottom:10px;\">Public Portfolios</h2>";
                    html += "<div class=\"grid\">";

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
                                        char* buf = (char*)malloc((size_t)st.st_size + 1);
                                        ssize_t rb = read(fd, buf, (size_t)st.st_size);
                                        if (rb >= 0) {
                                            buf[rb] = '\0';
                                            char o_check[128];
                                            snprintf(o_check, sizeof(o_check), "OWNER: %s", u.username);
                                            if (strstr(buf, o_check)) {
                                                char* name_pos = strstr(buf, "NAME: ");
                                                if (name_pos) {
                                                    char rname[128];
                                                    sscanf(name_pos + 6, "%127[^\n]", rname);
                                                    html += "<div class=\"card\" style=\"padding:20px;\"><h3><a href=\"/" + std::string(u.username) + "/" + std::string(rname) + "\">" + std::string(rname) + "</a></h3><p style=\"color:#8b949e;font-size:14px;margin-bottom:0;\">Public artwork repository</p></div>";
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
                    html += "</div></div></div>" + build_html_footer();
                    if (write(client_fd, html.c_str(), html.length())) {}
                } else {
                    std::string resp = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                    if (write(client_fd, resp.c_str(), resp.length())) {}
                }
            } else {
                std::string resp = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                if (write(client_fd, resp.c_str(), resp.length())) {}
            }
        }
        close(client_fd);
        return;
    }

    std::string resp = "HTTP/1.1 405 Method Not Allowed\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    if (write(client_fd, resp.c_str(), resp.length())) {}
    close(client_fd);
}