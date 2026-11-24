#include "world_renderer.h"
#include "minecraft_redstone.h"
#include "raymath.h"
#include "rlgl.h"
#include "storage.h"
#include "posmap.h"
#include "hopper_shape.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { REGION_SIZE = 16 };
enum {
    SOLIDS, IRON, GLASS, DUST, REPEATERS, COMPARATORS,
    POWERED_RAILS, ACTIVATOR_RAILS, TORCHES, LEVERS, TRANSLUCENT, PARTS
};

typedef struct {
    BlockState state;
    uint32_t first_vertex;
    uint8_t solid_faces;
    bool pending;
} RenderCell;

typedef struct {
    Mesh mesh;
    unsigned uv_first, uv_end;
    int vertices_per_quad;
    bool dirty, rebuilt;
} RegionPart;

typedef struct {
    RegionPart parts[PARTS];
    BlockPos origin;
    RenderCell cells[SECTION_VOLUME];
    uint16_t pending[SECTION_VOLUME];
    size_t pending_count;
    bool seen;
} RenderRegion;

struct WorldRenderer {
    World *world;
    const BlockRenderer *blocks;
    PosMap region_map;
    size_t region_count, region_capacity;
    RenderRegion **regions;
    uint64_t topology_revision;
    WorldPlotBounds plot;
    bool failed;
    Material materials[PARTS];
};

static int part_for(unsigned id)
{
    switch (id) {
    case BLOCK_IRON: return IRON;
    case BLOCK_REDSTONE_BLOCK: case BLOCK_OBSERVER:
    case BLOCK_BLUE_CONCRETE: case BLOCK_GRAY_CONCRETE:
    case BLOCK_GREEN_CONCRETE: case BLOCK_PURPLE_CONCRETE:
    case BLOCK_YELLOW_CONCRETE:
    case BLOCK_PISTON: case BLOCK_STICKY_PISTON: case BLOCK_BARREL:
    case BLOCK_NOTE_BLOCK:
    case BLOCK_TARGET: case BLOCK_HOPPER: case BLOCK_DROPPER:
    case BLOCK_REDSTONE_LAMP:
    case BLOCK_PISTON_HEAD: case BLOCK_STICKY_PISTON_HEAD:
        return SOLIDS;
    case BLOCK_GLASS: return GLASS;
    case BLOCK_SLIME: case BLOCK_HONEY: return TRANSLUCENT;
    case BLOCK_REDSTONE_WIRE: return DUST;
    case BLOCK_REDSTONE_REPEATER: return REPEATERS;
    case BLOCK_REDSTONE_COMPARATOR: return COMPARATORS;
    case BLOCK_POWERED_RAIL: return POWERED_RAILS;
    case BLOCK_ACTIVATOR_RAIL: return ACTIVATOR_RAILS;
    case BLOCK_REDSTONE_TORCH: return TORCHES;
    case BLOCK_LEVER: return LEVERS;
    default: return -1;
    }
}

static bool solid_part(int kind)
{
    return kind == SOLIDS || kind == IRON;
}

static bool cube_part(int kind)
{
    return solid_part(kind) || kind == GLASS || kind == TRANSLUCENT;
}

static void dirty_solid_parts(RenderRegion *region)
{
    region->parts[SOLIDS].dirty = true;
    region->parts[IRON].dirty = true;
    region->parts[TRANSLUCENT].dirty = true;
}

static bool state_solid(BlockState state)
{
    if (!solid_part(part_for(state.id))) return false;
    /* Vanilla honey uses noOcclusion and an inset collision shape. */
    if (state.id == BLOCK_HONEY || state.id == BLOCK_HOPPER) return false;
    if (block_is_piston_head((BlockId)state.id)) return false;
    return !block_is_piston_base((BlockId)state.id) ||
           !state.data.piston.extended;
}

static bool state_cube(BlockState state)
{
    return state_solid(state) || state.id == BLOCK_GLASS;
}

