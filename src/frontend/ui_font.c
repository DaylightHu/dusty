#include "ui_font.h"
#include "gui.h"
#include "terminal_font_fallback.h"
#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { FONT_CACHE_SIZE = 128, FALLBACK_SIZE = 16 };
static const char font_path[] = "res/fonts/ComicMono-Bold.ttf";
/* Rasterize at the requested pixel size so small text stays sharp. */
static Font fonts[FONT_CACHE_SIZE];
static bool attempted[FONT_CACHE_SIZE];
static float font_scale = 1.0f;

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

bool ui_font_init(void)
{
    ui_font_unload();
    if (!font_for_size(FALLBACK_SIZE).texture.id) {
        fprintf(stderr, "Cannot load UI font: %s\n", font_path);
        return false;
    }
    return true;
}

void ui_font_unload(void)
{
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

void ui_text_draw(const char *text, int x, int y, int size, Color color)
{
    if (!text || !text[0] || size <= 0) return;
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
    return (int)ceilf(MeasureTextEx(font_for_size(size), text, (float)size, 0).x);
}

/* Keep the terminal atlas separate from the GUI's ASCII font. */
static Font terminal_font;
static int terminal_font_size;
static char terminal_font_path[4096];

static Font terminal_font_for_size(int size, const char *path)
{
    if (terminal_font_size == size && strcmp(path, terminal_font_path) == 0)
        return terminal_font.texture.id ? terminal_font : font_for_size(size);
    if (terminal_font.texture.id) UnloadFont(terminal_font);
    terminal_font = (Font){0};
    terminal_font_size = size;
    snprintf(terminal_font_path, sizeof terminal_font_path, "%s", path);
    const char *file = *path ? path : "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
    if (FileExists(file)) {
        int codepoints[512], count = 0;
        for (int cp = 32; cp <= 126; ++cp) codepoints[count++] = cp;
        for (int cp = 160; cp <= 255; ++cp) codepoints[count++] = cp;
        for (int cp = 0x2500; cp <= 0x259f; ++cp) codepoints[count++] = cp;
        for (int cp = 0x2000; cp <= 0x203f; ++cp) codepoints[count++] = cp;
        Font font = LoadFontEx(file, size, codepoints, count);
        if (font.texture.id && font.texture.id != GetFontDefault().texture.id) {
            terminal_font = font;
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        }
    }
    return terminal_font.texture.id ? terminal_font : font_for_size(size);
}

void ui_terminal_font_unload(void)
{
    terminal_font_fallback_unload();
    if (terminal_font.texture.id) UnloadFont(terminal_font);
    terminal_font = (Font){0};
    terminal_font_size = 0;
}

int ui_terminal_cell_width(int size, const char *path)
{
    Font font = terminal_font_for_size(size, path);
    int width = (int)ceilf(MeasureTextEx(font, "M", (float)size, 0).x);
    return width > 0 ? width : 1;
}

void ui_terminal_glyph(int codepoint, Rectangle cell, int size,
    bool bold, bool italic, Color color)
{
    Font font = terminal_font.texture.id ? terminal_font : font_for_size(size);
    if (!font.glyphs || font.glyphCount <= 0) return;
    int index = GetGlyphIndex(font, codepoint);
    Texture2D atlas = {0};
    Rectangle source;
    bool use_fallback = font.glyphs[index].value != codepoint &&
        terminal_font_fallback_glyph(codepoint, (int)cell.width - 1,
                                     (int)cell.height - 2, &atlas, &source);
    if (font.glyphs[index].value != codepoint && codepoint >= 0x2500 && codepoint <= 0x257f)
        codepoint = codepoint == 0x2500 ? '-' : codepoint == 0x2502 ? '|' : '+';
    if (italic) {
        rlPushMatrix();
        rlTranslatef(cell.x, cell.y, 0);
        float skew[16] = {1,0,0,0, -.18f,1,0,0, 0,0,1,0, size*.13f,0,0,1};
        rlMultMatrixf(skew);
        cell.x = cell.y = 0;
    }
    if (use_fallback) {
        Vector2 position = {cell.x + floorf((cell.width - source.width)/2),
                            cell.y + floorf((cell.height - source.height)/2)};
        DrawTextureRec(atlas, source, position, color);
        if (bold) {
            position.x += 1;
            DrawTextureRec(atlas, source, position, color);
        }
    } else {
        DrawTextCodepoint(font, codepoint, (Vector2){cell.x, cell.y}, (float)size, color);
        if (bold)
            DrawTextCodepoint(font, codepoint, (Vector2){cell.x+1, cell.y}, (float)size, color);
    }
    if (italic) rlPopMatrix();
}
