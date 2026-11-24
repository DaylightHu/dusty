#include "minecraft_model.h"
#include "raymath.h"

#define JSMN_STATIC
#define JSMN_STRICT
#include "../../third_party/jsmn/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { JSON_TOKENS = 8192, MODEL_PARENTS = 12, TEXTURE_VARIABLES = 48 };

typedef struct {
    char *text;
    jsmntok_t *tokens;
    int count;
} Json;

typedef struct {
    char key[MINECRAFT_MODEL_PATH], value[MINECRAFT_MODEL_PATH];
} TextureVariable;

typedef struct {
    MinecraftModel *model;
    TextureVariable variables[TEXTURE_VARIABLES];
    char references[MINECRAFT_MODEL_QUADS][MINECRAFT_MODEL_PATH];
    unsigned variable_count;
    bool has_elements, has_gui;
} ModelReader;

static void json_free(Json *json)
{
    free(json->text);
    free(json->tokens);
    *json = (Json){0};
}

static bool json_read(const ResourcePack *pack, const char *path, Json *json)
{
    json->text = resource_pack_json(pack, path);
    if (!json->text) return false;
    json->tokens = malloc(JSON_TOKENS*sizeof(*json->tokens));
    if (json->tokens) {
        jsmn_parser parser;
        jsmn_init(&parser);
        json->count = jsmn_parse(&parser, json->text, strlen(json->text),
                                 json->tokens, JSON_TOKENS);
        if (json->count > 0 && json->tokens[0].type == JSMN_OBJECT) {
            const char *tail = json->text + json->tokens[0].end;
            if (tail[strspn(tail, " \t\r\n")] == '\0') return true;
        }
    }
    json_free(json);
    return false;
}

static int next_token(const Json *json, int token)
{
    int end = json->tokens[token].end;
    while (++token < json->count && json->tokens[token].start < end) {}
    return token;
}

static bool token_equals(const Json *json, int token, const char *value)
{
    if (token < 0 || token >= json->count) return false;
    jsmntok_t t = json->tokens[token];
    size_t size = (size_t)(t.end - t.start);
    return strlen(value) == size && !memcmp(json->text + t.start, value, size);
}

static int field(const Json *json, int object, const char *key)
{
    if (object < 0 || json->tokens[object].type != JSMN_OBJECT) return -1;
    int end = next_token(json, object);
    for (int token = object + 1; token + 1 < end; token = next_token(json, token + 1))
        if (json->tokens[token].type == JSMN_STRING && token_equals(json, token, key))
            return token + 1;
    return -1;
}

static bool string(const Json *json, int token, char dest[MINECRAFT_MODEL_PATH])
{
    if (token < 0 || json->tokens[token].type != JSMN_STRING) return false;
    jsmntok_t t = json->tokens[token];
    size_t size = (size_t)(t.end - t.start);
    if (!size || size >= MINECRAFT_MODEL_PATH) return false;
    memcpy(dest, json->text + t.start, size);
    dest[size] = 0;
    return true;
}

static bool number(const Json *json, int token, float *value)
{
    if (token < 0 || json->tokens[token].type != JSMN_PRIMITIVE) return false;
    jsmntok_t t = json->tokens[token];
    char *end;
    float result = strtof(json->text + t.start, &end);
    if (end == json->text + t.start || end != json->text + t.end ||
        !isfinite(result) || fabsf(result) > 360.0f) return false;
    *value = result;
    return true;
}

static bool numbers(const Json *json, int token, float *values, int count)
{
    if (token < 0 || json->tokens[token].type != JSMN_ARRAY ||
        json->tokens[token].size != count) return false;
    int end = next_token(json, token);
    if (end - token != count + 1) return false;
    for (int i = 0; i < count; ++i)
        if (!number(json, token + 1 + i, values + i)) return false;
    return true;
}

static bool vector(const Json *json, int token, Vector3 *value)
{
    float xyz[3];
    if (!numbers(json, token, xyz, 3)) return false;
    *value = (Vector3){xyz[0], xyz[1], xyz[2]};
    return true;
}

static const char *minecraft_path(const char *name)
{
    if (!strncmp(name, "minecraft:", 10)) name += 10;
    static const char allowed[] = "abcdefghijklmnopqrstuvwxyz0123456789/_-.";
    return *name && *name != '/' && !strstr(name, "..") &&
        strspn(name, allowed) == strlen(name) ? name : NULL;
}

Image minecraft_texture(const ResourcePack *pack, const char *name)
{
    const char *path = minecraft_path(name);
    char png[MINECRAFT_MODEL_PATH + sizeof(".png")];
    if (!path || strlen(path) >= MINECRAFT_MODEL_PATH) return (Image){0};
    snprintf(png, sizeof(png), "%s.png", path);
    Image image = resource_pack_image(pack, png);
    if (image.data && image.height > image.width && image.height % image.width == 0)
        ImageCrop(&image, (Rectangle){0, 0, image.width, image.width});
    return image;
}