static BlockPos region_origin_for(BlockPos p)
{
    return (BlockPos){floor_div16(p.x) * 16, floor_div16(p.y) * 16, floor_div16(p.z) * 16};
}
static RenderRegion *find_region(const WorldRenderer *r, BlockPos p)
{
    size_t slot;
    return posmap_get(&r->region_map, region_origin_for(p), &slot) ? r->regions[slot] : NULL;
}
static bool solid_at(const WorldRenderer *r, BlockPos p)
{
    return state_solid(world_get(r->world, p));
}

static bool cube_at(const WorldRenderer *r, BlockPos p)
{
    return state_cube(world_get(r->world, p));
}

static unsigned wire_sides(BlockState state)
{
    const unsigned horizontal = (1u << DIR_EAST) | (1u << DIR_WEST) |
                                (1u << DIR_SOUTH) | (1u << DIR_NORTH);
    return state.data.wire.connections & state.data.wire.up_connections & horizontal;
}

static void on_change(void *context, BlockPos p, BlockState before, BlockState after)
{
    WorldRenderer *r = context;
    size_t index = section_index(p);
    RenderRegion *region = find_region(r,p);
    int old_part = part_for(before.id), new_part = part_for(after.id);
    /* New sections are discovered during sync, even if no cache exists yet. */
    bool solid_changed = state_solid(before) != state_solid(after);
    bool cube_changed = state_cube(before) != state_cube(after);
    bool translucent_changed = before.id != after.id &&
        (old_part == TRANSLUCENT || new_part == TRANSLUCENT);
    if (solid_changed || cube_changed || translucent_changed) {
        for (Direction face = 0; face < DIR_COUNT; ++face) {
            RenderRegion *neighbor = find_region(r,block_offset(p,face));
            if (!neighbor) continue;
            if (solid_changed) dirty_solid_parts(neighbor);
            if (cube_changed) neighbor->parts[GLASS].dirty = true;
            if (translucent_changed) neighbor->parts[TRANSLUCENT].dirty = true;
        }
    }
    if (!region) return;
    RenderCell *cell = &region->cells[index];
    bool geometry = old_part != new_part || translucent_changed;
    bool visual = before.id != after.id;
    const BlockGeometry *current_model = minecraft_redstone_geometry(r->blocks->minecraft, after);
    if (old_part == new_part && current_model) {
        visual = current_model != minecraft_redstone_geometry(r->blocks->minecraft, before);
    } else if (old_part == new_part && new_part == DUST) {
        geometry = wire_sides(before) != wire_sides(after);
        visual = geometry || before.data.wire.connections != after.data.wire.connections ||
                 before.data.wire.power != after.data.wire.power;
    } else if (old_part == new_part && new_part == TORCHES) {
        geometry = before.data.torch.support_dir != after.data.torch.support_dir;
        visual = geometry || before.data.torch.lit != after.data.torch.lit;
    } else if (old_part == new_part && new_part == LEVERS) {
        geometry = before.data.lever.support_dir != after.data.lever.support_dir ||
                   before.data.lever.facing != after.data.lever.facing;
        visual = geometry ||
                 before.data.lever.powered != after.data.lever.powered;
    } else if (old_part == new_part && new_part == REPEATERS) {
        visual = before.data.repeater.facing != after.data.repeater.facing ||
                 before.data.repeater.delay != after.data.repeater.delay ||
                 before.data.repeater.powered != after.data.repeater.powered;
    } else if (old_part == new_part && new_part == COMPARATORS) {
        visual = before.data.comparator.facing != after.data.comparator.facing ||
                 before.data.comparator.subtract != after.data.comparator.subtract ||
                 before.data.comparator.powered != after.data.comparator.powered;
    } else if (old_part == new_part &&
               (new_part == POWERED_RAILS || new_part == ACTIVATOR_RAILS)) {
        geometry = before.data.rail.facing != after.data.rail.facing ||
                   before.data.rail.ascending != after.data.rail.ascending;
        visual = geometry ||
                 before.data.rail.powered != after.data.rail.powered;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == BLOCK_OBSERVER && after.id == BLOCK_OBSERVER) {
        visual = before.data.observer.facing != after.data.observer.facing ||
                 before.data.observer.powered != after.data.observer.powered;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == after.id &&
               block_is_piston_base((BlockId)before.id)) {
        geometry = before.data.piston.facing != after.data.piston.facing ||
                   before.data.piston.extended != after.data.piston.extended;
        visual = geometry ||
                 before.data.piston.powered != after.data.piston.powered;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == after.id &&
               block_is_piston_head((BlockId)before.id)) {
        geometry = before.data.piston.facing != after.data.piston.facing;
        visual = geometry ||
                 before.data.piston.powered != after.data.piston.powered;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == BLOCK_HOPPER && after.id == BLOCK_HOPPER) {
        geometry = before.data.hopper.facing != after.data.hopper.facing;
        visual = geometry;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == BLOCK_DROPPER && after.id == BLOCK_DROPPER) {
        visual = before.data.dropper.facing != after.data.dropper.facing ||
                 before.data.dropper.triggered != after.data.dropper.triggered;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == BLOCK_REDSTONE_LAMP && after.id == BLOCK_REDSTONE_LAMP) {
        visual = before.data.lamp.lit != after.data.lamp.lit;
    } else if (old_part == new_part && new_part == SOLIDS &&
               before.id == BLOCK_BARREL && after.id == BLOCK_BARREL) {
        visual = before.data.barrel.facing != after.data.barrel.facing;
    }
    cell->state = after;
    if (!visual && !geometry) return;

    if (geometry) {
        if (old_part >= 0) region->parts[old_part].dirty = true;
        if (new_part >= 0) region->parts[new_part].dirty = true;
        cell->first_vertex = UINT32_MAX;
    }
    if (!cell->pending) {
        cell->pending = true;
        region->pending[region->pending_count++] = (uint16_t)index;
    }
}

