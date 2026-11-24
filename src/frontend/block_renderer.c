#include "block_renderer.h"
#include "blocks/concrete.h"
#include "minecraft_redstone.h"
#include "rlgl.h"
#include "external/glad.h"
#include "hopper_shape.h"

#include <stddef.h>
#include <string.h>

enum {
    TILE_SIZE = 32,
    OBSERVER_TEXTURES = 4,
    OBSERVER_STATES = 2,
    OBSERVER_TILE_FIRST = 1,
    BARREL_TILE_FIRST = OBSERVER_TILE_FIRST + OBSERVER_TEXTURES*OBSERVER_STATES,
    BARREL_TEXTURES = 4,
    SLIME_TILE = BARREL_TILE_FIRST + BARREL_TEXTURES,
    NOTE_BLOCK_TILE = SLIME_TILE + 1,
    HONEY_TILE = NOTE_BLOCK_TILE + 1,
    TARGET_SIDE_TILE = HONEY_TILE + 1,
    TARGET_TOP_TILE = TARGET_SIDE_TILE + 1,
    HOPPER_OUTSIDE_TILE = TARGET_TOP_TILE + 1,
    HOPPER_INSIDE_TILE = HOPPER_OUTSIDE_TILE + 1,
    DROPPER_FRONT_TILE = HOPPER_INSIDE_TILE + 1,
    DROPPER_VERTICAL_FRONT_TILE = DROPPER_FRONT_TILE + 1,
    DROPPER_TOP_TILE = DROPPER_VERTICAL_FRONT_TILE + 1,
    DROPPER_SIDE_TILE = DROPPER_TOP_TILE + 1,
    LAMP_OFF_TILE = DROPPER_SIDE_TILE + 1,
    LAMP_ON_TILE = LAMP_OFF_TILE + 1,
    CONCRETE_TILE_FIRST = LAMP_ON_TILE + 1,
    TILE_COUNT = CONCRETE_TILE_FIRST + COLORED_CONCRETE_COUNT
};
enum { OBSERVER_BACK, OBSERVER_FRONT, OBSERVER_SIDE, OBSERVER_TOP };
enum { BARREL_SIDE, BARREL_TOP, BARREL_TOP_OPEN, BARREL_BOTTOM };
enum {
    WIRE_SHAPES = 16, WIRE_STATES = 16,
    WIRE_SIDE_FIRST = WIRE_SHAPES*WIRE_STATES,
    WIRE_TILES = WIRE_SIDE_FIRST + WIRE_STATES,
    WIRE_COLUMNS = 16,
    WIRE_ROWS = (WIRE_TILES + WIRE_COLUMNS - 1)/WIRE_COLUMNS
};
enum { ATLAS_STATES = 2 };

static const float wire_offset = 0.002f;
static const float rail_offset = 0.003f;
static const Direction wire_directions[] = {
    DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH
};

/* Match raylib's default desktop vertex shader; only binary-alpha pixels
 * survive, so transparent texels cannot write depth or cover other blocks. */
static const char cutout_fragment_shader[] =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 texel = texture(texture0, fragTexCoord);\n"
    "    if (texel.a < 0.5) discard;\n"
    "    finalColor = texel*fragColor*colDiffuse;\n"
    "}\n";

typedef struct {
    Vector3 normal;
    Vector3 vertices[4];
} CubeFace;

/* Counterclockwise vertices as seen from outside each face. */
static const CubeFace cube_faces[] = {
    {{ 0,  0,  1}, {{-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f},
                    { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f}}},
    {{ 0,  0, -1}, {{ 0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
                    {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}}},
    {{ 1,  0,  0}, {{ 0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f, -0.5f},
                    { 0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f}}},
    {{-1,  0,  0}, {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f,  0.5f},
                    {-0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f, -0.5f}}},
    {{ 0,  1,  0}, {{-0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f},
                    { 0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f}}},
    {{ 0, -1,  0}, {{-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f},
                    { 0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f,  0.5f}}}
};

/* Vanilla torch model: four full-texture side sheets and two caps. */
static const CubeFace torch_faces[] = {
    {{ 0,  0,  1}, {{-0.5f, 0,  0.0625f}, { 0.5f, 0,  0.0625f},
                    { 0.5f, 1,  0.0625f}, {-0.5f, 1,  0.0625f}}},
    {{ 0,  0, -1}, {{ 0.5f, 0, -0.0625f}, {-0.5f, 0, -0.0625f},
                    {-0.5f, 1, -0.0625f}, { 0.5f, 1, -0.0625f}}},
    {{ 1,  0,  0}, {{ 0.0625f, 0,  0.5f}, { 0.0625f, 0, -0.5f},
                    { 0.0625f, 1, -0.5f}, { 0.0625f, 1,  0.5f}}},
    {{-1,  0,  0}, {{-0.0625f, 0, -0.5f}, {-0.0625f, 0,  0.5f},
                    {-0.0625f, 1,  0.5f}, {-0.0625f, 1, -0.5f}}},
    {{ 0,  1,  0}, {{-0.0625f, 0.625f,  0.0625f}, { 0.0625f, 0.625f,  0.0625f},
                    { 0.0625f, 0.625f, -0.0625f}, {-0.0625f, 0.625f, -0.0625f}}},
    {{ 0, -1,  0}, {{-0.0625f, 0, -0.0625f}, { 0.0625f, 0, -0.0625f},
                    { 0.0625f, 0,  0.0625f}, {-0.0625f, 0,  0.0625f}}}
};

