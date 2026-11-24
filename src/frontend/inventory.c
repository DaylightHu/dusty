#include "inventory.h"
#include "minecraft_model.h"
#include "ui_colors.h"
#include "ui_font.h"

#include <stddef.h>

enum {
    INVENTORY_COLUMNS = 8,
    INVENTORY_ROWS = 4,
    INVENTORY_SLOT_SIZE = 58,
    INVENTORY_SLOT_GAP = 4,
    INVENTORY_PANEL_PADDING = 18,
    INVENTORY_HEADER_HEIGHT = 52,
    INVENTORY_FOOTER_HEIGHT = 32
};

_Static_assert(INVENTORY_COLUMNS*INVENTORY_ROWS == INVENTORY_SLOT_COUNT,
        "inventory grid must match slot capacity");

typedef struct {
    BlockId id;
    const char *minecraft_model;
} BlockTexture;

typedef struct {
    Rectangle panel;
    float grid_x;
    float grid_y;
} InventoryLayout;

static const BlockTexture block_textures[] = {
    {BLOCK_IRON, "white_concrete"},
    {BLOCK_BLUE_CONCRETE, "blue_concrete"},
    {BLOCK_GRAY_CONCRETE, "gray_concrete"},
    {BLOCK_GREEN_CONCRETE, "green_concrete"},
    {BLOCK_PURPLE_CONCRETE, "purple_concrete"},
    {BLOCK_YELLOW_CONCRETE, "yellow_concrete"},
    {BLOCK_GLASS, "glass"},
    {BLOCK_REDSTONE_BLOCK, "redstone_block"},
    {BLOCK_REDSTONE_WIRE, "redstone"},
    {BLOCK_REDSTONE_TORCH, "redstone_torch"},
    {BLOCK_REDSTONE_REPEATER, "repeater"},
    {BLOCK_REDSTONE_COMPARATOR, "comparator"},
    {BLOCK_POWERED_RAIL, "powered_rail"},
    {BLOCK_ACTIVATOR_RAIL, "activator_rail"},
    {BLOCK_OBSERVER, "observer"},
    {BLOCK_PISTON, "piston"},
    {BLOCK_STICKY_PISTON, "sticky_piston"},
    {BLOCK_BARREL, "barrel"},
    {BLOCK_SLIME, "slime_block"},
    {BLOCK_HONEY, "honey_block"},
    {BLOCK_NOTE_BLOCK, "note_block"},
    {BLOCK_TARGET, "target"},
    {BLOCK_HOPPER, "hopper"},
    {BLOCK_DROPPER, "dropper"},
    {BLOCK_REDSTONE_LAMP, "redstone_lamp"},
    {BLOCK_LEVER, "lever"}
};

static const char *const minecraft_icons[BLOCK_COUNT] = {
    [BLOCK_IRON] = "block/white_concrete.png",
    [BLOCK_BLUE_CONCRETE] = "block/blue_concrete.png",
    [BLOCK_GRAY_CONCRETE] = "block/gray_concrete.png",
    [BLOCK_GREEN_CONCRETE] = "block/green_concrete.png",
    [BLOCK_PURPLE_CONCRETE] = "block/purple_concrete.png",
    [BLOCK_YELLOW_CONCRETE] = "block/yellow_concrete.png",
    [BLOCK_GLASS] = "block/glass.png",
    [BLOCK_REDSTONE_BLOCK] = "block/redstone_block.png",
    [BLOCK_REDSTONE_WIRE] = "item/redstone.png",
    [BLOCK_REDSTONE_TORCH] = "block/redstone_torch.png",
    [BLOCK_REDSTONE_REPEATER] = "item/repeater.png",
    [BLOCK_REDSTONE_COMPARATOR] = "item/comparator.png",
    [BLOCK_HOPPER] = "item/hopper.png"
};

static Image load_icon(const ResourcePack *pack, const char *model, const char *texture)
{
    Image image = minecraft_item_image(pack, model);
    if (!image.data) image = resource_pack_image(pack, texture);
    if (!image.data)
        TraceLog(LOG_ERROR, "INVENTORY: Cannot load %s from the resource pack", model);
    return image;
}

static InventoryLayout inventory_layout(void)
{
    const int grid_width =
        INVENTORY_COLUMNS*INVENTORY_SLOT_SIZE +
        (INVENTORY_COLUMNS - 1)*INVENTORY_SLOT_GAP;
    const int grid_height =
        INVENTORY_ROWS*INVENTORY_SLOT_SIZE +
        (INVENTORY_ROWS - 1)*INVENTORY_SLOT_GAP;
    const int panel_width = grid_width + 2*INVENTORY_PANEL_PADDING;
    const int panel_height = INVENTORY_PANEL_PADDING +
        INVENTORY_HEADER_HEIGHT + grid_height +
        INVENTORY_FOOTER_HEIGHT + INVENTORY_PANEL_PADDING;
    InventoryLayout layout = {
        {
            (GetScreenWidth() - panel_width)*0.5f,
            (GetScreenHeight() - 80 - panel_height)*0.5f,
            (float)panel_width,
            (float)panel_height
        },
        0.0f,
        0.0f
    };
    layout.grid_x = layout.panel.x + INVENTORY_PANEL_PADDING;
    layout.grid_y = layout.panel.y + INVENTORY_PANEL_PADDING +
                    INVENTORY_HEADER_HEIGHT;
    return layout;
}

