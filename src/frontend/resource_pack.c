#include "resource_pack.h"
#include "external/stb_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ZIP_END_SIZE = 22,
    ZIP_DIRECTORY_HEADER = 46,
    ZIP_LOCAL_HEADER = 30,
    ZIP_TAIL_LIMIT = ZIP_END_SIZE + 65535,
    ZIP_STORED = 0,
    ZIP_DEFLATED = 8,
    ZIP_ENCRYPTED = (1u << 0) | (1u << 6) | (1u << 13),
    DIRECTORY_LIMIT = 16*1024*1024,
    JSON_BYTES_LIMIT = 64*1024,
    TEXTURE_BYTES_LIMIT = 8*1024*1024,
    TEXTURE_DIMENSION_LIMIT = 2048
};

static const char texture_prefix[] = "assets/minecraft/textures/";
static const char asset_prefix[] = "assets/minecraft/";

struct ResourcePack {
    FILE *file;
    unsigned char *directory;
    uint32_t directory_offset, directory_size;
};

static uint16_t zip_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t zip_u32(const unsigned char *p)
{
    return (uint32_t)zip_u16(p) | ((uint32_t)zip_u16(p + 2) << 16);
}

static bool read_at(FILE *file, long offset, void *buffer, size_t size)
{
    return fseek(file, offset, SEEK_SET) == 0 && fread(buffer, 1, size, file) == size;
}

static size_t directory_entry_size(const unsigned char *entry)
{
    return ZIP_DIRECTORY_HEADER + (size_t)zip_u16(entry + 28) +
           zip_u16(entry + 30) + zip_u16(entry + 32);
}

static bool directory_valid(const ResourcePack *pack, uint16_t count)
{
    size_t remaining = pack->directory_size;
    const unsigned char *entry = pack->directory;
    bool has_textures = false;
    for (uint16_t index = 0; index < count; ++index) {
        if (remaining < ZIP_DIRECTORY_HEADER || zip_u32(entry) != 0x02014b50)
            return false;
        size_t size = directory_entry_size(entry);
        if (size > remaining) return false;
        size_t name_size = zip_u16(entry + 28);
        const unsigned char *name = entry + ZIP_DIRECTORY_HEADER;
        if (name_size >= sizeof(texture_prefix) + 4 &&
            !memcmp(name, texture_prefix, sizeof(texture_prefix) - 1) &&
            !memcmp(name + name_size - 4, ".png", 4)) has_textures = true;
        remaining -= size;
        entry += size;
    }
    return remaining == 0 && has_textures;
}

static bool load_directory(ResourcePack *pack)
{
    if (fseek(pack->file, 0, SEEK_END) != 0) return false;
    long file_size = ftell(pack->file);
    if (file_size < ZIP_END_SIZE) return false;
    size_t tail_size = file_size < ZIP_TAIL_LIMIT ? (size_t)file_size : ZIP_TAIL_LIMIT;
    long tail_offset = file_size - (long)tail_size;
    unsigned char *tail = malloc(tail_size);
    if (!tail) return false;
    bool found = false;
    uint16_t count = 0;
    if (read_at(pack->file, tail_offset, tail, tail_size)) {
        for (size_t pos = tail_size - ZIP_END_SIZE + 1; pos-- > 0;) {
            const unsigned char *end = tail + pos;
            if (zip_u32(end) != 0x06054b50 ||
                tail_size - pos - ZIP_END_SIZE != zip_u16(end + 20)) continue;
            count = zip_u16(end + 10);
            pack->directory_size = zip_u32(end + 12);
            pack->directory_offset = zip_u32(end + 16);
            /* Standard single-file ZIP archives cover client JARs and packs. */
            found = zip_u32(end + 4) == 0 && zip_u16(end + 8) == count &&
                count != UINT16_MAX && pack->directory_size <= DIRECTORY_LIMIT &&
                pack->directory_size >= ZIP_DIRECTORY_HEADER &&
                (uint64_t)pack->directory_offset + pack->directory_size ==
                    (uint64_t)tail_offset + pos;
            if (found) break;
        }
    }
    free(tail);
    if (!found) return false;
    pack->directory = malloc(pack->directory_size);
    return pack->directory &&
        read_at(pack->file, (long)pack->directory_offset, pack->directory, pack->directory_size) &&
        directory_valid(pack, count);
}