typedef struct {
    Vector3 origin;
    Vector3 x_axis, y_axis, z_axis;
} TorchTransform;

static Image load_pack_tile(const ResourcePack *pack, const char *path)
{
    Image image = resource_pack_image(pack, path);
    if (!image.data) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot read %s from the resource pack", path);
        return image;
    }
    /* Animated block PNGs stack square frames vertically. Use the first frame. */
    if (image.height > image.width && image.height % image.width == 0)
        ImageCrop(&image, (Rectangle){0, 0, image.width, image.width});
    if (image.width != image.height) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: %s must have square frames", path);
        UnloadImage(image);
        return (Image){0};
    }
    if (image.width != TILE_SIZE) ImageResizeNN(&image, TILE_SIZE, TILE_SIZE);
    return image;
}

static Image create_solid_atlas(const ResourcePack *pack)
{
    static const struct {
        const char *path;
        int quarter_turn;
    } tiles[] = {
        {"block/redstone_block.png", 0},
        {"block/observer_back.png", 0},
        {"block/observer_front.png", 0},
        {"block/observer_side.png", 1},
        {"block/observer_top.png", 0},
        {"block/observer_back_on.png", 0},
        {"block/observer_front.png", 0},
        {"block/observer_side.png", 1},
        {"block/observer_top.png", 0},
        {"block/barrel_side.png", -1},
        {"block/barrel_top.png", 0},
        {"block/barrel_top_open.png", 0},
        {"block/barrel_bottom.png", 0},
        {"block/slime_block.png", 0},
        {"block/note_block.png", 0},
        {"block/honey_block_side.png", 0},
        {"block/target_side.png", 0},
        {"block/target_top.png", 0},
        {"block/hopper_outside.png", 0},
        {"block/hopper_inside.png", 0},
        {"block/dropper_front.png", 0},
        {"block/dropper_front_vertical.png", 0},
        {"block/furnace_top.png", 0},
        {"block/furnace_side.png", 0},
        {"block/redstone_lamp.png", 0},
        {"block/redstone_lamp_on.png", 0},
        /* Colored concrete tiles follow colored_concrete_index(). */
        {"block/blue_concrete.png", 0},
        {"block/gray_concrete.png", 0},
        {"block/green_concrete.png", 0},
        {"block/purple_concrete.png", 0},
        {"block/yellow_concrete.png", 0},
        {"block/black_concrete.png", 0},
        {"block/brown_concrete.png", 0},
        {"block/cyan_concrete.png", 0},
        {"block/light_blue_concrete.png", 0},
        {"block/light_gray_concrete.png", 0},
        {"block/lime_concrete.png", 0},
        {"block/magenta_concrete.png", 0},
        {"block/orange_concrete.png", 0},
        {"block/pink_concrete.png", 0},
        {"block/red_concrete.png", 0},
    };
    _Static_assert(sizeof(tiles)/sizeof(*tiles) == TILE_COUNT, "solid atlas tile count");
    Image sources[TILE_COUNT] = {0};
    Image atlas = {0};
    Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};

    for (int tile = 0; tile < TILE_COUNT; ++tile) {
        sources[tile] = load_pack_tile(pack, tiles[tile].path);
        if (!sources[tile].data) goto cleanup;
        if (tiles[tile].quarter_turn > 0) ImageRotateCW(&sources[tile]);
        if (tiles[tile].quarter_turn < 0) ImageRotateCCW(&sources[tile]);
        /* Keep the pack's RGB colors while drawing these cubes as opaque. */
        if (tile == SLIME_TILE || tile == HONEY_TILE)
            ImageFormat(&sources[tile], PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    }
    atlas = GenImageColor(TILE_COUNT*TILE_SIZE, TILE_SIZE, BLANK);
    if (atlas.data)
        for (int tile = 0; tile < TILE_COUNT; ++tile) {
            Rectangle destination = {
                tile*TILE_SIZE, 0, TILE_SIZE, TILE_SIZE
            };
            ImageDraw(&atlas, sources[tile], full, destination, WHITE);
        }

cleanup:
    for (int tile = 0; tile < TILE_COUNT; ++tile)
        if (sources[tile].data) UnloadImage(sources[tile]);
    return atlas;
}

