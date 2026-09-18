#include "client_repo.h"
#include "cdc_hash.h"
#include "protocol.h"
#include "manifest.h"
#include "repository.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <string>
#include <vector>
#include <sstream>

struct PendingAsset {
    char filename[256];
    uint64_t manifest_hash;
    uint64_t file_size;
};

ClientRepo::ClientRepo(const char* dir, const char* default_host, int default_port) : inotify_fd(-1), watch_fd(-1) {
    strncpy(base_dir, dir, sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';
    snprintf(repo_dir, sizeof(repo_dir), "%s/.gitimg", base_dir);
    
    strncpy(srv_host, default_host, sizeof(srv_host) - 1);
    srv_host[sizeof(srv_host) - 1] = '\0';
    srv_port = default_port;
    
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/remote_config", repo_dir);
    FILE* fp = fopen(config_path, "r");
    if (fp) {
        char h_buf[256];
        int p;
        if (fscanf(fp, "%255s %d", h_buf, &p) == 2) {
            strncpy(srv_host, h_buf, sizeof(srv_host) - 1);
            srv_host[sizeof(srv_host) - 1] = '\0';
            srv_port = p;
        }
        fclose(fp);
    }
    
    strncpy(current_repo, "default", sizeof(current_repo) - 1); current_repo[sizeof(current_repo) - 1] = '\0';
    strncpy(current_owner, "anonymous", sizeof(current_owner) - 1); current_owner[sizeof(current_owner) - 1] = '\0';
    
    snprintf(config_path, sizeof(config_path), "%s/repo_name", repo_dir);
    int fd = open(config_path, O_RDONLY);
    if (fd >= 0) {
        char buf[256] = {0};
        ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
        if (bytes > 0) {
            buf[bytes] = '\0';
            for (int i = 0; i < bytes; i++) if (buf[i] == '\n') buf[i] = '\0';
            char* slash = strchr(buf, '/');
            if (slash) {
                *slash = '\0';
                strncpy(current_owner, buf, 127); current_owner[127] = '\0';
                strncpy(current_repo, slash + 1, 127); current_repo[127] = '\0';
            } else {
                strncpy(current_repo, buf, 127); current_repo[127] = '\0';
            }
        }
        close(fd);
    }
}

ClientRepo::~ClientRepo() {
    if (watch_fd >= 0 && inotify_fd >= 0) inotify_rm_watch(inotify_fd, watch_fd);
    if (inotify_fd >= 0) close(inotify_fd);
}

bool ClientRepo::init(const char* repo_target, const char* host, int port) {
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    if (stat(repo_dir, &st) == -1) {
        if (mkdir(repo_dir, 0755) != 0) return false;
    }
    
    char manifest_dir[1024], object_dir[1024], commit_dir[1024];
    snprintf(manifest_dir, sizeof(manifest_dir), "%s/manifests", repo_dir);
    snprintf(object_dir, sizeof(object_dir), "%s/objects", repo_dir);
    snprintf(commit_dir, sizeof(commit_dir), "%s/commits", repo_dir);
    
    if (stat(manifest_dir, &st) == -1) mkdir(manifest_dir, 0755);
    if (stat(object_dir, &st) == -1) mkdir(object_dir, 0755);
    if (stat(commit_dir, &st) == -1) mkdir(commit_dir, 0755);
    
    strncpy(srv_host, host, sizeof(srv_host) - 1); srv_host[sizeof(srv_host) - 1] = '\0';
    srv_port = port;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/remote_config", repo_dir);
    FILE* fp = fopen(config_path, "w");
    if (fp) {
        fprintf(fp, "%s %d\n", srv_host, srv_port);
        fclose(fp);
    }
    
    const char* slash = strchr(repo_target, '/');
    if (slash) {
        size_t owner_len = slash - repo_target;
        if (owner_len > 127) owner_len = 127;
        strncpy(current_owner, repo_target, owner_len);
        current_owner[owner_len] = '\0';
        strncpy(current_repo, slash + 1, sizeof(current_repo) - 1);
        current_repo[sizeof(current_repo) - 1] = '\0';
    } else {
        strncpy(current_owner, "anonymous", sizeof(current_owner) - 1);
        current_owner[sizeof(current_owner) - 1] = '\0';
        strncpy(current_repo, repo_target, sizeof(current_repo) - 1);
        current_repo[sizeof(current_repo) - 1] = '\0';
    }

    snprintf(config_path, sizeof(config_path), "%s/repo_name", repo_dir);
    int fd = open(config_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char target_buf[256];
        snprintf(target_buf, sizeof(target_buf), "%s/%s", current_owner, current_repo);
        if (write(fd, target_buf, strlen(target_buf))) {}
        close(fd);
    }
    return true;
}

bool ClientRepo::is_tracked_file(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;
    if (strcasecmp(dot, ".wpk") == 0) return true;
    if (strcasecmp(dot, ".png") == 0) return true;
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return true;
    if (strcasecmp(dot, ".gif") == 0) return true;
    if (strcasecmp(dot, ".webp") == 0) return true;
    if (strcasecmp(dot, ".apng") == 0) return true;
    return false;
}

uint64_t ClientRepo::chunk_and_push(const char* filepath, uint64_t* out_size, uint32_t* skipped_chunks, uint32_t* new_chunks) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        *out_size = 0;
        return 0;
    }

    *out_size = st.st_size;
    uint8_t* file_data = (uint8_t*)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_data == MAP_FAILED) {
        close(fd);
        return 0;
    }

    uint64_t full_hash = CDCHasher::fnv1a_hash(file_data, st.st_size);
    Manifest manifest;

    size_t offset = 0;
    while (offset < (size_t)st.st_size) {
        size_t remaining = st.st_size - offset;
        size_t chunk_size = CDCHasher::find_chunk_boundary(file_data + offset, remaining);
        
        uint64_t chunk_hash = CDCHasher::fnv1a_hash(file_data + offset, chunk_size);
        
        if (!Protocol::check_chunk(srv_host, srv_port, chunk_hash)) {
            Protocol::push_chunk(srv_host, srv_port, chunk_hash, file_data + offset, chunk_size);
            (*new_chunks)++;
        } else {
            (*skipped_chunks)++;
        }

        char obj_path[1024];
        snprintf(obj_path, sizeof(obj_path), "%s/objects/%lx", repo_dir, chunk_hash);
        int obj_fd = open(obj_path, O_WRONLY | O_CREAT, 0644);
        if (obj_fd >= 0) {
            if (write(obj_fd, file_data + offset, chunk_size)) {}
            close(obj_fd);
        }

        manifest.add_chunk(chunk_hash);
        offset += chunk_size;
    }

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifests/%lx.manifest", repo_dir, full_hash);
    manifest.save_to_file(manifest_path);

    size_t m_size;
    uint8_t* m_data = manifest.serialize(&m_size);
    if (m_data) {
        Protocol::push_manifest(srv_host, srv_port, full_hash, m_data, m_size);
        free(m_data);
    }

    munmap(file_data, st.st_size);
    close(fd);
    return full_hash;
}

