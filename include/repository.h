#ifndef REPOSITORY_H
#define REPOSITORY_H

#include <stdint.h>
#include <stddef.h>

struct AssetMetadata {
    char filename[256];
    char mime_type[64];
    uint64_t upload_time;
    uint64_t commit_hash;
    uint64_t manifest_hash;
    char thumbnail_path[256];
    uint64_t file_size;
};

struct RepoMetadata {
    char id[64];
    char name[128];
    char owner[128];
    uint64_t creation_time;
};

class RepositoryManager {
public:
    static const char* detect_mime_type(const char* filename);
    static size_t serialize_asset(const AssetMetadata* meta, char* buffer, size_t max_len);
    static size_t serialize_repo(const RepoMetadata* meta, char* buffer, size_t max_len);
};

#endif