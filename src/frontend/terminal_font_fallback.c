#include "terminal_font_fallback.h"
#include "rlgl.h"

#include <string.h>
#ifdef __APPLE__
#include <CoreText/CoreText.h>
#endif

/* Keep this copy private. raylib uses the same vendored rasterizer. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "external/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

enum { SLOT_SIZE = 64, ATLAS_COLUMNS = 32, SLOT_COUNT = 1024 };
typedef struct {
    int codepoint;
    Rectangle source;
    bool occupied;
} FallbackGlyph;

static struct {
    int size;
    unsigned revision;
    unsigned char *data;
    stbtt_fontinfo font;
    Texture2D atlas;
    FallbackGlyph glyphs[SLOT_COUNT];
} fallback;

static bool postscript_name(const stbtt_fontinfo *font, const char *name)
{
    int length;
    const char *value = stbtt_GetFontNameString(font, &length, 3, 1, 0x409, 6);
    if (!value || (size_t)length != 2 * strlen(name)) return false;
    for (; *name; value += 2, ++name)
        if (value[0] != 0 || value[1] != *name) return false;
    return true;
}

static bool load_fallback(const char *path, const char *preferred_face)
{
    if (!FileExists(path)) return false;
    int length = 0;
    unsigned char *data = LoadFileData(path, &length);
    if (!data) return false;
    int faces = length >= 12 ? stbtt_GetNumberOfFonts(data) : 0;
    int first_usable = -1;
    for (int i = 0; i < faces; ++i) {
        int offset = stbtt_GetFontOffsetForIndex(data, i);
        if (offset < 0 || offset > length - 12 ||
            !stbtt_InitFont(&fallback.font, data, offset) ||
            !stbtt_FindGlyphIndex(&fallback.font, 0x4e2d)) continue;
        if (first_usable < 0) first_usable = offset;
        if (!preferred_face || postscript_name(&fallback.font, preferred_face)) {
            first_usable = offset;
            break;
        }
    }
    if (first_usable < 0 || !stbtt_InitFont(&fallback.font, data, first_usable)) {
        UnloadFileData(data);
        return false;
    }
    fallback.data = data;
    return true;
}

#ifdef __APPLE__
static bool load_macos_fallback(void)
{
    CTFontRef font = CTFontCreateWithName(CFSTR("PingFangSC-Regular"), 12, NULL);
    if (!font) return false;
    CFURLRef url = CTFontCopyAttribute(font, kCTFontURLAttribute);
    CFRelease(font);
    if (!url) return false;
    UInt8 path[4096];
    bool found = CFURLGetFileSystemRepresentation(url, true, path, sizeof path);
    CFRelease(url);
    return found && load_fallback((const char *)path, "PingFangSC-Regular");
}
#endif

void terminal_font_fallback_unload(void)
{
    if (fallback.atlas.id) UnloadTexture(fallback.atlas);
    if (fallback.data) UnloadFileData(fallback.data);
    memset(&fallback, 0, sizeof fallback);
}

void terminal_font_fallback_prepare(const char *path, int size, unsigned revision)
{
    if (size == fallback.size && revision == fallback.revision) return;
    terminal_font_fallback_unload();
    fallback.size = size;
    fallback.revision = revision;
    if (*path) {
        if (!load_fallback(path, "NotoSansCJKsc-Regular"))
            TraceLog(LOG_WARNING, "TERMINAL: Cannot load Chinese fallback font: %s", path);
        return;
    }
#ifdef __APPLE__
    if (!load_macos_fallback())
        (void)load_fallback("/System/Library/Fonts/STHeiti Medium.ttc", "STHeitiSC-Medium");
#elif defined(__linux__)
    (void)load_fallback("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                        "NotoSansCJKsc-Regular");
#endif
}

static void cache_glyph(FallbackGlyph *entry, int slot, int codepoint)
{
    bool replace = entry->occupied;
    *entry = (FallbackGlyph){.codepoint = codepoint, .occupied = replace};
    int index = stbtt_FindGlyphIndex(&fallback.font, codepoint);
    if (!index) return;
    int size = fallback.size < SLOT_SIZE - 4 ? fallback.size : SLOT_SIZE - 4;
    float scale = stbtt_ScaleForMappingEmToPixels(&fallback.font, size);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&fallback.font, index, scale, scale, &x0, &y0, &x1, &y1);
    int width = x1 - x0, height = y1 - y0;
    if (width <= 0 || height <= 0 || width > SLOT_SIZE - 2 || height > SLOT_SIZE - 2) return;
    if (!fallback.atlas.id) {
        Image image = GenImageColor(ATLAS_COLUMNS * SLOT_SIZE,
                                    (SLOT_COUNT / ATLAS_COLUMNS) * SLOT_SIZE, BLANK);
        if (!image.data) return;
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA);
        if (image.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA)
            fallback.atlas = LoadTextureFromImage(image);
        UnloadImage(image);
        if (!fallback.atlas.id) return;
        SetTextureFilter(fallback.atlas, TEXTURE_FILTER_POINT);
    }
    unsigned char alpha[SLOT_SIZE * SLOT_SIZE];
    unsigned char pixels[2 * SLOT_SIZE * SLOT_SIZE];
    stbtt_MakeGlyphBitmap(&fallback.font, alpha, width, height, width, scale, scale, index);
    for (int i = 0; i < width * height; ++i) {
        pixels[2*i] = 255;
        pixels[2*i+1] = alpha[i];
    }
    entry->source = (Rectangle){(float)((slot % ATLAS_COLUMNS) * SLOT_SIZE + 1),
                                (float)((slot / ATLAS_COLUMNS) * SLOT_SIZE + 1),
                                (float)width, (float)height};
    /* Finish draws that may still sample this slot before overwriting it. */
    if (replace) rlDrawRenderBatchActive();
    UpdateTextureRec(fallback.atlas, entry->source, pixels);
    entry->occupied = true;
}

bool terminal_font_fallback_glyph(int codepoint, Texture2D *atlas, Rectangle *source)
{
    if (!fallback.data || codepoint <= 0) return false;
    int slot = codepoint % SLOT_COUNT;
    FallbackGlyph *entry = &fallback.glyphs[slot];
    if (entry->codepoint != codepoint) cache_glyph(entry, slot, codepoint);
    if (entry->source.width <= 0) return false;
    *atlas = fallback.atlas;
    *source = entry->source;
    return true;
}
