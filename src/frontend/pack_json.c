#define JSMN_STATIC
#define JSMN_STRICT
#include "../../third_party/jsmn/jsmn.h"
#include "pack_json.h"

#include <stdlib.h>
#include <string.h>

enum { JSON_TOKENS = 8192 };

void pack_json_free(PackJson *json)
{
    free(json->text);
    free(json->tokens);
    *json = (PackJson){0};
}

bool pack_json_read(const ResourcePack *pack, const char *path, PackJson *json)
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
    pack_json_free(json);
    return false;
}

int pack_json_next(const PackJson *json, int token)
{
    int end = json->tokens[token].end;
    while (++token < json->count && json->tokens[token].start < end) {}
    return token;
}

bool pack_json_equals(const PackJson *json, int token, const char *value)
{
    if (token < 0 || token >= json->count) return false;
    jsmntok_t t = json->tokens[token];
    size_t size = (size_t)(t.end - t.start);
    return strlen(value) == size && !memcmp(json->text + t.start, value, size);
}

int pack_json_field(const PackJson *json, int object, const char *key)
{
    if (object < 0 || json->tokens[object].type != JSMN_OBJECT) return -1;
    int end = pack_json_next(json, object);
    for (int token = object + 1; token + 1 < end; token = pack_json_next(json, token + 1))
        if (json->tokens[token].type == JSMN_STRING && pack_json_equals(json, token, key))
            return token + 1;
    return -1;
}