static Rectangle wire_tile_rectangle(int tile)
{
    return (Rectangle){(tile%WIRE_COLUMNS)*TILE_SIZE,
                       (tile/WIRE_COLUMNS)*TILE_SIZE, TILE_SIZE, TILE_SIZE};
}

static Color wire_color(int power)
{
    /* RedStoneWireBlock's color curve. Power changes need no per-frame image
     * processing: all sixteen levels are baked into the atlas once. */
    float strength = power/15.0f;
    float red = strength*0.6f + (power ? 0.4f : 0.3f);
    float green = strength*strength*0.7f - 0.5f;
    float blue = strength*strength*0.6f - 0.7f;
    if (green < 0) green = 0;
    if (blue < 0) blue = 0;
    return (Color){(unsigned char)(red*255), (unsigned char)(green*255),
                   (unsigned char)(blue*255), 255};
}

static Image create_wire_atlas(const ResourcePack *pack)
{
    /* The source arm points down in image space (+Z / south in the world).
     * CW turns map south -> west -> north -> east. */
    static const int clockwise_turns[] = {3, 1, 0, 2};
    Image arms[4] = {0};
    Image dot = load_pack_tile(pack, "block/redstone_dust_dot.png");
    Image line = load_pack_tile(pack, "block/redstone_dust_line0.png");
    Image atlas = {0};
    Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};
    Rectangle half = {0, TILE_SIZE/2, TILE_SIZE, TILE_SIZE/2};

    if (!dot.data || !line.data) goto cleanup;
    for (int direction = 0; direction < 4; ++direction) {
        arms[direction] = GenImageColor(TILE_SIZE, TILE_SIZE, BLANK);
        if (!arms[direction].data) goto cleanup;
        ImageDraw(&arms[direction], line, half, half, WHITE);
        for (int turn = 0; turn < clockwise_turns[direction]; ++turn)
            ImageRotateCW(&arms[direction]);
    }
    atlas = GenImageColor(WIRE_COLUMNS*TILE_SIZE, WIRE_ROWS*TILE_SIZE, BLANK);
    if (!atlas.data) goto cleanup;

    for (int power = 0; power < WIRE_STATES; ++power) {
        Color tint = wire_color(power);
        for (int mask = 0; mask < WIRE_SHAPES; ++mask) {
            Rectangle destination = wire_tile_rectangle(power*WIRE_SHAPES + mask);
            for (int direction = 0; direction < 4; ++direction)
                if (mask & (1 << direction))
                    ImageDraw(&atlas, arms[direction], full, destination, tint);
            /* Opposite pairs form straight lines without a center dot. */
            if (mask != 3 && mask != 12)
                ImageDraw(&atlas, dot, full, destination, tint);
        }
        Rectangle side = wire_tile_rectangle(WIRE_SIDE_FIRST + power);
        ImageDraw(&atlas, line, full, side, tint);
    }

cleanup:
    if (dot.data) UnloadImage(dot);
    if (line.data) UnloadImage(line);
    for (int direction = 0; direction < 4; ++direction)
        if (arms[direction].data) UnloadImage(arms[direction]);
    return atlas;
}

static Image create_two_state_atlas(const ResourcePack *pack,
                                    const char *off_path, const char *on_path)
{
    const char *const paths[ATLAS_STATES] = {off_path, on_path};
    Image sources[ATLAS_STATES] = {0};
    Image atlas = {0};
    for (int state = 0; state < ATLAS_STATES; ++state) {
        sources[state] = load_pack_tile(pack, paths[state]);
        if (!sources[state].data) goto cleanup;
    }

    atlas = GenImageColor(ATLAS_STATES*TILE_SIZE, TILE_SIZE, BLANK);
    if (atlas.data) {
        Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};
        for (int state = 0; state < ATLAS_STATES; ++state) {
            Rectangle destination = {state*TILE_SIZE, 0, TILE_SIZE, TILE_SIZE};
            ImageDraw(&atlas, sources[state], full, destination, WHITE);
        }
    }

cleanup:
    for (int state = 0; state < ATLAS_STATES; ++state)
        if (sources[state].data) UnloadImage(sources[state]);
    return atlas;
}

