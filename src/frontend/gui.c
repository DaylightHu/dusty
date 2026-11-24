#include "gui.h"
#include "ui_font.h"
#include "app_config.h"

#include "block_renderer.h"
#include "block_shape.h"
#include "command_completion.h"
#include "edit_command.h"
#include "flexible_placement.h"
#include "hotbar.h"
#include "inventory.h"
#include "plot_command.h"
#include "tick_command.h"
#include "voxel_pick.h"
#include "world_renderer.h"
#include "world_msaa.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "rs/session.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SCREEN_WIDTH = 1080,
    SCREEN_HEIGHT = 800,
    MESSAGE_CAPACITY = 192,
    COMMAND_CAPACITY = 128,
    CHAT_HISTORY_CAPACITY = 100,
    CHAT_TEXT_CAPACITY = MESSAGE_CAPACITY + 8,
    CHAT_FONT_SIZE = 18,
    CHAT_LINE_HEIGHT = 22
};

static void begin_gui_target(RenderTexture2D target)
{
    BeginTextureMode(target);
    /* Keep widgets in logical units while the viewport uses target pixels. */
    rlSetMatrixProjection(MatrixOrtho(0, GetScreenWidth(), GetScreenHeight(),
                                      0, 0.0, 1.0));
}

static void begin_gui_scissor(int x, int y, int width, int height)
{
    float scale_x = (float)GetRenderWidth()/GetScreenWidth();
    float scale_y = (float)GetRenderHeight()/GetScreenHeight();
    int left = (int)roundf(x*scale_x);
    int top = (int)roundf(y*scale_y);
    int right = (int)roundf((x + width)*scale_x);
    int bottom = (int)roundf((y + height)*scale_y);
    BeginScissorMode(left, top, right - left, bottom - top);
}

typedef enum {
    GUI_WORLD,
    GUI_COMMAND,
    GUI_INVENTORY,
    GUI_BARREL,
    GUI_HOPPER,
    GUI_DROPPER,
    GUI_PAUSE,
    GUI_SAVE_ERROR
} GuiScreen;

typedef struct {
    char text[CHAT_TEXT_CAPACITY];
    double time;
    bool error;
} ChatMessage;

typedef struct {
    BlockRenderer blocks;
    Inventory inventory;
    Hotbar hotbar;
    ResourcePack *resource_pack;
    GameSession *session;
    WorldRenderer *world_renderer;
    Camera3D camera;
    GuiScreen screen;
    GuiScreen screen_before_error;
    char message[MESSAGE_CAPACITY];
    char command[COMMAND_CAPACITY];
    size_t command_length;
    CommandCompletion completion;
    PlotCommandState plot_command;
    ChatMessage chat[CHAT_HISTORY_CAPACITY];
    size_t chat_count;
    int chat_scroll;
    BlockPos barrel_pos;
    BlockPos container_pos;
    bool mouse_captured;
    bool close_latched;
    bool running;
    bool fatal_error;
    bool close_after_save;
    Color sky_color;
    RenderTexture2D gui_target;
    WorldMsaa world_msaa;
} GuiApp;

static void add_chat_message(GuiApp *app, const char *text, bool error)
{
    if (!text[0]) return;
    if (app->chat_count == CHAT_HISTORY_CAPACITY) {
        memmove(app->chat, app->chat + 1,
                (CHAT_HISTORY_CAPACITY - 1)*sizeof(app->chat[0]));
        --app->chat_count;
    }
    ChatMessage *message = &app->chat[app->chat_count++];
    snprintf(message->text, sizeof(message->text), "%s%s",
             error ? "error: " : "", text);
    message->time = GetTime();
    message->error = error;
    app->chat_scroll = 0;
}

typedef struct {
    Rectangle panel;
    float grid_x;
    float grid_y;
    int columns;
    int rows;
} ContainerLayout;

enum {
    CONTAINER_SLOT_SIZE = 54,
    CONTAINER_SLOT_GAP = 4,
    SIGNAL_BUTTON_SIZE = 29,
    SIGNAL_BUTTON_GAP = 3
};

static bool point_in(Rectangle rectangle)
{
    return CheckCollisionPointRec(GetMousePosition(), rectangle);
}

