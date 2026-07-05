#include "cdc_hash.h"

uint64_t CDCHasher::fnv1a_hash(const uint8_t* data, size_t length) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

size_t CDCHasher::find_chunk_boundary(const uint8_t* data, size_t length) {
    const size_t MIN_CHUNK = 2048;
    const size_t MAX_CHUNK = 16384;
    const uint32_t MASK = 0x0FFF;
    
    if (length <= MIN_CHUNK) return length;

    uint32_t rolling_hash = 0;
    
    for (size_t i = MIN_CHUNK; i < length; i++) {
        rolling_hash = (rolling_hash << 1) ^ data[i];
        
        if ((rolling_hash & MASK) == 0) {
            return i + 1;
        }
        
        if (i >= MAX_CHUNK) {
            return MAX_CHUNK;
        }
    }
    
    return length;
}