ResourcePack *resource_pack_open(const char *path)
{
    if (!path || !*path) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    ResourcePack *pack = calloc(1, sizeof(*pack));
    if (!pack) { fclose(file); return NULL; }
    pack->file = file;
    if (load_directory(pack)) return pack;
    resource_pack_close(pack);
    return NULL;
}

void resource_pack_close(ResourcePack *pack)
{
    if (!pack) return;
    fclose(pack->file);
    free(pack->directory);
    free(pack);
}

static unsigned char *read_entry(const ResourcePack *pack, const unsigned char *entry,
                                 uint32_t limit)
{
    uint16_t flags = zip_u16(entry + 8), method = zip_u16(entry + 10);
    uint32_t packed_size = zip_u32(entry + 20), size = zip_u32(entry + 24);
    uint32_t offset = zip_u32(entry + 42);
    unsigned char header[ZIP_LOCAL_HEADER];
    if ((flags & ZIP_ENCRYPTED) || zip_u16(entry + 34) ||
        (method != ZIP_STORED && method != ZIP_DEFLATED) ||
        (method == ZIP_STORED && packed_size != size) ||
        !size || size > limit || !packed_size || packed_size > TEXTURE_BYTES_LIMIT)
        return NULL;
    if (offset > pack->directory_offset || pack->directory_offset - offset < sizeof(header) ||
        !read_at(pack->file, (long)offset, header, sizeof(header)) ||
        zip_u32(header) != 0x04034b50 || zip_u16(header + 6) != flags ||
        zip_u16(header + 8) != method || zip_u16(header + 26) != zip_u16(entry + 28))
        return NULL;

    uint64_t start = (uint64_t)offset + sizeof(header) +
        zip_u16(header + 26) + zip_u16(header + 28);
    if (start + packed_size > pack->directory_offset) return NULL;
    unsigned char *bytes = malloc((size_t)packed_size + 1);
    if (!bytes) return NULL;
    if (!read_at(pack->file, (long)start, bytes, packed_size)) {
        free(bytes);
        return NULL;
    }
    if (method == ZIP_DEFLATED) {
        unsigned char *decoded = malloc((size_t)size + 1);
        int decoded_size = decoded ? stbi_zlib_decode_noheader_buffer(
            (char *)decoded, (int)size, (const char *)bytes, (int)packed_size) : -1;
        free(bytes);
        bytes = decoded;
        if (decoded_size != (int)size) {
            free(bytes);
            return NULL;
        }
    }
    if (ComputeCRC32(bytes, (int)size) == zip_u32(entry + 16)) {
        bytes[size] = 0;
        return bytes;
    }
    free(bytes);
    return NULL;
}

static unsigned char *read_asset(const ResourcePack *pack, const char *prefix,
                                 const char *path, uint32_t limit, int *size)
{
    if (!pack || !path) return NULL;
    size_t prefix_size = strlen(prefix), path_size = strlen(path);
    const unsigned char *end = pack->directory + pack->directory_size;
    for (const unsigned char *entry = pack->directory; entry < end;
         entry += directory_entry_size(entry)) {
        const unsigned char *name = entry + ZIP_DIRECTORY_HEADER;
        if (zip_u16(entry + 28) != prefix_size + path_size ||
            memcmp(name, prefix, prefix_size) ||
            memcmp(name + prefix_size, path, path_size)) continue;

        unsigned char *bytes = read_entry(pack, entry, limit);
        if (bytes) *size = (int)zip_u32(entry + 24);
        return bytes;
    }
    return NULL;
}

Image resource_pack_image(const ResourcePack *pack, const char *path)
{
    int size;
    unsigned char *bytes = read_asset(pack, texture_prefix, path, TEXTURE_BYTES_LIMIT, &size);
    if (!bytes) return (Image){0};
    int width, height;
    Image image = {0};
    if (size >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) &&
        stbi_info_from_memory(bytes, size, &width, &height, NULL) &&
        width > 0 && width <= TEXTURE_DIMENSION_LIMIT &&
        height > 0 && height <= TEXTURE_DIMENSION_LIMIT)
        image = LoadImageFromMemory(".png", bytes, size);
    free(bytes);
    return image;
}

char *resource_pack_json(const ResourcePack *pack, const char *path)
{
    int size;
    unsigned char *bytes = read_asset(pack, asset_prefix, path, JSON_BYTES_LIMIT, &size);
    if (bytes && memchr(bytes, 0, (size_t)size)) {
        free(bytes);
        return NULL;
    }
    return (char *)bytes;
}
