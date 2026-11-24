#ifndef VOXILOG_MINECRAFT_REDSTONE_H
#define VOXILOG_MINECRAFT_REDSTONE_H

#include "block_renderer.h"

/* Bake model variants and their shared atlas before releasing the archive. */
MinecraftRedstone *minecraft_redstone_load(const ResourcePack *pack, Image *atlas);
void minecraft_redstone_free(MinecraftRedstone *models);
const BlockGeometry *minecraft_redstone_geometry(const MinecraftRedstone *models, BlockState state);

#endif