bool block_renderer_init(BlockRenderer *renderer, const ResourcePack *pack)
{
    if (!renderer) return false;
    *renderer = (BlockRenderer){0};

    Image atlas = create_solid_atlas(pack);
    if (atlas.data) {
        renderer->atlas = LoadTextureFromImage(atlas);
        UnloadImage(atlas);
    }

    if (!renderer->atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the block atlas");
        return false;
    }

    SetTextureFilter(renderer->atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->atlas, TEXTURE_WRAP_CLAMP);

    Image iron = load_pack_tile(pack, "block/white_concrete.png");
    if (iron.data) {
        renderer->iron_texture = LoadTextureFromImage(iron);
        UnloadImage(iron);
    }
    if (!renderer->iron_texture.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot load the white concrete texture");
        block_renderer_unload(renderer);
        return false;
    }
    /* A separate texture lets small mip levels average just the iron tile.
     * Keep pixel edges sharp when the tile is magnified. */
    SetTextureWrap(renderer->iron_texture, TEXTURE_WRAP_CLAMP);
    GenTextureMipmaps(&renderer->iron_texture);
    SetTextureFilter(renderer->iron_texture, TEXTURE_FILTER_TRILINEAR);
    rlTextureParameters(renderer->iron_texture.id, RL_TEXTURE_MAG_FILTER,
                        RL_TEXTURE_FILTER_NEAREST);
    if (renderer->iron_texture.mipmaps > 1 &&
        (GLAD_GL_EXT_texture_filter_anisotropic || GLAD_GL_ARB_texture_filter_anisotropic)) {
        float maximum = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maximum);
        if (maximum >= 2.0f)
            rlTextureParameters(renderer->iron_texture.id, RL_TEXTURE_FILTER_ANISOTROPIC,
                                (int)(maximum < 8.0f ? maximum : 8.0f));
    }

    Image glass = load_pack_tile(pack, "block/glass.png");
    if (glass.data) {
        renderer->glass_texture = LoadTextureFromImage(glass);
        UnloadImage(glass);
    }
    if (!renderer->glass_texture.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot load the glass texture");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->glass_texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->glass_texture, TEXTURE_WRAP_CLAMP);

    Image wire_atlas = create_wire_atlas(pack);
    if (wire_atlas.data) {
        renderer->wire_atlas = LoadTextureFromImage(wire_atlas);
        UnloadImage(wire_atlas);
    }
    if (!renderer->wire_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the redstone dust atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->wire_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->wire_atlas, TEXTURE_WRAP_CLAMP);

    Image torch_atlas = create_two_state_atlas(
        pack, "block/redstone_torch_off.png", "block/redstone_torch.png");
    if (torch_atlas.data) {
        renderer->torch_atlas = LoadTextureFromImage(torch_atlas);
        UnloadImage(torch_atlas);
    }
    if (!renderer->torch_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the redstone torch atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->torch_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->torch_atlas, TEXTURE_WRAP_CLAMP);

    Image powered_rail_atlas = create_two_state_atlas(
        pack,
        "block/powered_rail.png", "block/powered_rail_on.png");
    if (powered_rail_atlas.data) {
        renderer->powered_rail_atlas = LoadTextureFromImage(powered_rail_atlas);
        UnloadImage(powered_rail_atlas);
    }
    if (!renderer->powered_rail_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the powered rail atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->powered_rail_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->powered_rail_atlas, TEXTURE_WRAP_CLAMP);

    Image activator_rail_atlas = create_two_state_atlas(
        pack,
        "block/activator_rail.png", "block/activator_rail_on.png");
    if (activator_rail_atlas.data) {
        renderer->activator_rail_atlas = LoadTextureFromImage(activator_rail_atlas);
        UnloadImage(activator_rail_atlas);
    }
    if (!renderer->activator_rail_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the activator rail atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->activator_rail_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->activator_rail_atlas, TEXTURE_WRAP_CLAMP);

    renderer->cutout_shader = LoadShaderFromMemory(NULL, cutout_fragment_shader);
    if (renderer->cutout_shader.id == rlGetShaderIdDefault()) {
        /* LoadShaderFromMemory allocates its own locations even on fallback. */
        MemFree(renderer->cutout_shader.locs);
        renderer->cutout_shader = (Shader){0};
    }
    if (!renderer->cutout_shader.id || !renderer->cutout_shader.locs) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot load the cutout shader (OpenGL 3.3)");
        block_renderer_unload(renderer);
        return false;
    }
    Image minecraft_atlas;
    renderer->minecraft = minecraft_redstone_load(pack, &minecraft_atlas);
    if (minecraft_atlas.data) {
        renderer->minecraft_atlas = LoadTextureFromImage(minecraft_atlas);
        UnloadImage(minecraft_atlas);
    }
    if (!renderer->minecraft_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the redstone model atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->minecraft_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->minecraft_atlas, TEXTURE_WRAP_CLAMP);
    return true;
}

void block_renderer_unload(BlockRenderer *renderer)
{
    if (!renderer) return;
    minecraft_redstone_free(renderer->minecraft);
    if (renderer->minecraft_atlas.id) UnloadTexture(renderer->minecraft_atlas);
    if (renderer->cutout_shader.id) UnloadShader(renderer->cutout_shader);
    if (renderer->activator_rail_atlas.id)
        UnloadTexture(renderer->activator_rail_atlas);
    if (renderer->powered_rail_atlas.id)
        UnloadTexture(renderer->powered_rail_atlas);
    if (renderer->torch_atlas.id) UnloadTexture(renderer->torch_atlas);
    if (renderer->wire_atlas.id) UnloadTexture(renderer->wire_atlas);
    if (renderer->glass_texture.id) UnloadTexture(renderer->glass_texture);
    if (renderer->iron_texture.id) UnloadTexture(renderer->iron_texture);
    if (renderer->atlas.id) UnloadTexture(renderer->atlas);
    *renderer = (BlockRenderer){0};
}