static bool button_with_font(Rectangle rectangle, const char *label, bool enabled,
                             int font_size)
{
    bool hovered = enabled && point_in(rectangle);
    Color fill = enabled ? (hovered ? (Color){78, 101, 116, 255}
                                    : (Color){55, 72, 84, 255})
                         : (Color){48, 48, 48, 190};
    Color border = enabled ? (hovered ? (Color){244, 190, 88, 255}
                                      : (Color){142, 158, 168, 255})
                           : (Color){85, 85, 85, 180};
    Color text = enabled ? RAYWHITE : (Color){125, 125, 125, 255};
    DrawRectangleRec(rectangle, fill);
    DrawRectangleLinesEx(rectangle, 2.0f, border);
    int width = ui_text_width(label, font_size);
    ui_text_draw(label, (int)(rectangle.x + (rectangle.width - width)*0.5f),
             (int)(rectangle.y + (rectangle.height - font_size)*0.5f),
             font_size, text);
    return hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

static bool button(Rectangle rectangle, const char *label, bool enabled)
{
    return button_with_font(rectangle, label, enabled, 20);
}

static void draw_centered(const char *text, int y, int font_size, Color color)
{
    ui_text_draw(text, (GetScreenWidth() - ui_text_width(text, font_size))/2,
             y, font_size, color);
}

static void release_mouse(GuiApp *app)
{
    if (app->mouse_captured) EnableCursor();
    app->mouse_captured = false;
}

static void capture_mouse(GuiApp *app)
{
    DisableCursor();
    app->mouse_captured = true;
}

static ContainerLayout container_layout(BlockId id)
{
    bool barrel = id == BLOCK_BARREL;
    int columns = barrel ? 9 : id == BLOCK_HOPPER ? 5 : 3;
    int rows = id == BLOCK_HOPPER ? 1 : 3;
    int grid_width = columns*CONTAINER_SLOT_SIZE +
                     (columns - 1)*CONTAINER_SLOT_GAP;
    int grid_height = rows*CONTAINER_SLOT_SIZE +
                      (rows - 1)*CONTAINER_SLOT_GAP;
    int header = barrel ? 76 : 58;
    int panel_width = barrel ? grid_width + 40 : 480;
    int panel_height = header + grid_height + (barrel ? 42 : 0) + 18;
    ContainerLayout layout = {0};
    layout.panel = (Rectangle){
        (GetScreenWidth() - panel_width)*0.5f,
        (GetScreenHeight() - 80 - panel_height)*0.5f,
        (float)panel_width, (float)panel_height
    };
    layout.grid_x = layout.panel.x + (panel_width - grid_width)*0.5f;
    layout.grid_y = layout.panel.y + header;
    layout.columns = columns;
    layout.rows = rows;
    return layout;
}

static Rectangle container_slot_rectangle(ContainerLayout layout, int index)
{
    int column = index % layout.columns;
    int row = index / layout.columns;
    return (Rectangle){
        layout.grid_x + column*(CONTAINER_SLOT_SIZE + CONTAINER_SLOT_GAP),
        layout.grid_y + row*(CONTAINER_SLOT_SIZE + CONTAINER_SLOT_GAP),
        CONTAINER_SLOT_SIZE, CONTAINER_SLOT_SIZE
    };
}

static Rectangle barrel_signal_rectangle(ContainerLayout layout, int level)
{
    int width = 16*SIGNAL_BUTTON_SIZE + 15*SIGNAL_BUTTON_GAP;
    return (Rectangle){
        layout.panel.x + (layout.panel.width - width)*0.5f +
        level*(SIGNAL_BUTTON_SIZE + SIGNAL_BUTTON_GAP),
        layout.grid_y + layout.rows*(CONTAINER_SLOT_SIZE + CONTAINER_SLOT_GAP) + 8,
        SIGNAL_BUTTON_SIZE, 30
    };
}

static void open_barrel(GuiApp *app, BlockPos pos)
{
    RsResult result = world_open_barrel(game_session_world(app->session), pos);
    if (result != RS_OK) {
        snprintf(app->message, sizeof(app->message),
                 "Open barrel: %s", rs_result_name(result));
        return;
    }
    app->barrel_pos = pos;
    release_mouse(app);
    app->screen = GUI_BARREL;
    snprintf(app->message, sizeof(app->message),
             "Barrel: store items or use the signal shortcuts");
}

static void close_barrel(GuiApp *app)
{
    (void)world_close_barrel(game_session_world(app->session), app->barrel_pos);
    app->screen = GUI_WORLD;
    capture_mouse(app);
}

static float camera_axis_within(float value, int minimum, int maximum)
{
    if ((double)value < minimum) return (float)minimum;
    if ((double)value > maximum) {
        float edge = (float)maximum;
        return (double)edge > maximum ? nextafterf(edge, -INFINITY) : edge;
    }
    return value;
}

static bool prepare_world(GuiApp *app)
{
    World *world = game_session_world(app->session);
    WorldPlotBounds bounds = world_plot_bounds(world);
    BlockPos spawn = {
        (int)camera_axis_within(16.0f, bounds.min_x, bounds.max_x), 3,
        (int)camera_axis_within(16.0f, bounds.min_z, bounds.max_z)
    };
    RsResult result = world_stream(world, spawn, app_config_current()->stream_radius);
    if (result != RS_OK) {
        fprintf(stderr, "Cannot load world chunks: %s\n", rs_result_name(result));
        return false;
    }

    app->camera = (Camera3D){0};
    app->camera.position = (Vector3){(float)spawn.x, 3.0f, (float)spawn.z};
    app->camera.target = (Vector3){(float)spawn.x, 2.0f, (float)spawn.z - 1.0f};
    app->camera.up = (Vector3){0.0f, 1.0f, 0.0f};
    app->camera.fovy = app_config_current()->fov;
    app->camera.projection = CAMERA_PERSPECTIVE;
    return true;
}

static void save_world(GuiApp *app, bool close_after_save)
{
    GameSessionResult result = game_session_save(app->session);
    if (result == GAME_SESSION_OK) {
        if (close_after_save) app->running = false;
        else {
            app->screen = GUI_PAUSE;
            add_chat_message(app, "World saved", false);
        }
        return;
    }
    release_mouse(app);
    if (app->screen != GUI_SAVE_ERROR) app->screen_before_error = app->screen;
    app->screen = GUI_SAVE_ERROR;
    app->close_after_save = close_after_save;
    snprintf(app->message, sizeof(app->message), "Save failed: %s",
             game_session_result_name(result));
}

static Direction camera_horizontal_direction(const Camera3D *camera)
{
    float x = camera->target.x - camera->position.x;
    float z = camera->target.z - camera->position.z;
    if (fabsf(x) > fabsf(z)) return x >= 0.0f ? DIR_EAST : DIR_WEST;
    return z >= 0.0f ? DIR_SOUTH : DIR_NORTH;
}

static const char *horizontal_direction_name(Direction direction)
{
    switch (direction) {
    case DIR_EAST: return "East (+X)";
    case DIR_WEST: return "West (-X)";
    case DIR_SOUTH: return "South (+Z)";
    case DIR_NORTH: return "North (-Z)";
    default: return "Unknown";
    }
}

static Direction camera_direction(const Camera3D *camera)
{
    float x = camera->target.x - camera->position.x;
    float y = camera->target.y - camera->position.y;
    float z = camera->target.z - camera->position.z;
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    if (ay > ax && ay > az) return y >= 0.0f ? DIR_UP : DIR_DOWN;
    return camera_horizontal_direction(camera);
}

static EditCommandContext edit_context(const GuiApp *app)
{
    EditCommandContext context = {.facing=camera_direction(&app->camera)};
    double x=floor((double)app->camera.position.x+0.5);
    double y=floor((double)app->camera.position.y+0.5);
    double z=floor((double)app->camera.position.z+0.5);
    if (isfinite(x) && isfinite(y) && isfinite(z) &&
        x>=INT_MIN && x<=INT_MAX && y>=INT_MIN && y<=INT_MAX && z>=INT_MIN && z<=INT_MAX) {
        context.position=(BlockPos){(int)x,(int)y,(int)z};
        context.has_position=true;
    }
    return context;
}

static RsResult run_gui_edit(GuiApp *app, const EditCommand *command)
{
    return edit_command_run(app->session, command, edit_context(app),
                             app->message, sizeof(app->message));
}

static bool flexible_placement_held(BlockId selected)
{
    return selected > BLOCK_AIR && selected < BLOCK_INTERNAL_FIRST &&
           (app_input_world("placement_offset",true) || app_input_world("placement_rotation",true));
}

static void apply_edit(GuiApp *app, VoxelHit hit, BlockId selected, bool place,
                       Direction facing)
{
    if (!hit.hit) return;
    if (place && selected == BLOCK_AIR) {
        snprintf(app->message, sizeof(app->message),
                 "Selected hotbar slot is empty");
        return;
    }

    World *world = game_session_world(app->session);
    RsResult result;
    uint64_t before_revision = world_revision(world);
    bool flexible = place && flexible_placement_held(selected);
    if (place) {
        if (!hit.has_face) {
            snprintf(app->message, sizeof(app->message),
                     "Cannot place while inside a block");
            if (flexible) add_chat_message(app, app->message, true);
            return;
        }
        PlacementTarget plan = flexible_placement_target(hit, selected, facing,
            camera_direction(&app->camera), app_input_world("placement_offset",true),
            app_input_world("placement_rotation",true));
        if (!plan.valid) {
            snprintf(app->message, sizeof(app->message),
                     "%s does not support that direction", block_name(selected));
            if (flexible) add_chat_message(app, app->message, true);
            return;
        }
        BlockPos target = plan.pos;
        if (!world_in_bounds(world, target) ||
            world_get(world, target).id != BLOCK_AIR) {
            snprintf(app->message, sizeof(app->message),
                     "Target occupied or outside world");
            if (flexible) add_chat_message(app, app->message, true);
            return;
        }
        if ((selected == BLOCK_REDSTONE_WIRE ||
             selected == BLOCK_REDSTONE_REPEATER ||
             selected == BLOCK_REDSTONE_COMPARATOR ||
             selected == BLOCK_POWERED_RAIL ||
             selected == BLOCK_ACTIVATOR_RAIL) && hit.face != DIR_UP && !flexible) {
            snprintf(app->message, sizeof(app->message),
                     "%s must be placed on a top face", block_name(selected));
            return;
        }
        if (selected == BLOCK_LEVER)
            world_place_lever(world, target, plan.placement, facing);
        else
            world_place(world, target, selected, plan.placement);
    } else {
        world_remove(world, hit.pos);
    }
    result = world_result(world);
    if (result == RS_OK && world_revision(world) == before_revision)
        result = RS_UNCHANGED;
    world_clear_error(world);
    if (result == RS_OK || result == RS_UNCHANGED) {
        RsResult flushed = game_session_flush(app->session);
        if (flushed != RS_OK) result = flushed;
    }
    snprintf(app->message, sizeof(app->message), "%s: %s",
             place ? "Place" : "Remove", rs_result_name(result));
    if (flexible && result != RS_OK && result != RS_UNCHANGED)
        add_chat_message(app, app->message, true);
}

static BlockId pickable_block(BlockId id)
{
    if (id == BLOCK_PISTON_HEAD) return BLOCK_PISTON;
    if (id == BLOCK_STICKY_PISTON_HEAD) return BLOCK_STICKY_PISTON;
    return id > BLOCK_AIR && id < BLOCK_INTERNAL_FIRST ? id : BLOCK_AIR;
}

static void advance_world(GuiApp *app)
{
    RsResult result = game_session_advance(app->session, GetFrameTime());
    if (result != RS_OK && result != RS_UNCHANGED) {
        fprintf(stderr, "World simulation: %s\n", rs_result_name(result));
        app->fatal_error = true;
        app->running = false;
    }
}

static void update_command(GuiApp *app)
{
    float wheel = GetMouseWheelMove();
    if (wheel > 0.0f || IsKeyPressed(KEY_PAGE_UP)) app->chat_scroll += 3;
    if (wheel < 0.0f || IsKeyPressed(KEY_PAGE_DOWN)) app->chat_scroll -= 3;
    if (app->chat_scroll < 0) app->chat_scroll = 0;
    if (app->chat_scroll > CHAT_HISTORY_CAPACITY*CHAT_TEXT_CAPACITY)
        app->chat_scroll = CHAT_HISTORY_CAPACITY*CHAT_TEXT_CAPACITY;
    int character;
    while ((character = app_input_char()) != 0) {
        if (character >= 32 && character < 127 &&
            app->command_length + 1 < sizeof(app->command)) {
            app->command[app->command_length++] = (char)character;
            app->command[app->command_length] = '\0';
            app->completion.active = false;
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && app->command_length) {
        app->command[--app->command_length] = '\0';
        app->completion.active = false;
    }
    if (IsKeyPressed(KEY_TAB) && !app->plot_command.pending) {
        command_complete(&app->completion, app->command, sizeof(app->command));
        app->command_length = strlen(app->command);
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (app->plot_command.pending) {
            (void)plot_command_answer(game_session_world(app->session), &app->plot_command,
                                       "n", app->message, sizeof(app->message));
            add_chat_message(app, app->message, false);
        }
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }
    if (!IsKeyPressed(KEY_ENTER)) return;
    if (!app->plot_command.pending && !app->command[strspn(app->command, " \t")]) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }
    PlotCommand command;
    TickCommand tick;
    EditCommand edit;
    bool error = false;
    if (app->plot_command.pending) {
        RsResult result = plot_command_answer(game_session_world(app->session), &app->plot_command,
                                               app->command, app->message, sizeof(app->message));
        error = result != RS_OK;
        if (error && !app->message[0])
            snprintf(app->message, sizeof(app->message), "Shrink: %s", rs_result_name(result));
    } else if (edit_command_parse(app->command, &edit)) {
        error = run_gui_edit(app, &edit) != RS_OK;
    } else if (tick_command_parse(app->command, &tick)) {
        RsResult result = tick_command_run(game_session_world(app->session),
            &tick, app->message, sizeof(app->message));
        error = result != RS_OK;
        if (result != RS_OK && !app->message[0])
            snprintf(app->message, sizeof(app->message), "Tick command: %s",
                     rs_result_name(result));
    } else if (plot_command_parse(app->command, &command)) {
        RsResult result = plot_command_run(game_session_world(app->session),
                                           &command, &app->plot_command, app->message,
                                           sizeof(app->message));
        error = result != RS_OK;
        if (result != RS_OK && !app->message[0])
            snprintf(app->message, sizeof(app->message), "Plot command: %s",
                     rs_result_name(result));
    } else {
        error = true;
        snprintf(app->message, sizeof(app->message),
                 "Use pos1|pos2|copy|paste|stack|move|replace|undo|redo, schem save|load, tick freeze|step|noupdate, or plot info|expand|shrink");
    }
    add_chat_message(app, app->message, error);
    if (app->plot_command.pending) {
        app->command[0] = '\0';
        app->command_length = 0;
        app->completion = (CommandCompletion){0};
        return;
    }
    app->screen = GUI_WORLD;
    capture_mouse(app);
}

static void constrain_camera(GuiApp *app, const World *world)
{
    WorldPlotBounds bounds = world_plot_bounds(world);
    float x = app->camera.position.x;
    float z = app->camera.position.z;
    x = camera_axis_within(x, bounds.min_x, bounds.max_x);
    z = camera_axis_within(z, bounds.min_z, bounds.max_z);
    app->camera.target.x += x - app->camera.position.x;
    app->camera.target.z += z - app->camera.position.z;
    app->camera.position.x = x;
    app->camera.position.z = z;
}

static VoxelHit update_world(GuiApp *app)
{
    VoxelHit hit = {0};
    if (app_input_world("command",false)) {
        release_mouse(app);
        while (app_input_char() != 0) {} /* Consume the key that opened chat. */
        app->command[0] = '\0';
        app->command_length = 0;
        app->completion = (CommandCompletion){0};
        app->chat_scroll = 0;
        app->screen = GUI_COMMAND;
        return hit;
    }
    if (app_input_world("inventory",false)) {
        release_mouse(app);
        app->screen = GUI_INVENTORY;
        snprintf(app->message, sizeof(app->message),
                 "Inventory: choose a hotbar slot, then click an icon");
        return hit;
    }
    if (app_input_world("pause",false) || !IsWindowFocused()) {
        release_mouse(app);
        app->screen = GUI_PAUSE;
        game_session_reset_clock(app->session);
        return hit;
    }

    hotbar_update(&app->hotbar);
    const AppSettings *settings=app_config_current();
    float step=settings->move_speed*GetFrameTime();
    Vector2 mouse=GetMouseDelta();
    Vector3 movement={
        (app_input_world("forward",true)-app_input_world("backward",true))*step,
        (app_input_world("right",true)-app_input_world("left",true))*step,
        (app_input_world("up",true)-app_input_world("down",true))*step*(5.0f/5.4f)
    };
    float turn=0.03f*GetFrameTime();
    Vector3 rotation={
        (mouse.x*settings->mouse_sensitivity +
         (app_input_world("look_right",true)-app_input_world("look_left",true))*turn)*RAD2DEG,
        (mouse.y*settings->mouse_sensitivity +
         (app_input_world("look_down",true)-app_input_world("look_up",true))*turn)*RAD2DEG,
        -app_input_world("roll_left",true)*turn*RAD2DEG
    };
    if (IsGamepadAvailable(0)) {
        rotation.x+=GetGamepadAxisMovement(0,GAMEPAD_AXIS_RIGHT_X)*2*settings->mouse_sensitivity*RAD2DEG;
        rotation.y+=GetGamepadAxisMovement(0,GAMEPAD_AXIS_RIGHT_Y)*2*settings->mouse_sensitivity*RAD2DEG;
        float x=GetGamepadAxisMovement(0,GAMEPAD_AXIS_LEFT_X), y=GetGamepadAxisMovement(0,GAMEPAD_AXIS_LEFT_Y);
        if (x<=-0.25f) movement.y-=step;
        if (x>=0.25f) movement.y+=step;
        if (y<=-0.25f) movement.x+=step;
        if (y>=0.25f) movement.x-=step;
    }
    UpdateCameraPro(&app->camera,movement,rotation,0.0f);

    World *world = game_session_world(app->session);
    constrain_camera(app, world);

    Ray ray = GetScreenToWorldRayEx(
        (Vector2){GetScreenWidth()*0.5f, GetScreenHeight()*0.5f}, app->camera,
        GetScreenWidth(), GetScreenHeight());
    double camera_x = floor((double)app->camera.position.x + 0.5);
    double camera_z = floor((double)app->camera.position.z + 0.5);
    if (!isfinite(camera_x) || !isfinite(camera_z) || camera_x < (double)INT32_MIN+16 ||
        camera_x > (double)INT32_MAX-16 || camera_z < (double)INT32_MIN+16 || camera_z > (double)INT32_MAX-16 ||
        world_stream(world,(BlockPos){(int)camera_x,0,(int)camera_z},app_config_current()->stream_radius) != RS_OK) {
        snprintf(app->message,sizeof(app->message),"Cannot stream world chunks");
        app->fatal_error = true; app->running = false;
        return hit;
    }
    hit = voxel_pick(world,
        (PickVec3){ray.position.x, ray.position.y, ray.position.z},
        (PickVec3){ray.direction.x, ray.direction.y, ray.direction.z}, 6.0f);
    BlockId selected = hotbar_selected_block(&app->hotbar);
    Direction facing = camera_horizontal_direction(&app->camera);
    InventorySlot held = hotbar_selected(&app->hotbar);
    if (held.kind == INVENTORY_TOOL && held.tool == TOOL_WOODEN_AXE) {
        for (unsigned point = 1; point <= 2; ++point) {
            int button = point == 1 ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_RIGHT;
            if (!IsMouseButtonPressed(button)) continue;
            RsResult result = RS_INVALID_ARGUMENT;
            if (hit.hit) {
                EditCommand edit = {.kind = point == 1 ? EDIT_POS1 : EDIT_POS2,
                    .position = hit.pos, .has_position = true};
                result = run_gui_edit(app, &edit);
            } else {
                snprintf(app->message, sizeof(app->message), "Point at a block to select it");
            }
            add_chat_message(app, app->message, result != RS_OK);
        }
    } else {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            apply_edit(app, hit, selected, false, facing);
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) ||
                         IsKeyDown(KEY_RIGHT_SHIFT);
            BlockId target = hit.hit ? (BlockId)world_get(world, hit.pos).id :
                                      BLOCK_AIR;
            if (flexible_placement_held(selected)) {
                apply_edit(app, hit, selected, true, facing);
            } else if (hit.hit && !shift &&
                target == BLOCK_BARREL) {
                open_barrel(app, hit.pos);
            } else if (hit.hit && !shift &&
                       (target == BLOCK_HOPPER || target == BLOCK_DROPPER)) {
                app->container_pos = hit.pos;
                app->screen = target == BLOCK_HOPPER ? GUI_HOPPER : GUI_DROPPER;
                release_mouse(app);
            } else if (hit.hit && !shift && target == BLOCK_REDSTONE_WIRE) {
                RsResult result = world_toggle_wire_dot(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message), "Toggle dot: %s",
                         rs_result_name(result));
            } else if (hit.hit && !shift && target == BLOCK_LEVER) {
                BlockState state = world_get(world, hit.pos);
                uint64_t before_revision = world_revision(world);
                world_set_lever_powered(world, hit.pos,
                                        !state.data.lever.powered);
                RsResult result = world_result(world);
                if (result == RS_OK &&
                    world_revision(world) == before_revision)
                    result = RS_UNCHANGED;
                world_clear_error(world);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message),
                         "Toggle lever: %s", rs_result_name(result));
            } else if (hit.hit && !shift &&
                       target == BLOCK_REDSTONE_REPEATER) {
                RsResult result = world_cycle_repeater_delay(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message),
                         "Cycle repeater delay: %s", rs_result_name(result));
            } else if (hit.hit && !shift &&
                       target == BLOCK_REDSTONE_COMPARATOR) {
                RsResult result = world_toggle_comparator_mode(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message),
                         "Toggle comparator mode: %s", rs_result_name(result));
            } else if (hit.hit && !shift && target == BLOCK_NOTE_BLOCK) {
                RsResult result = world_cycle_note(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                BlockState state = world_get(world, hit.pos);
                if ((result == RS_OK || result == RS_UNCHANGED) && state.id == BLOCK_NOTE_BLOCK)
                    snprintf(app->message, sizeof(app->message), "Note: %u (%s)",
                        (unsigned)state.data.note_block.note,
                        note_instrument_name((NoteInstrument)state.data.note_block.instrument));
                else
                    snprintf(app->message, sizeof(app->message), "Tune note: %s", rs_result_name(result));
            } else {
                if (hotbar_selected(&app->hotbar).kind == INVENTORY_ITEM)
                    snprintf(app->message, sizeof(app->message),
                             "Open a hopper, dropper or barrel to store items");
                else apply_edit(app, hit, selected, true, facing);
            }
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) && hit.hit) {
        BlockId picked = pickable_block(
            (BlockId)world_get(world, hit.pos).id);
        if (picked != BLOCK_AIR &&
            hotbar_pick_block(&app->hotbar, picked)) {
            snprintf(app->message, sizeof(app->message),
                     "Hotbar slot %d: %s", app->hotbar.selected_slot + 1,
                     block_name(picked));
        } else {
            snprintf(app->message, sizeof(app->message),
                     "This block cannot be picked");
        }
    }

    return hit;
}

