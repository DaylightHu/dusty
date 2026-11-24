#include "minecraft_font.h"
#include "pack_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BITMAP_SHEETS = 16, BITMAP_GLYPHS = 8192,
    FONT_PATH_SIZE = 128, UNICODE_PAGES = 256, UNICODE_CELL = 16,
    UNICODE_SIZE = UNICODE_PAGES*UNICODE_PAGES
};

typedef struct {
    int codepoint;
    unsigned provider;
    MinecraftGlyph glyph;
} BitmapGlyph;

struct MinecraftFont {
    const ResourcePack *pack;
    BitmapGlyph *bitmaps;
    unsigned bitmap_count, sheet_count;
    Texture2D sheets[BITMAP_SHEETS];
    unsigned char *sizes;
    unsigned unicode_provider;
    char page_template[FONT_PATH_SIZE];
    size_t page_marker;
    Texture2D pages[UNICODE_PAGES];
    bool tried_pages[UNICODE_PAGES];
};

static bool font_path(const PackJson *json, int object, const char *key,
                      char path[FONT_PATH_SIZE])
{
    int token = pack_json_field(json, object, key);
    if (token < 0 || json->tokens[token].type != JSMN_STRING) return false;
    const char *text = json->text + json->tokens[token].start;
    size_t size = (size_t)(json->tokens[token].end - json->tokens[token].start);
    if (size > 10 && !memcmp(text, "minecraft:", 10)) {
        text += 10;
        size -= 10;
    }
    if (!size || size >= FONT_PATH_SIZE || text[0] == '/' ||
        memchr(text, '\\', size) || memchr(text, ':', size)) return false;
    memcpy(path, text, size);
    path[size] = '\0';
    return true;
}

static bool font_number(const PackJson *json, int token, int *value)
{
    if (token < 0 || json->tokens[token].type != JSMN_PRIMITIVE) return false;
    const char *text = json->text + json->tokens[token].start;
    char *end;
    long parsed = strtol(text, &end, 10);
    if (end == text || end != json->text + json->tokens[token].end ||
        parsed < -256 || parsed > 256) return false;
    *value = (int)parsed;
    return true;
}

static int hex4(const char *text)
{
    int codepoint = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char ch = (unsigned char)text[i];
        int digit = ch >= '0' && ch <= '9' ? ch - '0' :
                    ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                    ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (digit < 0) return -1;
        codepoint = codepoint*16 + digit;
    }
    return codepoint;
}