static Texture2D cube_texture(const BlockRenderer *renderer, BlockId id)
{
    if (id == BLOCK_IRON) return renderer->iron_texture;
    if (id == BLOCK_GLASS) return renderer->glass_texture;
    return renderer->atlas;
}

static void wire_uv(Texture2D atlas, int tile, Vector2 uv[4])
{
    Rectangle rectangle = wire_tile_rectangle(tile);
    float u0 = (rectangle.x + 0.5f)/atlas.width;
    float u1 = (rectangle.x + TILE_SIZE - 0.5f)/atlas.width;
    float v0 = (rectangle.y + 0.5f)/atlas.height;
    float v1 = (rectangle.y + TILE_SIZE - 0.5f)/atlas.height;
    uv[0] = (Vector2){u0, v1};
    uv[1] = (Vector2){u1, v1};
    uv[2] = (Vector2){u1, v0};
    uv[3] = (Vector2){u0, v0};
}

static int direction_turns(Direction right, Direction down,
                           Direction desired, bool source_points_down)
{
    if (source_points_down) {
        if (desired == down) return 0;
        if (desired == direction_opposite(right)) return 1;
        if (desired == direction_opposite(down)) return 2;
        if (desired == right) return 3;
    } else {
        if (desired == right) return 0;
        if (desired == down) return 1;
        if (desired == direction_opposite(right)) return 2;
        if (desired == direction_opposite(down)) return 3;
    }
    return 0;
}

static void solid_tile_uv(Texture2D atlas, int tile, int turns, Vector2 uv[4])
{
    float u0 = (tile*TILE_SIZE + 0.5f)/atlas.width;
    float u1 = ((tile + 1)*TILE_SIZE - 0.5f)/atlas.width;
    float v0 = 0.5f/atlas.height;
    float v1 = (TILE_SIZE - 0.5f)/atlas.height;
    const Vector2 base[4] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};
    for (int vertex = 0; vertex < 4; ++vertex)
        uv[vertex] = base[(vertex + turns) & 3];
}

static void observer_face_uv(Texture2D atlas, BlockState state, int face,
                             Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction output = (Direction)state.data.observer.facing;
    Direction face_direction = directions[face];
    int texture;
    Direction desired;
    bool points_down;

    if (face_direction == output) {
        texture = OBSERVER_BACK;
        desired = output == DIR_UP ? DIR_NORTH :
                  output == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
        points_down = true;
    } else if (face_direction == direction_opposite(output)) {
        texture = OBSERVER_FRONT;
        desired = output == DIR_UP ? DIR_NORTH :
                  output == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
        points_down = true;
    } else {
        bool top_face = direction_horizontal(output) ?
                        (face_direction == DIR_UP || face_direction == DIR_DOWN) :
                        (face_direction == DIR_NORTH || face_direction == DIR_SOUTH);
        texture = top_face ? OBSERVER_TOP : OBSERVER_SIDE;
        desired = top_face ? direction_opposite(output) : output;
        points_down = top_face;
    }

    int powered = state.data.observer.powered ? 1 : 0;
    int tile = OBSERVER_TILE_FIRST + powered*OBSERVER_TEXTURES + texture;
    solid_tile_uv(atlas, tile,
                  direction_turns(right[face], down[face], desired,
                                  points_down),
                  uv);
}

static void barrel_face_uv(Texture2D atlas, BlockState state, int face,
                           Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction facing = (Direction)state.data.barrel.facing;
    if ((unsigned)facing >= DIR_COUNT) facing = DIR_NORTH;
    Direction face_direction = directions[face];
    int texture;
    Direction desired;
    bool source_points_down = true;

    if (face_direction == facing) {
        texture = state.data.barrel.open ? BARREL_TOP_OPEN : BARREL_TOP;
        desired = facing == DIR_UP ? DIR_NORTH :
                  facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
    } else if (face_direction == direction_opposite(facing)) {
        texture = BARREL_BOTTOM;
        desired = facing == DIR_UP ? DIR_NORTH :
                  facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
    } else {
        texture = BARREL_SIDE;
        /* The source image's horizontal axis follows the barrel axis. */
        desired = direction_opposite(facing);
        source_points_down = false;
    }

    solid_tile_uv(atlas, BARREL_TILE_FIRST + texture,
                  direction_turns(right[face], down[face], desired,
                                  source_points_down),
                  uv);
}

