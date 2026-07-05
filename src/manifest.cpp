#include "manifest.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

Manifest::Manifest() : count(0), capacity(16) {
    chunks = (uint64_t*)malloc(capacity * sizeof(uint64_t));
}

Manifest::~Manifest() {
    if (chunks) free(chunks);
}

void Manifest::add_chunk(uint64_t hash) {
    if (count >= capacity) {
        capacity *= 2;
        uint64_t* new_chunks = (uint64_t*)realloc(chunks, capacity * sizeof(uint64_t));
        if (new_chunks) {
            chunks = new_chunks;
        } else {
            return;
        }
    }
    chunks[count++] = hash;
}

uint8_t* Manifest::serialize(size_t* out_size) {
    *out_size = count * sizeof(uint64_t);
    uint8_t* data = (uint8_t*)malloc(*out_size);
    if (data && *out_size > 0) {
        memcpy(data, chunks, *out_size);
    }
    return data;
}

bool Manifest::save_to_file(const char* filepath) {
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    
    ssize_t written = 0;
    if (count > 0) {
        written = write(fd, chunks, count * sizeof(uint64_t));
    }
    
    close(fd);
    return written == (ssize_t)(count * sizeof(uint64_t));
}