static int read_codepoint(const char **cursor, const char *end)
{
    const char *text = *cursor;
    if (text >= end) return -1;
    if (*text != '\\') {
        int bytes;
        int codepoint = GetCodepointNext(text, &bytes);
        if (bytes > end - text || (codepoint == '?' && *text != '?') ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return -1;
        *cursor += bytes;
        return codepoint;
    }
    if (end - text < 2) return -1;
    if (text[1] != 'u') {
        const char escapes[] = "\"\\/bfnrt";
        const char *found = strchr(escapes, text[1]);
        if (!found) return -1;
        *cursor += 2;
        return "\"\\/\b\f\n\r\t"[found - escapes];
    }
    if (end - text < 6) return -1;
    int codepoint = hex4(text + 2);
    text += 6;
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
        if (end - text < 6 || text[0] != '\\' || text[1] != 'u') return -1;
        int low = hex4(text + 2);
        if (low < 0xdc00 || low > 0xdfff) return -1;
        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
        text += 6;
    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return -1;
    *cursor = text;
    return codepoint;
}

static int row_width(const PackJson *json, int token)
{
    jsmntok_t row = json->tokens[token];
    if (row.type != JSMN_STRING) return 0;
    const char *cursor = json->text + row.start, *end = json->text + row.end;
    int width = 0;
    while (cursor < end) {
        if (read_codepoint(&cursor, end) < 0) return 0;
        ++width;
    }
    return width;
}

static int ink_width(const Color *pixels, int stride, int x, int y, int width, int height)
{
    for (int col = width - 1; col >= 0; --col)
        for (int row = 0; row < height; ++row)
            if (pixels[(y+row)*stride + x+col].a) return col + 1;
    return 0;
}

static bool load_bitmap(MinecraftFont *font, const PackJson *json, int provider,
                        unsigned priority)
{
    char path[FONT_PATH_SIZE];
    int height = 8, ascent;
    int custom_height = pack_json_field(json, provider, "height");
    int chars = pack_json_field(json, provider, "chars");
    if (font->sheet_count == BITMAP_SHEETS || chars < 0 ||
        json->tokens[chars].type != JSMN_ARRAY ||
        !font_path(json, provider, "file", path) ||
        !font_number(json, pack_json_field(json, provider, "ascent"), &ascent) ||
        (custom_height >= 0 && !font_number(json, custom_height, &height)) ||
        height <= 0 || ascent > height) return false;
    int rows = json->tokens[chars].size;
    int end = pack_json_next(json, chars);
    if (rows < 1 || end - chars != rows + 1) return false;
    int columns = row_width(json, chars+1);
    if (!columns) return false;

    Image image = resource_pack_image(font->pack, path);
    if (!image.data) return false;
    unsigned first = font->bitmap_count;
    if (image.width % columns || image.height % rows) {
        UnloadImage(image);
        return false;
    }
    int cell_width = image.width/columns, cell_height = image.height/rows;
    float scale = (float)height/cell_height;
    Color *pixels = LoadImageColors(image);
    bool ok = pixels != NULL;
    for (int y = 0; y < rows && ok; ++y) {
        jsmntok_t row = json->tokens[chars+1+y];
        if (row.type != JSMN_STRING) { ok = false; break; }
        const char *cursor = json->text + row.start, *limit = json->text + row.end;
        for (int x = 0; x < columns; ++x) {
            int codepoint = read_codepoint(&cursor, limit);
            if (codepoint < 0 || font->bitmap_count == BITMAP_GLYPHS) { ok = false; break; }
            if (codepoint == 0 || codepoint == ' ') continue;
            int px = x*cell_width, py = y*cell_height;
            int width = ink_width(pixels, image.width, px, py, cell_width, cell_height);
            font->bitmaps[font->bitmap_count++] = (BitmapGlyph){
                .codepoint = codepoint, .provider = priority,
                .glyph = {
                    .source = {px, py, width, cell_height},
                    .advance = floorf(width*scale + 0.5f) + 1,
                    .scale = scale, .top = 7-ascent
                }
            };
        }
        if (cursor != limit) ok = false;
    }
    if (pixels) UnloadImageColors(pixels);

    Texture2D texture = {0};
    if (ok && font->bitmap_count > first) texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (!texture.id) {
        font->bitmap_count = first;
        return false;
    }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    font->sheets[font->sheet_count++] = texture;
    for (unsigned i = first; i < font->bitmap_count; ++i)
        font->bitmaps[i].glyph.texture = texture;
    return true;
}

static void load_unicode_sizes(MinecraftFont *font, const PackJson *json, int provider,
                               unsigned priority)
{
    char sizes[FONT_PATH_SIZE], pattern[FONT_PATH_SIZE];
    if (font->sizes || !font_path(json, provider, "sizes", sizes) ||
        !font_path(json, provider, "template", pattern)) return;
    const char *marker = strchr(pattern, '%');
    if (!marker || marker[1] != 's' || strchr(marker + 2, '%')) return;
    int length;
    unsigned char *bytes = resource_pack_bytes(font->pack, sizes, &length);
    if (!bytes || length != UNICODE_SIZE) { free(bytes); return; }
    font->sizes = bytes;
    font->unicode_provider = priority;
    strcpy(font->page_template, pattern);
    font->page_marker = (size_t)(marker - pattern);
}

static int compare_glyphs(const void *left, const void *right)
{
    const BitmapGlyph *a = left, *b = right;
    if (a->codepoint != b->codepoint)
        return (a->codepoint > b->codepoint) - (a->codepoint < b->codepoint);
    return (a->provider > b->provider) - (a->provider < b->provider);
}

MinecraftFont *minecraft_font_load(const ResourcePack *pack)
{
    PackJson json = {0};
    if (!pack_json_read(pack, "font/default.json", &json)) return NULL;
    MinecraftFont *font = calloc(1, sizeof(*font));
    if (font) {
        font->pack = pack;
        font->bitmaps = malloc(BITMAP_GLYPHS*sizeof(*font->bitmaps));
        int providers = pack_json_field(&json, 0, "providers");
        if (providers >= 0 && json.tokens[providers].type == JSMN_ARRAY) {
            unsigned priority = 0;
            int end = pack_json_next(&json, providers);
            for (int i = providers + 1; i < end; i = pack_json_next(&json, i), ++priority) {
                int type = pack_json_field(&json, i, "type");
                if (font->bitmaps && pack_json_equals(&json, type, "bitmap"))
                    (void)load_bitmap(font, &json, i, priority);
                else if (pack_json_equals(&json, type, "legacy_unicode"))
                    load_unicode_sizes(font, &json, i, priority);
            }
        }
    }
    pack_json_free(&json);
    if (font && (font->bitmap_count || font->sizes)) {
        if (font->bitmap_count)
            qsort(font->bitmaps, font->bitmap_count, sizeof(*font->bitmaps), compare_glyphs);
        return font;
    }
    minecraft_font_unload(font);
    return NULL;
}

void minecraft_font_unload(MinecraftFont *font)
{
    if (!font) return;
    for (unsigned i = 0; i < font->sheet_count; ++i) UnloadTexture(font->sheets[i]);
    for (unsigned i = 0; i < UNICODE_PAGES; ++i)
        if (font->pages[i].id) UnloadTexture(font->pages[i]);
    free(font->sizes);
    free(font->bitmaps);
    free(font);
}

static const BitmapGlyph *find_bitmap(const MinecraftFont *font, int codepoint)
{
    unsigned lo = 0, hi = font->bitmap_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo)/2;
        if (font->bitmaps[mid].codepoint < codepoint) lo = mid + 1;
        else hi = mid;
    }
    return lo < font->bitmap_count && font->bitmaps[lo].codepoint == codepoint ?
        &font->bitmaps[lo] : NULL;
}

