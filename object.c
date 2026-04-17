// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

static const char *object_type_name(ObjectType type) {
    switch (type) {
        case OBJ_BLOB:
            return "blob";
        case OBJ_TREE:
            return "tree";
        case OBJ_COMMIT:
            return "commit";
        default:
            return NULL;
    }
}

static int parse_object_type(const char *type, size_t len, ObjectType *type_out) {
    if (len == 4 && strncmp(type, "blob", len) == 0) {
        *type_out = OBJ_BLOB;
        return 0;
    }
    if (len == 4 && strncmp(type, "tree", len) == 0) {
        *type_out = OBJ_TREE;
        return 0;
    }
    if (len == 6 && strncmp(type, "commit", len) == 0) {
        *type_out = OBJ_COMMIT;
        return 0;
    }
    return -1;
}

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    if (!data && len > 0) return -1;
    if (!id_out) return -1;

    const char *type_name = object_type_name(type);
    if (!type_name) return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_name, len);
    if (header_len < 0 || (size_t)header_len + 1 > sizeof(header)) return -1;

    size_t object_len = (size_t)header_len + 1 + len;
    uint8_t *object = malloc(object_len);
    if (!object) return -1;

    memcpy(object, header, (size_t)header_len);
    object[header_len] = '\0';
    if (len > 0) {
        memcpy(object + header_len + 1, data, len);
    }

    compute_hash(object, object_len, id_out);
    if (object_exists(id_out)) {
        free(object);
        return 0;
    }

    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[512];
    int shard_len = snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    if (shard_len < 0 || (size_t)shard_len >= sizeof(shard_dir)) {
        free(object);
        return -1;
    }

    if (mkdir(shard_dir, 0755) != 0 && errno != EEXIST) {
        free(object);
        return -1;
    }

    char temp_path[512];
    int temp_len = snprintf(temp_path, sizeof(temp_path), "%s/tmp_object_XXXXXX", shard_dir);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temp_path)) {
        free(object);
        return -1;
    }

    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(object);
        return -1;
    }

    size_t written = 0;
    while (written < object_len) {
        ssize_t n = write(fd, object + written, object_len - written);
        if (n <= 0) {
            close(fd);
            unlink(temp_path);
            free(object);
            return -1;
        }
        written += (size_t)n;
    }
    free(object);

    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(temp_path);
        return -1;
    }

    if (rename(temp_path, final_path) != 0) {
        unlink(temp_path);
        return -1;
    }

    int dir_fd = open(shard_dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (fsync(dir_fd) != 0) {
            close(dir_fd);
            return -1;
        }
        close(dir_fd);
    }

    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    if (!id || !type_out || !data_out || !len_out) return -1;

    *data_out = NULL;
    *len_out = 0;

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    uint8_t *object = malloc((size_t)file_size);
    if (!object && file_size > 0) {
        fclose(f);
        return -1;
    }

    size_t read_len = fread(object, 1, (size_t)file_size, f);
    fclose(f);
    if (read_len != (size_t)file_size) {
        free(object);
        return -1;
    }

    ObjectID actual;
    compute_hash(object, read_len, &actual);
    if (memcmp(actual.hash, id->hash, HASH_SIZE) != 0) {
        free(object);
        return -1;
    }

    void *header_end = memchr(object, '\0', read_len);
    if (!header_end) {
        free(object);
        return -1;
    }

    size_t header_len = (uint8_t *)header_end - object;
    char *space = memchr(object, ' ', header_len);
    if (!space) {
        free(object);
        return -1;
    }

    size_t type_len = (uint8_t *)space - object;
    if (parse_object_type((char *)object, type_len, type_out) != 0) {
        free(object);
        return -1;
    }

    char *size_start = space + 1;
    char *size_end = NULL;
    unsigned long long parsed_len = strtoull(size_start, &size_end, 10);
    if (size_end != (char *)header_end) {
        free(object);
        return -1;
    }

    size_t data_len = read_len - header_len - 1;
    if ((size_t)parsed_len != data_len) {
        free(object);
        return -1;
    }

    void *data = malloc(data_len ? data_len : 1);
    if (!data) {
        free(object);
        return -1;
    }
    if (data_len > 0) {
        memcpy(data, (uint8_t *)header_end + 1, data_len);
    }

    free(object);
    *data_out = data;
    *len_out = data_len;
    return 0;
}
