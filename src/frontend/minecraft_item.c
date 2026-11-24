#include "minecraft_model.h"
#include "raymath.h"

#include <stdlib.h>
#include <stdio.h>

enum { ICON_SIZE = 64 };

typedef struct {
    Vector2 points[4];
    Vector2 uv[4];
    float depth;
    Color light;
    unsigned texture;
} IconFace;

static Vector3 gui_point(const MinecraftModel *model, Vector3 point)
{
    const Vector3 rotation = Vector3Scale(model->gui_rotation, DEG2RAD);
    point = Vector3Multiply(point, model->gui_scale);
    point = Vector3RotateByAxisAngle(point, (Vector3){0,0,1}, rotation.z);
    point = Vector3RotateByAxisAngle(point, (Vector3){0,1,0}, rotation.y);
    point = Vector3RotateByAxisAngle(point, (Vector3){1,0,0}, rotation.x);
    return Vector3Add(point, Vector3Scale(model->gui_translation, 1.0f/16));
}

static IconFace project_face(const MinecraftModel *model, const MinecraftQuad *quad)
{
    IconFace face = {.texture = quad->texture};
    Vector3 p[4];
    for (int i = 0; i < 4; ++i) {
        p[i] = gui_point(model, quad->positions[i]);
        face.points[i] = (Vector2){(0.5f + p[i].x)*ICON_SIZE, (0.5f - p[i].y)*ICON_SIZE};
        face.uv[i] = quad->uv[i];
        face.depth += p[i].z*0.25f;
    }
    Vector3 normal = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(p[1], p[0]),
                                                          Vector3Subtract(p[3], p[0])));
    const Vector3 upper_left = {-0.4f, 0.8f, 0.44f};
    const Vector3 fill = {0.45f, 0.6f, -0.44f};
    float brightness = 0.52f + 0.35f*fmaxf(0, Vector3DotProduct(normal, upper_left)) +
                       0.19f*fmaxf(0, Vector3DotProduct(normal, fill));
    unsigned char shade = (unsigned char)(255*fminf(brightness, 1));
    face.light = (Color){shade, shade, shade, 255};
    return face;
}

static int compare_depth(const void *a, const void *b)
{
    float left = ((const IconFace *)a)->depth, right = ((const IconFace *)b)->depth;
    return (left > right) - (left < right);
}

static void paint_face(Image icon, Image texture, const IconFace *face)
{
    Vector2 right = Vector2Subtract(face->points[1], face->points[0]);
    Vector2 down = Vector2Subtract(face->points[3], face->points[0]);
    float det = right.x*down.y - right.y*down.x;
    if (fabsf(det) < 0.0001f) return;
    Vector2 uv_right = Vector2Subtract(face->uv[1], face->uv[0]);
    Vector2 uv_down = Vector2Subtract(face->uv[3], face->uv[0]);
    Color *pixels = icon.data;
    const Color *source = texture.data;
    for (int y = 0; y < icon.height; ++y) {
        for (int x = 0; x < icon.width; ++x) {
            float dx = x + 0.5f - face->points[0].x;
            float dy = y + 0.5f - face->points[0].y;
            float s = (dx*down.y - dy*down.x)/det;
            float t = (right.x*dy - right.y*dx)/det;
            if (s < 0 || t < 0 || s >= 1 || t >= 1) continue;
            float u = face->uv[0].x + s*uv_right.x + t*uv_down.x;
            float v = face->uv[0].y + s*uv_right.y + t*uv_down.y;
            int tx = (int)Clamp(u*texture.width/16, 0, texture.width - 1);
            int ty = (int)Clamp(v*texture.height/16, 0, texture.height - 1);
            Color texel = source[ty*texture.width + tx];
            Color *pixel = &pixels[y*icon.width + x];
            if (texel.a) *pixel = ColorAlphaBlend(*pixel, texel, face->light);
        }
    }
}

Image minecraft_item_image(const ResourcePack *pack, const char *name)
{
    if (!pack) return (Image){0};
    char path[MINECRAFT_MODEL_PATH];
    int length = snprintf(path, sizeof(path), "item/%s", name);
    MinecraftModel model;
    if (length < 0 || (size_t)length >= sizeof(path) ||
        !minecraft_model_read(pack, path, &model)) return (Image){0};
    if (model.generated) return minecraft_texture(pack, model.textures[0]);

    Image icon = {0}, textures[MINECRAFT_MODEL_TEXTURES] = {0};
    for (unsigned i = 0; i < model.texture_count; ++i) {
        textures[i] = minecraft_texture(pack, model.textures[i]);
        if (!textures[i].data) goto cleanup;
        ImageFormat(&textures[i], PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (!textures[i].data || textures[i].format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            goto cleanup;
    }
    icon = GenImageColor(ICON_SIZE, ICON_SIZE, BLANK);
    if (icon.data) {
        IconFace faces[MINECRAFT_MODEL_QUADS];
        for (unsigned i = 0; i < model.quad_count; ++i)
            faces[i] = project_face(&model, &model.quads[i]);
        qsort(faces, model.quad_count, sizeof(*faces), compare_depth);
        for (unsigned i = 0; i < model.quad_count; ++i)
            paint_face(icon, textures[faces[i].texture], &faces[i]);
    }
cleanup:
    for (unsigned i = 0; i < model.texture_count; ++i)
        if (textures[i].data) UnloadImage(textures[i]);
    return icon;
}