static Texture2D unicode_page(MinecraftFont *font, unsigned page)
{
    if (!font->tried_pages[page]) {
        font->tried_pages[page] = true;
        char path[FONT_PATH_SIZE];
        snprintf(path, sizeof(path), "%.*s%02x%s", (int)font->page_marker,
                 font->page_template, page, font->page_template + font->page_marker + 2);
        Image image = resource_pack_image(font->pack, path);
        if (image.width == UNICODE_CELL*UNICODE_CELL && image.height == image.width) {
            font->pages[page] = LoadTextureFromImage(image);
            if (font->pages[page].id) SetTextureFilter(font->pages[page], TEXTURE_FILTER_POINT);
        }
        if (image.data) UnloadImage(image);
    }
    return font->pages[page];
}

static bool unicode_glyph(MinecraftFont *font, int codepoint, MinecraftGlyph *glyph)
{
    if (!font->sizes || (unsigned)codepoint >= UNICODE_SIZE) return false;
    unsigned encoded = font->sizes[codepoint];
    int left = (int)(encoded >> 4), width = (int)(encoded & 15) - left + 1;
    if (!encoded || width < 1) return false;
    Texture2D texture = unicode_page(font, (unsigned)codepoint >> 8);
    if (!texture.id) return false;
    int column = codepoint & 15, row = (codepoint >> 4) & 15;
    *glyph = (MinecraftGlyph){
        .texture = texture,
        .source = {column*UNICODE_CELL + left, row*UNICODE_CELL, width, UNICODE_CELL},
        .advance = width/2 + 1, .scale = 0.5f
    };
    return true;
}

bool minecraft_font_glyph(MinecraftFont *font, int codepoint, MinecraftGlyph *glyph)
{
    if (!font) return false;
    const BitmapGlyph *bitmap = find_bitmap(font, codepoint);
    if ((!bitmap || (font->sizes && font->unicode_provider < bitmap->provider)) &&
        unicode_glyph(font, codepoint, glyph)) return true;
    if (!bitmap) return false;
    *glyph = bitmap->glyph;
    return true;
}
