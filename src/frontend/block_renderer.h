#ifndef MC_BLOCK_RENDERER_H
#define MC_BLOCK_RENDERER_H

#include "resource_pack.h"
#include "rs/block.h"

typedef struct MinecraftRedstone MinecraftRedstone;

typedef struct{
    Texture2D atlas;
    Texture2D iron_texture;
    Texture2D glass_texture;
    Texture2D wire_atlas;
    Texture2D torch_atlas;
    Texture2D powered_rail_atlas;
    Texture2D activator_rail_atlas;
    Texture2D minecraft_atlas;
    MinecraftRedstone *minecraft;
    Shader cutout_shader;
} BlockRenderer;

enum { BLOCK_GEOMETRY_QUADS = 32 };
typedef struct {
    Vector3 vertices[BLOCK_GEOMETRY_QUADS][4];
    Vector2 uv[BLOCK_GEOMETRY_QUADS][4];
    int quads;
} BlockGeometry;

bool block_renderer_init(BlockRenderer *renderer, const ResourcePack *pack);
void block_renderer_unload(BlockRenderer *renderer);

/* CPU geometry for persistent meshes. Solid face bits follow the renderer's
 * +Z,-Z,+X,-X,+Y,-Y order; cutouts ignore the mask. UV generation does not
 * calculate positions, so state changes can update only the GPU UV buffer. */
int block_renderer_build_uv(const BlockRenderer *renderer, BlockState state,
                            unsigned solid_faces,
                            Vector2 uv[BLOCK_GEOMETRY_QUADS][4]);
void block_renderer_build_geometry(const BlockRenderer *renderer, BlockState state,
                                   Vector3 center, unsigned solid_faces,
                                   BlockGeometry *geometry);

#endif