static unsigned visible_faces(const WorldRenderer *r, BlockPos p)
{
    static const BlockPos offsets[] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    unsigned mask = 0;
    for (int face = 0; face < 6; ++face) {
        BlockPos n = {p.x+offsets[face].x,p.y+offsets[face].y,p.z+offsets[face].z};
        if (!solid_at(r,n)) mask |= 1u << face;
    }
    return mask;
}


static unsigned visible_glass_faces(const WorldRenderer *r, BlockPos p)
{
    static const BlockPos offsets[] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    unsigned mask = 0;
    for (int face = 0; face < 6; ++face) {
        BlockPos n = {p.x+offsets[face].x,p.y+offsets[face].y,p.z+offsets[face].z};
        if (!cube_at(r,n)) mask |= 1u << face;
    }
    return mask;
}

static unsigned visible_translucent_faces(const WorldRenderer *r, BlockPos p,
                                          BlockId id)
{
    static const BlockPos offsets[] = {
        {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}
    };
    unsigned mask = 0;
    for (int face = 0; face < 6; ++face) {
        BlockPos n = {p.x+offsets[face].x, p.y+offsets[face].y,
                      p.z+offsets[face].z};
        if (!solid_at(r,n) && world_get(r->world,n).id != id)
            mask |= 1u << face;
    }
    return mask;
}

static unsigned bit_count(unsigned bits)
{
    unsigned count = 0;
    for (; bits; bits &= bits-1) ++count;
    return count;
}

