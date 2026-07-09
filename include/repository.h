#ifndef REPOSITORY_H
#define REPOSITORY_H

#include <stdint.h>
#include <stddef.h>

struct AssetMetadata {
    char asset_id[64];
    char filename[256];
    char mime_type[64];
    uint64_t upload_time;
    uint64_t commit_hash;
    uint64_t manifest_hash;
    char thumbnail_path[256];
    uint64_t file_size;
    uint32_t width;
    uint32_t height;
};

struct RepoMetadata {
    char id[64];
    char name[128];
    char owner[128];
    uint64_t creation_time;
    int visibility; // 0 = public, 1 = private
    char collaborators[512]; // comma separated
};

// Prepared Metadata Structures for Future Scale
struct CommentMeta {
    uint64_t comment_id;
    char user[64];
    char text[1024];
    uint64_t timestamp;
};

struct LikeMeta {
    char user[64];
    uint64_t timestamp;
};

struct FollowMeta {
    char follower[64];
    char target[64];
    uint64_t timestamp;
};

class RepositoryManager {
public:
    static const char* detect_mime_type(const char* filename);
    static size_t serialize_asset(const AssetMetadata* meta, char* buffer, size_t max_len);
    static size_t serialize_repo(const RepoMetadata* meta, char* buffer, size_t max_len);
    static void generate_asset_id(const char* filename, uint64_t commit_hash, char* out_id);
};

#endif