static void update_inventory(GuiApp *app)
{
    if (app_input_world("inventory",false) || IsKeyPressed(KEY_ESCAPE)) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }

    hotbar_update(&app->hotbar);
    InventorySlot picked = inventory_update(&app->inventory);
    if (picked.kind != INVENTORY_EMPTY) {
        bool changed = hotbar_pick(&app->hotbar, picked);
        InventorySlot selected = hotbar_selected(&app->hotbar);
        if (selected.kind == INVENTORY_ITEM)
            snprintf(app->message, sizeof(app->message),
                     "Hotbar slot %d: %s x%u%s", app->hotbar.selected_slot + 1,
                     inventory_slot_name(selected), (unsigned)selected.stack.count,
                     changed ? "" : " (full)");
        else
            snprintf(app->message, sizeof(app->message),
                     "Hotbar slot %d: %s", app->hotbar.selected_slot + 1,
                     inventory_slot_name(selected));
    }

}

static bool write_container_slot(World *world, BlockPos pos, size_t index,
                                 ItemStack contents, RsResult *result)
{
    *result = world_set_container_slot(world, pos, index, contents);
    /* A simulation error may be reported after the slot was written. */
    ContainerStack committed;
    if (world_get_container_slot(world, pos, index, &committed) == RS_OK &&
        committed.item == contents.item && committed.count == contents.count &&
        committed.limit == contents.limit) return true;
    if (*result == RS_OK || *result == RS_UNCHANGED)
        *result = RS_INTERNAL_ERROR;
    return false;
}

