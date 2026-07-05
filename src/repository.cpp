#include "repository.h"
#include <string.h>
#include <stdio.h>

const char* RepositoryManager::detect_mime_type(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    if (strcmp(dot, ".webp") == 0) return "image/webp";
    if (strcmp(dot, ".wpk") == 0) return "application/x-wpk";
    return "application/octet-stream";
}

size_t RepositoryManager::serialize_asset(const AssetMetadata* meta, char* buffer, size_t max_len) {
    int len = snprintf(buffer, max_len,
        "FILE: %s\n"
        "MIME: %s\n"
        "TIME: %lu\n"
        "COMMIT: %lx\n"
        "MANIFEST: %lx\n"
        "THUMB: %s\n"
        "SIZE: %lu\n\n",
        meta->filename, meta->mime_type, meta->upload_time,
        meta->commit_hash, meta->manifest_hash, meta->thumbnail_path, meta->file_size);
    return (len > 0 && (size_t)len < max_len) ? len : 0;
}

size_t RepositoryManager::serialize_repo(const RepoMetadata* meta, char* buffer, size_t max_len) {
    int len = snprintf(buffer, max_len,
        "ID: %s\n"
        "NAME: %s\n"
        "OWNER: %s\n"
        "CREATED: %lu\n\n",
        meta->id, meta->name, meta->owner, meta->creation_time);
    return (len > 0 && (size_t)len < max_len) ? len : 0;
}