static void dropper_face_uv(Texture2D atlas, BlockState state, int face,
                            Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    Direction facing = (Direction)state.data.dropper.facing;
    bool vertical = facing == DIR_UP || facing == DIR_DOWN;
    bool front = directions[face] == facing;
    int tile;
    if (front) tile = vertical ? DROPPER_VERTICAL_FRONT_TILE : DROPPER_FRONT_TILE;
    else if (vertical || face >= 4) tile = DROPPER_TOP_TILE;
    else tile = DROPPER_SIDE_TILE;
    solid_tile_uv(atlas, tile, 0, uv);
}

static TorchTransform torch_transform(Vector3 center, Direction support)
{
    TorchTransform transform = {
        {center.x, center.y - 0.5f, center.z},
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}
    };
    /* Facing is away from the support, whose direction points toward it. */
    Vector3 facing;
    switch (support) {
    case DIR_EAST:  facing = (Vector3){-1, 0,  0}; break;
    case DIR_WEST:  facing = (Vector3){ 1, 0,  0}; break;
    case DIR_SOUTH: facing = (Vector3){ 0, 0, -1}; break;
    case DIR_NORTH: facing = (Vector3){ 0, 0,  1}; break;
    default: return transform;
    }

    /* Vanilla's wall mount is 3.5/16 above the cell bottom, tilted 22.5
     * degrees outward. The stem's bottom intersects the supporting face. */
    const float sine = 0.3826834324f, cosine = 0.9238795325f;
    transform.origin.x -= facing.x*0.5f;
    transform.origin.y += 3.5f/16.0f;
    transform.origin.z -= facing.z*0.5f;
    transform.x_axis = (Vector3){facing.x*cosine, -sine, facing.z*cosine};
    transform.y_axis = (Vector3){facing.x*sine, cosine, facing.z*sine};
    transform.z_axis = (Vector3){-facing.z, 0, facing.x};
    return transform;
}

static Vector3 torch_rotate(TorchTransform transform, Vector3 vector)
{
    return (Vector3){
        transform.x_axis.x*vector.x + transform.y_axis.x*vector.y + transform.z_axis.x*vector.z,
        transform.x_axis.y*vector.x + transform.y_axis.y*vector.y + transform.z_axis.y*vector.z,
        transform.x_axis.z*vector.x + transform.y_axis.z*vector.y + transform.z_axis.z*vector.z
    };
}

static void torch_uv(Texture2D atlas, int lit, Rectangle source, Vector2 uv[4])
{
    /* Use texel boundaries so the four-pixel stem meets its caps exactly.
     * Point filtering preserves pixels; tile sides are transparent gutters. */
    float u0 = (lit*TILE_SIZE + source.x)/atlas.width;
    float u1 = (lit*TILE_SIZE + source.x + source.width)/atlas.width;
    float v0 = source.y/atlas.height;
    float v1 = (source.y + source.height)/atlas.height;
    uv[0] = (Vector2){u0, v1};
    uv[1] = (Vector2){u1, v1};
    uv[2] = (Vector2){u1, v0};
    uv[3] = (Vector2){u0, v0};
}

static Texture2D rail_texture(const BlockRenderer *renderer, BlockId id)
{
    return id == BLOCK_POWERED_RAIL ? renderer->powered_rail_atlas :
           renderer->activator_rail_atlas;
}

static void rail_uv(Texture2D atlas, BlockState state, Vector2 uv[4])
{
    Direction facing = (Direction)state.data.rail.facing;
    int turns = direction_turns(DIR_EAST, DIR_SOUTH, direction_opposite(facing), true);
    solid_tile_uv(atlas, state.data.rail.powered ? 1 : 0, turns, uv);
}

static bool rail_vertex_high(Direction facing, Vector3 point)
{
    switch (facing) {
    case DIR_EAST:  return point.x > 0.0f;
    case DIR_WEST:  return point.x < 0.0f;
    case DIR_SOUTH: return point.z > 0.0f;
    case DIR_NORTH: return point.z < 0.0f;
    default: return false;
    }
}

static void rail_geometry(BlockState state, Vector3 center,
                          Vector3 vertices[4])
{
    Direction facing = (Direction)state.data.rail.facing;
    float bottom = center.y - 0.5f + rail_offset;
    for (int vertex = 0; vertex < 4; ++vertex) {
        Vector3 point = cube_faces[4].vertices[vertex];
        float rise = state.data.rail.ascending &&
                     rail_vertex_high(facing, point) ? 1.0f : 0.0f;
        vertices[vertex] = (Vector3){
            center.x + point.x, bottom + rise, center.z + point.z
        };
    }
}

static int hopper_geometry(const BlockRenderer *renderer, BlockState state,
                            Vector3 center, BlockGeometry *geometry);

