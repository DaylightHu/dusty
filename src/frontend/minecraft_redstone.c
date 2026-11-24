#include "minecraft_redstone.h"
#include "minecraft_model.h"
#include "raymath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LEVER, REPEATER, COMPARATOR, PISTON, FAMILY_COUNT,
    LEVER_VARIANTS = 24, REPEATER_VARIANTS = 64, COMPARATOR_VARIANTS = 16,
    PISTON_BASE_VARIANTS = 2*DIR_COUNT, PISTON_HEAD_FIRST = 2*PISTON_BASE_VARIANTS,
    PISTON_VARIANTS = PISTON_HEAD_FIRST + 2*DIR_COUNT,
    KEY_SIZE = 84, ATLAS_TILE = 32, ATLAS_COLUMNS = 8, ATLAS_ROWS = 4,
    ATLAS_TEXTURES = ATLAS_COLUMNS*ATLAS_ROWS
};

static const char *const family_names[] = {"lever", "repeater", "comparator", "piston"};

struct MinecraftRedstone {
    BlockGeometry *families[FAMILY_COUNT];
};

typedef struct {
    char names[ATLAS_TEXTURES][MINECRAFT_MODEL_PATH];
    Image textures[ATLAS_TEXTURES];
    unsigned count;
} Atlas;

static unsigned family(BlockId id)
{
    switch (id) {
    case BLOCK_LEVER: return LEVER;
    case BLOCK_REDSTONE_REPEATER: return REPEATER;
    case BLOCK_REDSTONE_COMPARATOR: return COMPARATOR;
    case BLOCK_PISTON: case BLOCK_STICKY_PISTON:
    case BLOCK_PISTON_HEAD: case BLOCK_STICKY_PISTON_HEAD: return PISTON;
    default: return FAMILY_COUNT;
    }
}

static unsigned horizontal(Direction direction)
{
    switch (direction) {
    case DIR_EAST: return 1;
    case DIR_SOUTH: return 2;
    case DIR_WEST: return 3;
    default: return 0;
    }
}

static void variant_keys(unsigned kind, char keys[][KEY_SIZE], unsigned count)
{
    static const char *const directions[] = {"north","east","south","west"};
    static const char *const axes[] = {"east","west","up","down","south","north"};
    static const char *const faces[] = {"floor","wall","ceiling"};
    static const char *const flags[] = {"false","true"};
    for (unsigned i = 0; i < count; ++i) {
        switch (kind) {
        case LEVER:
            snprintf(keys[i], KEY_SIZE, "face=%s,facing=%s,powered=%s",
                     faces[i/8], directions[(i/2)%4], flags[i%2]);
            break;
        case REPEATER:
            /* Voxilog stores the output direction. Minecraft FACING points to input. */
            snprintf(keys[i], KEY_SIZE, "delay=%u,facing=%s,locked=%s,powered=%s",
                     i/16 + 1, directions[(i/4 + 2)%4], flags[(i/2)%2], flags[i%2]);
            break;
        case COMPARATOR:
            snprintf(keys[i], KEY_SIZE, "facing=%s,mode=%s,powered=%s",
                     directions[(i/4 + 2)%4], i/2%2 ? "subtract" : "compare", flags[i%2]);
            break;
        case PISTON:
            if (i < PISTON_HEAD_FIRST) {
                snprintf(keys[i], KEY_SIZE, "extended=%s,facing=%s",
                         flags[(i/DIR_COUNT)%2], axes[i%DIR_COUNT]);
            } else {
                snprintf(keys[i], KEY_SIZE, "facing=%s,short=false,type=%s",
                         axes[i%DIR_COUNT], i-PISTON_HEAD_FIRST < DIR_COUNT ? "normal" : "sticky");
            }
            break;
        }
    }
}

static int atlas_tile(Atlas *atlas, const ResourcePack *pack, const char *name)
{
    for (unsigned i = 0; i < atlas->count; ++i)
        if (!strcmp(atlas->names[i], name)) return (int)i;
    if (atlas->count == ATLAS_TEXTURES) return -1;
    Image image = minecraft_texture(pack, name);
    if (!image.data) return -1;
    if (image.width != ATLAS_TILE || image.height != ATLAS_TILE)
        ImageResizeNN(&image, ATLAS_TILE, ATLAS_TILE);
    unsigned index = atlas->count++;
    strcpy(atlas->names[index], name);
    atlas->textures[index] = image;
    return (int)index;
}

static void rollback_tiles(Atlas *atlas, unsigned count)
{
    while (atlas->count > count) UnloadImage(atlas->textures[--atlas->count]);
}

static Rectangle tile_rect(unsigned tile)
{
    return (Rectangle){tile%ATLAS_COLUMNS*ATLAS_TILE, tile/ATLAS_COLUMNS*ATLAS_TILE,
                       ATLAS_TILE, ATLAS_TILE};
}

static bool bake_variant(const ResourcePack *pack, Atlas *atlas,
                         const MinecraftVariant *variant, BlockGeometry *baked)
{
    MinecraftModel model;
    if (!minecraft_model_read(pack, variant->model, &model) || model.generated ||
        model.quad_count > BLOCK_GEOMETRY_QUADS) return false;
    int tiles[MINECRAFT_MODEL_TEXTURES];
    for (unsigned i = 0; i < model.texture_count; ++i) {
        tiles[i] = atlas_tile(atlas, pack, model.textures[i]);
        if (tiles[i] < 0) return false;
    }
    baked->quads = (int)model.quad_count;
    for (unsigned face = 0; face < model.quad_count; ++face) {
        const MinecraftQuad *quad = &model.quads[face];
        Rectangle tile = tile_rect((unsigned)tiles[quad->texture]);
        for (int v = 0; v < 4; ++v) {
            Vector3 point = Vector3RotateByAxisAngle(quad->positions[v], (Vector3){1,0,0},
                                                      -variant->x*DEG2RAD);
            baked->vertices[face][v] = Vector3RotateByAxisAngle(point, (Vector3){0,1,0},
                                                               -variant->y*DEG2RAD);
            float u = Clamp(quad->uv[v].x, 0, 16)*(ATLAS_TILE - 1)/16 + 0.5f;
            float t = Clamp(quad->uv[v].y, 0, 16)*(ATLAS_TILE - 1)/16 + 0.5f;
            baked->uv[face][v] = (Vector2){
                (tile.x + u)/(ATLAS_COLUMNS*ATLAS_TILE),
                (tile.y + t)/(ATLAS_ROWS*ATLAS_TILE)
            };
        }
    }
    return true;
}

