#ifndef MANIFEST_H
#define MANIFEST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

class Manifest {
public:
    Manifest();
    ~Manifest();
    
    void add_chunk(uint64_t hash);
    uint8_t* serialize(size_t* out_size);
    bool save_to_file(const char* filepath);

private:
    uint64_t* chunks;
    size_t count;
    size_t capacity;
};

#endif