static bool rebuild_part(WorldRenderer *r, size_t region_id, int kind)
{
    RenderRegion *region = r->regions[region_id];
    RegionPart *part = &region->parts[kind];
    BlockPos origin = region->origin;
    int end_x = origin.x+REGION_SIZE, end_y = origin.y+REGION_SIZE, end_z = origin.z+REGION_SIZE;
    int quads = 0;
    for (int y = origin.y; y < end_y; ++y)
    for (int z = origin.z; z < end_z; ++z)
    for (int x = origin.x; x < end_x; ++x) {
        BlockPos p = {x,y,z};
        RenderCell *cell = &region->cells[section_index(p)];
        if (part_for(cell->state.id) != kind) continue;
        const BlockGeometry *baked = minecraft_redstone_geometry(r->blocks->minecraft, cell->state);
        if (baked) quads += baked->quads;
        else if (cube_part(kind)) {
            cell->solid_faces = (uint8_t)(
                kind == GLASS ? visible_glass_faces(r,p) :
                kind == TRANSLUCENT ? visible_translucent_faces(r,p,(BlockId)cell->state.id) :
                visible_faces(r,p));
            quads += cell->state.id == BLOCK_HOPPER ? HOPPER_QUAD_COUNT :
                     (int)bit_count(cell->solid_faces);
            if (block_is_piston_head((BlockId)cell->state.id)) quads += 4;
        } else if (kind == DUST) quads += 1+(int)bit_count(wire_sides(cell->state));
        else if (kind == REPEATERS || kind == COMPARATORS)
            quads += REDSTONE_GATE_GEOMETRY_QUADS;
        else if (kind == POWERED_RAILS || kind == ACTIVATOR_RAILS)
            quads += 1;
        else if (kind == LEVERS) quads += 1;
        else quads += 6;
    }

    Mesh mesh = {0};
    /* Ordinary regions use indexed quads. Extremely dense cutout regions
     * fall back to non-indexed triangles, avoiding 16-bit index overflow. */
    int stride = quads*4 <= UINT16_MAX ? 4 : 6;
    mesh.vertexCount = quads*stride;
    mesh.triangleCount = quads*2;
    if (quads) {
        mesh.vertices = MemAlloc((unsigned)mesh.vertexCount*3*sizeof(float));
        mesh.texcoords = MemAlloc((unsigned)mesh.vertexCount*2*sizeof(float));
        if (stride == 4) mesh.indices = MemAlloc((unsigned)quads*6*sizeof(unsigned short));
        if (!mesh.vertices || !mesh.texcoords || (stride == 4 && !mesh.indices)) {
            UnloadMesh(mesh);
            return false;
        }
        static const int triangles[] = {0,1,2,0,2,3};
        int next = 0;
        for (int y = origin.y; y < end_y; ++y)
        for (int z = origin.z; z < end_z; ++z)
        for (int x = origin.x; x < end_x; ++x) {
            BlockPos p = {x,y,z};
            RenderCell *cell = &region->cells[section_index(p)];
            if (part_for(cell->state.id) != kind) continue;
            BlockGeometry geometry;
            BlockState state = cell->state;
            block_renderer_build_geometry(r->blocks,state,(Vector3){(float)x,(float)y,(float)z},cell->solid_faces,&geometry);
            cell->first_vertex = (uint32_t)next;
            for (int face = 0; face < geometry.quads; ++face) {
                if (stride == 4)
                    for (int i = 0; i < 6; ++i)
                        mesh.indices[(next/4)*6+i] = (unsigned short)(next+triangles[i]);
                for (int i = 0; i < stride; ++i) {
                    int v = stride == 4 ? i : triangles[i];
                    Vector3 p3 = geometry.vertices[face][v];
                    Vector2 p2 = geometry.uv[face][v];
                    mesh.vertices[next*3] = p3.x;
                    mesh.vertices[next*3+1] = p3.y;
                    mesh.vertices[next*3+2] = p3.z;
                    mesh.texcoords[next*2] = p2.x;
                    mesh.texcoords[next*2+1] = p2.y;
                    ++next;
                }
            }
        }
        UploadMesh(&mesh,true);
        if (!mesh.vboId || !mesh.vboId[0] || !mesh.vboId[1]) {
            UnloadMesh(mesh);
            return false;
        }
        /* Positions stay on the GPU. Keep UVs for local/coalesced updates and
         * keep the index pointer required by raylib's indexed draw path. */
        if (kind != TRANSLUCENT) {
            MemFree(mesh.vertices);
            mesh.vertices = NULL;
        }
    }
    UnloadMesh(part->mesh);
    part->mesh = mesh;
    part->vertices_per_quad = stride;
    part->uv_first = UINT32_MAX;
    part->uv_end = 0;
    part->dirty = false;
    part->rebuilt = true;
    return true;
}

