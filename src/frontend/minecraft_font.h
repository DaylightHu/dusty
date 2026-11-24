#ifndef VOXILOG_MINECRAFT_FONT_H
#define VOXILOG_MINECRAFT_FONT_H

#include "resource_pack.h"

typedef struct MinecraftFont MinecraftFont;

typedef struct {
    Texture2D texture;
    Rectangle source;
    float advance, scale, top; /* In the eight-pixel GUI font's coordinate space. */
} MinecraftGlyph;

/* The archive stays open for Unicode pages fetched during text layout. */
MinecraftFont *minecraft_font_load(const ResourcePack *pack);
void minecraft_font_unload(MinecraftFont *font);
bool minecraft_font_glyph(MinecraftFont *font, int codepoint, MinecraftGlyph *glyph);

#endif