static unsigned store_hotbar_items(World *world, BlockPos pos,
                                    InventorySlot *slot, unsigned amount,
                                    RsResult *result)
{
    unsigned moved = 0;
    size_t count = world_container_size(world, pos);
    for (int pass = 0; pass < 2 && moved < amount; ++pass) {
        for (size_t index = 0; index < count && moved < amount; ++index) {
            ContainerStack contents;
            *result = world_get_container_slot(world, pos, index, &contents);
            if (*result != RS_OK) return moved;
            if ((contents.count == 0) != (pass == 1)) continue;
            ItemStack remaining = slot->stack;
            unsigned added = item_stack_transfer(&contents, &remaining,
                                                   amount - moved);
            if (!added) continue;
            if (!write_container_slot(world, pos, index, contents, result))
                return moved;
            *slot = remaining.count ? (InventorySlot){
                .kind = INVENTORY_ITEM, .stack = remaining
            } : (InventorySlot){0};
            moved += added;
            if (*result != RS_OK && *result != RS_UNCHANGED) return moved;
        }
    }
    return moved;
}

static unsigned take_container_items(World *world, BlockPos pos, size_t index,
                                      Hotbar *hotbar, ItemStack contents,
                                      unsigned amount, RsResult *result)
{
    Hotbar updated = *hotbar;
    unsigned moved = 0;
    for (int pass = 0; pass < 3 && moved < amount; ++pass) {
        for (int slot = 0; slot < HOTBAR_SLOT_COUNT && moved < amount; ++slot) {
            InventorySlot *destination = &updated.slots[slot];
            if (pass == 0 && destination->kind != INVENTORY_ITEM) continue;
            if (pass == 1 && destination->kind != INVENTORY_EMPTY) continue;
            /* Creative block icons can be replaced in the selected slot. */
            if (pass == 2 && (slot != updated.selected_slot ||
                             destination->kind != INVENTORY_BLOCK)) continue;
            ItemStack stack = destination->kind == INVENTORY_ITEM ?
                              destination->stack : (ItemStack){0};
            unsigned added = item_stack_transfer(&stack, &contents,
                                                   amount - moved);
            if (!added) continue;
            *destination = (InventorySlot){.kind = INVENTORY_ITEM, .stack = stack};
            moved += added;
        }
    }
    if (!moved || !write_container_slot(world, pos, index, contents, result))
        return 0;
    *hotbar = updated;
    return moved;
}