bool ClientRepo::commit(const char* message) {
    DIR* dir = opendir(base_dir);
    if (!dir) return false;

    PendingAsset assets[512];
    size_t asset_count = 0;
    uint32_t total_skipped = 0;
    uint32_t total_new = 0;

    char commit_buffer[4096];
    time_t now = time(NULL);
    int pos = snprintf(commit_buffer, sizeof(commit_buffer), "TIME: %ld\nMSG: %s\n\n", now, message);

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (is_tracked_file(ent->d_name)) {
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, ent->d_name);
            
            uint64_t f_size = 0;
            uint64_t file_hash = chunk_and_push(full_path, &f_size, &total_skipped, &total_new);
            
            if (asset_count < 512) {
                strncpy(assets[asset_count].filename, ent->d_name, 255);
                assets[asset_count].filename[255] = '\0';
                assets[asset_count].manifest_hash = file_hash;
                assets[asset_count].file_size = f_size;
                asset_count++;
            }
            pos += snprintf(commit_buffer + pos, sizeof(commit_buffer) - pos, "%lx %s\n", file_hash, ent->d_name);
        }
    }
    closedir(dir);

    if (asset_count == 0) return true;

    printf("Uploading %zu assets...\n", asset_count);

    uint64_t commit_hash = CDCHasher::fnv1a_hash((uint8_t*)commit_buffer, pos);
    
    char local_commit_path[1024];
    snprintf(local_commit_path, sizeof(local_commit_path), "%s/commits/%lx.commit", repo_dir, commit_hash);
    int c_fd = open(local_commit_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (c_fd >= 0) {
        if (write(c_fd, commit_buffer, pos)) {}
        close(c_fd);
    }

    char head_path[1024];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", repo_dir);
    FILE* hf = fopen(head_path, "w");
    if (hf) {
        fprintf(hf, "%lx\n", commit_hash);
        fclose(hf);
    }

    if (!Protocol::push_commit(srv_host, srv_port, current_owner, current_repo, commit_hash, (uint8_t*)commit_buffer, pos)) {
        return false;
    }

    for (size_t i = 0; i < asset_count; i++) {
        AssetMetadata meta;
        RepositoryManager::generate_asset_id(assets[i].filename, commit_hash, meta.asset_id);
        strncpy(meta.filename, assets[i].filename, 255); meta.filename[255] = '\0';
        strncpy(meta.mime_type, RepositoryManager::detect_mime_type(assets[i].filename), 63); meta.mime_type[63] = '\0';
        meta.upload_time = now;
        meta.commit_hash = commit_hash;
        meta.manifest_hash = assets[i].manifest_hash;
        meta.file_size = assets[i].file_size;
        meta.width = 0;
        meta.height = 0;
        strncpy(meta.thumbnail_path, "pending_gen", 255); meta.thumbnail_path[255] = '\0';
        
        char meta_buf[1024];
        size_t m_len = RepositoryManager::serialize_asset(&meta, meta_buf, sizeof(meta_buf));
        if (m_len > 0) {
            Protocol::push_asset_meta(srv_host, srv_port, current_owner, current_repo, (const uint8_t*)meta_buf, m_len);
        }
    }

    printf("Skipped %u duplicate chunks.\n", total_skipped);
    printf("Uploaded %u new chunks.\n", total_new);
    printf("Generating thumbnails...\n");

    return true;
}

