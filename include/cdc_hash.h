#ifndef CDC_HASH_H
#define CDC_HASH_H

#include <stdint.h>
#include <stddef.h>

class CDCHasher {
public:
    static uint64_t fnv1a_hash(const uint8_t* data, size_t length);
    static size_t find_chunk_boundary(const uint8_t* data, size_t length);
};

#endif