static void update_container_slots(GuiApp *app, BlockPos pos,
                                    ContainerLayout layout)
{
    hotbar_update(&app->hotbar);
    bool left = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!left && !(right && shift)) return;

    int hotbar_slot = hotbar_slot_at(GetMousePosition());
    int container_slot = -1;
    if (hotbar_slot >= 0) app->hotbar.selected_slot = hotbar_slot;
    else {
        for (int index = 0; index < layout.columns*layout.rows; ++index) {
            if (point_in(container_slot_rectangle(layout, index))) {
                container_slot = index;
                break;
            }
        }
    }
    if (hotbar_slot < 0 && container_slot < 0) return;

    RsResult result = game_session_flush(app->session);
    if (result != RS_OK) {
        snprintf(app->message, sizeof(app->message), "Transfer failed: %s",
                 rs_result_name(result));
        return;
    }
    World *world = game_session_world(app->session);
    ItemStack source;
    if (hotbar_slot >= 0) {
        InventorySlot slot = app->hotbar.slots[hotbar_slot];
        if (slot.kind != INVENTORY_ITEM) return;
        source = slot.stack;
    } else {
        result = world_get_container_slot(world, pos, (size_t)container_slot,
                                          &source);
        if (result != RS_OK) return;
    }
    if (!source.count) return;
    unsigned amount = !shift ? 1u : left ? source.count :
                      ((unsigned)source.count + 1u)/2u;
    unsigned moved = hotbar_slot >= 0 ?
        store_hotbar_items(world, pos, &app->hotbar.slots[hotbar_slot], amount,
                            &result) :
        take_container_items(world, pos, (size_t)container_slot, &app->hotbar,
                               source, amount, &result);
    if (!moved && (result == RS_OK || result == RS_UNCHANGED)) {
        snprintf(app->message, sizeof(app->message),
                 "No items moved: destination has no free space");
        return;
    }
    if (result == RS_OK || result == RS_UNCHANGED)
        result = game_session_flush(app->session);
    snprintf(app->message, sizeof(app->message), "%s %u: %s",
             hotbar_slot >= 0 ? "Stored" : "Retrieved", moved,
             rs_result_name(result));
}

static void update_barrel(GuiApp *app)
{
    World *world = game_session_world(app->session);
    BlockState state = world_get(world, app->barrel_pos);
    if (state.id != BLOCK_BARREL) {
        close_barrel(app);
        snprintf(app->message, sizeof(app->message),
                 "Barrel is no longer available");
        return;
    }
    if (app_input_world("inventory",false) || IsKeyPressed(KEY_ESCAPE)) {
        close_barrel(app);
        return;
    }

    ContainerLayout layout = container_layout(BLOCK_BARREL);
    update_container_slots(app, app->barrel_pos, layout);
    int level = -1;
    state = world_get(world, app->barrel_pos);
    if (IsKeyPressed(KEY_LEFT) && state.data.barrel.level > 0)
        level = state.data.barrel.level - 1;
    if (IsKeyPressed(KEY_RIGHT) && state.data.barrel.level < 15)
        level = state.data.barrel.level + 1;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        for (int value = 0; value <= 15; ++value) {
            if (point_in(barrel_signal_rectangle(layout, value))) {
                level = value;
                break;
            }
        }
    }
    if (level >= 0) {
        RsResult result = world_set_barrel_level(world, app->barrel_pos,
                                                 (uint8_t)level);
        if (result == RS_OK || result == RS_UNCHANGED)
            result = game_session_flush(app->session);
        snprintf(app->message, sizeof(app->message), "Barrel signal %d: %s",
                 level, rs_result_name(result));
    }
}

static void update_container(GuiApp *app)
{
    World *world = game_session_world(app->session);
    BlockId id = app->screen == GUI_DROPPER ? BLOCK_DROPPER : BLOCK_HOPPER;
    if (world_get(world, app->container_pos).id != id ||
        app_input_world("inventory",false) || IsKeyPressed(KEY_ESCAPE)) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }
    update_container_slots(app, app->container_pos, container_layout(id));
}

static void draw_container(const GuiApp *app, BlockId id)
{
    bool barrel = id == BLOCK_BARREL;
    World *world = game_session_world(app->session);
    BlockPos pos = barrel ? app->barrel_pos : app->container_pos;
    ContainerLayout layout = container_layout(id);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  (Color){10, 18, 20, 155});
    DrawRectangleRec(layout.panel, (Color){226, 218, 196, 250});
    DrawRectangleLinesEx(layout.panel, 3.0f, (Color){73, 48, 24, 255});
    draw_centered(barrel ? "BARREL" : id == BLOCK_HOPPER ? "HOPPER" : "DROPPER",
                  (int)layout.panel.y + 18,
                  26, (Color){73, 48, 24, 255});
    BlockState state = world_get(world, pos);
    int level = barrel && state.id == BLOCK_BARREL ? state.data.barrel.level : 0;
    if (barrel)
        draw_centered(TextFormat("Comparator output: %d", level),
                      (int)layout.panel.y + 49, 17, (Color){100, 71, 36, 255});

    for (int index = 0; index < layout.columns*layout.rows; ++index) {
        Rectangle slot = container_slot_rectangle(layout, index);
        bool hovered = point_in(slot);
        DrawRectangleRec(slot, hovered ? (Color){102, 116, 112, 255} :
                                        (Color){75, 75, 75, 235});
        DrawRectangleLinesEx(slot, hovered ? 2.0f : 1.0f,
                             hovered ? (Color){244, 190, 88, 255} :
                                       (Color){145, 145, 145, 255});
        ContainerStack stack;
        if (world_get_container_slot(world, pos, (size_t)index, &stack) == RS_OK)
            inventory_draw_stack(&app->inventory, stack, slot);
    }
    if (barrel) {
        for (int value = 0; value <= 15; ++value) {
            Rectangle shortcut = barrel_signal_rectangle(layout, value);
            bool hovered = point_in(shortcut);
            DrawRectangleRec(shortcut, hovered ? (Color){111, 79, 50, 255} :
                                                (Color){82, 58, 37, 255});
            DrawRectangleLinesEx(shortcut, value == level ? 3.0f : 1.0f,
                                 value == level ? (Color){244, 190, 88, 255} :
                                                  (Color){142, 119, 86, 255});
            const char *text = TextFormat("%d", value);
            ui_text_draw(text,
                     (int)(shortcut.x + (shortcut.width - ui_text_width(text, 16))*0.5f),
                     (int)shortcut.y + 7, 16, RAYWHITE);
        }
    }
    hotbar_draw(&app->hotbar, &app->inventory);
}

static void draw_block_outline(BlockState state, Vector3 center)
{
    static BlockShape cached;
    static BlockShapeEdge edges[BLOCK_SHAPE_EDGES];
    static unsigned edge_count;
    BlockShape shape;
    block_outline_shape(state, &shape);
    if (shape.count != cached.count ||
        memcmp(shape.boxes, cached.boxes, shape.count*sizeof(shape.boxes[0]))) {
        cached = shape;
        edge_count = block_shape_edges(&shape, edges);
    }
    for (unsigned i = 0; i < edge_count; ++i) {
        const BlockShapeEdge *edge = &edges[i];
        Vector3 start = {center.x + edge->start[0]/16.0f - 0.5f,
                         center.y + edge->start[1]/16.0f - 0.5f,
                         center.z + edge->start[2]/16.0f - 0.5f};
        Vector3 end = {center.x + edge->end[0]/16.0f - 0.5f,
                       center.y + edge->end[1]/16.0f - 0.5f,
                       center.z + edge->end[2]/16.0f - 0.5f};
        DrawLine3D(start, end, YELLOW);
    }
}

static Vector3 placement_axis(Direction direction)
{
    BlockPos axis = block_offset((BlockPos){0}, direction);
    return (Vector3){(float)axis.x, (float)axis.y, (float)axis.z};
}

static Vector3 placement_grid_point(VoxelHit hit, PlacementTarget plan,
                                    float u, float v, float lift)
{
    Vector3 normal = placement_axis(hit.face);
    Vector3 right = placement_axis(plan.right), up = placement_axis(plan.up);
    float plane = hit.local.x*normal.x + hit.local.y*normal.y + hit.local.z*normal.z;
    return (Vector3){
        hit.pos.x + normal.x*(plane + lift) + right.x*u + up.x*v,
        hit.pos.y + normal.y*(plane + lift) + right.y*u + up.y*v,
        hit.pos.z + normal.z*(plane + lift) + right.z*u + up.z*v};
}