int block_renderer_build_uv(const BlockRenderer *renderer, BlockState state,
                            unsigned solid_faces,
                            Vector2 uv[BLOCK_GEOMETRY_QUADS][4])
{
    int count = 0;
    if (state.id == BLOCK_HOPPER) {
        BlockGeometry geometry;
        count = hopper_geometry(renderer, state, (Vector3){0}, &geometry);
        memcpy(uv, geometry.uv, (size_t)count * sizeof(geometry.uv[0]));
        return count;
    }
    int concrete = colored_concrete_index((BlockId)state.id);
    if (concrete >= 0 || state.id == BLOCK_IRON || state.id == BLOCK_REDSTONE_BLOCK ||
        state.id == BLOCK_GLASS || state.id == BLOCK_SLIME ||
        state.id == BLOCK_NOTE_BLOCK || state.id == BLOCK_HONEY ||
        state.id == BLOCK_REDSTONE_LAMP) {
        Texture2D texture = cube_texture(renderer, (BlockId)state.id);
        int tile = concrete >= 0 ? CONCRETE_TILE_FIRST + concrete :
                   state.id == BLOCK_SLIME ? SLIME_TILE :
                   state.id == BLOCK_NOTE_BLOCK ? NOTE_BLOCK_TILE :
                   state.id == BLOCK_HONEY ? HONEY_TILE :
                   state.id == BLOCK_REDSTONE_LAMP ?
                       (state.data.lamp.lit ? LAMP_ON_TILE : LAMP_OFF_TILE) : 0;
        float u0 = (tile*TILE_SIZE + 0.5f)/texture.width;
        float u1 = ((tile + 1)*TILE_SIZE - 0.5f)/texture.width;
        float v0 = 0.5f/texture.height;
        float v1 = (TILE_SIZE - 0.5f)/texture.height;
        Vector2 face_uv[4] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                memcpy(uv[count++], face_uv, sizeof(face_uv));
    } else if (state.id == BLOCK_TARGET) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                solid_tile_uv(renderer->atlas, face < 4 ? TARGET_SIDE_TILE : TARGET_TOP_TILE,
                              0, uv[count++]);
    } else if (state.id == BLOCK_OBSERVER) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                observer_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (state.id == BLOCK_DROPPER) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                dropper_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (state.id == BLOCK_BARREL) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                barrel_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (state.id == BLOCK_REDSTONE_WIRE) {
        int mask = 0;
        for (int direction = 0; direction < 4; ++direction)
            if (state.data.wire.connections & (1u << wire_directions[direction]))
                mask |= 1 << direction;
        int power = state.data.wire.power;
        wire_uv(renderer->wire_atlas, power*WIRE_SHAPES + mask, uv[count++]);
        for (int direction = 0; direction < 4; ++direction) {
            unsigned bit = 1u << wire_directions[direction];
            if (state.data.wire.up_connections & state.data.wire.connections & bit)
                wire_uv(renderer->wire_atlas, WIRE_SIDE_FIRST + power, uv[count++]);
        }
    } else if (state.id == BLOCK_REDSTONE_TORCH) {
        int lit = state.data.torch.lit ? 1 : 0;
        for (int face = 0; face < 6; ++face) {
            Rectangle source = {0,0,TILE_SIZE,TILE_SIZE};
            if (face == 4) source = (Rectangle){14,12,4,4};
            if (face == 5) source = (Rectangle){14,26,4,4};
            torch_uv(renderer->torch_atlas, lit, source, uv[count++]);
        }
    } else if (state.id == BLOCK_POWERED_RAIL ||
               state.id == BLOCK_ACTIVATOR_RAIL) {
        rail_uv(rail_texture(renderer, (BlockId)state.id), state, uv[0]);
        count = 1;
    }
    return count;
}

static void box_face_vertices(Vector3 center, Vector3 minimum,
                              Vector3 maximum, int face, Vector3 output[4])
{
    for (int vertex = 0; vertex < 4; ++vertex) {
        Vector3 point = cube_faces[face].vertices[vertex];
        output[vertex] = (Vector3){
            center.x + (point.x < 0.0f ? minimum.x : maximum.x),
            center.y + (point.y < 0.0f ? minimum.y : maximum.y),
            center.z + (point.z < 0.0f ? minimum.z : maximum.z)
        };
    }
}