static void patch_cell(WorldRenderer *r, RenderRegion *region, uint16_t index)
{
    RenderCell *cell = &region->cells[index];
    int kind = part_for(cell->state.id);
    if (kind < 0 || cell->first_vertex == UINT32_MAX) return;
    RegionPart *part = &region->parts[kind];
    if (part->rebuilt || !part->mesh.texcoords) return;
    Vector2 uv[BLOCK_GEOMETRY_QUADS][4];
    BlockState state = cell->state;
    const BlockGeometry *baked = minecraft_redstone_geometry(r->blocks->minecraft, state);
    int quads = baked ? baked->quads :
        block_renderer_build_uv(r->blocks,state,cell->solid_faces,uv);
    unsigned first = cell->first_vertex;
    unsigned end = first+(unsigned)(quads*part->vertices_per_quad);
    static const int triangles[] = {0,1,2,0,2,3};
    float vertices[BLOCK_GEOMETRY_QUADS*6*3];
    float *point = vertices;
    Vector3 center = {region->origin.x+(index&15), region->origin.y+(index>>8),
                      region->origin.z+((index>>4)&15)};
    bool changed = false;
    unsigned next = first;
    for (int face = 0; face < quads; ++face)
        for (int i = 0; i < part->vertices_per_quad; ++i) {
            int v = part->vertices_per_quad == 4 ? i : triangles[i];
            Vector2 coord = baked ? baked->uv[face][v] : uv[face][v];
            if (baked) {
                Vector3 p = baked->vertices[face][v];
                *point++ = p.x + center.x;
                *point++ = p.y + center.y;
                *point++ = p.z + center.z;
            }
            float *target = &part->mesh.texcoords[next*2];
            if (target[0] != coord.x || target[1] != coord.y) {
                target[0] = coord.x;
                target[1] = coord.y;
                changed = true;
            }
            ++next;
        }
    if (baked) {
        int bytes = (int)((end-first)*3*sizeof(float));
        int offset = (int)(first*3*sizeof(float));
        UpdateMeshBuffer(part->mesh, 0, vertices, bytes, offset);
    }
    if (!changed) return;
    if (first < part->uv_first) part->uv_first = first;
    if (end > part->uv_end) part->uv_end = end;
}
static void destroy_region(RenderRegion *region)
{
    for (int kind = 0; kind < PARTS; ++kind) UnloadMesh(region->parts[kind].mesh);
    free(region);
}
static void visit_section(void *context, BlockPos origin)
{
    WorldRenderer *r = context;
    RenderRegion *region = find_region(r,origin);
    if (region) { region->seen = true; return; }
    if (r->failed) return;
    if (r->region_count == r->region_capacity) {
        size_t capacity = r->region_capacity ? r->region_capacity*2 : 32;
        if (capacity < r->region_capacity || capacity > SIZE_MAX/sizeof(RenderRegion *)) { r->failed = true; return; }
        RenderRegion **next = realloc(r->regions,capacity*sizeof(RenderRegion *));
        if (!next) { r->failed = true; return; }
        r->regions = next; r->region_capacity = capacity;
    }
    region = calloc(1,sizeof(*region));
    if (!region) { r->failed = true; return; }
    region->origin = origin; region->seen = true;
    for (size_t i = 0; i < SECTION_VOLUME; ++i) {
        BlockPos p = {origin.x+(int)(i&15),origin.y+(int)(i>>8),origin.z+(int)((i>>4)&15)};
        region->cells[i].state = world_get(r->world,p);
        region->cells[i].first_vertex = UINT32_MAX;
    }
    for (int kind = 0; kind < PARTS; ++kind) region->parts[kind].dirty = true;
    if (!posmap_set(&r->region_map,origin,r->region_count)) { free(region); r->failed = true; return; }
    r->regions[r->region_count++] = region;
}
WorldRenderer *world_renderer_create(World *world, const BlockRenderer *blocks)
{
    if (!world || !blocks) return NULL;
    WorldRenderer *r = calloc(1,sizeof(*r));
    if (!r) return NULL;
    r->blocks = blocks; r->world = world; r->topology_revision = UINT64_MAX;
    Texture2D textures[] = {
        blocks->atlas, blocks->iron_texture, blocks->glass_texture, blocks->wire_atlas,
        blocks->repeater_atlas, blocks->comparator_atlas,
        blocks->powered_rail_atlas, blocks->activator_rail_atlas,
        blocks->torch_atlas, blocks->lever_atlas, blocks->atlas
    };
    if (minecraft_redstone_has(blocks->minecraft, BLOCK_LEVER))
        textures[LEVERS] = blocks->minecraft_atlas;
    if (minecraft_redstone_has(blocks->minecraft, BLOCK_REDSTONE_REPEATER))
        textures[REPEATERS] = blocks->minecraft_atlas;
    if (minecraft_redstone_has(blocks->minecraft, BLOCK_REDSTONE_COMPARATOR))
        textures[COMPARATORS] = blocks->minecraft_atlas;
    for (int kind = 0; kind < PARTS; ++kind) {
        r->materials[kind] = LoadMaterialDefault();
        if (!r->materials[kind].maps) { world_renderer_destroy(r); return NULL; }
        r->materials[kind].maps[MATERIAL_MAP_DIFFUSE].texture = textures[kind];
        if (!solid_part(kind) && kind != TRANSLUCENT)
            r->materials[kind].shader = blocks->cutout_shader;
    }
    world_set_change_observer(world,on_change,r);
    return r;
}
void world_renderer_destroy(WorldRenderer *r)
{
    if (!r) return;
    if (r->world) world_set_change_observer(r->world,NULL,NULL);
    for (size_t i = 0; i < r->region_count; ++i) destroy_region(r->regions[i]);
    for (int kind = 0; kind < PARTS; ++kind) MemFree(r->materials[kind].maps);
    posmap_destroy(&r->region_map); free(r->regions); free(r);
}
bool world_renderer_sync(WorldRenderer *r)
{
    if (!r || r->failed) return false;
    WorldPlotBounds plot = world_plot_bounds(r->world);
    if (plot.min_x != r->plot.min_x || plot.max_x != r->plot.max_x ||
        plot.min_z != r->plot.min_z || plot.max_z != r->plot.max_z) {
        /* Border faces can change without a retained cell being written. */
        for (size_t i = 0; i < r->region_count; ++i) {
            dirty_solid_parts(r->regions[i]);
            r->regions[i]->parts[GLASS].dirty = true;
        }
        r->plot = plot;
    }
    uint64_t topology = world_topology_revision(r->world);
    if (r->topology_revision != topology) {
        for (size_t i = 0; i < r->region_count; ++i) r->regions[i]->seen = false;
        world_visit_sections(r->world,visit_section,r);
        if (r->failed) return false;
        for (size_t i = 0; i < r->region_count;) {
            RenderRegion *region = r->regions[i];
            if (!region->seen) {
                posmap_remove(&r->region_map,region->origin); destroy_region(region);
                --r->region_count;
                if (i < r->region_count) {
                    r->regions[i] = r->regions[r->region_count];
                    (void)posmap_set(&r->region_map,r->regions[i]->origin,i);
                }
            } else ++i;
        }
        r->topology_revision = topology;
    }
    for (size_t i = 0; i < r->region_count; ++i) {
        RenderRegion *region = r->regions[i];
        for (int kind = 0; kind < PARTS; ++kind) {
            RegionPart *part = &region->parts[kind]; part->rebuilt = false;
            if (part->dirty && !rebuild_part(r,i,kind)) return false;
        }
        for (size_t j = 0; j < region->pending_count; ++j) {
            uint16_t index = region->pending[j];
            patch_cell(r,region,index); region->cells[index].pending = false;
        }
        region->pending_count = 0;
        for (int kind = 0; kind < PARTS; ++kind) {
            RegionPart *part = &region->parts[kind];
            if (part->uv_first == UINT32_MAX || part->uv_end <= part->uv_first) continue;
            int bytes = (int)((part->uv_end-part->uv_first)*2*sizeof(float));
            int offset = (int)(part->uv_first*2*sizeof(float));
            UpdateMeshBuffer(part->mesh,1,part->mesh.texcoords+part->uv_first*2,bytes,offset);
            part->uv_first = UINT32_MAX; part->uv_end = 0;
        }
    }
    return true;
}

