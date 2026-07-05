#include "client_repo.h"
#include "cdc_hash.h"
#include "protocol.h"
#include "manifest.h"
#include "repository.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

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
            srv_port = p;
        }
        fclose(fp);
    }
    
    strncpy(current_repo, "default", sizeof(current_repo));
    snprintf(config_path, sizeof(config_path), "%s/repo_name", repo_dir);
    int fd = open(config_path, O_RDONLY);
    if (fd >= 0) {
        ssize_t bytes = read(fd, current_repo, sizeof(current_repo) - 1);
        if (bytes > 0) {
            current_repo[bytes] = '\0';
            for (int i=0; i<bytes; i++) if (current_repo[i] == '\n') current_repo[i] = '\0';
        }
        close(fd);
    }
}

ClientRepo::~ClientRepo() {
    if (watch_fd >= 0 && inotify_fd >= 0) inotify_rm_watch(inotify_fd, watch_fd);
    if (inotify_fd >= 0) close(inotify_fd);
}

bool ClientRepo::init(const char* repo_name, const char* host, int port) {
    struct stat st = {0};
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
    
    strncpy(srv_host, host, sizeof(srv_host) - 1);
    srv_port = port;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/remote_config", repo_dir);
    FILE* fp = fopen(config_path, "w");
    if (fp) {
        fprintf(fp, "%s %d\n", srv_host, srv_port);
        fclose(fp);
    }
    
    strncpy(current_repo, repo_name, sizeof(current_repo) - 1);
    snprintf(config_path, sizeof(config_path), "%s/repo_name", repo_dir);
    int fd = open(config_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, current_repo, strlen(current_repo));
        close(fd);
    }
    
    printf("Initialized local workspace in %s\nTargeting remote %s:%d (Repo: %s)\n", repo_dir, srv_host, srv_port, current_repo);
    return true;
}

bool ClientRepo::is_tracked_file(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;
    if (strcmp(dot, ".wpk") == 0) return true;
    if (strcmp(dot, ".png") == 0) return true;
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return true;
    if (strcmp(dot, ".gif") == 0) return true;
    if (strcmp(dot, ".webp") == 0) return true;
    return false;
}

uint64_t ClientRepo::chunk_and_push(const char* filepath, uint64_t* out_size) {
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
        }

        char obj_path[2048];
        snprintf(obj_path, sizeof(obj_path), "%s/objects/%lx", repo_dir, chunk_hash);
        int obj_fd = open(obj_path, O_WRONLY | O_CREAT, 0644);
        if (obj_fd >= 0) {
            write(obj_fd, file_data + offset, chunk_size);
            close(obj_fd);
        }

        manifest.add_chunk(chunk_hash);
        offset += chunk_size;
    }

    char manifest_path[2048];
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

    char commit_buffer[4096];
    time_t now = time(NULL);
    int pos = snprintf(commit_buffer, sizeof(commit_buffer), "TIME: %ld\nMSG: %s\n\n", now, message);

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (is_tracked_file(ent->d_name)) {
            char full_path[2048];
            snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, ent->d_name);
            
            uint64_t f_size = 0;
            uint64_t file_hash = chunk_and_push(full_path, &f_size);
            
            if (asset_count < 512) {
                strncpy(assets[asset_count].filename, ent->d_name, 255);
                assets[asset_count].manifest_hash = file_hash;
                assets[asset_count].file_size = f_size;
                asset_count++;
            }
            pos += snprintf(commit_buffer + pos, sizeof(commit_buffer) - pos, "%lx %s\n", file_hash, ent->d_name);
        }
    }
    closedir(dir);

    uint64_t commit_hash = CDCHasher::fnv1a_hash((uint8_t*)commit_buffer, pos);
    Protocol::push_commit(srv_host, srv_port, commit_hash, (uint8_t*)commit_buffer, pos);

    for (size_t i = 0; i < asset_count; i++) {
        AssetMetadata meta;
        strncpy(meta.filename, assets[i].filename, 255);
        strncpy(meta.mime_type, RepositoryManager::detect_mime_type(assets[i].filename), 63);
        meta.upload_time = now;
        meta.commit_hash = commit_hash;
        meta.manifest_hash = assets[i].manifest_hash;
        meta.file_size = assets[i].file_size;
        strncpy(meta.thumbnail_path, "pending_gen", 255);
        
        char meta_buf[1024];
        size_t m_len = RepositoryManager::serialize_asset(&meta, meta_buf, sizeof(meta_buf));
        if (m_len > 0) {
            Protocol::push_asset_meta(srv_host, srv_port, current_repo, (const uint8_t*)meta_buf, m_len);
        }
    }

    printf("[master %lx] %s (Tracking %zu assets)\n", commit_hash, message, asset_count);
    return true;
}

bool ClientRepo::checkout(const char* commit_hash_str) {
    char commit_path[2048];
    snprintf(commit_path, sizeof(commit_path), "%s/commits/%s.commit", repo_dir, commit_hash_str);

    printf("Fetching commit metadata from %s...\n", srv_host);
    if (!Protocol::fetch_commit(srv_host, srv_port, commit_hash_str, commit_path)) {
        printf("Failed to fetch commit %s from server.\n", commit_hash_str);
        return false;
    }

    FILE* fp = fopen(commit_path, "r");
    if (!fp) return false;

    char line[1024];
    bool reading_files = false;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '\n') {
            reading_files = true;
            continue;
        }
        if (reading_files) {
            uint64_t m_hash;
            char fname[512];
            if (sscanf(line, "%lx %511s", &m_hash, fname) == 2) {
                printf("Reconstructing %s...\n", fname);

                char manifest_path[2048];
                snprintf(manifest_path, sizeof(manifest_path), "%s/manifests/%lx.manifest", repo_dir, m_hash);
                
                if (!Protocol::fetch_manifest(srv_host, srv_port, m_hash, manifest_path)) {
                    printf("  -> Error: Could not fetch manifest for %s\n", fname);
                    continue;
                }

                int m_fd = open(manifest_path, O_RDONLY);
                if (m_fd < 0) continue;

                char target_path[2048];
                snprintf(target_path, sizeof(target_path), "%s/%s", base_dir, fname);
                int target_fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (target_fd < 0) {
                    close(m_fd);
                    continue;
                }

                uint64_t chunk_hash;
                while (read(m_fd, &chunk_hash, sizeof(uint64_t)) == sizeof(uint64_t)) {
                    char obj_path[2048];
                    snprintf(obj_path, sizeof(obj_path), "%s/objects/%lx", repo_dir, chunk_hash);

                    struct stat st;
                    if (stat(obj_path, &st) == -1) {
                        if (!Protocol::fetch_chunk(srv_host, srv_port, chunk_hash, obj_path)) {
                            printf("  -> Error: Could not fetch chunk %lx\n", chunk_hash);
                            continue;
                        }
                    }

                    int obj_fd = open(obj_path, O_RDONLY);
                    if (obj_fd >= 0) {
                        char buf[16384];
                        ssize_t bytes_read;
                        while ((bytes_read = read(obj_fd, buf, sizeof(buf))) > 0) {
                            write(target_fd, buf, bytes_read);
                        }
                        close(obj_fd);
                    }
                }
                close(target_fd);
                close(m_fd);
                printf("  -> Successfully restored %s\n", fname);
            }
        }
    }
    fclose(fp);
    printf("Checkout complete.\n");
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
                    printf("Auto-sync triggered by %s\n", event->name);
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

    printf("GitImg watching directory: %s\n", base_dir);

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