static const char *find_variable(const ModelReader *reader, const char *key)
{
    for (unsigned i = 0; i < reader->variable_count; ++i)
        if (!strcmp(reader->variables[i].key, key)) return reader->variables[i].value;
    return NULL;
}

static bool read_textures(ModelReader *reader, const Json *json, int token)
{
    if (token < 0) return true;
    if (json->tokens[token].type != JSMN_OBJECT) return false;
    int end = next_token(json, token);
    for (int i = token + 1; i + 1 < end; i = next_token(json, i + 1)) {
        TextureVariable entry;
        if (!string(json, i, entry.key) || !string(json, i + 1, entry.value)) return false;
        /* The child model's bindings override its parent's. */
        if (find_variable(reader, entry.key)) continue;
        if (reader->variable_count == TEXTURE_VARIABLES) return false;
        reader->variables[reader->variable_count++] = entry;
    }
    return true;
}

static int texture_index(ModelReader *reader, const char *name)
{
    for (unsigned i = 0; i < TEXTURE_VARIABLES && name && *name == '#'; ++i)
        name = find_variable(reader, name + 1);
    if (!name) return -1;
    const char *path = minecraft_path(name);
    if (!path) return -1;
    MinecraftModel *model = reader->model;
    for (unsigned i = 0; i < model->texture_count; ++i)
        if (!strcmp(model->textures[i], path)) return (int)i;
    if (model->texture_count == MINECRAFT_MODEL_TEXTURES) return -1;
    unsigned index = model->texture_count++;
    strcpy(model->textures[index], path);
    return (int)index;
}

static void default_uv(unsigned face, Vector3 from, Vector3 to, float uv[4])
{
    const float rectangles[][4] = {
        {from.x, 16-to.y, to.x, 16-from.y},       /* south */
        {16-to.x, 16-to.y, 16-from.x, 16-from.y}, /* north */
        {16-to.z, 16-to.y, 16-from.z, 16-from.y}, /* east */
        {from.z, 16-to.y, to.z, 16-from.y},       /* west */
        {from.x, from.z, to.x, to.z},            /* up */
        {from.x, 16-to.z, to.x, 16-from.z}        /* down */
    };
    memcpy(uv, rectangles[face], sizeof(rectangles[face]));
}

static bool read_element(ModelReader *reader, const Json *json, int element)
{
    Vector3 from, to;
    if (!vector(json, field(json, element, "from"), &from) ||
        !vector(json, field(json, element, "to"), &to) ||
        from.x > to.x || from.y > to.y || from.z > to.z) return false;
    const Vector3 corners[6][4] = {
        {{from.x,from.y,to.z}, {to.x,from.y,to.z}, {to.x,to.y,to.z}, {from.x,to.y,to.z}},
        {{to.x,from.y,from.z}, {from.x,from.y,from.z}, {from.x,to.y,from.z}, {to.x,to.y,from.z}},
        {{to.x,from.y,to.z}, {to.x,from.y,from.z}, {to.x,to.y,from.z}, {to.x,to.y,to.z}},
        {{from.x,from.y,from.z}, {from.x,from.y,to.z}, {from.x,to.y,to.z}, {from.x,to.y,from.z}},
        {{from.x,to.y,to.z}, {to.x,to.y,to.z}, {to.x,to.y,from.z}, {from.x,to.y,from.z}},
        {{from.x,from.y,from.z}, {to.x,from.y,from.z}, {to.x,from.y,to.z}, {from.x,from.y,to.z}}
    };
    Vector3 origin = {0}, axis = {0}, rescale = {1,1,1};
    float angle = 0;
    int rotation = field(json, element, "rotation");
    if (rotation >= 0) {
        int axis_token = field(json, rotation, "axis");
        if (token_equals(json, axis_token, "x")) axis.x = 1;
        else if (token_equals(json, axis_token, "y")) axis.y = 1;
        else if (token_equals(json, axis_token, "z")) axis.z = 1;
        else return false;
        if (!vector(json, field(json, rotation, "origin"), &origin) ||
            !number(json, field(json, rotation, "angle"), &angle) || fabsf(angle) > 45)
            return false;
        angle *= DEG2RAD;
        if (token_equals(json, field(json, rotation, "rescale"), "true")) {
            float scale = 1/cosf(angle);
            rescale = (Vector3){axis.x ? 1 : scale, axis.y ? 1 : scale, axis.z ? 1 : scale};
        }
    }

    static const char *const names[] = {"south", "north", "east", "west", "up", "down"};
    int faces = field(json, element, "faces");
    if (faces < 0 || json->tokens[faces].type != JSMN_OBJECT) return false;
    MinecraftModel *model = reader->model;
    for (unsigned side = 0; side < sizeof(names)/sizeof(*names); ++side) {
        int face = field(json, faces, names[side]);
        if (face < 0) continue;
        if (model->quad_count == MINECRAFT_MODEL_QUADS) return false;
        unsigned index = model->quad_count++;
        if (!string(json, field(json, face, "texture"), reader->references[index])) return false;

        float uv[4], turn = 0;
        int uv_token = field(json, face, "uv"), twist = field(json, face, "rotation");
        if (uv_token < 0) default_uv(side, from, to, uv);
        else if (!numbers(json, uv_token, uv, 4)) return false;
        if (twist >= 0 && (!number(json, twist, &turn) || turn < 0 ||
                           turn >= 360 || fmodf(turn, 90) != 0)) return false;
        const Vector2 corners_uv[4] = {
            {uv[0], uv[3]}, {uv[2], uv[3]}, {uv[2], uv[1]}, {uv[0], uv[1]}
        };
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            Vector3 point = corners[side][vertex];
            if (angle != 0) {
                point = Vector3RotateByAxisAngle(Vector3Subtract(point, origin), axis, angle);
                point = Vector3Add(Vector3Multiply(point, rescale), origin);
            }
            model->quads[index].positions[vertex] =
                Vector3Scale(Vector3SubtractValue(point, 8), 1.0f/16);
            model->quads[index].uv[vertex] = corners_uv[(vertex + (unsigned)(turn/90)) % 4];
        }
    }
    return true;
}

