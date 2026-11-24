#ifndef VOXILOG_RESOURCE_PACK_H
#define VOXILOG_RESOURCE_PACK_H

#include "raylib.h"

typedef struct ResourcePack ResourcePack;

ResourcePack *resource_pack_open(const char *path);
void resource_pack_close(ResourcePack *pack);
/* Paths are relative to assets/minecraft/textures in the archive. */
Image resource_pack_image(const ResourcePack *pack, const char *path);
/* Read a small JSON document under assets/minecraft. Release it with free(). */
char *resource_pack_json(const ResourcePack *pack, const char *path);

#endif