typedef struct {
    const Mesh *mesh;
    uint32_t first;
    int stride;
    float distance_squared;
} TranslucentFace;

static int compare_translucent_faces(const void *left, const void *right)
{
    const TranslucentFace *a = left, *b = right;
    if (a->distance_squared < b->distance_squared) return 1;
    if (a->distance_squared > b->distance_squared) return -1;
    return 0;
}

static void draw_translucent_faces(WorldRenderer *r, Vector3 camera_position)
{
    size_t count = 0;
    for (size_t i = 0; i < r->region_count; ++i) {
        RegionPart *part = &r->regions[i]->parts[TRANSLUCENT];
        if (part->mesh.vertexCount) {
            size_t quads = (size_t)part->mesh.vertexCount/part->vertices_per_quad;
            if (quads > SIZE_MAX - count) return;
            count += quads;
        }
    }
    if (!count || count > SIZE_MAX/sizeof(TranslucentFace)) return;
    TranslucentFace *faces = malloc(count*sizeof(*faces));
    if (!faces) return;
    size_t next = 0;
    for (size_t i = 0; i < r->region_count; ++i) {
        RegionPart *part = &r->regions[i]->parts[TRANSLUCENT];
        const Mesh *mesh = &part->mesh;
        int stride = part->vertices_per_quad;
        for (int first = 0; first < mesh->vertexCount; first += stride) {
            /* Opposite corners give the face center for depth sorting. */
            const float *a = mesh->vertices + first*3;
            const float *b = mesh->vertices + (first+2)*3;
            float dx = (a[0]+b[0])*0.5f-camera_position.x;
            float dy = (a[1]+b[1])*0.5f-camera_position.y;
            float dz = (a[2]+b[2])*0.5f-camera_position.z;
            faces[next++] = (TranslucentFace){mesh, (uint32_t)first, stride,
                                               dx*dx+dy*dy+dz*dz};
        }
    }
    qsort(faces, count, sizeof(*faces), compare_translucent_faces);

    /* Opaque and cutout geometry already populated the depth buffer. Blend
     * translucent faces behind one another without hiding later faces. */
    rlDrawRenderBatchActive();
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    rlSetTexture(r->blocks->atlas.id);
    for (size_t i = 0; i < count; ++i) {
        const TranslucentFace *face = &faces[i];
        rlCheckRenderBatchLimit(4);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        for (int v = 0; v < 4; ++v) {
            int index = (int)face->first +
                        (face->stride == 6 && v == 3 ? 5 : v);
            const float *vertex = face->mesh->vertices + index*3;
            const float *uv = face->mesh->texcoords + index*2;
            rlTexCoord2f(uv[0], uv[1]);
            rlVertex3f(vertex[0], vertex[1], vertex[2]);
        }
        rlEnd();
    }
    rlSetTexture(0);
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
    free(faces);
}

void world_renderer_draw(WorldRenderer *r, Vector3 camera_position)
{
    if (!r) return;
    rlDrawRenderBatchActive();
    Matrix identity = MatrixIdentity();
    for (int kind = 0; kind < PARTS; ++kind) {
        if (kind == TRANSLUCENT) continue;
        /* Dust and redstone gates are surface quads; keep their reverse sides
         * visible to a noclip camera and through transparent glass. */
        bool two_sided = kind == DUST || kind == REPEATERS ||
                         kind == COMPARATORS || kind == POWERED_RAILS ||
                         kind == ACTIVATOR_RAILS;
        if (two_sided) rlDisableBackfaceCulling();
        for (size_t region = 0; region < r->region_count; ++region) {
            Mesh mesh = r->regions[region]->parts[kind].mesh;
            if (!mesh.vertexCount) continue;
            DrawMesh(mesh,r->materials[kind],identity);
        }
        if (two_sided) rlEnableBackfaceCulling();
    }
    draw_translucent_faces(r, camera_position);
}