static bool read_gui(ModelReader *reader, const Json *json, int gui)
{
    if (json->tokens[gui].type != JSMN_OBJECT) return false;
    MinecraftModel *model = reader->model;
    int rotation = field(json, gui, "rotation");
    int translation = field(json, gui, "translation");
    int scale = field(json, gui, "scale");
    return (rotation < 0 || vector(json, rotation, &model->gui_rotation)) &&
           (translation < 0 || vector(json, translation, &model->gui_translation)) &&
           (scale < 0 || vector(json, scale, &model->gui_scale));
}

static bool read_definition(ModelReader *reader, const ResourcePack *pack,
                            const char *name, unsigned depth)
{
    const char *reference = minecraft_path(name);
    if (!reference || strlen(reference) >= MINECRAFT_MODEL_PATH || depth == MODEL_PARENTS)
        return false;
    if (!strcmp(reference, "builtin/generated")) {
        reader->model->generated = true;
        return true;
    }
    char path[MINECRAFT_MODEL_PATH + sizeof("models/.json")];
    snprintf(path, sizeof(path), "models/%s.json", reference);
    Json json = {0};
    if (!json_read(pack, path, &json)) return false;

    bool ok = read_textures(reader, &json, field(&json, 0, "textures"));
    int gui = field(&json, field(&json, 0, "display"), "gui");
    if (ok && gui >= 0 && !reader->has_gui) {
        ok = read_gui(reader, &json, gui);
        reader->has_gui = true;
    }
    int elements = field(&json, 0, "elements");
    if (ok && elements >= 0 && !reader->has_elements) {
        ok = json.tokens[elements].type == JSMN_ARRAY;
        reader->has_elements = true;
        int end = next_token(&json, elements);
        for (int element = elements + 1; ok && element < end; element = next_token(&json, element))
            ok = read_element(reader, &json, element);
    }
    int parent = field(&json, 0, "parent");
    char parent_name[MINECRAFT_MODEL_PATH];
    if (parent >= 0) ok = ok && string(&json, parent, parent_name);
    json_free(&json);
    return ok && (parent < 0 || read_definition(reader, pack, parent_name, depth + 1));
}

bool minecraft_model_read(const ResourcePack *pack, const char *name, MinecraftModel *model)
{
    if (!pack) return false;
    *model = (MinecraftModel){.gui_scale = {1,1,1}};
    ModelReader reader = {.model = model};
    if (!read_definition(&reader, pack, name, 0)) return false;
    if (model->generated) return texture_index(&reader, "#layer0") == 0;
    if (!model->quad_count) return false;
    for (unsigned face = 0; face < model->quad_count; ++face) {
        int index = texture_index(&reader, reader.references[face]);
        if (index < 0) return false;
        model->quads[face].texture = (unsigned)index;
    }
    return true;
}

bool minecraft_variants_read(const ResourcePack *pack, const char *block,
                             const char *const *keys, MinecraftVariant *variants, unsigned count)
{
    char path[MINECRAFT_MODEL_PATH];
    snprintf(path, sizeof(path), "blockstates/%s.json", block);
    Json json = {0};
    if (!json_read(pack, path, &json)) return false;
    int table = field(&json, 0, "variants");
    bool ok = true;
    for (unsigned i = 0; ok && i < count; ++i) {
        int variant = field(&json, table, keys[i]);
        int x = field(&json, variant, "x"), y = field(&json, variant, "y");
        variants[i] = (MinecraftVariant){0};
        ok = string(&json, field(&json, variant, "model"), variants[i].model) &&
             (x < 0 || number(&json, x, &variants[i].x)) &&
             (y < 0 || number(&json, y, &variants[i].y)) &&
             !token_equals(&json, field(&json, variant, "uvlock"), "true");
    }
    json_free(&json);
    return ok;
}