static Rectangle inventory_slot_rectangle(InventoryLayout layout, int index)
{
    int column = index%INVENTORY_COLUMNS;
    int row = index/INVENTORY_COLUMNS;
    return (Rectangle){
        layout.grid_x + column*(INVENTORY_SLOT_SIZE + INVENTORY_SLOT_GAP),
        layout.grid_y + row*(INVENTORY_SLOT_SIZE + INVENTORY_SLOT_GAP),
        INVENTORY_SLOT_SIZE,
        INVENTORY_SLOT_SIZE
    };
}

bool inventory_init(Inventory *inventory, const ResourcePack *pack)
{
    if (!inventory) return false;
    *inventory = (Inventory){0};

    size_t count = sizeof(block_textures)/sizeof(block_textures[0]);
    if (count + 2 > INVENTORY_SLOT_COUNT) {
        TraceLog(LOG_ERROR, "INVENTORY: Too many item textures (%d > %d)",
                 (int)count + 2, INVENTORY_SLOT_COUNT);
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        BlockTexture item = block_textures[index];
        Image icon = load_icon(pack, item.minecraft_model, minecraft_icons[item.id]);
        Texture2D texture = LoadTextureFromImage(icon);
        if (icon.data) UnloadImage(icon);
        if (!texture.id) {
            TraceLog(LOG_ERROR, "INVENTORY: Cannot create the %s texture", item.minecraft_model);
            inventory_unload(inventory);
            return false;
        }
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        inventory->block_textures[item.id] = texture;
        inventory->slots[index] = (InventorySlot){
            .kind = INVENTORY_BLOCK, .block = item.id
        };
    }
    Image potato = load_icon(pack, "poisonous_potato", "item/poisonous_potato.png");
    if (!potato.data) {
        inventory_unload(inventory);
        return false;
    }
    ImageAlphaCrop(&potato, 0.0f);
    Texture2D texture = LoadTextureFromImage(potato);
    UnloadImage(potato);
    if (!texture.id) {
        inventory_unload(inventory);
        return false;
    }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    inventory->item_textures[ITEM_POISONOUS_POTATO] = texture;
    inventory->slots[count] = (InventorySlot){
        .kind = INVENTORY_ITEM,
        .stack = {ITEM_POISONOUS_POTATO, 1,
                  item_stack_limit(ITEM_POISONOUS_POTATO)}
    };
    Image axe = load_icon(pack, "wooden_axe", "item/wooden_axe.png");
    if (!axe.data) { inventory_unload(inventory); return false; }
    ImageAlphaCrop(&axe, 0.0f);
    texture = LoadTextureFromImage(axe);
    UnloadImage(axe);
    if (!texture.id) { inventory_unload(inventory); return false; }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    inventory->tool_textures[TOOL_WOODEN_AXE] = texture;
    inventory->slots[count + 1] = (InventorySlot){
        .kind = INVENTORY_TOOL, .tool = TOOL_WOODEN_AXE
    };
    inventory->count = (int)count + 2;
    return true;
}

void inventory_unload(Inventory *inventory)
{
    if (!inventory) return;
    for (int id = 0; id < BLOCK_COUNT; ++id) {
        if (inventory->block_textures[id].id)
            UnloadTexture(inventory->block_textures[id]);
    }
    for (int id = 0; id < ITEM_COUNT; ++id) {
        if (inventory->item_textures[id].id)
            UnloadTexture(inventory->item_textures[id]);
    }
    for (int id = 0; id < TOOL_COUNT; ++id) {
        if (inventory->tool_textures[id].id)
            UnloadTexture(inventory->tool_textures[id]);
    }
    *inventory = (Inventory){0};
}

static void draw_icon(Texture2D texture, Rectangle slot)
{
    if (!texture.id) return;

    const float padding = 5.0f;
    float scale_x = (slot.width - 2.0f*padding)/(float)texture.width;
    float scale_y = (slot.height - 2.0f*padding)/(float)texture.height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    float width = texture.width*scale;
    float height = texture.height*scale;
    Rectangle source = {0, 0, (float)texture.width, (float)texture.height};
    Rectangle destination = {
        slot.x + (slot.width - width)*0.5f,
        slot.y + (slot.height - height)*0.5f,
        width,
        height
    };
    DrawTexturePro(texture, source, destination, (Vector2){0, 0}, 0.0f, WHITE);
}

