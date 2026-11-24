#ifndef VOXILOG_MINECRAFT_MODEL_H
#define VOXILOG_MINECRAFT_MODEL_H

#include "resource_pack.h"

enum {
    MINECRAFT_MODEL_PATH = 128,
    MINECRAFT_MODEL_QUADS = 64,
    MINECRAFT_MODEL_TEXTURES = 16
};

typedef struct {
    Vector3 positions[4];
    Vector2 uv[4]; /* In Minecraft's 0..16 texture coordinates. */
    unsigned texture;
} MinecraftQuad;

typedef struct {
    MinecraftQuad quads[MINECRAFT_MODEL_QUADS];
    char textures[MINECRAFT_MODEL_TEXTURES][MINECRAFT_MODEL_PATH];
    unsigned quad_count, texture_count;
    Vector3 gui_rotation, gui_translation, gui_scale;
    bool generated;
} MinecraftModel;

typedef struct {
    char model[MINECRAFT_MODEL_PATH];
    float x, y;
} MinecraftVariant;

bool minecraft_model_read(const ResourcePack *pack, const char *name, MinecraftModel *model);
bool minecraft_variants_read(const ResourcePack *pack, const char *block,
                             const char *const *keys, MinecraftVariant *variants, unsigned count);
/* Texture paths are relative to textures/, without the .png suffix. */
Image minecraft_texture(const ResourcePack *pack, const char *path);
Image minecraft_item_image(const ResourcePack *pack, const char *name);

#endif
