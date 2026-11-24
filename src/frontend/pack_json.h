#ifndef VOXILOG_PACK_JSON_H
#define VOXILOG_PACK_JSON_H

#include "resource_pack.h"

#define JSMN_HEADER
#include "../../third_party/jsmn/jsmn.h"
#undef JSMN_HEADER

typedef struct {
    char *text;
    jsmntok_t *tokens;
    int count;
} PackJson;

bool pack_json_read(const ResourcePack *pack, const char *path, PackJson *json);
void pack_json_free(PackJson *json);
int pack_json_next(const PackJson *json, int token);
bool pack_json_equals(const PackJson *json, int token, const char *value);
int pack_json_field(const PackJson *json, int object, const char *key);

#endif
