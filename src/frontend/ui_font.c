#include "ui_font.h"
#include "gui.h"
#include "minecraft_font.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { FONT_CACHE_SIZE = 128, FALLBACK_SIZE = 16 };
static const char font_path[] = "res/fonts/ComicMono-Bold.ttf";
/* Rasterize at the requested pixel size so small text stays sharp. */
static Font fonts[FONT_CACHE_SIZE];
static bool attempted[FONT_CACHE_SIZE];
static float font_scale = 1.0f;
static MinecraftFont *minecraft_font;

static Font font_for_size(int size)
{
    int index = size > 0 && size < FONT_CACHE_SIZE ? size : FALLBACK_SIZE;
    if (!attempted[index]) {
        attempted[index] = true;
        Font font = LoadFontEx(gui_asset_path(font_path), index, NULL, 0);
        if (font.texture.id && font.texture.id != GetFontDefault().texture.id) {
            fonts[index] = font;
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        } else if (font.glyphs && font.glyphs != GetFontDefault().glyphs) UnloadFont(font);
    }
    return fonts[index].texture.id ? fonts[index] : fonts[FALLBACK_SIZE];
}

bool ui_font_init(const ResourcePack *pack)
{
    ui_font_unload();
    if (!font_for_size(FALLBACK_SIZE).texture.id) {
        fprintf(stderr, "Cannot load UI font: %s\n", font_path);
        return false;
    }
    minecraft_font = minecraft_font_load(pack);
    if (minecraft_font) TraceLog(LOG_INFO, "UI FONT: Using Minecraft font from resource pack");
    return true;
}

void ui_font_unload(void)
{
    minecraft_font_unload(minecraft_font);
    minecraft_font = NULL;
    for (int i = 0; i < FONT_CACHE_SIZE; ++i)
        if (fonts[i].texture.id) UnloadFont(fonts[i]);
    memset(fonts, 0, sizeof(fonts));
    memset(attempted, 0, sizeof(attempted));
    font_scale = 1.0f;
}

void ui_font_set_scale(float scale)
{
    font_scale = fmaxf(scale, 1.0f);
}

/* Drawing and measurement walk the same glyphs and advances. */
static float minecraft_text(const char *text, float x, float y, int size, Color color, bool draw)
{
    float units = size/8.0f, width = 0, longest = 0;
    Font fallback = {0};
    for (const char *cursor = text; *cursor;) {
        int bytes;
        int codepoint = GetCodepointNext(cursor, &bytes);
        cursor += bytes;
        if (codepoint == '\n') {
            longest = fmaxf(longest, width);
            width = 0;
            y += size + 2;
        } else if (codepoint == ' ' || codepoint == '\t') {
            width += (codepoint == ' ' ? 4 : 16)*units;
        } else {
            MinecraftGlyph glyph;
            if (minecraft_font_glyph(minecraft_font, codepoint, &glyph)) {
                if (draw && glyph.source.width > 0) {
                    float scale = glyph.scale*units;
                    Rectangle target = {x + width, y + glyph.top*units,
                                        glyph.source.width*scale, glyph.source.height*scale};
                    DrawTexturePro(glyph.texture, glyph.source, target, (Vector2){0}, 0, color);
                }
                width += glyph.advance*units;
            } else {
                if (!fallback.texture.id) fallback = font_for_size(size);
                int index = GetGlyphIndex(fallback, codepoint);
                if (draw) DrawTextCodepoint(fallback, codepoint, (Vector2){x + width, y}, size, color);
                float advance = fallback.glyphs[index].advanceX ?
                    fallback.glyphs[index].advanceX : fallback.recs[index].width;
                width += advance*size/fallback.baseSize;
            }
        }
    }
    return fmaxf(longest, width);
}

void ui_text_draw(const char *text, int x, int y, int size, Color color)
{
    if (!text || !text[0] || size <= 0) return;
    if (minecraft_font) {
        (void)minecraft_text(text, x, y, size, color, true);
        return;
    }
    Font font = font_for_size(size);
    float spacing = 0.0f;
    if (font_scale > 1.0f) {
        int pixels = (int)fminf(roundf(size*font_scale), FONT_CACHE_SIZE - 1);
        Font dense = font_for_size(pixels);
        /* Rasterization rounds glyph advances at each pixel size. Keep
         * Comic Mono's logical cell width for cursor and selection positions. */
        spacing = MeasureTextEx(font, "M", (float)size, 0).x -
                  MeasureTextEx(dense, "M", (float)size, 0).x;
        font = dense;
    }
    DrawTextEx(font, text, (Vector2){(float)x, (float)y}, (float)size, spacing, color);
}

int ui_text_width(const char *text, int size)
{
    if (!text || !text[0] || size <= 0) return 0;
    if (minecraft_font) return (int)ceilf(minecraft_text(text, 0, 0, size, BLANK, false));
    return (int)ceilf(MeasureTextEx(font_for_size(size), text, (float)size, 0).x);
}