static void draw_placement_quad(VoxelHit hit, PlacementTarget plan,
                                const float points[4][2], float lift, Color color)
{
    Vector3 vertices[4];
    for (int i = 0; i < 4; ++i)
        vertices[i] = placement_grid_point(hit, plan, points[i][0], points[i][1], lift);
    DrawTriangle3D(vertices[0], vertices[1], vertices[2], color);
    DrawTriangle3D(vertices[0], vertices[2], vertices[3], color);
}

static void draw_flexible_placement(GuiApp *app, VoxelHit hit)
{
    BlockId selected = hotbar_selected_block(&app->hotbar);
    if (app->screen != GUI_WORLD || !hit.hit || !hit.has_face ||
        !flexible_placement_held(selected)) return;
    PlacementTarget plan = flexible_placement_target(hit, selected,
        camera_horizontal_direction(&app->camera), camera_direction(&app->camera),
        app_input_world("placement_offset",true), app_input_world("placement_rotation",true));
    if ((unsigned)plan.right >= DIR_COUNT) return;
    World *world = game_session_world(app->session);
    bool available = plan.valid && world_in_bounds(world, plan.pos) &&
                     world_get(world, plan.pos).id == BLOCK_AIR;
    Color color = available ? (Color){72, 48, 184, 255} : RED;
    static const float outer[4][2] = {
        {-0.5f,-0.5f}, {0.5f,-0.5f}, {0.5f,0.5f}, {-0.5f,0.5f}};
    static const float regions[5][4][2] = {
        {{-0.25f,-0.25f}, {0.25f,-0.25f}, {0.25f,0.25f}, {-0.25f,0.25f}},
        {{-0.5f,-0.5f}, {-0.25f,-0.25f}, {-0.25f,0.25f}, {-0.5f,0.5f}},
        {{0.25f,-0.25f}, {0.5f,-0.5f}, {0.5f,0.5f}, {0.25f,0.25f}},
        {{-0.25f,0.25f}, {0.25f,0.25f}, {0.5f,0.5f}, {-0.5f,0.5f}},
        {{-0.5f,-0.5f}, {0.5f,-0.5f}, {0.25f,-0.25f}, {-0.25f,-0.25f}}
    };
    Color background = color;
    background.a = 40;
    draw_placement_quad(hit, plan, outer, 0.002f, background);
    Color highlight = color;
    highlight.a = 160;
    draw_placement_quad(hit, plan, regions[plan.region], 0.004f, highlight);
    for (int i = 0; i < 4; ++i) {
        int next = (i + 1)%4;
        Vector3 corner = placement_grid_point(hit, plan, outer[i][0], outer[i][1], 0.006f);
        Vector3 inner = placement_grid_point(hit, plan,
            regions[PLACEMENT_CENTER][i][0], regions[PLACEMENT_CENTER][i][1], 0.006f);
        DrawLine3D(corner, placement_grid_point(hit, plan,
            outer[next][0], outer[next][1], 0.006f), color);
        DrawLine3D(inner, placement_grid_point(hit, plan,
            regions[PLACEMENT_CENTER][next][0], regions[PLACEMENT_CENTER][next][1], 0.006f), color);
        DrawLine3D(corner, inner, color);
    }
    Vector3 center = {(float)plan.pos.x, (float)plan.pos.y, (float)plan.pos.z};
    DrawCubeWires(center, 1.002f, 1.002f, 1.002f, color);
    if (app_input_world("placement_rotation",true) && plan.directional) {
        Vector3 direction = placement_axis(plan.direction);
        Direction wing_axis = plan.direction == plan.up ||
            direction_opposite(plan.direction) == plan.up ? plan.right : plan.up;
        Vector3 wing = placement_axis(wing_axis);
        Vector3 start = {center.x - direction.x*0.3f, center.y - direction.y*0.3f,
                         center.z - direction.z*0.3f};
        Vector3 end = {center.x + direction.x*0.4f, center.y + direction.y*0.4f,
                       center.z + direction.z*0.4f};
        DrawLine3D(start, end, color);
        for (int side = -1; side <= 1; side += 2) {
            Vector3 tip = {end.x - direction.x*0.16f + wing.x*0.12f*side,
                           end.y - direction.y*0.16f + wing.y*0.12f*side,
                           end.z - direction.z*0.16f + wing.z*0.12f*side};
            DrawLine3D(end, tip, color);
        }
    }
}

static void draw_selection(GuiApp *app)
{
    RegionEditor *editor=game_session_editor(app->session);
    BlockPos first, second;
    unsigned selected=region_editor_selection(editor,&first,&second);
    Color color={72,48,184,255};
    RegionBounds bounds;
    if (region_editor_bounds(editor,&bounds)) {
        BoundingBox box={
            {(float)((double)bounds.min.x-0.505),(float)((double)bounds.min.y-0.505),(float)((double)bounds.min.z-0.505)},
            {(float)((double)bounds.max.x+0.505),(float)((double)bounds.max.y+0.505),(float)((double)bounds.max.z+0.505)}};
        DrawBoundingBox(box,color);
    } else if (selected) {
        BlockPos pos=(selected&1) ? first : second;
        DrawCubeWires((Vector3){(float)pos.x,(float)pos.y,(float)pos.z},1.01f,1.01f,1.01f,color);
    }
}

static void draw_redstone_hud(const World *world, VoxelHit hit)
{
    if (!hit.hit) return;
    BlockState state = world_get(world, hit.pos);
    if (state.id != BLOCK_REDSTONE_WIRE) return;
    ui_text_draw(TextFormat("Redstone power: %u / 15",
                 (unsigned)state.data.wire.power),
                 15, 87, 18, DARKBLUE);
}

static void draw_world(GuiApp *app, VoxelHit hit)
{
    /* BeginTextureMode already set the viewport and matrices to this size. */
    if (app->world_msaa.id) rlEnableFramebuffer(app->world_msaa.id);
    ClearBackground(app->sky_color);
    BeginMode3D(app->camera);
        world_renderer_draw(app->world_renderer);
        if (hit.hit) {
            Vector3 center = {(float)hit.pos.x, (float)hit.pos.y,
                              (float)hit.pos.z};
            draw_block_outline(world_get(game_session_world(app->session), hit.pos),
                                center);
        }
        draw_flexible_placement(app, hit);
        draw_selection(app);
    EndMode3D();
    if (app->world_msaa.id)
        world_msaa_resolve(&app->world_msaa, app->gui_target.id);

    if (app->screen == GUI_WORLD) {
        int cx = GetScreenWidth()/2;
        int cy = GetScreenHeight()/2;
        DrawLine(cx - 6, cy, cx + 6, cy, BLACK);
        DrawLine(cx, cy - 6, cx, cy + 6, BLACK);
    }
    ui_text_draw(TextFormat("Position: (%.2f, %.2f, %.2f)",
             app->camera.position.x,
             app->camera.position.y,
             app->camera.position.z),
             15, 15, 18, DARKBLUE);
    if (hit.hit) {
        ui_text_draw(TextFormat("Target: (%d, %d, %d)",
                 hit.pos.x, hit.pos.y, hit.pos.z),
                 15, 39, 18, DARKBLUE);
    } else {
        ui_text_draw("Target: none", 15, 39, 18, DARKBLUE);
    }
    ui_text_draw(TextFormat("Facing: %s", horizontal_direction_name(
                 camera_horizontal_direction(&app->camera))),
             15, 63, 18, DARKBLUE);
    const World *world = game_session_world_const(app->session);
    draw_redstone_hud(world, hit);
    ui_text_draw(sim_is_frozen(world) ? "FROZEN" : "RUNNING", 15, 111, 18, DARKBLUE);
    hotbar_draw(&app->hotbar, &app->inventory);
}