static int hopper_geometry(const BlockRenderer *renderer, BlockState state,
                            Vector3 center, BlockGeometry *geometry)
{
    HopperBox boxes[HOPPER_BOX_COUNT];
    hopper_shape((Direction)state.data.hopper.facing, boxes);
    int count = 0;
    for (int b = 0; b < HOPPER_BOX_COUNT; ++b) {
        const HopperBox *box = &boxes[b];
        Vector3 minimum = {box->minimum[0]/16-0.5f, box->minimum[1]/16-0.5f, box->minimum[2]/16-0.5f};
        Vector3 maximum = {box->maximum[0]/16-0.5f, box->maximum[1]/16-0.5f, box->maximum[2]/16-0.5f};
        for (int face = 0; face < 6; ++face) {
            if (!(box->faces & (1u << face))) continue;
            box_face_vertices(center, minimum, maximum, face, geometry->vertices[count]);
            int tile = b == 0 && face == 4 ? HOPPER_INSIDE_TILE : HOPPER_OUTSIDE_TILE;
            for (int v = 0; v < 4; ++v) {
                Vector3 vertex = geometry->vertices[count][v];
                float x = vertex.x-center.x+0.5f;
                float y = vertex.y-center.y+0.5f;
                float z = vertex.z-center.z+0.5f;
                float u = face == 0 || face >= 4 ? x : face == 1 ? 1-x : face == 2 ? 1-z : z;
                float t = face == 4 ? z : face == 5 ? 1-z : 1-y;
                geometry->uv[count][v] = (Vector2){
                    (tile*TILE_SIZE+0.5f+u*(TILE_SIZE-1))/renderer->atlas.width,
                    (0.5f+t*(TILE_SIZE-1))/renderer->atlas.height};
            }
            ++count;
        }
    }
    geometry->quads = count;
    return count;
}

void block_renderer_build_geometry(const BlockRenderer *renderer, BlockState state,
                                   Vector3 center, unsigned solid_faces,
                                   BlockGeometry *geometry)
{
    const BlockGeometry *baked = minecraft_redstone_geometry(renderer->minecraft, state);
    if (baked) {
        geometry->quads = baked->quads;
        memcpy(geometry->uv, baked->uv, (size_t)baked->quads*sizeof(baked->uv[0]));
        for (int face = 0; face < baked->quads; ++face) {
            for (int v = 0; v < 4; ++v) {
                Vector3 p = baked->vertices[face][v];
                geometry->vertices[face][v] = (Vector3){p.x+center.x,p.y+center.y,p.z+center.z};
            }
        }
        return;
    }
    if (state.id == BLOCK_HOPPER) {
        (void)hopper_geometry(renderer, state, center, geometry);
        return;
    }
    geometry->quads = block_renderer_build_uv(renderer, state, solid_faces, geometry->uv);
    int count = 0;
    if (block_is_concrete((BlockId)state.id) || state.id == BLOCK_REDSTONE_BLOCK ||
        state.id == BLOCK_GLASS || state.id == BLOCK_OBSERVER ||
        state.id == BLOCK_BARREL || state.id == BLOCK_SLIME ||
        state.id == BLOCK_NOTE_BLOCK || state.id == BLOCK_HONEY ||
        state.id == BLOCK_TARGET || state.id == BLOCK_DROPPER ||
        state.id == BLOCK_REDSTONE_LAMP) {
        for (int face = 0; face < 6; ++face) {
            if (!(solid_faces & (1u << face))) continue;
            for (int vertex = 0; vertex < 4; ++vertex) {
                Vector3 p = cube_faces[face].vertices[vertex];
                geometry->vertices[count][vertex] = (Vector3){center.x+p.x,center.y+p.y,center.z+p.z};
            }
            ++count;
        }
    } else if (state.id == BLOCK_REDSTONE_WIRE) {
        for (int vertex = 0; vertex < 4; ++vertex) {
            Vector3 p = cube_faces[4].vertices[vertex];
            geometry->vertices[0][vertex] = (Vector3){center.x+p.x,center.y-1+wire_offset+p.y,center.z+p.z};
        }
        count = 1;
        static const int side_faces[] = {3,2,1,0};
        static const Vector3 offsets[] = {{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
        for (int direction = 0; direction < 4; ++direction) {
            unsigned bit = 1u << wire_directions[direction];
            if (!(state.data.wire.up_connections & state.data.wire.connections & bit)) continue;
            const CubeFace *face = &cube_faces[side_faces[direction]];
            for (int vertex = 0; vertex < 4; ++vertex) {
                Vector3 p = face->vertices[vertex];
                if (p.y < 0) {
                    p.x += face->normal.x*wire_offset;
                    p.z += face->normal.z*wire_offset;
                }
                geometry->vertices[count][vertex] = (Vector3){
                    center.x+offsets[direction].x+p.x,
                    center.y+wire_offset+p.y,center.z+offsets[direction].z+p.z};
            }
            ++count;
        }
    } else if (state.id == BLOCK_REDSTONE_TORCH) {
        TorchTransform transform = torch_transform(center, (Direction)state.data.torch.support_dir);
        for (int face = 0; face < 6; ++face)
            for (int vertex = 0; vertex < 4; ++vertex) {
                Vector3 p = torch_rotate(transform, torch_faces[face].vertices[vertex]);
                geometry->vertices[face][vertex] = (Vector3){transform.origin.x+p.x,transform.origin.y+p.y,transform.origin.z+p.z};
            }
    } else if (state.id == BLOCK_POWERED_RAIL ||
               state.id == BLOCK_ACTIVATOR_RAIL) {
        rail_geometry(state, center, geometry->vertices[0]);
    }
}