std::vector<CommitEntry> ClientRepo::get_commit_list() {
    std::vector<CommitEntry> commits;
    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint), "/api/commits/%s/%s", current_owner, current_repo);

    std::vector<char> buffer(65536, 0);
    if (!Protocol::fetch_http_get(srv_host, srv_port, endpoint, buffer.data(), buffer.size())) {
        return commits;
    }

    std::string json(buffer.data());
    size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string::npos) {
        size_t end = json.find('}', pos);
        if (end == std::string::npos) break;

        std::string obj = json.substr(pos, end - pos + 1);
        CommitEntry entry;
        entry.timestamp = 0;

        size_t hash_pos = obj.find("\"hash\":");
        if (hash_pos != std::string::npos) {
            size_t q1 = obj.find('"', hash_pos + 7);
            if (q1 != std::string::npos) {
                size_t q2 = obj.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    entry.hash = obj.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        size_t msg_pos = obj.find("\"msg\":");
        if (msg_pos != std::string::npos) {
            size_t q1 = obj.find('"', msg_pos + 6);
            if (q1 != std::string::npos) {
                size_t q2 = obj.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    entry.message = obj.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        size_t time_pos = obj.find("\"time\":");
        if (time_pos != std::string::npos) {
            size_t val_start = time_pos + 7;
            while (val_start < obj.size() && (obj[val_start] == ' ' || obj[val_start] == ':')) val_start++;
            entry.timestamp = strtoull(obj.c_str() + val_start, nullptr, 10);
        }

        if (!entry.hash.empty()) {
            commits.push_back(entry);
        }
        pos = end + 1;
    }

    return commits;
}

bool ClientRepo::log(bool json_format) {
    auto commits = get_commit_list();
    if (commits.empty()) {
        if (json_format) {
            printf("[]\n");
        } else {
            printf("No commits found for %s/%s.\n", current_owner, current_repo);
        }
        return true;
    }

    if (json_format) {
        printf("[\n");
        for (size_t i = 0; i < commits.size(); i++) {
            printf("  {\n");
            printf("    \"commit_hash\": \"%s\",\n", commits[i].hash.c_str());
            printf("    \"timestamp\": %lu,\n", commits[i].timestamp);
            printf("    \"commit_message\": \"%s\"\n", commits[i].message.c_str());
            printf("  }%s\n", (i + 1 < commits.size()) ? "," : "");
        }
        printf("]\n");
    } else {
        for (const auto& c : commits) {
            printf("%s|%lu|%s\n", c.hash.c_str(), c.timestamp, c.message.c_str());
        }
    }

    return true;
}

bool ClientRepo::checkout(const char* commit_hash_str) {
    std::string target_hash = commit_hash_str;
    auto commits = get_commit_list();
    for (const auto& c : commits) {
        if (c.hash.rfind(commit_hash_str, 0) == 0) {
            target_hash = c.hash;
            break;
        }
    }

    char commit_path[1024];
    snprintf(commit_path, sizeof(commit_path), "%s/commits/%s.commit", repo_dir, target_hash.c_str());

    if (access(commit_path, F_OK) != 0) {
        if (!Protocol::fetch_commit(srv_host, srv_port, target_hash.c_str(), commit_path)) {
            return false;
        }
    }

    FILE* fp = fopen(commit_path, "r");
    if (!fp) return false;

    char rollback_dir[1024];
    snprintf(rollback_dir, sizeof(rollback_dir), "%s/rollback", repo_dir);
    mkdir(rollback_dir, 0755);

    DIR* d = opendir(base_dir);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            if (is_tracked_file(ent->d_name)) {
                char src[1024];
                snprintf(src, sizeof(src), "%s/%s", base_dir, ent->d_name);
                char dst[1024];
                snprintf(dst, sizeof(dst), "%s/%s", rollback_dir, ent->d_name);

                FILE* fsrc = fopen(src, "rb");
                if (fsrc) {
                    FILE* fdst = fopen(dst, "wb");
                    if (fdst) {
                        char buf[8192];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
                            fwrite(buf, 1, n, fdst);
                        }
                        fclose(fdst);
                    }
                    fclose(fsrc);
                }
                unlink(src);
            }
        }
        closedir(d);
    }

    char line[1024];
    bool reading_files = false;
    int restored_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '\n' || line[0] == '\r') {
            reading_files = true;
            continue;
        }
        if (reading_files) {
            uint64_t m_hash;
            char fname[512];
            if (sscanf(line, "%lx %511s", &m_hash, fname) == 2) {
                char manifest_path[1024];
                snprintf(manifest_path, sizeof(manifest_path), "%s/manifests/%lx.manifest", repo_dir, m_hash);
                
                if (access(manifest_path, F_OK) != 0) {
                    if (!Protocol::fetch_manifest(srv_host, srv_port, m_hash, manifest_path)) {
                        continue;
                    }
                }

                int m_fd = open(manifest_path, O_RDONLY);
                if (m_fd < 0) continue;

                char target_path[1024];
                snprintf(target_path, sizeof(target_path), "%s/%s", base_dir, fname);
                int target_fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (target_fd < 0) {
                    close(m_fd);
                    continue;
                }

                uint64_t chunk_hash;
                while (read(m_fd, &chunk_hash, sizeof(uint64_t)) == sizeof(uint64_t)) {
                    char obj_path[1024];
                    snprintf(obj_path, sizeof(obj_path), "%s/objects/%lx", repo_dir, chunk_hash);

                    struct stat st;
                    if (stat(obj_path, &st) == -1) {
                        if (!Protocol::fetch_chunk(srv_host, srv_port, chunk_hash, obj_path)) {
                            continue;
                        }
                    }

                    int obj_fd = open(obj_path, O_RDONLY);
                    if (obj_fd >= 0) {
                        char buf[16384];
                        ssize_t bytes_read;
                        while ((bytes_read = read(obj_fd, buf, sizeof(buf))) > 0) {
                            if (write(target_fd, buf, bytes_read)) {}
                        }
                        close(obj_fd);
                    }
                }
                close(target_fd);
                close(m_fd);
                restored_count++;
            }
        }
    }
    fclose(fp);

    char head_path[1024];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", repo_dir);
    FILE* hf = fopen(head_path, "w");
    if (hf) {
        fprintf(hf, "%s\n", target_hash.c_str());
        fclose(hf);
    }

    printf("HEAD is now at %s (%d files restored)\n", target_hash.substr(0, 7).c_str(), restored_count);
    return true;
}

void ClientRepo::handle_events(int fd) {
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    ssize_t len;

    for (;;) {
        len = read(fd, buffer, sizeof(buffer));
        if (len == -1 && errno != EAGAIN) exit(EXIT_FAILURE);
        if (len <= 0) break;

        for (char* ptr = buffer; ptr < buffer + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *) ptr;
            if (event->len && (event->mask & IN_CLOSE_WRITE)) {
                if (is_tracked_file(event->name)) {
                    commit("Auto-sync via watcher");
                }
            }
        }
    }
}

void ClientRepo::watch() {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1) return;

    watch_fd = inotify_add_watch(inotify_fd, base_dir, IN_CLOSE_WRITE);
    if (watch_fd == -1) return;

    while (true) {
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(inotify_fd, &descriptors);

        int ret = select(inotify_fd + 1, &descriptors, NULL, NULL, NULL);
        if (ret > 0 && FD_ISSET(inotify_fd, &descriptors)) {
            handle_events(inotify_fd);
        }
    }
}