/* Return wrapped ranges so resizing also reflows messages already in history. */
static int wrap_chat_message(const char *text, int width,
                             size_t starts[CHAT_TEXT_CAPACITY],
                             size_t lengths[CHAT_TEXT_CAPACITY])
{
    int rows = 0;
    size_t offset = 0;
    while (text[offset] && rows < CHAT_TEXT_CAPACITY) {
        char line[CHAT_TEXT_CAPACITY];
        size_t length = 0, word_end = 0;
        while (text[offset + length] && text[offset + length] != '\n' &&
               length + 1 < sizeof(line)) {
            line[length] = text[offset + length];
            line[length + 1] = '\0';
            if (length && ui_text_width(line, CHAT_FONT_SIZE) > width) break;
            ++length;
            if (line[length - 1] == ' ') word_end = length;
        }
        if (text[offset + length] && text[offset + length] != '\n' && word_end)
            length = word_end;
        starts[rows] = offset;
        lengths[rows] = length;
        while (lengths[rows] && text[offset + lengths[rows] - 1] == ' ')
            --lengths[rows];
        ++rows;
        offset += length;
        if (text[offset] == '\n') ++offset;
        while (text[offset] == ' ') ++offset;
    }
    return rows;
}

static void draw_chat(GuiApp *app)
{
    bool open = app->screen == GUI_COMMAND;
    int width = GetScreenWidth() - 36;
    if (width > 760) width = 760;
    int bottom = GetScreenHeight() - 100;
    int visible = (bottom - 100)/CHAT_LINE_HEIGHT;
    int limit = open ? 20 : 6;
    if (visible > limit) visible = limit;
    if (width < 24 || visible <= 0) return;

    size_t starts[CHAT_TEXT_CAPACITY], lengths[CHAT_TEXT_CAPACITY];
    if (open) {
        int total = 0;
        for (size_t i = 0; i < app->chat_count; ++i)
            total += wrap_chat_message(app->chat[i].text, width - 12,
                                       starts, lengths);
        int maximum = total > visible ? total - visible : 0;
        if (app->chat_scroll > maximum) app->chat_scroll = maximum;
    }
    int skip = open ? app->chat_scroll : 0;
    int drawn = 0;
    double now = GetTime();
    for (size_t i = app->chat_count; i > 0 && drawn < visible; --i) {
        const ChatMessage *message = &app->chat[i - 1];
        if (!open && now - message->time >= 10.0) continue;
        int rows = wrap_chat_message(message->text, width - 12, starts, lengths);
        for (int row = rows - 1; row >= 0 && drawn < visible; --row) {
            if (skip) { --skip; continue; }
            char line[CHAT_TEXT_CAPACITY];
            memcpy(line, message->text + starts[row], lengths[row]);
            line[lengths[row]] = '\0';
            int y = bottom - (++drawn)*CHAT_LINE_HEIGHT;
            DrawRectangle(18, y - 2, width, CHAT_LINE_HEIGHT,
                          (Color){15, 27, 30, open ? 230 : 190});
            ui_text_draw(line, 24, y, CHAT_FONT_SIZE,
                     message->error ? (Color){255, 125, 125, 255} : RAYWHITE);
        }
    }
}

static void draw_command(const GuiApp *app)
{
    if (app->plot_command.pending) {
        int panel_width = GetScreenWidth() - 36;
        if (panel_width > 660) panel_width = 660;
        if (panel_width >= 48) {
            WorldPlotBounds b = app->plot_command.preview.removed;
            char warning[CHAT_TEXT_CAPACITY];
            snprintf(warning, sizeof(warning),
                     "Shrinking deletes cropped blocks and inventories, and may break connected blocks.\n"
                     "X %d..%d, Z %d..%d\nType y to confirm, or n to cancel.",
                     b.min_x,b.max_x,b.min_z,b.max_z);
            size_t starts[CHAT_TEXT_CAPACITY], lengths[CHAT_TEXT_CAPACITY];
            int rows = wrap_chat_message(warning, panel_width - 32, starts, lengths);
            int height = rows*CHAT_LINE_HEIGHT + 32;
            int x = (GetScreenWidth() - panel_width)/2, top = (GetScreenHeight() - height)/2;
            DrawRectangle(x,top,panel_width,height,(Color){15,27,30,245});
            DrawRectangleLines(x,top,panel_width,height,(Color){244,190,88,255});
            for (int i = 0; i < rows; ++i) {
                char line[CHAT_TEXT_CAPACITY];
                memcpy(line,warning + starts[i],lengths[i]); line[lengths[i]] = '\0';
                ui_text_draw(line,x + 16,top + 16 + i*CHAT_LINE_HEIGHT,CHAT_FONT_SIZE,
                         (Color){244,190,88,255});
            }
        }
    }
    int y = GetScreenHeight() - 88;
    int width = GetScreenWidth() - 36;
    DrawRectangle(18, y, width, 52,
                  (Color){15, 27, 30, 235});
    DrawRectangleLines(18, y, width, 52,
                       (Color){244, 190, 88, 255});
    const char *text = TextFormat("%s%s", app->plot_command.pending ? "y/n > " : "/", app->command);
    int text_x = 32;
    int cursor_x = text_x + ui_text_width(text, 21);
    int right = 18 + width - 14;
    if (cursor_x > right) {
        text_x -= cursor_x - right;
        cursor_x = right;
    }
    begin_gui_scissor(20, y + 2, width - 4, 48);
    ui_text_draw(text, text_x, y + 15, 21, RAYWHITE);
    if (((int)(GetTime()*2.0)) % 2 == 0)
        DrawLine(cursor_x, y + 11, cursor_x, y + 40, RAYWHITE);
    EndScissorMode();
}

static void draw_pause(GuiApp *app)
{
    bool active = IsWindowFocused();
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  (Color){10, 18, 20, 150});

    int center = GetScreenWidth()/2;
    Rectangle panel = {(float)center - 190, 105, 380, 330};
    DrawRectangleRec(panel, (Color){43, 43, 46, 255});
    DrawRectangleLinesEx(panel, 3.0f, (Color){84, 84, 91, 255});
    draw_centered("World menu", 137, 30, RAYWHITE);

    Rectangle resume = {(float)center - 140, 205, 280, 44};
    Rectangle save = {(float)center - 140, 265, 280, 44};
    Rectangle exit = {(float)center - 140, 325, 280, 44};
    if (button(resume, "Continue", active)) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
    } else if (button(save, "Save", active)) {
        save_world(app, false);
    } else if (button(exit, "Save and close", active)) {
        save_world(app, true);
    }
}

static void draw_save_error(GuiApp *app)
{
    bool active = IsWindowFocused();
    ClearBackground((Color){39, 48, 49, 255});
    draw_centered("SAVE FAILED", 130, 30, RAYWHITE);
    draw_centered(app->message, 185, 19, (Color){244, 190, 88, 255});

    int center = GetScreenWidth()/2;
    Rectangle retry = {(float)center - 160, 290, 320, 46};
    Rectangle cancel = {(float)center - 160, 352, 320, 46};
    Rectangle leave = {(float)center - 160, 414, 320, 46};
    if (button(retry, "Retry save", active)) {
        save_world(app, app->close_after_save);
    } else if (button(cancel, "Return", active)) {
        app->screen = app->screen_before_error;
        if (app->screen == GUI_WORLD) capture_mouse(app);
    } else if (app->close_after_save && button(leave, "Exit without saving", active)) {
        app->running = false;
    }
}

