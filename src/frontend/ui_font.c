#include "ui_font.h"
#include "minecraft_font.h"

#include <math.h>
#include <stddef.h>

static MinecraftFont *minecraft_font;

bool ui_font_init(const ResourcePack *pack)
{
    ui_font_unload();
    minecraft_font = minecraft_font_load(pack);
    MinecraftGlyph replacement;
    if (!minecraft_font || !minecraft_font_glyph(minecraft_font, '?', &replacement)) {
        TraceLog(LOG_ERROR, "UI FONT: Cannot load the default font from the resource pack");
        ui_font_unload();
        return false;
    }
    return true;
}

void ui_font_unload(void)
{
    minecraft_font_unload(minecraft_font);
    minecraft_font = NULL;
}

/* Drawing and measurement walk the same glyphs and advances. */
static float minecraft_text(const char *text, float x, float y, int size, Color color, bool draw)
{
    float units = size/8.0f, width = 0, longest = 0;
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
            if (!minecraft_font_glyph(minecraft_font, codepoint, &glyph) &&
                !minecraft_font_glyph(minecraft_font, '?', &glyph)) continue;
            if (draw && glyph.source.width > 0) {
                float scale = glyph.scale*units;
                Rectangle target = {x + width, y + glyph.top*units,
                                    glyph.source.width*scale, glyph.source.height*scale};
                DrawTexturePro(glyph.texture, glyph.source, target, (Vector2){0}, 0, color);
            }
            width += glyph.advance*units;
        }
    }
    return fmaxf(longest, width);
}

void ui_text_draw(const char *text, int x, int y, int size, Color color)
{
    if (!text || !text[0] || size <= 0) return;
    (void)minecraft_text(text, x, y, size, color, true);
}

int ui_text_width(const char *text, int size)
{
    if (!text || !text[0] || size <= 0) return 0;
    return (int)ceilf(minecraft_text(text, 0, 0, size, BLANK, false));
}
