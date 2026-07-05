#include "packfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

PackfileManager::PackfileManager(const char* storage_dir) : pack_fd(-1), idx_fd(-1), index_map(nullptr), index_count(0) {
    snprintf(pack_path, sizeof(pack_path), "%s/volume_1.pack", storage_dir);
    snprintf(idx_path, sizeof(idx_path), "%s/volume_1.idx", storage_dir);
}

PackfileManager::~PackfileManager() {
    if (index_map && index_map != MAP_FAILED) {
        munmap(index_map, index_count * sizeof(IndexEntry));
    }
    if (pack_fd >= 0) close(pack_fd);
    if (idx_fd >= 0) close(idx_fd);
}

bool PackfileManager::init() {
    pack_fd = open(pack_path, O_RDWR | O_CREAT | O_APPEND, 0644);
    idx_fd = open(idx_path, O_RDWR | O_CREAT, 0644);
    
    if (pack_fd < 0 || idx_fd < 0) return false;
    
    load_index();
    return true;
}

void PackfileManager::load_index() {
    struct stat st;
    if (fstat(idx_fd, &st) == 0 && st.st_size > 0) {
        index_count = st.st_size / sizeof(IndexEntry);
        index_map = (IndexEntry*)mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, idx_fd, 0);
    }
}

bool PackfileManager::has_chunk(uint64_t hash) {
    if (!index_map || index_map == MAP_FAILED) return false;
    for (size_t i = 0; i < index_count; i++) {
        if (index_map[i].hash == hash) return true;
    }
    return false;
}

bool PackfileManager::append_chunk(uint64_t hash, const uint8_t* data, uint32_t size) {
    if (pack_fd < 0 || idx_fd < 0) return false;

    if (has_chunk(hash)) return true;

    off_t current_offset = lseek(pack_fd, 0, SEEK_END);
    if (current_offset == (off_t)-1) return false;

    if (write(pack_fd, data, size) != size) return false;

    IndexEntry new_entry;
    new_entry.hash = hash;
    new_entry.offset = (uint64_t)current_offset;
    new_entry.size = size;

    lseek(idx_fd, 0, SEEK_END);
    write(idx_fd, &new_entry, sizeof(IndexEntry));

    if (index_map && index_map != MAP_FAILED) {
        munmap(index_map, index_count * sizeof(IndexEntry));
    }
    load_index();

    return true;
}

bool PackfileManager::read_chunk(uint64_t hash, uint8_t* buffer, uint32_t* out_size) {
    if (!index_map || index_map == MAP_FAILED) return false;

    for (size_t i = 0; i < index_count; i++) {
        if (index_map[i].hash == hash) {
            if (pread(pack_fd, buffer, index_map[i].size, index_map[i].offset) == index_map[i].size) {
                *out_size = index_map[i].size;
                return true;
            }
            break;
        }
    }
    return false;
}