static BlockGeometry *load_family(const ResourcePack *pack, Atlas *atlas, unsigned kind)
{
    static const unsigned counts[] = {LEVER_VARIANTS, REPEATER_VARIANTS, COMPARATOR_VARIANTS, PISTON_VARIANTS};
    char keys[REPEATER_VARIANTS][KEY_SIZE];
    const char *key_paths[REPEATER_VARIANTS];
    MinecraftVariant variants[REPEATER_VARIANTS];
    unsigned count = counts[kind];
    variant_keys(kind, keys, count);
    for (unsigned i = 0; i < count; ++i) key_paths[i] = keys[i];
    if (kind == PISTON) {
        if (!minecraft_variants_read(pack, "piston", key_paths, variants, PISTON_BASE_VARIANTS) ||
            !minecraft_variants_read(pack, "sticky_piston", key_paths + PISTON_BASE_VARIANTS,
                                     variants + PISTON_BASE_VARIANTS, PISTON_BASE_VARIANTS) ||
            !minecraft_variants_read(pack, "piston_head", key_paths + PISTON_HEAD_FIRST,
                                     variants + PISTON_HEAD_FIRST, 2*DIR_COUNT)) return NULL;
    } else if (!minecraft_variants_read(pack, family_names[kind], key_paths, variants, count)) return NULL;

    BlockGeometry *baked = calloc(count, sizeof(*baked));
    if (!baked) return NULL;
    unsigned tile_count = atlas->count;
    int max_quads = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!bake_variant(pack, atlas, variants + i, baked + i)) {
            rollback_tiles(atlas, tile_count);
            free(baked);
            return NULL;
        }
        if (baked[i].quads > max_quads) max_quads = baked[i].quads;
    }
    /* The zero-filled extra quads are degenerate. Stable mesh slots let a
     * state change upload just that block, even when its face count changes. */
    for (unsigned i = 0; i < count; ++i) baked[i].quads = max_quads;
    return baked;
}

MinecraftRedstone *minecraft_redstone_load(const ResourcePack *pack, Image *image)
{
    *image = (Image){0};
    if (!pack) return NULL;
    MinecraftRedstone *models = calloc(1, sizeof(*models));
    if (!models) return NULL;
    Atlas atlas = {0};
    for (unsigned i = 0; i < FAMILY_COUNT; ++i) {
        models->families[i] = load_family(pack, &atlas, i);
        if (!models->families[i]) {
            TraceLog(LOG_ERROR, "BLOCK MODEL: Cannot load %s variants from the resource pack",
                     family_names[i]);
            goto cleanup;
        }
    }
    *image = GenImageColor(ATLAS_COLUMNS*ATLAS_TILE, ATLAS_ROWS*ATLAS_TILE, BLANK);
    if (image->data) {
        Rectangle tile = {0, 0, ATLAS_TILE, ATLAS_TILE};
        for (unsigned i = 0; i < atlas.count; ++i)
            ImageDraw(image, atlas.textures[i], tile, tile_rect(i), WHITE);
    }

cleanup:
    rollback_tiles(&atlas, 0);
    if (image->data) return models;
    minecraft_redstone_free(models);
    return NULL;
}

void minecraft_redstone_free(MinecraftRedstone *models)
{
    if (!models) return;
    for (unsigned i = 0; i < FAMILY_COUNT; ++i) free(models->families[i]);
    free(models);
}

const BlockGeometry *minecraft_redstone_geometry(const MinecraftRedstone *models, BlockState state)
{
    unsigned kind = family((BlockId)state.id);
    if (kind == FAMILY_COUNT) return NULL;
    unsigned index;
    if (kind == LEVER) {
        Direction support = (Direction)state.data.lever.support_dir;
        unsigned mount = support == DIR_DOWN ? 0 : support == DIR_UP ? 2 : 1;
        index = mount*8 + horizontal((Direction)state.data.lever.facing)*2 +
            state.data.lever.powered;
    } else if (kind == REPEATER) {
        unsigned delay = state.data.repeater.delay;
        if (delay < 1 || delay > 4) delay = 1;
        index = (delay-1)*16 + horizontal((Direction)state.data.repeater.facing)*4 +
            state.data.repeater.locked*2 + state.data.repeater.powered;
    } else if (kind == COMPARATOR) {
        index = horizontal((Direction)state.data.comparator.facing)*4 +
            state.data.comparator.subtract*2 + state.data.comparator.powered;
    } else {
        unsigned facing = state.data.piston.facing;
        if (facing >= DIR_COUNT) facing = DIR_NORTH;
        if (block_is_piston_head((BlockId)state.id))
            index = PISTON_HEAD_FIRST + (state.id == BLOCK_STICKY_PISTON_HEAD)*DIR_COUNT + facing;
        else
            index = (state.id == BLOCK_STICKY_PISTON)*PISTON_BASE_VARIANTS +
                    state.data.piston.extended*DIR_COUNT + facing;
    }
    return &models->families[kind][index];
}