void inventory_draw_stack(const Inventory *inventory, ItemStack stack,
                           Rectangle slot)
{
    if (!inventory || !stack.count) return;
    Texture2D texture = stack.item < ITEM_COUNT ?
                        inventory->item_textures[stack.item] : (Texture2D){0};
    if (texture.id) draw_icon(texture, slot);
    else ui_text_draw("?", (int)slot.x + 18, (int)slot.y + 10, 26, RAYWHITE);
    if (stack.count > 1) {
        const char *count = TextFormat("%u", (unsigned)stack.count);
        int x = (int)(slot.x + slot.width) - ui_text_width(count, 18) - 4;
        int y = (int)(slot.y + slot.height) - 22;
        ui_text_draw(count, x + 1, y + 1, 18, BLACK);
        ui_text_draw(count, x, y, 18, RAYWHITE);
    }
}

void inventory_draw_slot(const Inventory *inventory, InventorySlot contents,
                          Rectangle slot)
{
    if (!inventory) return;
    if (contents.kind == INVENTORY_BLOCK && contents.block > BLOCK_AIR &&
        contents.block < BLOCK_COUNT)
        draw_icon(inventory->block_textures[contents.block], slot);
    else if (contents.kind == INVENTORY_ITEM)
        inventory_draw_stack(inventory, contents.stack, slot);
    else if (contents.kind == INVENTORY_TOOL && contents.tool > TOOL_NONE && contents.tool < TOOL_COUNT)
        draw_icon(inventory->tool_textures[contents.tool], slot);
}

const char *inventory_slot_name(InventorySlot slot)
{
    if (slot.kind == INVENTORY_BLOCK) return block_name(slot.block);
    if (slot.kind == INVENTORY_ITEM) return item_name(slot.stack.item);
    if (slot.kind == INVENTORY_TOOL && slot.tool == TOOL_WOODEN_AXE)
        return "Wooden axe (selection)";
    return "Empty";
}

InventorySlot inventory_update(const Inventory *inventory)
{
    bool left = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    bool right = IsMouseButtonReleased(MOUSE_BUTTON_RIGHT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!inventory || (!left && !(shift && right)))
        return (InventorySlot){0};

    InventoryLayout layout = inventory_layout();
    Vector2 mouse = GetMousePosition();
    for (int index = 0; index < inventory->count; ++index) {
        Rectangle slot = inventory_slot_rectangle(layout, index);
        if (!CheckCollisionPointRec(mouse, slot)) continue;
        InventorySlot picked = inventory->slots[index];
        if (picked.kind == INVENTORY_ITEM) {
            picked.stack.count = !shift ? 1 :
                                 (left ? picked.stack.limit : picked.stack.limit/2);
            return picked;
        }
        if (left) return picked;
    }
    return (InventorySlot){0};
}

void inventory_draw(const Inventory *inventory, InventorySlot selected)
{
    if (!inventory) return;

    InventoryLayout layout = inventory_layout();
    Vector2 mouse = GetMousePosition();
    InventorySlot hovered = {0};

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  (Color){10, 18, 20, 155});
    DrawRectangleRec(layout.panel, UI_PANEL_FILL);
    DrawRectangleLinesEx(layout.panel, 3.0f, UI_PANEL_BORDER);

    const char *title = "INVENTORY";
    int title_size = 25;
    ui_text_draw(title,
             (int)(layout.panel.x +
                   (layout.panel.width - ui_text_width(title, title_size))*0.5f),
             (int)layout.panel.y + 17, title_size, RAYWHITE);

    for (int index = 0; index < INVENTORY_SLOT_COUNT; ++index) {
        Rectangle slot = inventory_slot_rectangle(layout, index);
        bool occupied = index < inventory->count;
        bool under_mouse = CheckCollisionPointRec(mouse, slot);
        InventorySlot contents = occupied ? inventory->slots[index] :
                                            (InventorySlot){0};
        Color fill = under_mouse && occupied ? UI_SLOT_HOVER : UI_SLOT_FILL;
        DrawRectangleRec(slot, fill);
        DrawRectangleLinesEx(slot, 1.0f, UI_SLOT_BORDER);
        if (occupied) inventory_draw_slot(inventory, contents, slot);
        bool same = contents.kind == selected.kind &&
                    ((contents.kind == INVENTORY_BLOCK &&
                      contents.block == selected.block) ||
                     (contents.kind == INVENTORY_ITEM &&
                      contents.stack.item == selected.stack.item) ||
                     (contents.kind == INVENTORY_TOOL &&
                      contents.tool == selected.tool));
        if (same)
            DrawRectangleLinesEx(slot, 3.0f, UI_ACCENT);
        if (under_mouse && occupied) hovered = contents;
    }

    if (hovered.kind != INVENTORY_EMPTY) {
        const char *label = inventory_slot_name(hovered);
        int label_size = 19;
        int label_y = (int)(layout.grid_y +
            INVENTORY_ROWS*INVENTORY_SLOT_SIZE +
            (INVENTORY_ROWS - 1)*INVENTORY_SLOT_GAP + 8);
        ui_text_draw(label,
                 (int)(layout.panel.x +
                       (layout.panel.width - ui_text_width(label, label_size))*0.5f),
                 label_y, label_size, RAYWHITE);
    }
}
