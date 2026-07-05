#ifndef PACKFILE_H
#define PACKFILE_H

#include <stdint.h>
#include <stddef.h>

struct IndexEntry {
    uint64_t hash;
    uint64_t offset;
    uint32_t size;
};

class PackfileManager {
public:
    PackfileManager(const char* storage_dir);
    ~PackfileManager();
    
    bool init();
    bool append_chunk(uint64_t hash, const uint8_t* data, uint32_t size);
    bool read_chunk(uint64_t hash, uint8_t* buffer, uint32_t* out_size);
    bool has_chunk(uint64_t hash);

private:
    char pack_path[1024];
    char idx_path[1024];
    int pack_fd;
    int idx_fd;
    
    IndexEntry* index_map;
    size_t index_count;
    
    void load_index();
    void sync_index();
};

#endif