static bool initialize(GuiApp *app, GameSession *session, const char *world_name)
{
    *app = (GuiApp){.session = session, .running = true};
    if (!app_config_init()) {
        fprintf(stderr, "%s\n", app_config_error());
        return false;
    }
    const AppSettings *settings = app_config_current();
    if (!*settings->resource_pack) {
        fputs("Set assets.resource_pack in the Voxilog config.toml to the absolute path "
              "of your Minecraft Java 1.16.2 client JAR or compatible ZIP. "
              "See README.md for the config location.\n", stderr);
        return false;
    }
    ResourcePack *pack = app->resource_pack = resource_pack_open(settings->resource_pack);
    if (!pack) {
        fprintf(stderr, "Cannot open the Minecraft resource pack at '%s'. "
                        "Check assets.resource_pack in config.toml.\n", settings->resource_pack);
        return false;
    }
    if (!prepare_world(app)) return false;
    unsigned int window_flags = FLAG_WINDOW_RESIZABLE;
    if (settings->vsync) window_flags |= FLAG_VSYNC_HINT;
#if defined(__APPLE__)
    window_flags |= FLAG_WINDOW_HIGHDPI;
#endif
    SetConfigFlags(window_flags);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, TextFormat("Voxilog - %s", world_name));
    if (!IsWindowReady()) {
        fprintf(stderr, "Cannot open the Voxilog window\n");
        return false;
    }
#if defined(__linux__)
    Image icon = LoadImage("res/icon.png");
    if (icon.data) {
        ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        SetWindowIcon(icon);
        UnloadImage(icon);
    } else {
        TraceLog(LOG_WARNING, "Cannot load Voxilog window icon");
    }
#endif
    SetWindowMinSize(1000, 650);
    SetExitKey(KEY_NULL);
    SetTargetFPS(settings->fps_limit);
    /* Java Edition plains sky tint at full daylight. */
    app->sky_color = (Color){120, 167, 255, 255};
    TraceLog(LOG_INFO, "RESOURCE PACK: Loading Minecraft assets from '%s'", settings->resource_pack);
    if (!ui_font_init(pack) || !block_renderer_init(&app->blocks, pack) ||
        !inventory_init(&app->inventory, pack)) {
        fprintf(stderr, "Cannot load the Minecraft assets from '%s'. "
                        "Use the 1.16.2 client JAR or a ZIP containing the required assets.\n",
                settings->resource_pack);
        return false;
    }
    if (!hotbar_init(&app->hotbar, &app->inventory)) {
        fputs("Cannot initialize the hotbar\n", stderr);
        return false;
    }
    app->world_renderer = world_renderer_create(game_session_world(session), &app->blocks);
    if (!app->world_renderer || !world_renderer_sync(app->world_renderer)) {
        fprintf(stderr, "Cannot create the world render cache\n");
        return false;
    }
    app->screen = GUI_WORLD;
    capture_mouse(app);
    return true;
}

static void shutdown_gui(GuiApp *app)
{
    if (!IsWindowReady()) {
        resource_pack_close(app->resource_pack);
        return;
    }
    release_mouse(app);
    world_msaa_destroy(&app->world_msaa);
    world_renderer_destroy(app->world_renderer);
    if (app->gui_target.id) UnloadRenderTexture(app->gui_target);
    hotbar_unload(&app->hotbar);
    inventory_unload(&app->inventory);
    block_renderer_unload(&app->blocks);
    ui_font_unload();
    resource_pack_close(app->resource_pack);
    CloseWindow();
}

static bool prepare_gui_view(GuiApp *app)
{
    int width = GetRenderWidth(), height = GetRenderHeight();
    if (width <= 0 || height <= 0 || GetScreenHeight() <= 0) return false;
    if (!app->gui_target.id || app->gui_target.texture.width != width ||
        app->gui_target.texture.height != height) {
        RenderTexture2D target = LoadRenderTexture(width, height);
        if (!target.id || !target.texture.id || !rlFramebufferComplete(target.id)) {
            if (target.id) UnloadRenderTexture(target);
            fprintf(stderr, "Cannot create the world view render target\n");
            app->fatal_error = true;
            app->running = false;
            return false;
        }
        SetTextureFilter(target.texture, TEXTURE_FILTER_POINT);
        if (app->gui_target.id) UnloadRenderTexture(app->gui_target);
        app->gui_target = target;
    }
    world_msaa_prepare(&app->world_msaa, width, height,
                       app_config_current()->msaa_samples);
    return true;
}

static void pause_world(GuiApp *app)
{
    if (app->screen == GUI_BARREL)
        (void)world_close_barrel(game_session_world(app->session), app->barrel_pos);
    if (app->screen == GUI_COMMAND && app->plot_command.pending) {
        (void)plot_command_answer(game_session_world(app->session), &app->plot_command,
                                   "n", app->message, sizeof(app->message));
        add_chat_message(app, app->message, false);
    }
    release_mouse(app);
    app->screen = GUI_PAUSE;
    game_session_reset_clock(app->session);
}

static VoxelHit update_input(GuiApp *app)
{
    app_input_frame();
    if (!IsWindowFocused()) {
        if (app->screen != GUI_PAUSE && app->screen != GUI_SAVE_ERROR)
            pause_world(app);
        return (VoxelHit){0};
    }

    switch (app->screen) {
    case GUI_WORLD:
        return update_world(app);
    case GUI_COMMAND:
        update_command(app);
        break;
    case GUI_INVENTORY:
        update_inventory(app);
        break;
    case GUI_BARREL:
        update_barrel(app);
        break;
    case GUI_HOPPER:
    case GUI_DROPPER:
        update_container(app);
        break;
    case GUI_PAUSE:
        if (app_input_world("pause", false)) {
            app->screen = GUI_WORLD;
            capture_mouse(app);
        }
        break;
    case GUI_SAVE_ERROR:
        break;
    }
    return (VoxelHit){0};
}

static void draw_view(GuiApp *app, VoxelHit hit)
{
    begin_gui_target(app->gui_target);
    if (app->screen == GUI_SAVE_ERROR) {
        draw_save_error(app);
    } else {
        draw_world(app, hit);
        if (app->screen == GUI_INVENTORY) {
            inventory_draw(&app->inventory, hotbar_selected(&app->hotbar));
            hotbar_draw(&app->hotbar, &app->inventory);
        }
        if (app->screen == GUI_BARREL) draw_container(app, BLOCK_BARREL);
        if (app->screen == GUI_HOPPER) draw_container(app, BLOCK_HOPPER);
        if (app->screen == GUI_DROPPER) draw_container(app, BLOCK_DROPPER);
        if (app->screen == GUI_WORLD || app->screen == GUI_COMMAND) draw_chat(app);
        if (app->screen == GUI_COMMAND) draw_command(app);
        if (app->screen == GUI_PAUSE) draw_pause(app);
    }
    EndTextureMode();

    Texture2D texture = app->gui_target.texture;
    DrawTexturePro(texture, (Rectangle){0, 0, (float)texture.width, -(float)texture.height},
        (Rectangle){0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
        (Vector2){0}, 0, WHITE);
}

int gui_run(GameSession *session, const char *world_name)
{
    GuiApp app;
    if (!initialize(&app, session, world_name)) {
        shutdown_gui(&app);
        return EXIT_FAILURE;
    }

    while (app.running) {
        bool close_requested = WindowShouldClose();
        if (close_requested && !app.close_latched && app.screen != GUI_SAVE_ERROR) {
            if (app.screen == GUI_BARREL) {
                (void)world_close_barrel(game_session_world(session), app.barrel_pos);
                app.screen = GUI_WORLD;
            }
            if (game_session_dirty(session)) save_world(&app, true);
            else app.running = false;
        }
        app.close_latched = close_requested;
        if (!app.running) break;

        VoxelHit hit = update_input(&app);
        if (!app.running) break;
        if (app.screen != GUI_SAVE_ERROR) advance_world(&app);
        if (!app.running) break;

        if (!world_renderer_sync(app.world_renderer)) {
            fprintf(stderr, "Cannot update render cache\n");
            app.fatal_error = true;
            break;
        }

        bool view_ready = prepare_gui_view(&app);
        if (!app.running) break;
        BeginDrawing();
        if (view_ready) draw_view(&app, hit);
        EndDrawing();
    }

    if (app.fatal_error && game_session_dirty(session)) {
        GameSessionResult result = game_session_save(session);
        if (result != GAME_SESSION_OK)
            fprintf(stderr, "Cannot save world: %s\n", game_session_result_name(result));
    }
    shutdown_gui(&app);
    return app.fatal_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
