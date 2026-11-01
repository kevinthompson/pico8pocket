#include "p8p/runtime.h"
#include "p8p/pico8_font.h"
#include "p8p/audio.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <fix32.h>
/* Lua internals for the all() iterator's direct table access. */
#include <lstate.h>
#include <lgc.h>
#include <ltable.h>
#include <lvm.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__riscv)
#define P8P_FASTTEXT __attribute__((section(".app_fasttext"), noinline, \
                                   optimize("align-functions=4", \
                                            "align-loops=4")))
#else
#define P8P_FASTTEXT
#endif

using z8::fix32;

#include "runtime_internal.h"

struct p8p_runtime_state_header {
    uint8_t magic[8];
    uint32_t version;
    uint32_t fixed_size;
    uint32_t audio_size;
    uint32_t lua_size;
};

struct p8p_runtime_fixed_state {
    uint8_t ram[0x10000];
    uint8_t framebuffer[128 * 128];
    uint8_t screen_ram_dirty;
    uint8_t draw_palette[16];
    uint8_t screen_palette[16];
    uint8_t transparent[16];
    uint8_t buttons;
    uint8_t previous_buttons;
    uint16_t held_frames[6];
    uint32_t rng[2];
    int32_t camera_x;
    int32_t camera_y;
    int32_t clip_x0;
    int32_t clip_y0;
    int32_t clip_x1;
    int32_t clip_y1;
    int32_t draw_color;
    int32_t cursor_x;
    int32_t cursor_y;
    int32_t target_fps;
    uint32_t frame_count;
};

static const uint8_t runtime_state_magic[8] = {
    'P', '8', 'P', 'S', 'T', 'A', 'T', 'E'
};

static_assert(offsetof(p8p_runtime, ram) % 4 == 0,
              "screen packing uses word access to RAM");
static_assert(offsetof(p8p_runtime, framebuffer) % 4 == 0,
              "screen packing uses word access to the framebuffer");

p8p_runtime_t *p8p_active_runtime;
#define active_runtime p8p_active_runtime

static void profile_api(p8p_runtime_api_category_t category) {
    if (active_runtime && active_runtime->profile_hook &&
        (unsigned)category < P8P_API_CATEGORY_COUNT)
        ++active_runtime->api_profile_calls[category];
}

static void update_palette_default_flags(p8p_runtime_t *runtime) {
    int palettes_default = 1;
    int transparency_default = runtime->transparent[0] != 0;
    for (int i = 0; i < 16; ++i) {
        if (runtime->draw_palette[i] != i ||
            runtime->screen_palette[i] != i)
            palettes_default = 0;
        if (i && runtime->transparent[i])
            transparency_default = 0;
    }
    runtime->palettes_default = (uint8_t)palettes_default;
    runtime->transparency_default = (uint8_t)transparency_default;
}

static void cartdata_flush(p8p_runtime_t *runtime) {
    if (runtime && runtime->cartdata_active && runtime->cartdata_dirty &&
        runtime->cartdata_save &&
        runtime->cartdata_save(runtime->cartdata_userdata,
                               runtime->cartdata_id,
                               runtime->ram + 0x5e00, 0x100) == 0)
        runtime->cartdata_dirty = 0;
}

static void cartdata_mark_dirty(p8p_runtime_t *runtime) {
    if (runtime && runtime->cartdata_active)
        runtime->cartdata_dirty = 1;
}

static void runtime_service_lua_hook(lua_State *lua, lua_Debug *) {
    if (active_runtime && active_runtime->service_hook)
        active_runtime->service_hook(active_runtime->service_userdata);
    /* Top-level and _init code is allowed to be written as a permanent loop.
     * flip() normally supplies the cooperative boundary, but a few real carts
     * intentionally omit it on some paths.  Yield after a bounded instruction
     * slice so one such path cannot lock the Pocket forever. */
    if (active_runtime && lua == active_runtime->cart_thread &&
        active_runtime->cart_thread_kind != 2 &&
        ++active_runtime->cart_instruction_slices >= 16)
        lua_yield(lua, 0);
}

/* The count hook counts servicepoints (jumps, loop back-edges and calls;
 * patches/z8lua-servicepoints.patch), about one per four instructions:
 * 2048 keeps the previous every-8192-instructions service rate (measured
 * on Moss Moss, Kiloman and Celeste 2). */
static void install_service_hook(p8p_runtime_t *runtime) {
    if (!runtime || !runtime->lua)
        return;
    if (runtime->service_hook)
        lua_sethook(runtime->lua, runtime_service_lua_hook,
                    LUA_MASKCOUNT, 2048);
    else
        lua_sethook(runtime->lua, NULL, 0, 0);
    if (runtime->cart_thread)
        lua_sethook(runtime->cart_thread, runtime_service_lua_hook,
                    LUA_MASKCOUNT, 2048);
}

/* Arguments are read straight from the C function's stack frame: about
 * 1,900 Lua -> API calls a frame in carts like Celeste made the generic
 * lua_gettop/lua_type/lua_tonumberx calls a measurable cost. */
static inline int arg_count(lua_State *lua) {
    return (int)(lua->top - (lua->ci->func + 1));
}

static inline const TValue *arg_value(lua_State *lua, int index) {
    StkId slot = lua->ci->func + index;
    return slot < lua->top ? slot : NULL;
}

static int32_t arg_int(lua_State *lua, int index, int32_t fallback) {
    const TValue *value = arg_value(lua, index);
    if (!value || ttisnil(value))
        return fallback;
    if (ttisnumber(value))
        return (int32_t)nvalue(value);
    return (int32_t)lua_tonumber(lua, index);
}

/* A colour argument to a drawing call also becomes the pen colour, as in
 * PICO-8: rectfill(0, 0, 9, 9, 8) pset(20, 20) draws both in red. */
static int32_t arg_pen(lua_State *lua, int index, p8p_runtime_t *runtime) {
    const TValue *value = arg_value(lua, index);
    if (!value || ttisnil(value))
        return runtime->draw_color;
    int32_t color = ttisnumber(value) ? (int32_t)nvalue(value) :
                                        (int32_t)lua_tonumber(lua, index);
    runtime->draw_color = color & 255;
    runtime->ram[0x5f25] = (uint8_t)runtime->draw_color;
    return color;
}

static fix32 arg_number(lua_State *lua, int index, fix32 fallback) {
    const TValue *value = arg_value(lua, index);
    if (!value || ttisnil(value))
        return fallback;
    if (ttisnumber(value))
        return nvalue(value);
    return lua_tonumber(lua, index);
}

/*
 * Lua allocates and frees huge numbers of small blocks (tables, closures,
 * strings, hash parts).  musl's malloc is comparatively expensive on Pocket,
 * so small blocks are served from per-size-class free lists carved out of
 * larger chunks.  Lua 5.2 always passes the existing block's size as osize,
 * which lets frees and reallocs find the right class without headers.
 * Small blocks are recycled but never returned to malloc; that is bounded by
 * the peak Lua heap.
 */
enum {
    LUA_POOL_GRANULE = 8,
    LUA_POOL_MAX = 256,
    LUA_POOL_CLASSES = LUA_POOL_MAX / LUA_POOL_GRANULE,
    LUA_POOL_CHUNK = 64 * 1024
};

typedef struct lua_pool_block {
    struct lua_pool_block *next;
} lua_pool_block_t;

static lua_pool_block_t *lua_pool_free[LUA_POOL_CLASSES];
static uint8_t *lua_pool_cursor;
static size_t lua_pool_remaining;

static inline size_t lua_pool_class(size_t size) {
    return (size - 1) / LUA_POOL_GRANULE;
}

static void *lua_pool_take(size_t size) {
    if (size > LUA_POOL_MAX)
        return malloc(size);
    size_t index = lua_pool_class(size);
    lua_pool_block_t *block = lua_pool_free[index];
    if (block) {
        lua_pool_free[index] = block->next;
        return block;
    }
    size_t rounded = (index + 1) * LUA_POOL_GRANULE;
    if (lua_pool_remaining < rounded) {
        /* The unused tail of the previous chunk is simply abandoned. */
        uint8_t *chunk = (uint8_t *)malloc(LUA_POOL_CHUNK);
        if (!chunk)
            return NULL;
        lua_pool_cursor = chunk;
        lua_pool_remaining = LUA_POOL_CHUNK;
    }
    void *result = lua_pool_cursor;
    lua_pool_cursor += rounded;
    lua_pool_remaining -= rounded;
    return result;
}

static void lua_pool_give(void *pointer, size_t size) {
    if (size > LUA_POOL_MAX) {
        free(pointer);
        return;
    }
    lua_pool_block_t *block = (lua_pool_block_t *)pointer;
    size_t index = lua_pool_class(size);
    block->next = lua_pool_free[index];
    lua_pool_free[index] = block;
}

static void *lua_pool_alloc(void *, void *pointer, size_t old_size,
                            size_t new_size) {
    if (!pointer) {
        /* old_size is a type tag here, not a size. */
        return new_size ? lua_pool_take(new_size) : NULL;
    }
    if (new_size == 0) {
        lua_pool_give(pointer, old_size);
        return NULL;
    }
    if (old_size > LUA_POOL_MAX && new_size > LUA_POOL_MAX)
        return realloc(pointer, new_size);
    if (old_size <= LUA_POOL_MAX && new_size <= LUA_POOL_MAX &&
        lua_pool_class(old_size) == lua_pool_class(new_size))
        return pointer;
    void *moved = lua_pool_take(new_size);
    if (!moved)
        return NULL;  /* Lua keeps the original block on failure. */
    memcpy(moved, pointer, old_size < new_size ? old_size : new_size);
    lua_pool_give(pointer, old_size);
    return moved;
}

/* Start a collection cycle when the heap reaches 4x the live data instead of
 * Lua's default 2x.  Carts are capped near PICO-8's 2 MiB of Lua memory, so
 * this costs at most a few MiB of Pocket RAM in exchange for fewer cycles. */
#ifndef P8P_LUA_GC_PAUSE
#define P8P_LUA_GC_PAUSE 400
#endif

static int lua_pool_panic(lua_State *lua) {
    /* Matches luaL_newstate's handler; Lua aborts after it returns. */
    fprintf(stderr, "PANIC: unprotected error in call to Lua API (%s)\n",
            lua_tostring(lua, -1));
    return 0;
}

/* What lua_pushnumber does for a fix32, without the call.  C functions
 * have LUA_MINSTACK free slots; larger pushes reserve them first. */
static inline void push_int(lua_State *lua, int32_t value) {
    setnvalue(lua->top, fix32(value));
    ++lua->top;
}

static int in_clip(const p8p_runtime_t *runtime, int x, int y) {
    return x >= runtime->clip_x0 && x < runtime->clip_x1 &&
           y >= runtime->clip_y0 && y < runtime->clip_y1;
}

static void screen_to_ram(p8p_runtime_t *runtime) {
    if (!runtime->screen_ram_dirty)
        return;
    /* Eight pixels per pair of word loads, one word store.  Both buffers are
     * 4-byte aligned (see the static_asserts); telling the compiler lets
     * RV32 use real lw/sw instead of byte accesses. */
    const uint8_t *source =
        (const uint8_t *)__builtin_assume_aligned(runtime->framebuffer, 4);
    uint8_t *destination = (uint8_t *)__builtin_assume_aligned(
        runtime->ram + runtime->draw_target, 4);
    for (int i = 0; i < 128 * 128; i += 8) {
        uint32_t a, b;
        memcpy(&a, source + i, 4);
        memcpy(&b, source + i + 4, 4);
        a &= 0x0f0f0f0fu;
        b &= 0x0f0f0f0fu;
        a |= a >> 4;
        b |= b >> 4;
        uint32_t packed = (a & 0xff) | ((a >> 16) & 0xff) << 8 |
                          (b & 0xff) << 16 | ((b >> 16) & 0xff) << 24;
        memcpy(destination + i / 2, &packed, 4);
    }
    runtime->screen_ram_dirty = 0;
}

static void ram_to_screen(p8p_runtime_t *runtime) {
    /* Two packed bytes per halfword load, four pixels per word store. */
    const uint8_t *source = (const uint8_t *)__builtin_assume_aligned(
        runtime->ram + runtime->draw_target, 4);
    uint8_t *destination =
        (uint8_t *)__builtin_assume_aligned(runtime->framebuffer, 4);
    for (int i = 0; i < 64 * 128; i += 2) {
        uint16_t two;
        memcpy(&two, source + i, 2);
        uint32_t lo = two & 0xff, hi = two >> 8;
        uint32_t pixels = (lo & 15) | (lo >> 4) << 8 |
                          (hi & 15) << 16 | (hi >> 4) << 24;
        memcpy(destination + i * 2, &pixels, 4);
    }
    runtime->screen_ram_dirty = 0;
}

/* Whether a RAM range overlaps the memory the framebuffer mirrors. */
static int range_touches_screen(int address, int length) {
    int end;
    int start = active_runtime ? active_runtime->draw_target : 0x6000;
    int stop = start + 0x2000;
    if (length <= 0)
        return 0;
    address &= 0xffff;
    end = address + length;
    if (end <= 0x10000)
        return address < stop && end > start;
    return (address < stop) || ((end - 0x10000) > start);
}

/*
 * After RAM bytes overlapping the draw target were written, refresh only the
 * framebuffer pixels they cover.  Writes need no prior screen_to_ram(): the
 * framebuffer stays authoritative, the written bytes become consistent, and
 * a pending dirty flag still covers the rest of the packed copy.
 */
static void ram_to_screen_range(p8p_runtime_t *runtime, int address,
                                int length) {
    int base = runtime->draw_target;
    address &= 0xffff;
    while (length > 0) {
        int chunk = length < 0x10000 - address ? length : 0x10000 - address;
        int start = address > base ? address : base;
        int stop = address + chunk < base + 0x2000 ? address + chunk
                                                   : base + 0x2000;
        for (int i = start; i < stop; ++i) {
            uint8_t packed = runtime->ram[i];
            int offset = (i - base) * 2;
            runtime->framebuffer[offset] = packed & 15;
            runtime->framebuffer[offset + 1] = packed >> 4;
        }
        length -= chunk;
        address = 0;
    }
}

/* PICO-8 maps 0x5f55 = 0 to the sprite sheet, 0x80+ to upper memory and
 * every other value to the screen. */
static uint16_t draw_target_from_ram(const p8p_runtime_t *runtime) {
    uint8_t value = runtime->ram[0x5f55];
    if (value == 0)
        return 0x0000;
    if (value >= 0x80)
        return (uint16_t)((value & 0xe0) << 8);
    return 0x6000;
}

static void sync_draw_target(p8p_runtime_t *runtime) {
    uint16_t target = draw_target_from_ram(runtime);
    if (target == runtime->draw_target)
        return;
    screen_to_ram(runtime);
    runtime->draw_target = target;
    ram_to_screen(runtime);
}

/* Sprite reads come from RAM; flush pixels drawn into the sheet first. */
static inline void flush_redirected_drawing(p8p_runtime_t *runtime) {
    if (runtime->draw_target != 0x6000)
        screen_to_ram(runtime);
}

static int range_touches_cartdata(int address, int length) {
    if (length <= 0)
        return 0;
    return address < 0x5f00 && address + length > 0x5e00;
}

/* Includes 0x5f55, the draw-target mapping. */
static int range_touches_draw_state(int address, int length) {
    return length > 0 && address < 0x5f56 && address + length > 0x5f00;
}

static void draw_state_to_ram(p8p_runtime_t *runtime) {
    for (int i = 0; i < 16; ++i) {
        runtime->ram[0x5f00 + i] = (uint8_t)(runtime->draw_palette[i] |
            (runtime->transparent[i] ? 0x10 : 0));
        runtime->ram[0x5f10 + i] = runtime->screen_palette[i];
    }
    runtime->ram[0x5f20] = (uint8_t)runtime->clip_x0;
    runtime->ram[0x5f21] = (uint8_t)runtime->clip_y0;
    runtime->ram[0x5f22] = (uint8_t)runtime->clip_x1;
    runtime->ram[0x5f23] = (uint8_t)runtime->clip_y1;
    runtime->ram[0x5f25] = (uint8_t)runtime->draw_color;
    runtime->ram[0x5f26] = (uint8_t)runtime->cursor_x;
    runtime->ram[0x5f27] = (uint8_t)runtime->cursor_y;
    runtime->ram[0x5f28] = (uint8_t)runtime->camera_x;
    runtime->ram[0x5f29] = (uint8_t)(runtime->camera_x >> 8);
    runtime->ram[0x5f2a] = (uint8_t)runtime->camera_y;
    runtime->ram[0x5f2b] = (uint8_t)(runtime->camera_y >> 8);
    runtime->ram[0x5f31] = (uint8_t)runtime->fill_pattern;
    runtime->ram[0x5f32] = (uint8_t)(runtime->fill_pattern >> 8);
    runtime->ram[0x5f33] = runtime->fill_pattern_transparent;
}

static void palette_entry_to_ram(p8p_runtime_t *runtime, int color) {
    runtime->ram[0x5f00 + color] =
        (uint8_t)(runtime->draw_palette[color] |
                  (runtime->transparent[color] ? 0x10 : 0));
    runtime->ram[0x5f10 + color] = runtime->screen_palette[color];
}

static void cursor_to_ram(p8p_runtime_t *runtime) {
    runtime->ram[0x5f26] = (uint8_t)runtime->cursor_x;
    runtime->ram[0x5f27] = (uint8_t)runtime->cursor_y;
}

static void camera_to_ram(p8p_runtime_t *runtime) {
    runtime->ram[0x5f28] = (uint8_t)runtime->camera_x;
    runtime->ram[0x5f29] = (uint8_t)(runtime->camera_x >> 8);
    runtime->ram[0x5f2a] = (uint8_t)runtime->camera_y;
    runtime->ram[0x5f2b] = (uint8_t)(runtime->camera_y >> 8);
}

static void sync_draw_target(p8p_runtime_t *runtime);

static void draw_state_from_ram(p8p_runtime_t *runtime) {
    sync_draw_target(runtime);
    for (int i = 0; i < 16; ++i) {
        runtime->draw_palette[i] = runtime->ram[0x5f00 + i] & 15;
        runtime->transparent[i] =
            (uint8_t)((runtime->ram[0x5f00 + i] & 0x10) != 0);
        runtime->screen_palette[i] = runtime->ram[0x5f10 + i] & 0x8f;
    }
    update_palette_default_flags(runtime);
    runtime->clip_x0 = runtime->ram[0x5f20];
    runtime->clip_y0 = runtime->ram[0x5f21];
    /* Poked clip bounds can exceed the screen; every renderer trusts the
     * clip rectangle to stay inside the 128x128 framebuffer. */
    runtime->clip_x1 = runtime->ram[0x5f22] > 128 ? 128 : runtime->ram[0x5f22];
    runtime->clip_y1 = runtime->ram[0x5f23] > 128 ? 128 : runtime->ram[0x5f23];
    runtime->draw_color = runtime->ram[0x5f25];
    runtime->cursor_x = runtime->ram[0x5f26];
    runtime->cursor_y = runtime->ram[0x5f27];
    runtime->camera_x = (int16_t)(runtime->ram[0x5f28] |
        ((uint16_t)runtime->ram[0x5f29] << 8));
    runtime->camera_y = (int16_t)(runtime->ram[0x5f2a] |
        ((uint16_t)runtime->ram[0x5f2b] << 8));
    runtime->fill_pattern = (uint16_t)(runtime->ram[0x5f31] |
        ((uint16_t)runtime->ram[0x5f32] << 8));
    runtime->fill_pattern_transparent = runtime->ram[0x5f33] & 1;
}

static uint8_t screen_get(const p8p_runtime_t *runtime, int x, int y) {
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return 0;
    return runtime->framebuffer[y * 128 + x];
}

static void screen_set_raw(p8p_runtime_t *runtime, int x, int y, uint8_t color) {
    if ((unsigned)x >= 128u || (unsigned)y >= 128u || !in_clip(runtime, x, y))
        return;
    runtime->framebuffer[y * 128 + x] = color & 15;
    runtime->screen_ram_dirty = 1;
}

static void screen_set_unchecked(p8p_runtime_t *runtime, int x, int y,
                                 uint8_t color) {
    runtime->framebuffer[y * 128 + x] = color & 15;
    runtime->screen_ram_dirty = 1;
}

static void screen_set(p8p_runtime_t *runtime, int x, int y, int color) {
    int screen_x = x - runtime->camera_x;
    int screen_y = y - runtime->camera_y;
    int selected = color & 0x0f;
    if (!runtime->fill_pattern) {
        screen_set_raw(runtime, screen_x, screen_y,
                       runtime->draw_palette[selected]);
        return;
    }
    int bit = 15 - ((screen_x & 3) + 4 * (screen_y & 3));
    if ((runtime->fill_pattern >> bit) & 1) {
        if (runtime->fill_pattern_transparent)
            return;
        selected = (color >> 4) & 0x0f;
    }
    screen_set_raw(runtime, screen_x, screen_y,
                   runtime->draw_palette[selected]);
}

static uint8_t sprite_get(const p8p_runtime_t *runtime, int x, int y) {
    uint8_t packed;
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return 0;
    int base = (int)runtime->ram[0x5f54] << 8;
    int address = base + y * 64 + x / 2;
    if (address < 0 || address >= 0x10000)
        return 0;
    packed = runtime->ram[address];
    return (uint8_t)((x & 1) ? packed >> 4 : packed & 0x0f);
}

static uint8_t sprite_get_at_base(const p8p_runtime_t *runtime, int base,
                                  int x, int y) {
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return 0;
    int address = base + y * 64 + x / 2;
    if ((unsigned)address >= 0x10000u)
        return 0;
    uint8_t packed = runtime->ram[address];
    return (uint8_t)((x & 1) ? packed >> 4 : packed & 0x0f);
}

static void sprite_set(p8p_runtime_t *runtime, int x, int y, uint8_t color) {
    uint8_t *packed;
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return;
    int base = (int)runtime->ram[0x5f54] << 8;
    int address = base + y * 64 + x / 2;
    if (address < 0 || address >= 0x10000)
        return;
    packed = &runtime->ram[address];
    if (x & 1)
        *packed = (uint8_t)((*packed & 0x0f) | ((color & 0x0f) << 4));
    else
        *packed = (uint8_t)((*packed & 0xf0) | (color & 0x0f));
    if (range_touches_screen(address, 1)) {
        /* Drawing is redirected here, so the framebuffer is authoritative.
         * Write just this pixel: unpacking the whole byte could restore a
         * stale neighbouring pixel from RAM. */
        runtime->framebuffer[(address - runtime->draw_target) * 2 + (x & 1)] =
            color & 0x0f;
    }
}

static uint8_t map_get(const p8p_runtime_t *runtime, int x, int y) {
    int width = runtime->ram[0x5f57] ? runtime->ram[0x5f57] : 256;
    int mapping = runtime->ram[0x5f56];
    int size = mapping >= 0x80 ? 0x10000 - (mapping << 8) : 8192;
    int height = size / width;
    if (x < 0 || y < 0 || x >= width || y >= height)
        return 0;
    int index = y * width + x;
    if (mapping >= 0x80)
        return runtime->ram[(mapping << 8) + index];
    return index < 4096 ? runtime->ram[0x2000 + index]
                        : runtime->ram[index];
}

static void map_set(p8p_runtime_t *runtime, int x, int y, uint8_t value) {
    int width = runtime->ram[0x5f57] ? runtime->ram[0x5f57] : 256;
    int mapping = runtime->ram[0x5f56];
    int size = mapping >= 0x80 ? 0x10000 - (mapping << 8) : 8192;
    int height = size / width;
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    int index = y * width + x;
    int address = mapping >= 0x80 ? (mapping << 8) + index :
                  index < 4096 ? 0x2000 + index : index;
    runtime->ram[address] = value;
    if (range_touches_screen(address, 1))
        ram_to_screen_range(runtime, address, 1);
}

static void draw_line(p8p_runtime_t *runtime, int x0, int y0, int x1, int y1,
                      int color) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        screen_set(runtime, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void blit_tile(p8p_runtime_t *runtime, int sprite, int screen_x,
                      int screen_y, int sprite_base);

/* Not BRAM-resident: common sprites are handed to blit_tile(), which is. */
static void draw_sprite(p8p_runtime_t *runtime, int sprite, int x,
                        int y, int width, int height, int flip_x,
                        int flip_y) {
    profile_api(P8P_API_SPRITE);
    int sprite_base = (int)runtime->ram[0x5f54] << 8;
    /* Unflipped 8x8-tile sprites that stay inside the sheet are exactly the
     * map tile case; reuse its unpacked two-pixels-per-byte blitter. */
    if (!flip_x && !flip_y && sprite >= 0 && sprite < 256 &&
        width >= 1 && height >= 1 &&
        (sprite & 15) + width <= 16 && (sprite >> 4) + height <= 16 &&
        sprite_base + 8192 <= 0x10000) {
        int screen_x = x - runtime->camera_x;
        int screen_y = y - runtime->camera_y;
        for (int tile_y = 0; tile_y < height; ++tile_y)
            for (int tile_x = 0; tile_x < width; ++tile_x)
                blit_tile(runtime, sprite + tile_x + tile_y * 16,
                          screen_x + tile_x * 8, screen_y + tile_y * 8,
                          sprite_base);
        return;
    }
    int source_x = (sprite & 15) * 8;
    int source_y = (sprite >> 4) * 8;
    int pixel_width = width * 8;
    int pixel_height = height * 8;

    int destination_x = x - runtime->camera_x;
    int destination_y = y - runtime->camera_y;
    int start_x = destination_x < runtime->clip_x0 ?
        runtime->clip_x0 - destination_x : 0;
    int start_y = destination_y < runtime->clip_y0 ?
        runtime->clip_y0 - destination_y : 0;
    int end_x = pixel_width;
    int end_y = pixel_height;
    if (destination_x + end_x > runtime->clip_x1)
        end_x = runtime->clip_x1 - destination_x;
    if (destination_y + end_y > runtime->clip_y1)
        end_y = runtime->clip_y1 - destination_y;
    if (start_x >= end_x || start_y >= end_y)
        return;

    int source_in_bounds = source_x >= 0 && source_y >= 0 &&
        source_x + pixel_width <= 128 && source_y + pixel_height <= 128 &&
        sprite_base >= 0 && sprite_base + 8192 <= 0x10000;
    runtime->screen_ram_dirty = 1;
    for (int py = start_y; py < end_y; ++py) {
        int sy = flip_y ? pixel_height - 1 - py : py;
        const uint8_t *source_row = source_in_bounds ?
            runtime->ram + sprite_base + (source_y + sy) * 64 : NULL;
        uint8_t *destination_row = runtime->framebuffer +
            (destination_y + py) * 128 + destination_x;
        for (int px = start_x; px < end_x; ++px) {
            int sx = flip_x ? pixel_width - 1 - px : px;
            int sample_x = source_x + sx;
            uint8_t color;
            if (source_row) {
                uint8_t packed = source_row[sample_x >> 1];
                color = (uint8_t)((sample_x & 1) ? packed >> 4 : packed & 15);
            } else {
                color = sprite_get_at_base(runtime, sprite_base, sample_x,
                                           source_y + sy);
            }
            if (!runtime->transparent[color])
                destination_row[px] = runtime->draw_palette[color] & 15;
        }
    }
}

/* Fill patterns on screen coordinates already clipped to the clip rectangle.
 * Same result as screen_set() per pixel: a set pattern bit selects the high
 * nibble of the colour, or leaves the pixel alone when the pattern is
 * transparent.  Byte k of fill/keep is the pixel at x % 4 == k
 * (little-endian words); keep marks the pixels left alone.  Kept out of
 * BRAM; Celeste 2 fills its background columns and fog this way. */
struct pattern_row {
    uint32_t fill;
    uint32_t keep;
};

static pattern_row fill_pattern_row(const p8p_runtime_t *runtime, int y,
                                    int color) {
    unsigned row = (runtime->fill_pattern >> (12 - 4 * (y & 3))) & 15u;
    uint8_t clear = runtime->draw_palette[color & 15] & 15;
    uint8_t set = runtime->draw_palette[(color >> 4) & 15] & 15;
    pattern_row result = {0, 0};
    for (int k = 0; k < 4; ++k) {
        int pattern_bit = (row >> (3 - k)) & 1u;
        if (pattern_bit && runtime->fill_pattern_transparent)
            result.keep |= 0xffu << (8 * k);
        else
            result.fill |= (uint32_t)(pattern_bit ? set : clear) << (8 * k);
    }
    return result;
}

static inline void fill_pattern_span(uint8_t *row_pixels, int x0, int x1,
                                     pattern_row pattern) {
    int x = x0;
    for (; x <= x1 && (x & 3); ++x)
        if (!((pattern.keep >> (8 * (x & 3))) & 0xffu))
            row_pixels[x] = (uint8_t)(pattern.fill >> (8 * (x & 3)));
    uint8_t *aligned = (uint8_t *)__builtin_assume_aligned(row_pixels, 4);
    if (pattern.keep) {
        for (; x + 3 <= x1; x += 4) {
            uint32_t word;
            memcpy(&word, aligned + x, 4);
            word = (word & pattern.keep) | pattern.fill;
            memcpy(aligned + x, &word, 4);
        }
    } else {
        for (; x + 3 <= x1; x += 4)
            memcpy(aligned + x, &pattern.fill, 4);
    }
    for (; x <= x1; ++x)
        if (!((pattern.keep >> (8 * (x & 3))) & 0xffu))
            row_pixels[x] = (uint8_t)(pattern.fill >> (8 * (x & 3)));
}

static void __attribute__((noinline)) draw_hspan_pattern(
        p8p_runtime_t *runtime, int x0, int x1, int y, int color) {
    fill_pattern_span(runtime->framebuffer + y * 128, x0, x1,
                      fill_pattern_row(runtime, y, color));
    runtime->screen_ram_dirty = 1;
}

/* rectfill() with a fill pattern: the pattern repeats every four rows, so
 * work them out once instead of per row. */
static void __attribute__((noinline)) draw_rect_pattern(
        p8p_runtime_t *runtime, int x0, int x1, int y0, int y1, int color) {
    pattern_row rows[4];
    for (int k = 0; k < 4; ++k)
        rows[(y0 + k) & 3] = fill_pattern_row(runtime, y0 + k, color);
    for (int y = y0; y <= y1; ++y)
        fill_pattern_span(runtime->framebuffer + y * 128, x0, x1,
                          rows[y & 3]);
    runtime->screen_ram_dirty = 1;
}

/* Solid span fill for shapes that clip once up front: four pixels per
 * store between unaligned ends, without a call per row. */
static inline void fill_solid_span(uint8_t *row_pixels, int x0, int x1,
                                   uint8_t color) {
    int x = x0;
    for (; x <= x1 && (x & 3); ++x)
        row_pixels[x] = color;
    uint32_t word = color * 0x01010101u;
    uint8_t *aligned = (uint8_t *)__builtin_assume_aligned(row_pixels, 4);
    for (; x + 3 <= x1; x += 4)
        memcpy(aligned + x, &word, 4);
    for (; x <= x1; ++x)
        row_pixels[x] = color;
}

static P8P_FASTTEXT void draw_hspan(p8p_runtime_t *runtime, int x0, int x1,
                                    int y, int color) {
    x0 -= runtime->camera_x;
    x1 -= runtime->camera_x;
    y -= runtime->camera_y;
    if (x0 > x1) { int temporary = x0; x0 = x1; x1 = temporary; }
    if (y < runtime->clip_y0 || y >= runtime->clip_y1 ||
        x1 < runtime->clip_x0 || x0 >= runtime->clip_x1)
        return;
    if (x0 < runtime->clip_x0) x0 = runtime->clip_x0;
    if (x1 >= runtime->clip_x1) x1 = runtime->clip_x1 - 1;
    /* A clip rectangle whose left edge is past its right edge (for example
     * clip() starting beyond the screen) is empty. */
    if (x0 > x1)
        return;

    if (!runtime->fill_pattern) {
        uint8_t mapped = runtime->draw_palette[color & 15] & 15;
        memset(runtime->framebuffer + y * 128 + x0, mapped,
               (size_t)(x1 - x0 + 1));
        runtime->screen_ram_dirty = 1;
    } else {
        draw_hspan_pattern(runtime, x0, x1, y, color);
    }
}

static int api_cls(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    uint8_t color = runtime->draw_palette[arg_int(lua, 1, 0) & 15];
    memset(runtime->framebuffer, color & 15, sizeof(runtime->framebuffer));
    runtime->screen_ram_dirty = 1;
    runtime->cursor_x = 0;
    runtime->cursor_y = 0;
    cursor_to_ram(runtime);
    return 0;
}

static int api_pset(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    screen_set(runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
               arg_pen(lua, 3, runtime));
    return 0;
}

static int api_pget(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    int x = arg_int(lua, 1, 0) - active_runtime->camera_x;
    int y = arg_int(lua, 2, 0) - active_runtime->camera_y;
    push_int(lua, screen_get(active_runtime, x, y));
    return 1;
}

static int api_sget(lua_State *lua) {
    flush_redirected_drawing(active_runtime);
    profile_api(P8P_API_GRAPHICS);
    push_int(lua, sprite_get(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0)));
    return 1;
}

static int api_sset(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    sprite_set(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
               (uint8_t)arg_int(lua, 3, 0));
    return 0;
}

static int api_color(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    int previous = active_runtime->draw_color;
    if (arg_count(lua) >= 1)
        active_runtime->draw_color = arg_int(lua, 1, 6) & 255;
    active_runtime->ram[0x5f25] = (uint8_t)active_runtime->draw_color;
    push_int(lua, previous);
    return 1;
}

static int api_line(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, x0);
    int y1 = arg_int(lua, 4, y0);
    int color = arg_pen(lua, 5, runtime);
    draw_line(runtime, x0, y0, x1, y1, color);
    return 0;
}

static int api_rectfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, x0);
    int y1 = arg_int(lua, 4, y0);
    int color = arg_pen(lua, 5, runtime);
    if (y0 > y1) { int temp = y0; y0 = y1; y1 = temp; }

    /* Clamp in world coordinates before walking the rows.  Real cartridges
     * use rectfill() for very large ground/water planes (UFO draws down to
     * y=576); calling draw_hspan hundreds of times for rows which are wholly
     * outside the 128-pixel clip is pure overhead on Pocket. */
    int visible_y0 = runtime->camera_y + runtime->clip_y0;
    int visible_y1 = runtime->camera_y + runtime->clip_y1 - 1;
    if (y0 < visible_y0) y0 = visible_y0;
    if (y1 > visible_y1) y1 = visible_y1;
    if (y0 > y1)
        return 0;
    if (runtime->fill_pattern) {
        /* Same clipping as draw_hspan(), done once for every row. */
        int screen_x0 = x0 - runtime->camera_x;
        int screen_x1 = x1 - runtime->camera_x;
        if (screen_x0 > screen_x1) {
            int temporary = screen_x0;
            screen_x0 = screen_x1;
            screen_x1 = temporary;
        }
        if (screen_x0 < runtime->clip_x0) screen_x0 = runtime->clip_x0;
        if (screen_x1 >= runtime->clip_x1) screen_x1 = runtime->clip_x1 - 1;
        if (screen_x0 > screen_x1)
            return 0;
        draw_rect_pattern(runtime, screen_x0, screen_x1,
                          y0 - runtime->camera_y, y1 - runtime->camera_y,
                          color);
        return 0;
    }
    /* Solid fill: the same clipping once for the whole rectangle. */
    int screen_x0 = x0 - runtime->camera_x;
    int screen_x1 = x1 - runtime->camera_x;
    if (screen_x0 > screen_x1) {
        int temporary = screen_x0;
        screen_x0 = screen_x1;
        screen_x1 = temporary;
    }
    if (screen_x0 < runtime->clip_x0) screen_x0 = runtime->clip_x0;
    if (screen_x1 >= runtime->clip_x1) screen_x1 = runtime->clip_x1 - 1;
    if (screen_x0 > screen_x1)
        return 0;
    uint8_t mapped = runtime->draw_palette[color & 15] & 15;
    uint8_t *row = runtime->framebuffer +
                   (y0 - runtime->camera_y) * 128;
    for (int y = y0; y <= y1; ++y, row += 128)
        fill_solid_span(row, screen_x0, screen_x1, mapped);
    runtime->screen_ram_dirty = 1;
    return 0;
}

static int api_rect(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, x0);
    int y1 = arg_int(lua, 4, y0);
    int color = arg_pen(lua, 5, runtime);
    draw_line(runtime, x0, y0, x1, y0, color);
    draw_line(runtime, x1, y0, x1, y1, color);
    draw_line(runtime, x1, y1, x0, y1, color);
    draw_line(runtime, x0, y1, x0, y0, color);
    return 0;
}

/* Horizontal inset of a rounded corner ROW pixels from the top or bottom
 * edge.  It uses circfill's extents, so the manual's example holds: radius 2
 * leaves 3 pixels missing at each corner. */
static int rounded_corner_inset(int radius, int row) {
    if (row >= radius)
        return 0;
    int offset_y = radius - row;
    int extent = radius;
    while (extent * extent + offset_y * offset_y > radius * radius)
        --extent;
    return radius - extent;
}

static void draw_rounded_rect(p8p_runtime_t *runtime, int x, int y,
                              int width, int height, int radius, int color,
                              int filled) {
    if (width <= 0 || height <= 0)
        return;
    int max_radius = (width < height ? width : height) / 2;
    if (radius < 0) radius = 0;
    if (radius > max_radius) radius = max_radius;
    int x1 = x + width - 1;
    for (int row = 0; row < height; ++row) {
        int edge_row = row < height - 1 - row ? row : height - 1 - row;
        int inset = rounded_corner_inset(radius, edge_row);
        if (filled || edge_row == 0) {
            draw_hspan(runtime, x + inset, x1 - inset, y + row, color);
            continue;
        }
        /* Join this row's edge pixel to the row nearer the edge, so the
         * outline stays connected around the corner. */
        int outer_inset = rounded_corner_inset(radius, edge_row - 1);
        int run_end = outer_inset - 1 > inset ? outer_inset - 1 : inset;
        draw_hspan(runtime, x + inset, x + run_end, y + row, color);
        draw_hspan(runtime, x1 - run_end, x1 - inset, y + row, color);
    }
}

static int api_rrect(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    draw_rounded_rect(runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
                      arg_int(lua, 3, 0), arg_int(lua, 4, 0),
                      arg_int(lua, 5, 0), arg_pen(lua, 6, runtime), 0);
    return 0;
}

static int api_rrectfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    draw_rounded_rect(runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
                      arg_int(lua, 3, 0), arg_int(lua, 4, 0),
                      arg_int(lua, 5, 0), arg_pen(lua, 6, runtime), 1);
    return 0;
}

static int api_circfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int cx = arg_int(lua, 1, 0);
    int cy = arg_int(lua, 2, 0);
    int radius = arg_int(lua, 3, 4);
    int color = arg_pen(lua, 4, runtime);
    if (radius < 0)
        return 0;
    /* Work in screen coordinates and clip once: circles are often mostly
     * off screen or behind clip(), and a draw_hspan call per row (about
     * 110 circles a frame in Celeste 2) cost more than the filling. */
    int sx = cx - runtime->camera_x;
    int sy = cy - runtime->camera_y;
    int clip_x0 = runtime->clip_x0, clip_x1 = runtime->clip_x1 - 1;
    int clip_y0 = runtime->clip_y0, clip_y1 = runtime->clip_y1 - 1;
    if (sx + radius < clip_x0 || sx - radius > clip_x1 ||
        sy + radius < clip_y0 || sy - radius > clip_y1 ||
        clip_x0 > clip_x1 || clip_y0 > clip_y1)
        return 0;
    uint8_t mapped = runtime->draw_palette[color & 15] & 15;
    int patterned = runtime->fill_pattern != 0;
    int extent = radius;
    int radius_squared = radius * radius;
    /* Past this offset both rows are outside the clip rectangle. */
    int last_offset = sy - clip_y0 > clip_y1 - sy ? sy - clip_y0 : clip_y1 - sy;
    if (last_offset > radius) last_offset = radius;
    for (int offset_y = 0; offset_y <= last_offset; ++offset_y) {
        while (extent * extent + offset_y * offset_y > radius_squared)
            --extent;
        int x0 = sx - extent < clip_x0 ? clip_x0 : sx - extent;
        int x1 = sx + extent > clip_x1 ? clip_x1 : sx + extent;
        if (x0 > x1)
            continue;
        for (int side = 0; side < (offset_y ? 2 : 1); ++side) {
            int y = side ? sy + offset_y : sy - offset_y;
            if (y < clip_y0 || y > clip_y1)
                continue;
            uint8_t *row = runtime->framebuffer + y * 128;
            if (patterned)
                fill_pattern_span(row, x0, x1,
                                  fill_pattern_row(runtime, y, color));
            else
                fill_solid_span(row, x0, x1, mapped);
        }
    }
    runtime->screen_ram_dirty = 1;
    return 0;
}

static int api_circ(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int cx = arg_int(lua, 1, 0);
    int cy = arg_int(lua, 2, 0);
    int radius = arg_int(lua, 3, 4);
    int color = arg_pen(lua, 4, runtime);
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        screen_set(runtime, cx + x, cy + y, color);
        screen_set(runtime, cx + y, cy + x, color);
        screen_set(runtime, cx - y, cy + x, color);
        screen_set(runtime, cx - x, cy + y, color);
        screen_set(runtime, cx - x, cy - y, color);
        screen_set(runtime, cx - y, cy - x, color);
        screen_set(runtime, cx + y, cy - x, color);
        screen_set(runtime, cx + x, cy - y, color);
        ++y;
        if (error < 0)
            error += 2 * y + 1;
        else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
    return 0;
}

static int api_spr(lua_State *lua) {
    int sprite = arg_int(lua, 1, 0);
    if (sprite < 0) {
        /* Carts such as Kiloman use -1 as "no sprite"; PICO-8 draws nothing
         * rather than out-of-sheet pixels. */
        profile_api(P8P_API_SPRITE);
        return 0;
    }
    flush_redirected_drawing(active_runtime);
    draw_sprite(active_runtime, sprite, arg_int(lua, 2, 0),
                arg_int(lua, 3, 0), arg_int(lua, 4, 1), arg_int(lua, 5, 1),
                lua_toboolean(lua, 6), lua_toboolean(lua, 7));
    return 0;
}

static int api_sspr(lua_State *lua) {
    flush_redirected_drawing(active_runtime);
    profile_api(P8P_API_SPRITE);
    p8p_runtime_t *runtime = active_runtime;
    int source_x = arg_int(lua, 1, 0);
    int source_y = arg_int(lua, 2, 0);
    int source_w = arg_int(lua, 3, 0);
    int source_h = arg_int(lua, 4, 0);
    int destination_x = arg_int(lua, 5, 0);
    int destination_y = arg_int(lua, 6, 0);
    int destination_w = arg_int(lua, 7, source_w);
    int destination_h = arg_int(lua, 8, source_h);
    int flip_x = lua_toboolean(lua, 9);
    int flip_y = lua_toboolean(lua, 10);
    if (!source_w || !source_h || !destination_w || !destination_h)
        return 0;
    if (destination_w < 0) {
        destination_x += destination_w;
        destination_w = -destination_w;
        flip_x = !flip_x;
    }
    if (destination_h < 0) {
        destination_y += destination_h;
        destination_h = -destination_h;
        flip_y = !flip_y;
    }
    int screen_x = destination_x - runtime->camera_x;
    int screen_y = destination_y - runtime->camera_y;
    int start_x = screen_x < runtime->clip_x0 ?
        runtime->clip_x0 - screen_x : 0;
    int start_y = screen_y < runtime->clip_y0 ?
        runtime->clip_y0 - screen_y : 0;
    int end_x = destination_w;
    int end_y = destination_h;
    if (screen_x + end_x > runtime->clip_x1)
        end_x = runtime->clip_x1 - screen_x;
    if (screen_y + end_y > runtime->clip_y1)
        end_y = runtime->clip_y1 - screen_y;
    if (start_x >= end_x || start_y >= end_y)
        return 0;

    int sprite_base = (int)runtime->ram[0x5f54] << 8;
    int sy_numerator = start_y * source_h;
    int sy = sy_numerator / destination_h;
    int sy_error = sy_numerator % destination_h;
    for (int dy = start_y; dy < end_y; ++dy) {
        int sample_y = flip_y ? source_h - 1 - sy : sy;
        int sx_numerator = start_x * source_w;
        int sx = sx_numerator / destination_w;
        int sx_error = sx_numerator % destination_w;
        for (int dx = start_x; dx < end_x; ++dx) {
            int sample_x = flip_x ? source_w - 1 - sx : sx;
            uint8_t color = sprite_get_at_base(
                runtime, sprite_base, source_x + sample_x,
                source_y + sample_y);
            if (!runtime->transparent[color])
                screen_set_unchecked(runtime, screen_x + dx, screen_y + dy,
                                     runtime->draw_palette[color]);
            sx_error += source_w;
            while (sx_error >= destination_w) {
                sx_error -= destination_w;
                ++sx;
            }
        }
        sy_error += source_h;
        while (sy_error >= destination_h) {
            sy_error -= destination_h;
            ++sy;
        }
    }
    return 0;
}

static void draw_oval(p8p_runtime_t *runtime, int x0, int y0, int x1, int y1,
                      int color, int filled) {
    if (x0 > x1) { int temporary = x0; x0 = x1; x1 = temporary; }
    if (y0 > y1) { int temporary = y0; y0 = y1; y1 = temporary; }
    int a = x1 - x0;
    int b = y1 - y0;
    if (!a || !b) {
        draw_line(runtime, x0, y0, x1, y1, color);
        return;
    }
    int odd = b & 1;
    int dx = 4 * (1 - a) * b * b;
    int dy = 4 * (odd + 1) * a * a;
    int error = dx + dy + odd * a * a;
    y0 += (b + 1) / 2;
    y1 = y0 - odd;
    a = 8 * a * a;
    odd = 8 * b * b;

    do {
        if (filled) {
            draw_hspan(runtime, x0, x1, y0, color);
            if (y1 != y0) draw_hspan(runtime, x0, x1, y1, color);
        } else {
            screen_set(runtime, x1, y0, color);
            screen_set(runtime, x0, y0, color);
            if (y1 != y0) {
                screen_set(runtime, x0, y1, color);
                screen_set(runtime, x1, y1, color);
            }
        }
        int twice_error = 2 * error;
        if (twice_error <= dy) {
            ++y0;
            --y1;
            error += dy += a;
        }
        if (twice_error >= dx || 2 * error > dy) {
            ++x0;
            --x1;
            error += dx += odd;
        }
    } while (x0 <= x1);

    while (y0 - y1 < b) {
        if (filled) {
            draw_hspan(runtime, x0 - 1, x1 + 1, y0, color);
            draw_hspan(runtime, x0 - 1, x1 + 1, y1, color);
        } else {
            screen_set(runtime, x0 - 1, y0, color);
            screen_set(runtime, x1 + 1, y0, color);
            screen_set(runtime, x0 - 1, y1, color);
            screen_set(runtime, x1 + 1, y1, color);
        }
        ++y0;
        --y1;
    }
}

static int api_oval(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    draw_oval(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
              arg_int(lua, 3, 0), arg_int(lua, 4, 0),
              arg_pen(lua, 5, active_runtime), 0);
    return 0;
}

static int api_ovalfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    draw_oval(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
              arg_int(lua, 3, 0), arg_int(lua, 4, 0),
              arg_pen(lua, 5, active_runtime), 1);
    return 0;
}

static int api_tline(lua_State *lua) {
    flush_redirected_drawing(active_runtime);
    profile_api(P8P_API_SPRITE);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, 0);
    int y1 = arg_int(lua, 4, 0);
    fix32 mx = arg_number(lua, 5, fix32(0));
    fix32 my = arg_number(lua, 6, fix32(0));
    fix32 mdx = arg_number(lua, 7, fix32::frombits(0x2000));
    fix32 mdy = arg_number(lua, 8, fix32(0));
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        int32_t mx_bits = mx.bits();
        int32_t my_bits = my.bits();
        int wrap_x = runtime->ram[0x5f38] ? runtime->ram[0x5f38] : 256;
        int wrap_y = runtime->ram[0x5f39] ? runtime->ram[0x5f39] : 256;
        int map_x = (mx_bits >> 16) % wrap_x;
        int map_y = (my_bits >> 16) % wrap_y;
        if (map_x < 0) map_x += wrap_x;
        if (map_y < 0) map_y += wrap_y;
        map_x += runtime->ram[0x5f3a];
        map_y += runtime->ram[0x5f3b];
        int sprite = map_get(runtime, map_x, map_y);
        if (sprite) {
            int source_x = (sprite & 15) * 8 + ((mx_bits >> 13) & 7);
            int source_y = (sprite >> 4) * 8 + ((my_bits >> 13) & 7);
            uint8_t color = sprite_get(runtime, source_x, source_y);
            if (!runtime->transparent[color]) {
                int screen_x = x0 - runtime->camera_x;
                int screen_y = y0 - runtime->camera_y;
                if ((unsigned)screen_x < 128u && (unsigned)screen_y < 128u &&
                    in_clip(runtime, screen_x, screen_y))
                    screen_set_unchecked(runtime, screen_x, screen_y,
                                         runtime->draw_palette[color]);
            }
        }
        if (x0 == x1 && y0 == y1)
            break;
        int twice_error = error * 2;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
        mx += mdx;
        my += mdy;
    }
    return 0;
}

static int api_mget(lua_State *lua) {
    flush_redirected_drawing(active_runtime);
    profile_api(P8P_API_MEMORY);
    /* lua_tonumber(nil) is zero, which is exactly mget()'s fallback.  BAS
     * Escape calls this hundreds of times per frame; avoiding two gettop +
     * nil checks on every collision probe is measurable on the 100 MHz CPU. */
    int x = (int32_t)lua_tonumber(lua, 1);
    int y = (int32_t)lua_tonumber(lua, 2);
    push_int(lua, map_get(active_runtime, x, y));
    return 1;
}

static int api_mset(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    map_set(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
            (uint8_t)arg_int(lua, 3, 0));
    return 0;
}

static int api_fget(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int sprite = arg_int(lua, 1, 0) & 255;
    uint8_t flags = active_runtime->ram[0x3000 + sprite];
    if (arg_count(lua) < 2) {
        push_int(lua, flags);
    } else {
        int flag = arg_int(lua, 2, 0) & 7;
        setbvalue(lua->top, (flags & (1u << flag)) != 0);
        ++lua->top;
    }
    return 1;
}

static int api_fset(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int sprite = arg_int(lua, 1, 0) & 255;
    if (arg_count(lua) < 3) {
        active_runtime->ram[0x3000 + sprite] = (uint8_t)arg_int(lua, 2, 0);
    } else {
        int flag = arg_int(lua, 2, 0) & 7;
        if (lua_toboolean(lua, 3))
            active_runtime->ram[0x3000 + sprite] |= (uint8_t)(1u << flag);
        else
            active_runtime->ram[0x3000 + sprite] &= (uint8_t)~(1u << flag);
    }
    return 0;
}

/* map() spends most of its time dispatching hundreds of ordinary 8x8 tiles.
 * Keep that hot path separate from the fully general scaled/flipped sprite
 * renderer so it does not rebuild the same width, height and camera state for
 * every cell. */
static P8P_FASTTEXT void blit_tile(p8p_runtime_t *runtime, int sprite,
                                   int screen_x, int screen_y,
                                   int sprite_base) {
    int start_x = screen_x < runtime->clip_x0 ?
        runtime->clip_x0 - screen_x : 0;
    int start_y = screen_y < runtime->clip_y0 ?
        runtime->clip_y0 - screen_y : 0;
    int end_x = screen_x + 8 > runtime->clip_x1 ?
        runtime->clip_x1 - screen_x : 8;
    int end_y = screen_y + 8 > runtime->clip_y1 ?
        runtime->clip_y1 - screen_y : 8;
    if (start_x >= end_x || start_y >= end_y)
        return;

    int source_x = (sprite & 15) * 8;
    int source_y = (sprite >> 4) * 8;
    int default_colors = runtime->palettes_default &&
                         runtime->transparency_default;
    if (!runtime->palettes_default && runtime->transparency_default)
        default_colors = p8p_draw_palette_identity(runtime->draw_palette);
    runtime->screen_ram_dirty = 1;
    for (int y = start_y; y < end_y; ++y) {
        const uint8_t *source = runtime->ram + sprite_base +
            (source_y + y) * 64 + source_x / 2;
        uint8_t *destination = runtime->framebuffer +
            (screen_y + y) * 128 + screen_x;
        int x = start_x;
        if (x & 1) {
            uint8_t color = source[x >> 1] >> 4;
            if (default_colors) {
                if (color) destination[x] = color;
            } else if (!runtime->transparent[color]) {
                destination[x] = runtime->draw_palette[color] & 15;
            }
            ++x;
        }
        for (; x + 1 < end_x; x += 2) {
            uint8_t packed = source[x >> 1];
            uint8_t color0 = packed & 15;
            uint8_t color1 = packed >> 4;
            if (default_colors) {
                if (color0) destination[x] = color0;
                if (color1) destination[x + 1] = color1;
            } else {
                if (!runtime->transparent[color0])
                    destination[x] = runtime->draw_palette[color0] & 15;
                if (!runtime->transparent[color1])
                    destination[x + 1] = runtime->draw_palette[color1] & 15;
            }
        }
        if (x < end_x) {
            uint8_t packed = source[x >> 1];
            uint8_t color = (uint8_t)((x & 1) ? packed >> 4 : packed & 15);
            if (default_colors) {
                if (color) destination[x] = color;
            } else if (!runtime->transparent[color]) {
                destination[x] = runtime->draw_palette[color] & 15;
            }
        }
    }
}

static void draw_map_tile(p8p_runtime_t *runtime, int sprite,
                          int screen_x, int screen_y, int sprite_base) {
    profile_api(P8P_API_SPRITE);
    blit_tile(runtime, sprite, screen_x, screen_y, sprite_base);
}

static int api_map(lua_State *lua) {
    flush_redirected_drawing(active_runtime);
    profile_api(P8P_API_SPRITE);
    p8p_runtime_t *runtime = active_runtime;
    int cell_x = arg_int(lua, 1, 0);
    int cell_y = arg_int(lua, 2, 0);
    int screen_x = arg_int(lua, 3, 0);
    int screen_y = arg_int(lua, 4, 0);
    int cell_w = arg_int(lua, 5, 16);
    int cell_h = arg_int(lua, 6, 16);
    int layer = arg_int(lua, 7, 0);
    int origin_x = screen_x - runtime->camera_x;
    int origin_y = screen_y - runtime->camera_y;
    int first_x = runtime->clip_x0 > origin_x ?
        (runtime->clip_x0 - origin_x) / 8 : 0;
    int first_y = runtime->clip_y0 > origin_y ?
        (runtime->clip_y0 - origin_y) / 8 : 0;
    int visible_w = runtime->clip_x1 - origin_x;
    int visible_h = runtime->clip_y1 - origin_y;
    int end_x = visible_w > 0 ? (visible_w + 7) / 8 : 0;
    int end_y = visible_h > 0 ? (visible_h + 7) / 8 : 0;
    if (first_x < 0) first_x = 0;
    if (first_y < 0) first_y = 0;
    if (end_x > cell_w) end_x = cell_w;
    if (end_y > cell_h) end_y = cell_h;
    int sprite_base = (int)runtime->ram[0x5f54] << 8;
    int fast_sprite_base = sprite_base >= 0 &&
                           sprite_base + 8192 <= 0x10000;
    for (int y = first_y; y < end_y; ++y) {
        for (int x = first_x; x < end_x; ++x) {
            int sprite = map_get(runtime, cell_x + x, cell_y + y);
            if (sprite && (!layer || (runtime->ram[0x3000 + sprite] & layer))) {
                if (fast_sprite_base)
                    draw_map_tile(runtime, sprite, origin_x + x * 8,
                                  origin_y + y * 8, sprite_base);
                else
                    draw_sprite(runtime, sprite, screen_x + x * 8,
                                screen_y + y * 8, 1, 1, 0, 0);
            }
        }
    }
    return 0;
}

static int api_camera(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    int old_x = active_runtime->camera_x;
    int old_y = active_runtime->camera_y;
    active_runtime->camera_x = arg_int(lua, 1, 0);
    active_runtime->camera_y = arg_int(lua, 2, 0);
    camera_to_ram(active_runtime);
    push_int(lua, old_x);
    push_int(lua, old_y);
    return 2;
}

static int api_clip(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    int old_x0 = runtime->clip_x0;
    int old_x1 = runtime->clip_x1;
    int old_y0 = runtime->clip_y0;
    int old_y1 = runtime->clip_y1;
    if (arg_count(lua) == 0) {
        runtime->clip_x0 = runtime->clip_y0 = 0;
        runtime->clip_x1 = runtime->clip_y1 = 128;
    } else {
        int x = arg_int(lua, 1, 0);
        int y = arg_int(lua, 2, 0);
        int width = arg_int(lua, 3, 128);
        int height = arg_int(lua, 4, 128);
        runtime->clip_x0 = x < 0 ? 0 : x;
        runtime->clip_y0 = y < 0 ? 0 : y;
        runtime->clip_x1 = x + width > 128 ? 128 : x + width;
        runtime->clip_y1 = y + height > 128 ? 128 : y + height;
    }
    runtime->ram[0x5f20] = (uint8_t)runtime->clip_x0;
    runtime->ram[0x5f21] = (uint8_t)runtime->clip_y0;
    runtime->ram[0x5f22] = (uint8_t)runtime->clip_x1;
    runtime->ram[0x5f23] = (uint8_t)runtime->clip_y1;
    push_int(lua, old_x0);
    push_int(lua, old_x1);
    push_int(lua, old_y0);
    push_int(lua, old_y1);
    return 4;
}

static void reset_transparency(p8p_runtime_t *runtime) {
    if (runtime->transparency_default)
        return;
    memset(runtime->transparent, 0, sizeof(runtime->transparent));
    runtime->transparent[0] = 1;
    for (int color = 0; color < 16; ++color)
        runtime->ram[0x5f00 + color] = (uint8_t)(
            runtime->draw_palette[color] | (color == 0 ? 0x10 : 0));
    runtime->transparency_default = 1;
}

extern "C" __attribute__((noinline)) int p8p_api_pal_full(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    if (arg_count(lua) == 0) {
        /* Like PICO-8, pal() also resets transparency to palt(). */
        if (!runtime->palettes_default) {
            for (int i = 0; i < 16; ++i) {
                runtime->draw_palette[i] = (uint8_t)i;
                runtime->screen_palette[i] = (uint8_t)i;
                palette_entry_to_ram(runtime, i);
            }
            runtime->palettes_default = 1;
        }
        reset_transparency(runtime);
    } else if (lua_istable(lua, 1)) {
        int screen = arg_int(lua, 2, 0) == 1;
        lua_pushnil(lua);
        while (lua_next(lua, 1) != 0) {
            if (lua_isnumber(lua, -2) && lua_isnumber(lua, -1)) {
                int from = (int)lua_tonumber(lua, -2) & 15;
                int to = (int)lua_tonumber(lua, -1) &
                         (screen ? 0x8f : 15);
                uint8_t *entry = screen ? &runtime->screen_palette[from] :
                                          &runtime->draw_palette[from];
                if (*entry != (uint8_t)to) {
                    *entry = (uint8_t)to;
                    runtime->palettes_default = 0;
                    palette_entry_to_ram(runtime, from);
                }
            }
            lua_pop(lua, 1);
        }
    } else {
        int from = arg_int(lua, 1, 0) & 15;
        int screen = arg_int(lua, 3, 0) == 1;
        int to = arg_int(lua, 2, from) & (screen ? 0x8f : 15);
        int previous = screen ? runtime->screen_palette[from]
                              : runtime->draw_palette[from];
        if (previous != to) {
            if (screen)
                runtime->screen_palette[from] = (uint8_t)to;
            else
                runtime->draw_palette[from] = (uint8_t)to;
            runtime->palettes_default = 0;
            palette_entry_to_ram(runtime, from);
        }
        push_int(lua, previous);
        return 1;
    }
    return 0;
}

static int api_palt(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    if (arg_count(lua) == 0) {
        reset_transparency(runtime);
    } else if (arg_count(lua) == 1) {
        /* palt(bitfield): bit 15-i makes colour i transparent. */
        int bits = arg_int(lua, 1, 0) & 0xffff;
        for (int color = 0; color < 16; ++color) {
            runtime->transparent[color] = (uint8_t)((bits >> (15 - color)) & 1);
            runtime->ram[0x5f00 + color] = (uint8_t)(
                runtime->draw_palette[color] |
                (runtime->transparent[color] ? 0x10 : 0));
        }
        runtime->transparency_default = bits == 0x8000;
    } else {
        int color = arg_int(lua, 1, 0) & 15;
        uint8_t transparent =
            (uint8_t)(arg_count(lua) < 2 || lua_toboolean(lua, 2));
        if (runtime->transparent[color] != transparent) {
            runtime->transparent[color] = transparent;
            runtime->transparency_default = 0;
            runtime->ram[0x5f00 + color] = (uint8_t)(
                runtime->draw_palette[color] | (transparent ? 0x10 : 0));
        }
    }
    return 0;
}

static int api_btn(lua_State *lua) {
    profile_api(P8P_API_INPUT);
    if (arg_count(lua) == 0) {
        push_int(lua, active_runtime->buttons & 0x7f);
    } else {
        int button = arg_int(lua, 1, 0);
        int player = arg_int(lua, 2, 0);
        lua_pushboolean(lua, button >= 0 && button < 7 &&
                       player == 0 &&
                       (active_runtime->buttons & (1u << button)) != 0);
    }
    return 1;
}

/* 0x5f5c/0x5f5d hold the btnp() repeat delay and interval in frames; 0 means
 * the default (15, 4) and a delay of 255 disables repeating. */
static int btnp_fires(const p8p_runtime_t *runtime, uint16_t held) {
    if (held == 1)
        return 1;
    int delay = runtime->ram[0x5f5c] ? runtime->ram[0x5f5c] : 15;
    int interval = runtime->ram[0x5f5d] ? runtime->ram[0x5f5d] : 4;
    if (delay == 255)
        return 0;
    return held > delay && (held - delay) % interval == 0;
}

static int api_btnp(lua_State *lua) {
    profile_api(P8P_API_INPUT);
    p8p_runtime_t *runtime = active_runtime;
    if (arg_count(lua) == 0) {
        int mask = 0;
        for (int button = 0; button < 7; ++button)
            if (btnp_fires(runtime, runtime->held_frames[button]))
                mask |= 1 << button;
        push_int(lua, mask);
        return 1;
    }
    int button = arg_int(lua, 1, 0);
    int player = arg_int(lua, 2, 0);
    int pressed = 0;
    if (player == 0 && button >= 0 && button < 7) {
        pressed = btnp_fires(runtime, runtime->held_frames[button]);
    }
    lua_pushboolean(lua, pressed);
    return 1;
}

static int api_peek(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0) & 0xffff;
    int count = arg_int(lua, 2, 1);
    if (count < 1) count = 1;
    /* PICO-8 v0.2.5+ raised the multi-value peek limit from 8192. */
    if (count > 32767) count = 32767;
    /* C functions are only guaranteed LUA_MINSTACK free slots. */
    if (count > LUA_MINSTACK && !lua_checkstack(lua, count))
        return luaL_error(lua, "peek: stack overflow");
    if (range_touches_screen(address, count))
        screen_to_ram(active_runtime);
    for (int i = 0; i < count; ++i)
        push_int(lua, active_runtime->ram[(address + i) & 0xffff]);
    return count;
}

static int api_poke(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0) & 0xffff;
    int values = arg_count(lua) - 1;
    if (values < 1)
        values = 1;
    for (int i = 0; i < values; ++i)
        active_runtime->ram[(address + i) & 0xffff] =
            (uint8_t)arg_int(lua, i + 2, 0);
    if (range_touches_screen(address, values))
        ram_to_screen_range(active_runtime, address, values);
    if (range_touches_draw_state(address, values))
        draw_state_from_ram(active_runtime);
    if (range_touches_cartdata(address, values))
        cartdata_mark_dirty(active_runtime);
    return 0;
}

static int api_peek2(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    if (address < 0 || address > 0xfffe) {
        push_int(lua, 0);
        return 1;
    }
    if (range_touches_screen(address, 2))
        screen_to_ram(active_runtime);
    push_int(lua, active_runtime->ram[address] |
                  ((int)active_runtime->ram[address + 1] << 8));
    return 1;
}

static int api_poke2(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    int value = arg_int(lua, 2, 0);
    if (address < 0 || address > 0xfffe)
        return 0;
    active_runtime->ram[address] = (uint8_t)value;
    active_runtime->ram[address + 1] = (uint8_t)(value >> 8);
    if (range_touches_screen(address, 2))
        ram_to_screen_range(active_runtime, address, 2);
    if (range_touches_draw_state(address, 2))
        draw_state_from_ram(active_runtime);
    if (range_touches_cartdata(address, 2)) cartdata_mark_dirty(active_runtime);
    return 0;
}

static int api_peek4(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    if (address < 0 || address > 0xfffc) {
        lua_pushnumber(lua, fix32(0));
        return 1;
    }
    if (range_touches_screen(address, 4))
        screen_to_ram(active_runtime);
    uint32_t bits = (uint32_t)active_runtime->ram[address] |
        ((uint32_t)active_runtime->ram[address + 1] << 8) |
        ((uint32_t)active_runtime->ram[address + 2] << 16) |
        ((uint32_t)active_runtime->ram[address + 3] << 24);
    lua_pushnumber(lua, fix32::frombits((int32_t)bits));
    return 1;
}

static int api_poke4(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    if (address < 0 || address > 0xfffc)
        return 0;
    uint32_t bits = (uint32_t)arg_number(lua, 2, fix32(0)).bits();
    active_runtime->ram[address] = (uint8_t)bits;
    active_runtime->ram[address + 1] = (uint8_t)(bits >> 8);
    active_runtime->ram[address + 2] = (uint8_t)(bits >> 16);
    active_runtime->ram[address + 3] = (uint8_t)(bits >> 24);
    if (range_touches_screen(address, 4))
        ram_to_screen_range(active_runtime, address, 4);
    if (range_touches_draw_state(address, 4))
        draw_state_from_ram(active_runtime);
    if (range_touches_cartdata(address, 4)) cartdata_mark_dirty(active_runtime);
    return 0;
}

static int api_memset(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int destination = arg_int(lua, 1, 0);
    int value = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0);
    if (destination >= 0 && length >= 0 && destination + length <= 0x10000) {
        memset(active_runtime->ram + destination, value, (size_t)length);
        if (range_touches_screen(destination, length))
            ram_to_screen_range(active_runtime, destination, length);
        if (range_touches_draw_state(destination, length))
            draw_state_from_ram(active_runtime);
        if (range_touches_cartdata(destination, length))
            cartdata_mark_dirty(active_runtime);
    }
    return 0;
}

static int api_memcpy(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int destination = arg_int(lua, 1, 0);
    int source = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0);
    if (destination >= 0 && source >= 0 && length >= 0 &&
        destination + length <= 0x10000 && source + length <= 0x10000) {
        int reads_screen = range_touches_screen(source, length);
        int writes_screen = range_touches_screen(destination, length);
        if (reads_screen)
            screen_to_ram(active_runtime);
        memmove(active_runtime->ram + destination, active_runtime->ram + source,
                (size_t)length);
        if (writes_screen)
            ram_to_screen_range(active_runtime, destination, length);
        if (range_touches_draw_state(destination, length))
            draw_state_from_ram(active_runtime);
        if (range_touches_cartdata(destination, length))
            cartdata_mark_dirty(active_runtime);
    }
    return 0;
}

static int api_reload(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int destination = arg_int(lua, 1, 0);
    int source = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0x4300);
    if (destination >= 0 && source >= 0 && length >= 0 &&
        destination + length <= 0x10000 &&
        source + length <= (int)P8P_CART_ROM_SIZE) {
        int writes_screen = range_touches_screen(destination, length);
        memcpy(active_runtime->ram + destination, active_runtime->cart_rom + source,
               (size_t)length);
        if (writes_screen)
            ram_to_screen_range(active_runtime, destination, length);
        if (range_touches_draw_state(destination, length))
            draw_state_from_ram(active_runtime);
    }
    return 0;
}

static int api_cartdata(lua_State *lua) {
    p8p_runtime_t *runtime = active_runtime;
    size_t length = 0;
    const char *id = lua_tolstring(lua, 1, &length);
    if (!id || !length || length > 64)
        return luaL_error(lua, "cart data id must be 1-64 characters");
    if (runtime->cartdata_active &&
        (length != strlen(runtime->cartdata_id) ||
         memcmp(runtime->cartdata_id, id, length) != 0))
        cartdata_flush(runtime);
    memset(runtime->ram + 0x5e00, 0, 0x100);
    memcpy(runtime->cartdata_id, id, length);
    runtime->cartdata_id[length] = '\0';
    runtime->cartdata_active = 1;
    runtime->cartdata_dirty = 0;
    int loaded = runtime->cartdata_load &&
        runtime->cartdata_load(runtime->cartdata_userdata,
                               runtime->cartdata_id,
                               runtime->ram + 0x5e00, 0x100) == 0;
    lua_pushboolean(lua, loaded);
    return 1;
}

static int api_dget(lua_State *lua) {
    int index = arg_int(lua, 1, 0);
    if (index < 0 || index >= 64) {
        lua_pushnumber(lua, fix32(0));
        return 1;
    }
    int address = 0x5e00 + index * 4;
    uint32_t bits = (uint32_t)active_runtime->ram[address] |
        ((uint32_t)active_runtime->ram[address + 1] << 8) |
        ((uint32_t)active_runtime->ram[address + 2] << 16) |
        ((uint32_t)active_runtime->ram[address + 3] << 24);
    lua_pushnumber(lua, fix32::frombits((int32_t)bits));
    return 1;
}

static int api_dset(lua_State *lua) {
    int index = arg_int(lua, 1, 0);
    if (index < 0 || index >= 64)
        return 0;
    uint32_t bits = (uint32_t)arg_number(lua, 2, fix32(0)).bits();
    int address = 0x5e00 + index * 4;
    active_runtime->ram[address] = (uint8_t)bits;
    active_runtime->ram[address + 1] = (uint8_t)(bits >> 8);
    active_runtime->ram[address + 2] = (uint8_t)(bits >> 16);
    active_runtime->ram[address + 3] = (uint8_t)(bits >> 24);
    cartdata_mark_dirty(active_runtime);
    return 0;
}

static void update_rng(p8p_runtime_t *runtime) {
    runtime->rng[1] = runtime->rng[0] +
        ((runtime->rng[1] >> 16) | (runtime->rng[1] << 16));
    runtime->rng[0] += runtime->rng[1];
}

static int api_srand(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    p8p_runtime_t *runtime = active_runtime;
    fix32 seed = arg_number(lua, 1, fix32(0));
    runtime->rng[0] = seed ? (uint32_t)seed.bits() : 0xdeadbeef;
    runtime->rng[1] = runtime->rng[0] ^ 0xbead29ba;
    for (int i = 0; i < 32; ++i)
        update_rng(runtime);
    return 0;
}

static int api_rnd(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    p8p_runtime_t *runtime = active_runtime;
    update_rng(runtime);
    if (lua_istable(lua, 1)) {
        int length = (int)lua_rawlen(lua, 1);
        if (length <= 0) {
            lua_pushnil(lua);
        } else {
            lua_rawgeti(lua, 1, (int)(runtime->rng[1] % (uint32_t)length) + 1);
        }
    } else {
        fix32 range = arg_number(lua, 1, fix32(1));
        uint32_t bits = (uint32_t)range.bits();
        lua_pushnumber(lua, fix32::frombits(bits ? runtime->rng[1] % bits : 0));
    }
    return 1;
}

static int api_time(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    /* frame_count / fps in 16.16 fixed point, truncated and wrapped exactly
     * like fix32((double)frame_count / fps) but without the soft-float
     * divide and conversion (several hundred RV32 instructions per call;
     * Celeste 2 calls time() for every pixel of its grapple rope). */
    uint32_t fps = (uint32_t)active_runtime->target_fps;
    uint32_t frames = active_runtime->frame_count;
    uint32_t bits = (frames / fps) * 65536u + ((frames % fps) << 16) / fps;
    lua_pushnumber(lua, fix32::frombits((int32_t)bits));
    return 1;
}

static int api_cursor(lua_State *lua) {
    profile_api(P8P_API_TEXT);
    int old_x = active_runtime->cursor_x;
    int old_y = active_runtime->cursor_y;
    if (arg_count(lua) >= 1) active_runtime->cursor_x = arg_int(lua, 1, old_x);
    if (arg_count(lua) >= 2) active_runtime->cursor_y = arg_int(lua, 2, old_y);
    cursor_to_ram(active_runtime);
    push_int(lua, old_x);
    push_int(lua, old_y);
    return 2;
}

/*
 * PICO-8 text engine: P8SCII control codes, the full 256-glyph default font
 * and the custom font at 0x5600.  Ported from Fake-08's printHelper.cpp and
 * Graphics::drawCharacter (MIT), adapted to this runtime's RAM model.
 */
enum {
    PRINT_ON = 0x01,
    PRINT_PADDING = 0x02,
    PRINT_WIDE = 0x04,
    PRINT_TALL = 0x08,
    PRINT_SOLID_BG = 0x10,
    PRINT_INVERTED = 0x20,
    PRINT_STRIPEY = 0x40,
    PRINT_CUSTOM_FONT = 0x80
};

/* P8SCII parameter characters: 0-9 then a-z for 10-35. */
static int print_param(uint8_t character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a')
        return character - 'a' + 10;
    return 0;
}

static int print_hex(const uint8_t *text, size_t length, size_t at, int digits) {
    int value = 0;
    for (int i = 0; i < digits; ++i) {
        uint8_t c = at + (size_t)i < length ? text[at + (size_t)i] : '0';
        int nibble = c >= '0' && c <= '9' ? c - '0' :
                     c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                     c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
        value = value * 16 + nibble;
    }
    return value;
}

/* Same RAM side effects as poke(). */
static void write_ram_bytes(p8p_runtime_t *runtime, int address,
                            const uint8_t *bytes, int count) {
    address &= 0xffff;
    if (count <= 0)
        return;
    for (int i = 0; i < count; ++i)
        runtime->ram[(address + i) & 0xffff] = bytes[i];
    if (range_touches_screen(address, count))
        ram_to_screen_range(runtime, address, count);
    if (range_touches_draw_state(address, count))
        draw_state_from_ram(runtime);
    if (range_touches_cartdata(address, count))
        cartdata_mark_dirty(runtime);
}

/* Returns the extra width/height produced by wide/tall modes. */
static void draw_glyph_rows(p8p_runtime_t *runtime, const uint8_t *rows,
                            int x, int y, uint8_t fg, uint8_t bg, int mode,
                            int width, int height, int *extra_width,
                            int *extra_height) {
    int w_factor = 1, h_factor = 1;
    int stripey = 0, inverted = 0, solid_bg = 0;
    *extra_width = *extra_height = 0;
    if (mode & PRINT_ON) {
        if (mode & PRINT_WIDE) { w_factor = 2; *extra_width = width; }
        if (mode & PRINT_TALL) { h_factor = 2; *extra_height = height; }
        stripey = (mode & PRINT_STRIPEY) != 0;
        inverted = (mode & PRINT_INVERTED) != 0;
        solid_bg = (mode & PRINT_SOLID_BG) != 0;
    }
    x -= runtime->camera_x;
    y -= runtime->camera_y;
    fg &= 15;
    bg &= 15;
    int out_w = width * w_factor;
    int out_h = height * h_factor;
    if (w_factor == 1 && h_factor == 1 && !inverted && !solid_bg &&
        width <= 8 && height <= 8 &&
        x >= runtime->clip_x0 && x + out_w <= runtime->clip_x1 &&
        y >= runtime->clip_y0 && y + out_h <= runtime->clip_y1) {
        /* Common case: plain glyph fully inside the clip rectangle. */
        uint8_t *row_start = runtime->framebuffer + y * 128 + x;
        for (int row = 0; row < height; ++row, row_start += 128) {
            unsigned bits = rows[row];
            for (int column = 0; bits && column < width; ++column, bits >>= 1)
                if (bits & 1u)
                    row_start[column] = fg;
        }
        runtime->screen_ram_dirty = 1;
        return;
    }
    for (int dy = 0; dy < out_h; ++dy) {
        int font_row = dy / h_factor;
        if (font_row >= 8)
            continue;
        for (int dx = 0; dx < out_w; ++dx) {
            int font_column = dx / w_factor;
            if (font_column >= 8)
                continue;
            int on = (rows[font_row] >> font_column) & 1;
            if (stripey && h_factor > 1 && (dy & 1)) on = 0;
            if (stripey && w_factor > 1 && (dx & 1)) on = 0;
            int px = x + dx, py = y + dy;
            if (!in_clip(runtime, px, py) || (unsigned)px >= 128u ||
                (unsigned)py >= 128u)
                continue;
            if (inverted)
                on = !on;
            if (on)
                runtime->framebuffer[py * 128 + px] = fg;
            else if (solid_bg)
                runtime->framebuffer[py * 128 + px] = bg;
            else
                continue;
            runtime->screen_ram_dirty = 1;
        }
    }
}

static int draw_glyph(p8p_runtime_t *runtime, uint8_t character, int x, int y,
                      uint8_t fg, uint8_t bg, int mode, int force_width,
                      int force_height) {
    int extra = 0;
    int custom = (mode & PRINT_CUSTOM_FONT) != 0;
    const uint8_t *font = custom ? runtime->ram + 0x5600 : p8p_pico8_font;
    int char_width = font[0];
    int wide_width = font[1];
    int char_height = font[2];
    if (character > 0x0f) {
        int width_forced = force_width > -1 && force_width < 4;
        int render_width = width_forced ?
            (character < 0x80 ? force_width : force_width + 4) :
            custom ? 8 : (character < 0x80 ? char_width : wide_width);
        if (character >= 0x80)
            extra = wide_width - char_width;
        int render_height = force_height > -1 && force_height < 5 ?
            force_height : char_height;
        int sx = x - runtime->camera_x, sy = y - runtime->camera_y;
        if (!(mode & PRINT_ON) && render_width <= 8 && render_height <= 8 &&
            sx >= runtime->clip_x0 && sx + render_width <= runtime->clip_x1 &&
            sy >= runtime->clip_y0 && sy + render_height <= runtime->clip_y1) {
            /* Plain glyph fully inside the clip rectangle. */
            const uint8_t *rows = font + character * 8;
            unsigned mask = (1u << render_width) - 1u;
            uint8_t color = fg & 15;
            uint8_t *row_start = runtime->framebuffer + sy * 128 + sx;
            for (int row = 0; row < render_height; ++row, row_start += 128) {
                unsigned bits = rows[row] & mask;
                for (uint8_t *pixel = row_start; bits; bits >>= 1, ++pixel)
                    if (bits & 1u)
                        *pixel = color;
            }
            runtime->screen_ram_dirty = 1;
            return extra;
        }
        int extra_width, extra_height;
        draw_glyph_rows(runtime, font + character * 8, x, y, fg, bg, mode,
                        render_width, render_height, &extra_width,
                        &extra_height);
        extra += extra_width;
    }
    if (mode & PRINT_ON)
        return 0;
    return extra;
}

static int api_print(lua_State *lua) {
    profile_api(P8P_API_TEXT);
    p8p_runtime_t *runtime = active_runtime;
    size_t length = 0;
    /* Strings need no conversion; luaL_tolstring would still probe for a
     * __tostring metamethod and push a copy. */
    int converted = lua_type(lua, 1) != LUA_TSTRING;
    const uint8_t *text = (const uint8_t *)(converted ?
        luaL_tolstring(lua, 1, &length) : lua_tolstring(lua, 1, &length));
    int arguments = arg_count(lua) - converted;
    int x = runtime->cursor_x, y = runtime->cursor_y;
    if (arguments == 2) {
        /* print(str, col) */
        runtime->draw_color = arg_int(lua, 2, runtime->draw_color) & 255;
        runtime->ram[0x5f25] = (uint8_t)runtime->draw_color;
    } else if (arguments >= 3) {
        x = arg_int(lua, 2, x);
        y = arg_int(lua, 3, y);
        if (arguments >= 4) {
            runtime->draw_color = arg_int(lua, 4, runtime->draw_color) & 255;
            runtime->ram[0x5f25] = (uint8_t)runtime->draw_color;
        }
    }

    int home_x = x, home_y = y;
    int prev_x = x, prev_y = y;
    int right_x = x;
    int tab_width = 4;
    int char_width = 4, char_height = 6;
    int line_height = 0;
    int force_width = -1, force_height = -1;
    int bg_color = -1;
    int fg_color = runtime->draw_color & 15;
    int cancel_wrap = 0;
    int outline_color = 0, outline_neighbours = 0;
    int underline = 0;
    int wrap_x = -1;
    int mode = runtime->ram[0x5f58];
    if (!(mode & PRINT_ON))
        mode = 0;
    if (mode & PRINT_CUSTOM_FONT) {
        char_width = runtime->ram[0x5600];
        char_height = runtime->ram[0x5602];
    }
    const uint8_t *pal = runtime->draw_palette;
#define PRINT_NEXT() (n + 1 < length ? text[++n] : (++n, (uint8_t)0))
#define PRINT_BG() ((uint8_t)(bg_color < 0 ? 0 : pal[bg_color & 15]))

    for (size_t n = 0; n < length; ++n) {
        uint8_t ch = text[n];
        if (ch == 0) {
            break;
        } else if (ch == 1) {                 /* \* repeat */
            int times = print_param(PRINT_NEXT());
            uint8_t repeated = PRINT_NEXT();
            for (int i = 0; i < times; ++i)
                x += char_width + draw_glyph(runtime, repeated, x, y,
                    pal[fg_color], PRINT_BG(), mode, force_width, force_height);
        } else if (ch == 2) {                 /* \# background */
            bg_color = print_param(PRINT_NEXT());
            mode |= PRINT_ON | PRINT_SOLID_BG;
        } else if (ch == 3) {                 /* \- */
            x += print_param(PRINT_NEXT()) - 16;
        } else if (ch == 4) {                 /* \| */
            y += print_param(PRINT_NEXT()) - 16;
        } else if (ch == 5) {                 /* \+ */
            x += print_param(PRINT_NEXT()) - 16;
            y += print_param(PRINT_NEXT()) - 16;
        } else if (ch == 6) {                 /* \^ commands */
            uint8_t command = PRINT_NEXT();
            if (command >= '1' && command <= '9') {
                /* Frame delays are not emulated. */
            } else if (command == 'd' || command == 's' || command == 'r' ||
                       command == 'c' || command == 'x' || command == 'y') {
                int value = print_param(PRINT_NEXT());
                if (command == 's') {
                    tab_width = value;
                } else if (command == 'r') {
                    wrap_x = value * 4;
                } else if (command == 'c') {
                    uint8_t color = pal[value & 15] & 15;
                    memset(runtime->framebuffer, color,
                           sizeof(runtime->framebuffer));
                    runtime->screen_ram_dirty = 1;
                } else if (command == 'x') {
                    force_width = char_width = value;
                } else if (command == 'y') {
                    force_height = char_height = value;
                }
            } else if (command == 'g') {
                x = home_x;
                y = home_y;
            } else if (command == 'h') {
                home_x = x;
                home_y = y;
            } else if (command == 'j') {
                x = print_param(PRINT_NEXT()) * 4;
                y = print_param(PRINT_NEXT()) * 4;
            } else if (command == 'w') {
                mode |= PRINT_ON | PRINT_WIDE;
                char_width = 8;
            } else if (command == 't') {
                mode |= PRINT_ON | PRINT_TALL;
                char_height = 12;
            } else if (command == '=') {
                mode |= PRINT_ON | PRINT_STRIPEY;
            } else if (command == 'p') {
                mode |= PRINT_ON | PRINT_WIDE | PRINT_TALL | PRINT_STRIPEY;
                char_width = 8;
                char_height = 12;
            } else if (command == 'i') {
                mode |= PRINT_ON | PRINT_INVERTED;
            } else if (command == 'b') {
                mode |= PRINT_ON | PRINT_PADDING;
            } else if (command == '#') {
                mode |= PRINT_ON | PRINT_SOLID_BG;
            } else if (command == ':' || command == ';' ||
                       command == '.' || command == ',') {
                /* One-off 8x8 glyph from hex (: ;) or raw bytes (. ,). */
                uint8_t rows[8] = {0};
                int glyph_height = force_height > 0 ? force_height : 8;
                for (int i = 0; i < 8; ++i) {
                    if (command == ':' || command == ';') {
                        rows[i] = (uint8_t)print_hex(text, length, n + 1 + i * 2, 2);
                    } else {
                        rows[i] = n + 1 + i < length ? text[n + 1 + i] : 0;
                    }
                }
                n += (command == ':' || command == ';') ? 16 : 8;
                int extra_width, extra_height;
                draw_glyph_rows(runtime, rows, x, y, pal[fg_color], PRINT_BG(),
                                mode, 8, glyph_height, &extra_width,
                                &extra_height);
                x += 8 + extra_width;
                glyph_height += extra_height;
                if (glyph_height > line_height)
                    line_height = glyph_height;
            } else if (command == '-') {
                uint8_t off = PRINT_NEXT();
                if (mode) {
                    if (off == 'w') { mode &= ~PRINT_WIDE; char_width = 4; }
                    else if (off == 't') { mode &= ~PRINT_TALL; char_height = 6; }
                    else if (off == '=') mode &= ~PRINT_STRIPEY;
                    else if (off == 'p') {
                        mode &= ~(PRINT_WIDE | PRINT_TALL | PRINT_STRIPEY);
                        char_width = 4;
                        char_height = 6;
                    }
                    else if (off == 'i') mode &= ~PRINT_INVERTED;
                    else if (off == 'b') mode &= ~PRINT_PADDING;
                    else if (off == '#') mode &= ~PRINT_SOLID_BG;
                }
            } else if (command == '!') {
                /* Poke the rest of the string at a hex address. */
                int address = print_hex(text, length, n + 1, 4);
                size_t data = n + 5;
                if (data < length)
                    write_ram_bytes(runtime, address, text + data,
                                    (int)(length - data));
                n = length;
                cancel_wrap = 1;
            } else if (command == '@') {
                int address = print_hex(text, length, n + 1, 4);
                int count = print_hex(text, length, n + 5, 4);
                size_t data = n + 9;
                if (data > length) data = length;
                if ((size_t)count > length - data)
                    count = (int)(length - data);
                write_ram_bytes(runtime, address, text + data, count);
                n = data + (size_t)count - 1;
            } else if (command == 'o') {
                outline_color = print_param(PRINT_NEXT());
                outline_neighbours = print_hex(text, length, n + 1, 2);
                n += 2;
            } else if (command == 'u') {
                underline = 1;
            }
        } else if (ch == 7) {                 /* \a audio: skipped */
            while (n + 1 < length && text[n + 1] != ' ')
                ++n;
            if (n + 1 < length)
                ++n;
        } else if (ch == 11) {                /* \v decorate */
            int offset = print_param(PRINT_NEXT());
            uint8_t decoration = PRINT_NEXT();
            draw_glyph(runtime, decoration, prev_x + offset % 4 - 2,
                       prev_y + offset / 4 - 8, pal[fg_color], PRINT_BG(),
                       mode, force_width, force_height);
        } else if (ch == 12) {                /* \f foreground */
            fg_color = print_param(PRINT_NEXT()) & 15;
        } else if (ch == 14) {                /* custom font on */
            mode |= PRINT_CUSTOM_FONT;
            char_width = runtime->ram[0x5600];
            char_height = runtime->ram[0x5602];
        } else if (ch == 15) {                /* custom font off */
            mode &= ~PRINT_CUSTOM_FONT;
            char_width = 4;
            char_height = 6;
        } else if (ch == '\n') {
            x = home_x;
            y += line_height > 0 ? line_height : 6;
            line_height = 0;
        } else if (ch == '\t') {
            int stop = tab_width * 4;
            if (stop > 0)
                while (x % stop)
                    ++x;
        } else if (ch == '\b') {
            x -= char_width;
        } else if (ch == '\r') {
            x = home_x;
        } else if (ch >= 0x10) {
            if (char_height > line_height)
                line_height = char_height;
            if ((mode & PRINT_SOLID_BG) && bg_color >= 0) {
                int saved_color = runtime->draw_color;
                for (int row = y - 1; row < y + line_height - 1; ++row)
                    draw_hspan(runtime, x - 1, x + char_width - 1, row,
                               bg_color);
                runtime->draw_color = saved_color;
            }
            prev_x = x;
            prev_y = y;
            if (outline_color && outline_neighbours) {
                static const int8_t ox[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
                static const int8_t oy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
                for (int i = 0; i < 8; ++i)
                    if (outline_neighbours & (1 << i))
                        draw_glyph(runtime, ch, x + ox[i], y + oy[i],
                                   pal[outline_color & 15], PRINT_BG(), mode,
                                   force_width, force_height);
            }
            if (underline)
                draw_hspan(runtime, x - 1, x + char_width - 1,
                           y + line_height, fg_color);
            int extra = draw_glyph(runtime, ch, x, y, pal[fg_color],
                                   PRINT_BG(), mode, force_width, force_height);
            int adjust = 0;
            if ((mode & PRINT_CUSTOM_FONT) && (runtime->ram[0x5605] & 1)) {
                int nibble_index = ch - 16;
                if (nibble_index >= 0 && nibble_index < 240) {
                    uint8_t packed = runtime->ram[0x5608 + nibble_index / 2];
                    int nibble = (nibble_index & 1) ? packed >> 4 : packed & 15;
                    adjust = nibble & 7;
                    if (adjust >= 4)
                        adjust -= 8;
                }
            }
            x += char_width + extra + adjust;
        }
        if (x > right_x)
            right_x = x;
        if (wrap_x > 0 && x >= wrap_x) {
            x = home_x;
            y += line_height > 0 ? line_height : 6;
            line_height = 0;
        }
    }
#undef PRINT_NEXT
#undef PRINT_BG
    if (line_height <= 0)
        line_height = cancel_wrap ? 0 : 6;
    runtime->cursor_x = home_x;
    runtime->cursor_y = y + line_height;
    cursor_to_ram(runtime);
    if (converted)
        lua_pop(lua, 1);
    push_int(lua, right_x);
    return 1;
}

static int api_sfx(lua_State *lua) {
    int channel = p8p_audio_sfx(active_runtime->audio,
                                arg_int(lua, 1, -1), arg_int(lua, 2, -1),
                                arg_int(lua, 3, 0), arg_int(lua, 4, 0));
    push_int(lua, channel < 0 ? 0 : channel);
    return 1;
}

static int api_music(lua_State *lua) {
    p8p_audio_music(active_runtime->audio,
                    arg_int(lua, 1, -1), arg_int(lua, 2, 0),
                    arg_int(lua, 3, 0));
    return 0;
}

static int api_flip(lua_State *lua) {
    int is_main_thread = lua_pushthread(lua);
    lua_pop(lua, 1);
    if (is_main_thread)
        return 0;
    return lua_yield(lua, 0);
}

static int api_fillp(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    int32_t previous = ((int32_t)runtime->fill_pattern << 16) |
                       ((int32_t)runtime->fill_pattern_transparent << 8);
    int32_t bits = arg_number(lua, 1, fix32(0)).bits();
    runtime->fill_pattern = (uint16_t)((uint32_t)bits >> 16);
    runtime->fill_pattern_transparent = (uint8_t)((bits >> 15) & 1);
    runtime->ram[0x5f31] = (uint8_t)runtime->fill_pattern;
    runtime->ram[0x5f32] = (uint8_t)(runtime->fill_pattern >> 8);
    runtime->ram[0x5f33] = runtime->fill_pattern_transparent;
    lua_pushnumber(lua, fix32::frombits(previous));
    return 1;
}

static int api_stat(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    p8p_runtime_t *runtime = active_runtime;
    int item = arg_int(lua, 1, 0);
    switch (item) {
    case 0:
        lua_pushnumber(lua, fix32(1));
        break;
    case 1:
    case 2:
        /* A conservative non-zero load also keeps carts which use stat(1)
         * as a cooperative-yield heuristic from choosing a busy loop. */
        lua_pushnumber(lua, fix32::frombits(0xc000));
        break;
    case 4:
        lua_pushstring(lua, "");
        break;
    case 6:
        lua_pushstring(lua, runtime->params);
        break;
    case 5:
        push_int(lua, 42);
        break;
    case 7:
    case 8:
        push_int(lua, runtime->target_fps);
        break;
    case 16: case 17: case 18: case 19:
        push_int(lua, p8p_audio_channel_sfx(runtime->audio, item - 16));
        break;
    case 20: case 21: case 22: case 23:
        push_int(lua, p8p_audio_channel_note(runtime->audio, item - 20));
        break;
    case 24:
    case 54:
        push_int(lua, p8p_audio_music_pattern(runtime->audio));
        break;
    case 25:
    case 55:
        push_int(lua, p8p_audio_music_count(runtime->audio));
        break;
    case 26:
    case 56:
        push_int(lua, p8p_audio_music_ticks(runtime->audio));
        break;
    case 28:
    case 30:
    case 120:
    case 121:
    case 122:
        lua_pushboolean(lua, 0);
        break;
    case 29:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 38:
    case 39:
    case 90:
    case 91:
    case 92:
    case 93:
    case 94:
    case 95:
    case 102:
        push_int(lua, 0);
        break;
    case 31:
        lua_pushstring(lua, "");
        break;
    case 100:
        if (runtime->breadcrumb[0])
            lua_pushstring(lua, runtime->breadcrumb);
        else
            lua_pushnil(lua);
        break;
    case 101:
        lua_pushnil(lua);
        break;
    case 108:
        /* PCM samples still queued from serial(0x808). */
        push_int(lua, p8p_audio_pcm_queued(runtime->audio));
        break;
    case 46: case 47: case 48: case 49:
        push_int(lua, p8p_audio_channel_sfx(runtime->audio, item - 46));
        break;
    case 50: case 51: case 52: case 53:
        push_int(lua, p8p_audio_channel_note(runtime->audio, item - 50));
        break;
    default:
        lua_pushnil(lua);
        break;
    }
    return 1;
}

static int api_run(lua_State *lua) {
    if (!lua_isnoneornil(lua, 1)) {
        size_t size;
        const char *params = luaL_checklstring(lua, 1, &size);
        if (size >= sizeof(active_runtime->params) || memchr(params, 0, size))
            return luaL_error(lua, "run parameters too long or contain NUL");
        memcpy(active_runtime->params, params, size + 1);
    }
    active_runtime->restart_requested = 1;
    return 0;
}

/* These replacements are installed only after legacy permanent numbering. */
static int api_load_cart(lua_State *lua) {
    size_t length = 0;
    const char *id = lua_type(lua, 1) == LUA_TSTRING ?
        lua_tolstring(lua, 1, &length) : NULL;
    if (!id || !length || id[0] != '#') {
        int arguments = lua_gettop(lua);
        lua_pushvalue(lua, lua_upvalueindex(1));
        lua_insert(lua, 1);
        lua_call(lua, arguments, LUA_MULTRET);
        return lua_gettop(lua);
    }
    p8p_runtime_cart_request_t request = {};
    request.kind = P8P_RUNTIME_CART_REQUEST_LOAD;
    if (length < 2 || length > sizeof(request.id) || memchr(id, 0, length))
        return luaL_error(lua, "invalid cart id length");
    memcpy(request.id, id + 1, length - 1);
    size_t breadcrumb_size = 0, params_size = 0;
    const char *breadcrumb = luaL_optlstring(lua, 2, "", &breadcrumb_size);
    const char *params = luaL_optlstring(lua, 3, "", &params_size);
    if (breadcrumb_size >= sizeof(request.breadcrumb) ||
        params_size >= sizeof(request.params) ||
        memchr(breadcrumb, 0, breadcrumb_size) || memchr(params, 0, params_size))
        return luaL_error(lua, "cart context too long or contains NUL");
    memcpy(request.breadcrumb, breadcrumb, breadcrumb_size);
    memcpy(request.params, params, params_size);
    active_runtime->cart_request = request;
    active_runtime->cart_request_pending = 1;
    active_runtime->cart_halted = 1;
    return lua_yield(lua, 0);
}

/* A child coroutine's handoff must also stop the caller of resume(). */
static int api_resume_cart(lua_State *lua) {
    int arguments = lua_gettop(lua);
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    lua_call(lua, arguments, LUA_MULTRET);
    if (active_runtime->cart_halted)
        return lua_yield(lua, 0);
    return lua_gettop(lua);
}

static int api_protected_call_finish(lua_State *lua) {
    if (active_runtime->cart_halted)
        return lua_yield(lua, 0);
    int status = lua_getctx(lua, NULL);
    if (status != LUA_OK && status != LUA_YIELD)
        return lua_error(lua);
    return lua_gettop(lua);
}

static int api_handoff_error_handler(lua_State *lua) {
    if (active_runtime->cart_halted)
        return luaL_error(lua, "cart handoff pending");
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    lua_call(lua, lua_gettop(lua) - 1, LUA_MULTRET);
    return lua_gettop(lua);
}

/* Native callbacks such as sort/gsub cannot yield. Even if native pcall
 * catches that error, the pending handoff stops its caller before return. */
static int api_protected_call(lua_State *lua) {
    int arguments = lua_gettop(lua);
    if (lua_toboolean(lua, lua_upvalueindex(2)) && lua_isfunction(lua, 2)) {
        lua_pushvalue(lua, 2);
        lua_pushcclosure(lua, api_handoff_error_handler, 1);
        lua_replace(lua, 2);
    }
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    /* Eris records C continuations on protected-call frames. A plain callk
     * frame loses its continuation when a suspended coroutine is restored. */
    int status = lua_pcallk(lua, arguments, LUA_MULTRET, 0, 0,
                            api_protected_call_finish);
    if (active_runtime->cart_halted)
        return lua_yield(lua, 0);
    if (status != LUA_OK)
        return lua_error(lua);
    return lua_gettop(lua);
}

static int api_extcmd(lua_State *lua) {
    const char *command = lua_tostring(lua, 1);
    if (command && strcmp(command, "go_back") == 0 &&
        p8p_runtime_request_back(active_runtime) == 0)
        return lua_yield(lua, 0);
    return 0;
}

static int api_stub(lua_State *lua) {
    (void)lua;
    return 0;
}

/* menuitem(index, [label, [callback]]) adds, replaces, relabels (no
 * callback) or (without a label) removes pause-menu entry 1-5.  Bits 8-15 of index filter which button
 * presses reach the callback. */
static int api_menuitem(lua_State *lua) {
    p8p_runtime_t *runtime = active_runtime;
    int index = arg_int(lua, 1, 0);
    int slot = (index & 0xff) - 1;
    if (slot < 0 || slot >= 5)
        return 0;
    if (runtime->menu_ref == LUA_NOREF) {
        lua_newtable(lua);
        runtime->menu_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(lua, LUA_REGISTRYINDEX, runtime->menu_ref);
    if (lua_isstring(lua, 2)) {
        /* The system menu font is ASCII: button glyphs become letters and
         * arrows, other P8SCII characters '?'. */
        size_t length;
        const unsigned char *label =
            (const unsigned char *)lua_tolstring(lua, 2, &length);
        size_t used = 0;
        for (size_t i = 0; i < length && used < 16; ++i) {
            unsigned char c = label[i];
            char out = (c >= 32 && c < 127) ? (char)c :
                       c == 0x8b ? '<' : c == 0x91 ? '>' : c == 0x94 ? '^' :
                       c == 0x83 ? 'v' : c == 0x8e ? 'O' : c == 0x97 ? 'X' : '?';
            runtime->menu_labels[slot][used++] = out;
        }
        runtime->menu_labels[slot][used] = '\0';
        runtime->menu_filters[slot] = (uint8_t)((index >> 8) & 0xff);
        if (lua_isfunction(lua, 3)) {
            lua_pushvalue(lua, 3);
        } else {
            /* A new label alone keeps the callback, as PICO-8 does: carts
             * relabel a toggle from inside its own callback. */
            lua_pop(lua, 1);
            return 0;
        }
    } else {
        runtime->menu_labels[slot][0] = '\0';
        lua_pushnil(lua);
    }
    lua_rawseti(lua, -2, slot + 1);
    lua_pop(lua, 1);
    return 0;
}

/* trace([message]) returns a stack trace in PICO-8; the Lua debug library
 * is hidden here, so only the message comes back. */
static int api_trace(lua_State *lua) {
    int index = lua_type(lua, 1) == LUA_TTHREAD ? 2 : 1;
    if (lua_type(lua, index) == LUA_TSTRING)
        lua_pushvalue(lua, index);
    else
        lua_pushliteral(lua, "");
    return 1;
}

/* serial(channel, address, length).  Only 0x808, PICO-8's 8-bit 5512.5 Hz
 * PCM output, is implemented; other channels consume nothing. */
static int api_serial(lua_State *lua) {
    p8p_runtime_t *runtime = active_runtime;
    int channel = arg_int(lua, 1, 0);
    int address = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0);
    int processed = 0;
    if (channel == 0x808 && address >= 0 && length > 0 &&
        address + length <= 0x10000) {
        if (range_touches_screen(address, length))
            screen_to_ram(runtime);
        processed = p8p_audio_pcm_push(runtime->audio, runtime->ram + address,
                                       length);
    }
    push_int(lua, processed);
    return 1;
}

/** @brief Yield the next all(t) value with PICO-8 deletion semantics.
 * Uses the current C closure's table, index and previous-value upvalues. If
 * cart code removed the previous value, visits the same index again; otherwise
 * advances using raw equality. Skips nil slots up to the raw table boundary.
 * Updates the iterator and its GC barrier before pushing one value, or nil
 * when exhausted. Computes the boundary only when an empty slot requires it.
 */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static int all_next_slow(lua_State *lua) {
    CClosure *closure = clCvalue(lua->ci->func);
    TValue *table_value = &closure->upvalue[0];
    TValue *index_value = &closure->upvalue[1];
    TValue *previous = &closure->upvalue[2];
    if (!ttistable(table_value)) {
        lua_pushnil(lua);
        return 1;
    }
    Table *table = hvalue(table_value);
    int index;
    lua_number2int(index, nvalue(index_value));
    const TValue *slot = luaH_getint(table, index);
    /* Items are usually tables, whose raw equality is identity. */
    if (ttistable(slot) && ttistable(previous) ?
            hvalue(slot) == hvalue(previous) :
            luaV_rawequalobj(slot, previous))
        slot = luaH_getint(table, ++index);
    if (ttisnil(slot)) {
        int length = luaH_getn(table);
        while (index <= length && ttisnil(slot))
            slot = luaH_getint(table, ++index);
        if (index > length) {
            setnvalue(index_value, cast_num(index));
            setnilvalue(previous);
            lua_pushnil(lua);
            return 1;
        }
    }
    setnvalue(index_value, cast_num(index));
    setobj(lua, previous, slot);
    luaC_barrier(lua, closure, slot);
    setobj2s(lua, lua->top, slot);
    ++lua->top;
    return 1;
}

/** @brief Yield dense actor-table entries, falling back for every other case.
 * Reads array storage afresh on each invocation because cart code can resize
 * or mutate it between yields. Table identity determines whether to advance.
 * Every guard falls back before changing upvalues or the stack, so the slow
 * iterator resumes from the original state. A hit applies the same GC barrier
 * and pushes the same single value as all_next_slow.
 */
static int api_all_next(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    CClosure *closure = clCvalue(lua->ci->func);
    TValue *table_value = &closure->upvalue[0];
    if (!ttistable(table_value))
        return all_next_slow(lua);
    Table *table = hvalue(table_value);
    int index;
    lua_number2int(index, nvalue(&closure->upvalue[1]));
    unsigned int offset = static_cast<unsigned int>(index) - 1u;
    if (offset >= static_cast<unsigned int>(table->sizearray))
        return all_next_slow(lua);
    const TValue *slot = &table->array[offset];
    if (!ttistable(slot))
        return all_next_slow(lua);
    const TValue *previous = &closure->upvalue[2];
    if (ttistable(previous) && hvalue(slot) == hvalue(previous)) {
        if (++offset >= static_cast<unsigned int>(table->sizearray))
            return all_next_slow(lua);
        slot = &table->array[offset];
        if (!ttistable(slot))
            return all_next_slow(lua);
        ++index;
    }
    setnvalue(&closure->upvalue[1], cast_num(index));
    setobj(lua, &closure->upvalue[2], slot);
    luaC_barrier(lua, closure, slot);
    setobj2s(lua, lua->top, slot);
    ++lua->top;
    return 1;
}

/** @brief Create all(t)'s iterator with table, index 1 and nil previous value.
 * Captures the supplied value (or nil) in three GC-rooted closure upvalues and
 * returns the iterator as one Lua result; traversal validates the table later.
 */
static int api_all(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    if (arg_count(lua) >= 1)
        lua_pushvalue(lua, 1);
    else
        lua_pushnil(lua);
    lua_pushinteger(lua, 1);
    lua_pushnil(lua);
    lua_pushcclosure(lua, api_all_next, 3);
    return 1;
}

/* Keep this indirect callback helper compact and independent of caller IPA. */
#if defined(__riscv) && defined(__GNUC__)
__attribute__((noipa, optimize("align-loops=4")))
#endif
static int api_foreach(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    if (!lua_istable(lua, 1) || !lua_isfunction(lua, 2))
        return 0;
    lua_settop(lua, 2);

    int index = 1;
    while (index <= (int)lua_rawlen(lua, 1)) {
        lua_rawgeti(lua, 1, index);
        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 1);
            ++index;
            continue;
        }

        /* Keep the yielded value below the callback.  If the callback deletes
         * it, the next element shifts into this same index and must be visited
         * rather than skipped, matching PICO-8 foreach()/all() semantics. */
        lua_pushvalue(lua, 2);
        lua_pushvalue(lua, -2);
        lua_call(lua, 1, 0);

        lua_rawgeti(lua, 1, index);
        /* Actor lists usually contain tables; raw equality is identity here.
         * Read the stack only after the callback, which can grow or move it. */
        const TValue *current = lua->top - 1;
        const TValue *previous = lua->top - 2;
        int unchanged = ttistable(current) && ttistable(previous) ?
            hvalue(current) == hvalue(previous) : lua_rawequal(lua, -1, -2);
        lua_pop(lua, 2);
        if (unchanged)
            ++index;
    }
    return 0;
}

static int api_cooperative_stub(lua_State *lua) {
    int is_main_thread = lua_pushthread(lua);
    lua_pop(lua, 1);
    return is_main_thread ? 0 : lua_yield(lua, 0);
}

static const luaL_Reg runtime_api[] = {
    {"cls", api_cls}, {"pset", api_pset}, {"pget", api_pget},
    {"sget", api_sget}, {"sset", api_sset}, {"color", api_color},
    {"line", api_line}, {"rect", api_rect}, {"rectfill", api_rectfill},
    {"rrect", api_rrect}, {"rrectfill", api_rrectfill},
    {"circ", api_circ}, {"circfill", api_circfill}, {"oval", api_oval},
    {"ovalfill", api_ovalfill}, {"spr", api_spr}, {"sspr", api_sspr},
    {"tline", api_tline},
    {"mget", api_mget}, {"mset", api_mset}, {"map", api_map},
    {"mapdraw", api_map}, {"fget", api_fget}, {"fset", api_fset},
    {"camera", api_camera}, {"clip", api_clip}, {"pal", p8p_api_pal},
    {"palt", api_palt}, {"btn", api_btn}, {"btnp", api_btnp},
    {"peek", api_peek}, {"poke", api_poke}, {"peek2", api_peek2},
    {"poke2", api_poke2}, {"peek4", api_peek4}, {"poke4", api_poke4},
    {"memset", api_memset},
    {"memcpy", api_memcpy}, {"reload", api_reload}, {"rnd", api_rnd},
    {"cartdata", api_cartdata}, {"dget", api_dget}, {"dset", api_dset},
    {"srand", api_srand}, {"time", api_time}, {"t", api_time},
    {"flip", api_flip},
    {"cursor", api_cursor}, {"print", api_print}, {"sfx", api_sfx},
    {"music", api_music}, {"fillp", api_fillp}, 
    {"printh", api_stub}, {"extcmd", api_stub}, {"serial", api_serial},
    {"mkdir", api_cooperative_stub}, {"cd", api_stub},
    {"stat", api_stat}, {"run", api_run}, {"all", api_all},
    {"foreach", api_foreach}, {NULL, NULL}
};

/* Added after 0.0.33's first save-state format: numbered last by
 * eris.__p8p_init so existing permanent-object IDs do not move. Keep in
 * sync with the list in that function.  menuitem used to share api_stub,
 * which was numbered under "cd", so moving it here keeps the IDs too. */
static const luaL_Reg late_runtime_api[] = {
    {"cstore", api_stub}, {"trace", api_trace}, {"menuitem", api_menuitem},
    /* Not PICO-8 API, but Yarn calls exit() while drawing its score panel
     * once all levels are unlocked; a no-op lets it finish the frame. */
    {"exit", api_stub},
    {NULL, NULL}
};

static void register_pico8_button_constants(lua_State *lua) {
    static const uint8_t pico8_names[6] = {
        0x8b, /* left */
        0x91, /* right */
        0x94, /* up */
        0x83, /* down */
        0x8e, /* O */
        0x97  /* X */
    };
    static const char *utf8_names[6] = {
        "⬅️", "➡️", "⬆️", "⬇️", "🅾️", "❎"
    };
    char pico8_name[2] = {0, 0};
    for (int button = 0; button < 6; ++button) {
        pico8_name[0] = (char)pico8_names[button];
        push_int(lua, button);
        lua_setglobal(lua, pico8_name);
        push_int(lua, button);
        lua_setglobal(lua, utf8_names[button]);
    }
    /* Glyphs 128-153 double as fill patterns, e.g. fillp(\x81) is a
     * checkerboard with transparency.  Values as in PICO-8 (via Fake-08);
     * the button glyphs above keep their button numbers. */
    static const struct { uint8_t glyph; uint32_t bits; } fill_patterns[] = {
        {0x80, 0x00000000u}, {0x81, 0x5a5a8000u}, {0x82, 0x511f8000u},
        {0x84, 0x7d7d8000u}, {0x85, 0xb81d8000u}, {0x86, 0xf99f8000u},
        {0x87, 0x51bf8000u}, {0x88, 0xb5bf8000u}, {0x89, 0x999f8000u},
        {0x8a, 0xb11f8000u}, {0x8c, 0xa0e08000u}, {0x8d, 0x9b3f8000u},
        {0x8f, 0xb1bf8000u}, {0x90, 0xf5ff8000u}, {0x92, 0xb15f8000u},
        {0x93, 0x1b1f8000u}, {0x95, 0xf5bf8000u}, {0x96, 0x7adf8000u},
        {0x98, 0x0f0f8000u}, {0x99, 0x55558000u},
    };
    for (size_t i = 0; i < sizeof(fill_patterns) / sizeof(fill_patterns[0]); ++i) {
        pico8_name[0] = (char)fill_patterns[i].glyph;
        lua_pushnumber(lua, fix32::frombits((int32_t)fill_patterns[i].bits));
        lua_setglobal(lua, pico8_name);
    }
}

static const char bootstrap_lua[] =
    "function count(c,v) local n=0 for i=1,#c do if c[i]~=nil and (v==nil or c[i]==v) then n+=1 end end return n end\n"
    "function add(c,v,i) if c~=nil then i=i and mid(1,i\\1,#c+1) or #c+1 for j=#c,i,-1 do c[j+1]=c[j] end c[i]=v return v end end\n"
    "function del(c,v) if c~=nil then local n=#c for i=1,n do if c[i]==v then for j=i,n do c[j]=c[j+1] end return v end end end end\n"
    "function deli(c,i) if c~=nil then i=i and mid(1,i\\1,#c) or #c local v=c[i] for j=i,#c do c[j]=c[j+1] end return v end end\n"
    "sub=string.sub chr=chr ord=ord unpack=table.unpack\n"
    "cocreate=coroutine.create coresume=coroutine.resume "
    "costatus=coroutine.status yield=coroutine.yield\n"
    "rawset(debug.getregistry(),'__PICO8_SANDBOX',_G)\n"
    "eris.__p8p_perm={} eris.__p8p_unperm={} eris.__p8p_original={}\n"
    "function eris.__p8p_init()\n"
    " local late={'cstore','trace','menuitem','exit'} local skip={} for _,k in ipairs(late) do skip[k]=true end\n"
    " local keys={} for k in pairs(_G) do if not skip[k] then keys[#keys+1]=k end end table.sort(keys)\n"
    " local seen={} local n=0 local function permanent(v) local t=type(v)\n"
    "  if t~='table' and t~='function' and t~='userdata' and t~='thread' then return end\n"
    "  if seen[v] then return end seen[v]=true n+=1 eris.__p8p_perm[v]=n eris.__p8p_unperm[n]=v\n"
    "  if t=='table' and v~=_G and v~=eris then for k,x in pairs(v) do permanent(k) permanent(x) end permanent(getmetatable(v)) end\n"
    " end\n"
    " for i,k in ipairs(keys) do local v=_G[k] permanent(v) eris.__p8p_original[k]=v end\n"
    " for i,k in ipairs(late) do local v=_G[k] permanent(v) eris.__p8p_original[k]=v end\n"
    "end\n"
    "function eris.__p8p_save()\n"
    " local changed={} for k,v in pairs(_G) do if eris.__p8p_original[k]~=v then changed[k]=v end end\n"
    " return eris.persist(eris.__p8p_perm,changed)\n"
    "end\n"
    "function eris.__p8p_load(blob)\n"
    " local changed=eris.unpersist(eris.__p8p_unperm,blob)\n"
    " local stale={} for k,v in pairs(_G) do if eris.__p8p_original[k]~=v then stale[#stale+1]=k end end\n"
    " for k in all(stale) do _G[k]=nil end for k,v in pairs(changed) do _G[k]=v end\n"
    "end\n";

static void set_error(p8p_runtime_t *runtime, const char *prefix) {
    const char *message = runtime->lua ? lua_tostring(runtime->lua, -1) : NULL;
    snprintf(runtime->error, sizeof(runtime->error), "%s%s%s", prefix,
             message ? ": " : "", message ? message : "");
    if (runtime->lua && lua_gettop(runtime->lua) > 0)
        lua_pop(runtime->lua, 1);
}

static void register_api(lua_State *lua) {
    for (const luaL_Reg *entry = runtime_api; entry->name; ++entry)
        lua_register(lua, entry->name, entry->func);
    for (const luaL_Reg *entry = late_runtime_api; entry->name; ++entry)
        lua_register(lua, entry->name, entry->func);
}

/* Stack: eris, perm, unperm, originals, replacement. Leave replacement. */
static void append_handoff_permanent(lua_State *lua, int *next) {
    lua_pushvalue(lua, -1);
    lua_pushinteger(lua, ++*next);
    lua_rawset(lua, -6);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, -4, *next);
}

static int register_handoff_api(lua_State *lua) {
    lua_getglobal(lua, "eris");
    lua_getfield(lua, -1, "__p8p_perm");
    lua_getfield(lua, -2, "__p8p_unperm");
    lua_getfield(lua, -3, "__p8p_original");
    int next = (int)lua_rawlen(lua, -2);
    /* Provide the native compiler alias without changing the old sorted
     * globals. Its function is already numbered as the original load. */
    lua_getglobal(lua, "loadstring");
    int have_loadstring = !lua_isnil(lua, -1);
    lua_pop(lua, 1);
    if (!have_loadstring) {
        lua_getglobal(lua, "load");
        lua_pushvalue(lua, -1);
        lua_setfield(lua, -3, "loadstring");
        lua_setglobal(lua, "loadstring");
    }
    const char *names[] = {"load", "extcmd", "coresume"};
    for (int i = 0; i < 3; ++i) {
        if (i == 1) {
            lua_pushcfunction(lua, api_extcmd);
        } else {
            lua_getglobal(lua, names[i]);
            lua_pushcclosure(lua, i == 0 ? api_load_cart : api_resume_cart, 1);
        }
        append_handoff_permanent(lua, &next);
        lua_pushvalue(lua, -1);
        lua_setfield(lua, -3, names[i]);
        lua_setglobal(lua, names[i]);
    }
    lua_getglobal(lua, "coroutine");
    lua_getglobal(lua, "coresume");
    lua_setfield(lua, -2, "resume");
    lua_pop(lua, 1);
    /* wrap() normally resumes a child through a private C function. Route
     * it through the same boundary; returned Lua closures remain saveable. */
    static const char wrap_lua[] =
        "local create,resume,pack,unpack,raise=... "
        "return function(f) local co=create(f) return function(...) "
        "local r=pack(resume(co,...)) if not r[1] then raise(r[2],2) end "
        "return unpack(r,2,r.n) end end";
    if (luaL_loadstring(lua, wrap_lua) != LUA_OK)
        return -1;
    lua_getglobal(lua, "cocreate");
    lua_getglobal(lua, "coresume");
    lua_getglobal(lua, "table");
    lua_getfield(lua, -1, "pack");
    lua_getfield(lua, -2, "unpack");
    lua_remove(lua, -3);
    lua_getglobal(lua, "error");
    if (lua_pcall(lua, 5, 1, 0) != LUA_OK)
        return -1;
    append_handoff_permanent(lua, &next);
    lua_getglobal(lua, "coroutine");
    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, "wrap");
    lua_pop(lua, 2);
    const char *protected_names[] = {"pcall", "xpcall"};
    for (const char *name : protected_names) {
        lua_getglobal(lua, name);
        lua_pushboolean(lua, strcmp(name, "xpcall") == 0);
        lua_pushcclosure(lua, api_protected_call, 2);
        append_handoff_permanent(lua, &next);
        lua_pushvalue(lua, -1);
        lua_setfield(lua, -3, name);
        lua_setglobal(lua, name);
    }
    lua_pushcfunction(lua, api_protected_call_finish);
    append_handoff_permanent(lua, &next);
    lua_pop(lua, 1);
    lua_pushcfunction(lua, api_handoff_error_handler);
    append_handoff_permanent(lua, &next);
    lua_pop(lua, 1);
    lua_pop(lua, 4);
    return 0;
}

static void set_thread_error(p8p_runtime_t *runtime, const char *prefix,
                             lua_State *thread) {
    const char *message = thread ? lua_tostring(thread, -1) : NULL;
    snprintf(runtime->error, sizeof(runtime->error), "%s%s%s", prefix,
             message ? ": " : "", message ? message : "");
    if (thread && lua_gettop(thread) > 0)
        lua_pop(thread, 1);
}

static void release_cart_thread(p8p_runtime_t *runtime) {
    if (runtime->cart_thread_ref != LUA_NOREF)
        luaL_unref(runtime->lua, LUA_REGISTRYINDEX, runtime->cart_thread_ref);
    runtime->cart_thread_ref = LUA_NOREF;
    runtime->cart_thread = NULL;
    runtime->cart_thread_active = 0;
}

extern "C" void p8p_runtime_set_cart_features(p8p_runtime_t *runtime,
                                              uint32_t features) {
    if (runtime)
        runtime->cart_features = features;
}

extern "C" const p8p_runtime_cart_request_t *p8p_runtime_cart_request(
    p8p_runtime_t *runtime) {
    return runtime && runtime->cart_request_pending ? &runtime->cart_request : NULL;
}

extern "C" void p8p_runtime_clear_cart_request(p8p_runtime_t *runtime) {
    if (!runtime)
        return;
    runtime->cart_request_pending = 0;
    memset(&runtime->cart_request, 0, sizeof(runtime->cart_request));
    /* The retired cart cannot continue if the host fails to load its target. */
    if (runtime->cart_halted && runtime->lua)
        release_cart_thread(runtime);
}

extern "C" const char *p8p_runtime_breadcrumb(p8p_runtime_t *runtime) {
    return runtime ? runtime->breadcrumb : "";
}

extern "C" const char *p8p_runtime_parameters(p8p_runtime_t *runtime) {
    return runtime ? runtime->params : "";
}

extern "C" int p8p_runtime_request_back(p8p_runtime_t *runtime) {
    if (!runtime || !runtime->breadcrumb[0] || runtime->cart_request_pending)
        return -1;
    memset(&runtime->cart_request, 0, sizeof(runtime->cart_request));
    runtime->cart_request.kind = P8P_RUNTIME_CART_REQUEST_BACK;
    runtime->cart_request_pending = 1;
    runtime->cart_halted = 1;
    return 0;
}

static int start_init_thread(p8p_runtime_t *runtime) {
    lua_getglobal(runtime->lua, "_init");
    if (!lua_isfunction(runtime->lua, -1)) {
        lua_pop(runtime->lua, 1);
        return 0;
    }
    runtime->cart_thread = lua_newthread(runtime->lua);
    runtime->cart_thread_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_xmove(runtime->lua, runtime->cart_thread, 1);
    runtime->cart_thread_kind = 1;
    runtime->cart_instruction_slices = 0;
    install_service_hook(runtime);
    int status = lua_resume(runtime->cart_thread, runtime->lua, 0);
    if (runtime->cart_request_pending) {
        release_cart_thread(runtime);
        return 0;
    }
    if (status == LUA_YIELD) {
        runtime->cart_thread_active = 1;
        return 0;
    }
    if (status != LUA_OK) {
        set_thread_error(runtime, "_init", runtime->cart_thread);
        return -1;
    }
    release_cart_thread(runtime);
    return 0;
}

static int finish_cart_load(p8p_runtime_t *runtime) {
    lua_getglobal(runtime->lua, "_update60");
    runtime->target_fps = lua_isfunction(runtime->lua, -1) ? 60 : 30;
    lua_pop(runtime->lua, 1);
    return start_init_thread(runtime);
}

/* Profile events raised by the Lua frame dispatcher (0-3, see below). */
static int frame_profile_event(lua_State *lua) {
    static const p8p_runtime_profile_event_t events[4] = {
        P8P_PROFILE_UPDATE_BEGIN, P8P_PROFILE_UPDATE_END,
        P8P_PROFILE_DRAW_BEGIN, P8P_PROFILE_DRAW_END,
    };
    p8p_runtime_t *runtime = active_runtime;
    int event = (int)lua_tointeger(lua, 1);
    if (runtime && runtime->profile_hook && event >= 0 && event < 4)
        runtime->profile_hook(runtime->profile_userdata, events[event]);
    return 0;
}

/*
 * _update/_draw run in a coroutine so flip() inside them ends the frame as on
 * PICO-8: the next step resumes after flip() with fresh input.  Carts use this
 * for in-frame loops such as "while btn(5) do ... flip() end".  The
 * dispatcher lives in the registry, not in _G, so the Eris permanent-object
 * numbering (and existing save states) is unchanged.
 */
static const char frame_dispatcher_lua[] =
    "local prof=...\n"
    "return function(update, draw)\n"
    " local u=_ENV[update]\n"
    " if u then prof(0) u() prof(1) end\n"
    " if draw then local d=_draw if d then prof(2) d() prof(3) end end\n"
    "end\n";

static int start_frame_thread(p8p_runtime_t *runtime) {
    runtime->cart_thread = lua_newthread(runtime->lua);
    runtime->cart_thread_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    runtime->cart_thread_kind = 2;
    lua_rawgeti(runtime->cart_thread, LUA_REGISTRYINDEX, runtime->frame_ref);
    lua_pushstring(runtime->cart_thread,
                   runtime->target_fps == 60 ? "_update60" : "_update");
    lua_pushboolean(runtime->cart_thread, runtime->draw_frame);
    install_service_hook(runtime);
    int status = lua_resume(runtime->cart_thread, runtime->lua, 2);
    if (runtime->cart_request_pending) {
        release_cart_thread(runtime);
        return 0;
    }
    if (status == LUA_YIELD) {
        runtime->cart_thread_active = 1;
        return 0;
    }
    if (status != LUA_OK) {
        set_thread_error(runtime, "frame", runtime->cart_thread);
        release_cart_thread(runtime);
        return -2;
    }
    release_cart_thread(runtime);
    return 0;
}

extern "C" p8p_runtime_t *p8p_runtime_create(void) {
    p8p_runtime_t *runtime = (p8p_runtime_t *)calloc(1, sizeof(*runtime));
    if (!runtime)
        return NULL;
    runtime->target_fps = 30;
    runtime->draw_frame = 1;
    runtime->draw_color = 6;
    runtime->draw_target = 0x6000;
    runtime->clip_x1 = runtime->clip_y1 = 128;
    for (int i = 0; i < 16; ++i) {
        runtime->draw_palette[i] = (uint8_t)i;
        runtime->screen_palette[i] = (uint8_t)i;
    }
    runtime->transparent[0] = 1;
    runtime->palettes_default = 1;
    runtime->transparency_default = 1;
    runtime->audio = p8p_audio_create(runtime->ram);
    if (!runtime->audio) {
        free(runtime);
        return NULL;
    }
    return runtime;
}

extern "C" void p8p_runtime_destroy(p8p_runtime_t *runtime) {
    if (!runtime)
        return;
    cartdata_flush(runtime);
    if (runtime->lua)
        lua_close(runtime->lua);
    if (active_runtime == runtime)
        active_runtime = NULL;
    free(runtime->cart_lua);
    p8p_audio_destroy(runtime->audio);
    free(runtime);
}

extern "C" int p8p_runtime_load(p8p_runtime_t *runtime, const p8p_cart_t *cart) {
    return p8p_runtime_load_context(runtime, cart, "", "");
}

struct runtime_load_context_guard {
    p8p_runtime_t *runtime;
    char params[256];
    char breadcrumb[64];
    bool committed = false;
    explicit runtime_load_context_guard(p8p_runtime_t *value) : runtime(value) {
        memcpy(params, value->params, sizeof params);
        memcpy(breadcrumb, value->breadcrumb, sizeof breadcrumb);
    }
    ~runtime_load_context_guard() {
        if (committed) return;
        memcpy(runtime->params, params, sizeof params);
        memcpy(runtime->breadcrumb, breadcrumb, sizeof breadcrumb);
        runtime->cart_request_pending = 0;
        runtime->cart_halted = 1;
    }
};

extern "C" int p8p_runtime_load_context(p8p_runtime_t *runtime,
    const p8p_cart_t *cart, const char *params, const char *breadcrumb) {
    if (!runtime || !cart || !cart->lua)
        return -1;
    if (!params)
        params = "";
    if (!breadcrumb)
        breadcrumb = "";
    if (strlen(params) >= sizeof(runtime->params) ||
        strlen(breadcrumb) >= sizeof(runtime->breadcrumb))
        return -1;
    char next_params[256], next_breadcrumb[64];
    strcpy(next_params, params);
    strcpy(next_breadcrumb, breadcrumb);
    p8p_cart_hash_t cart_hash;
    p8p_cart_content_hash(cart, &cart_hash);
    const p8p_cart_incompatibility_t *incompatible =
        p8p_cart_find_incompatibility(&cart_hash);
    if (incompatible && (!incompatible->required_features ||
        (runtime->cart_features & incompatible->required_features) !=
            incompatible->required_features)) {
        snprintf(runtime->error, sizeof(runtime->error),
                 "%s incompatible.\n%s\n%s", incompatible->title,
                 incompatible->reason, incompatible->workaround);
        return P8P_RUNTIME_INCOMPATIBLE_CART;
    }
    char *cart_lua = (char *)malloc(cart->lua_size + 1);
    if (!cart_lua)
        return -2;
    memcpy(cart_lua, cart->lua, cart->lua_size);
    cart_lua[cart->lua_size] = '\0';
    runtime_load_context_guard context_guard(runtime);
    cartdata_flush(runtime);
    runtime->cartdata_active = 0;
    runtime->cartdata_dirty = 0;
    runtime->cartdata_id[0] = '\0';
    if (runtime->lua)
        lua_close(runtime->lua);
    free(runtime->cart_lua);
    runtime->cart_lua = cart_lua;
    strcpy(runtime->params, next_params);
    strcpy(runtime->breadcrumb, next_breadcrumb);
    runtime->cart_request_pending = 0;
    runtime->cart_halted = 0;
    runtime->cart_lua_size = cart->lua_size;
    runtime->cart_thread = NULL;
    runtime->cart_thread_ref = LUA_NOREF;
    runtime->cart_thread_active = 0;
    memset(runtime->menu_labels, 0, sizeof(runtime->menu_labels));
    memset(runtime->menu_filters, 0, sizeof(runtime->menu_filters));
    runtime->menu_ref = LUA_NOREF;
    memset(runtime->ram, 0, sizeof(runtime->ram));
    memcpy(runtime->cart_rom, cart->rom, sizeof(runtime->cart_rom));
    memcpy(runtime->ram, cart->rom, sizeof(runtime->cart_rom));
    runtime->ram[0x5f54] = 0x00;
    runtime->ram[0x5f55] = 0x60;
    runtime->draw_target = 0x6000;
    runtime->ram[0x5f56] = 0x20;
    runtime->ram[0x5f57] = 128;
    runtime->ram[0x5f5c] = 15;
    runtime->ram[0x5f5d] = 4;
    runtime->ram[0x5f5e] = 0xff;
    for (int i = 0; i < 16; ++i) {
        runtime->draw_palette[i] = (uint8_t)i;
        runtime->screen_palette[i] = (uint8_t)i;
    }
    memset(runtime->transparent, 0, sizeof(runtime->transparent));
    runtime->transparent[0] = 1;
    runtime->palettes_default = 1;
    runtime->transparency_default = 1;
    runtime->camera_x = runtime->camera_y = 0;
    runtime->clip_x0 = runtime->clip_y0 = 0;
    runtime->clip_x1 = runtime->clip_y1 = 128;
    runtime->draw_color = 6;
    runtime->cursor_x = runtime->cursor_y = 0;
    runtime->fill_pattern = 0;
    runtime->fill_pattern_transparent = 0;
    draw_state_to_ram(runtime);
    ram_to_screen(runtime);
    p8p_audio_reset(runtime->audio, runtime->ram);
    runtime->error[0] = '\0';
    runtime->frame_count = 0;
    runtime->buttons = runtime->previous_buttons = runtime->frame_buttons = 0;
    memset(runtime->held_frames, 0, sizeof(runtime->held_frames));
    runtime->lua = lua_newstate(lua_pool_alloc, NULL);
    if (runtime->lua) {
        lua_atpanic(runtime->lua, lua_pool_panic);
        lua_gc(runtime->lua, LUA_GCSETPAUSE, P8P_LUA_GC_PAUSE);
    }
    if (!runtime->lua) {
        snprintf(runtime->error, sizeof(runtime->error), "cannot create z8lua state");
        return -2;
    }
    active_runtime = runtime;
    luaL_openlibs(runtime->lua);
    lua_setpico8memory(runtime->lua, runtime->ram);
    register_api(runtime->lua);
    register_pico8_button_constants(runtime->lua);
    api_srand(runtime->lua);

    if (luaL_dostring(runtime->lua, bootstrap_lua) != LUA_OK) {
        set_error(runtime, "bootstrap");
        return -3;
    }
    lua_getglobal(runtime->lua, "eris");
    lua_getfield(runtime->lua, -1, "__p8p_init");
    if (lua_pcall(runtime->lua, 0, 0, 0) != LUA_OK) {
        set_error(runtime, "state init");
        lua_pop(runtime->lua, 1);
        return -3;
    }
    lua_getfield(runtime->lua, -1, "__p8p_save");
    runtime->persist_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_getfield(runtime->lua, -1, "__p8p_load");
    runtime->restore_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_pop(runtime->lua, 1);
    if (register_handoff_api(runtime->lua) != 0) {
        set_error(runtime, "handoff API");
        return -3;
    }
    /* PICO-8 has no debug library, and carts such as Tetyis use a global
     * named debug as their own flag.  Hide it only after Eris has numbered
     * the built-ins so state files keep the same permanent-object IDs. */
    lua_pushnil(runtime->lua);
    lua_setglobal(runtime->lua, "debug");
    if (luaL_loadstring(runtime->lua, frame_dispatcher_lua) != LUA_OK) {
        set_error(runtime, "frame dispatcher");
        return -3;
    }
    lua_pushcfunction(runtime->lua, frame_profile_event);
    if (lua_pcall(runtime->lua, 1, 1, 0) != LUA_OK) {
        set_error(runtime, "frame dispatcher");
        return -3;
    }
    runtime->frame_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    runtime->cart_thread = lua_newthread(runtime->lua);
    runtime->cart_thread_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    runtime->cart_thread_kind = 0;
    runtime->restart_requested = 0;
    /* Legacy compressed carts include source terminators in their decoded
     * byte count. Keep identity/ROM bytes intact and omit only trailing NULs
     * at the compiler boundary. */
    size_t compile_size = runtime->cart_lua_size;
    while (compile_size && runtime->cart_lua[compile_size - 1] == '\0')
        --compile_size;
    if (!runtime->cart_thread ||
        luaL_loadbufferx(runtime->cart_thread, runtime->cart_lua,
                         compile_size, "cart.p8",
                         "t") != LUA_OK) {
        set_thread_error(runtime, "cart", runtime->cart_thread);
        return -4;
    }
    runtime->cart_instruction_slices = 0;
    install_service_hook(runtime);
    int cart_status = lua_resume(runtime->cart_thread, runtime->lua, 0);
    if (runtime->cart_request_pending) {
        release_cart_thread(runtime);
        context_guard.committed = true;
        return 0;
    }
    if (cart_status == LUA_YIELD) {
        runtime->cart_thread_active = 1;
        runtime->target_fps = 30;
    } else if (cart_status == LUA_OK) {
        release_cart_thread(runtime);
        if (finish_cart_load(runtime) != 0)
            return -5;
    } else {
        set_thread_error(runtime, "cart", runtime->cart_thread);
        return -4;
    }
    install_service_hook(runtime);
    context_guard.committed = true;
    return 0;
}

extern "C" void p8p_runtime_set_service_hook(
    p8p_runtime_t *runtime, p8p_runtime_service_fn callback, void *userdata) {
    if (!runtime)
        return;
    runtime->service_hook = callback;
    runtime->service_userdata = userdata;
    install_service_hook(runtime);
}

extern "C" void p8p_runtime_set_profile_hook(
    p8p_runtime_t *runtime, p8p_runtime_profile_fn callback, void *userdata) {
    if (!runtime)
        return;
    runtime->profile_hook = callback;
    runtime->profile_userdata = userdata;
}

extern "C" void p8p_runtime_get_api_profile(
    const p8p_runtime_t *runtime, p8p_runtime_api_profile_t *profile) {
    if (!profile)
        return;
    if (!runtime) {
        memset(profile, 0, sizeof(*profile));
        return;
    }
    memcpy(profile->calls, runtime->api_profile_calls,
           sizeof(profile->calls));
}

extern "C" void p8p_runtime_set_cartdata_hooks(
    p8p_runtime_t *runtime, p8p_runtime_cartdata_load_fn load,
    p8p_runtime_cartdata_save_fn save, void *userdata) {
    if (!runtime)
        return;
    cartdata_flush(runtime);
    runtime->cartdata_load = load;
    runtime->cartdata_save = save;
    runtime->cartdata_userdata = userdata;
}

extern "C" void p8p_runtime_flush_cartdata(p8p_runtime_t *runtime) {
    cartdata_flush(runtime);
}

extern "C" int p8p_runtime_step(p8p_runtime_t *runtime, uint8_t buttons) {
    return p8p_runtime_step_with_draw(runtime, buttons, 1);
}

static int restart_current_cart(p8p_runtime_t *runtime) {
    p8p_cart_t *cart = (p8p_cart_t *)calloc(1, sizeof(*cart));
    if (!cart)
        return -2;
    memcpy(cart->rom, runtime->cart_rom, sizeof(cart->rom));
    cart->lua = runtime->cart_lua;
    cart->lua_size = runtime->cart_lua_size;
    int result = p8p_runtime_load_context(runtime, cart, runtime->params, runtime->breadcrumb);
    free(cart);
    return result;
}

extern "C" void p8p_runtime_set_live_buttons(p8p_runtime_t *runtime,
                                               uint8_t buttons) {
    if (!runtime)
        return;
    /* PICO-8 samples input once per frame.  Mid-frame updates exist so
     * busy-wait loops see new presses through btn(); they never drop a button
     * the frame started with (a short tap would vanish before the cart read
     * it) and leave btnp() timing to the next frame boundary (a press seen
     * here after the cart's btnp() check would otherwise never register). */
    runtime->buttons = (uint8_t)(runtime->frame_buttons | (buttons & 0x7f));
}

extern "C" int p8p_runtime_step_with_draw(p8p_runtime_t *runtime,
                                            uint8_t buttons,
                                            int draw_frame) {
    if (!runtime || !runtime->lua)
        return -1;
    if (runtime->cart_halted)
        return 0;
    if (runtime->restart_requested)
        return restart_current_cart(runtime);
    active_runtime = runtime;
    if (runtime->profile_hook)
        memset(runtime->api_profile_calls, 0,
               sizeof(runtime->api_profile_calls));
    runtime->draw_frame = draw_frame != 0;
    runtime->previous_buttons = runtime->frame_buttons;
    runtime->buttons = buttons & 0x7f;
    runtime->frame_buttons = runtime->buttons;
    for (int i = 0; i < 7; ++i) {
        if (runtime->buttons & (1u << i)) {
            if (runtime->held_frames[i] != 0xffff)
                ++runtime->held_frames[i];
        } else {
            runtime->held_frames[i] = 0;
        }
    }
    ++runtime->frame_count;
    if (runtime->cartdata_dirty &&
        runtime->frame_count % (uint32_t)(runtime->target_fps * 5) == 0)
        cartdata_flush(runtime);
    if (runtime->cart_thread_active) {
        int thread_kind = runtime->cart_thread_kind;
        runtime->cart_instruction_slices = 0;
        int status = lua_resume(runtime->cart_thread, runtime->lua, 0);
        if (runtime->cart_request_pending) {
            release_cart_thread(runtime);
            return 0;
        }
        if (status == LUA_YIELD)
            return 0;
        if (status != LUA_OK) {
            set_thread_error(runtime, "frame", runtime->cart_thread);
            return -2;
        }
        release_cart_thread(runtime);
        if (thread_kind == 0 && finish_cart_load(runtime) != 0)
            return -2;
        install_service_hook(runtime);
        return 0;
    }
    if (start_frame_thread(runtime) != 0)
        return -2;

    if (runtime->restart_requested)
        return restart_current_cart(runtime);

    return 0;
}

static void capture_fixed_state(const p8p_runtime_t *runtime,
                                p8p_runtime_fixed_state *state) {
    memcpy(state->ram, runtime->ram, sizeof(state->ram));
    memcpy(state->framebuffer, runtime->framebuffer, sizeof(state->framebuffer));
    state->screen_ram_dirty = runtime->screen_ram_dirty;
    memcpy(state->draw_palette, runtime->draw_palette, sizeof(state->draw_palette));
    memcpy(state->screen_palette, runtime->screen_palette, sizeof(state->screen_palette));
    memcpy(state->transparent, runtime->transparent, sizeof(state->transparent));
    state->buttons = runtime->buttons;
    state->previous_buttons = runtime->previous_buttons;
    memcpy(state->held_frames, runtime->held_frames, sizeof(state->held_frames));
    memcpy(state->rng, runtime->rng, sizeof(state->rng));
    state->camera_x = runtime->camera_x;
    state->camera_y = runtime->camera_y;
    state->clip_x0 = runtime->clip_x0;
    state->clip_y0 = runtime->clip_y0;
    state->clip_x1 = runtime->clip_x1;
    state->clip_y1 = runtime->clip_y1;
    state->draw_color = runtime->draw_color;
    state->cursor_x = runtime->cursor_x;
    state->cursor_y = runtime->cursor_y;
    state->target_fps = runtime->target_fps;
    state->frame_count = runtime->frame_count;
}

static void restore_fixed_state(p8p_runtime_t *runtime,
                                const p8p_runtime_fixed_state *state) {
    memcpy(runtime->ram, state->ram, sizeof(runtime->ram));
    memcpy(runtime->framebuffer, state->framebuffer, sizeof(runtime->framebuffer));
    runtime->screen_ram_dirty = state->screen_ram_dirty;
    /* The saved framebuffer mirrors whatever 0x5f55 selected at save time. */
    runtime->draw_target = draw_target_from_ram(runtime);
    memcpy(runtime->draw_palette, state->draw_palette, sizeof(runtime->draw_palette));
    memcpy(runtime->screen_palette, state->screen_palette, sizeof(runtime->screen_palette));
    memcpy(runtime->transparent, state->transparent, sizeof(runtime->transparent));
    update_palette_default_flags(runtime);
    runtime->buttons = state->buttons;
    runtime->previous_buttons = state->previous_buttons;
    runtime->frame_buttons = runtime->buttons;
    memcpy(runtime->held_frames, state->held_frames, sizeof(runtime->held_frames));
    runtime->held_frames[6] = 0;
    memcpy(runtime->rng, state->rng, sizeof(runtime->rng));
    runtime->camera_x = state->camera_x;
    runtime->camera_y = state->camera_y;
    runtime->clip_x0 = state->clip_x0;
    runtime->clip_y0 = state->clip_y0;
    runtime->clip_x1 = state->clip_x1;
    runtime->clip_y1 = state->clip_y1;
    runtime->draw_color = state->draw_color;
    runtime->cursor_x = state->cursor_x;
    runtime->cursor_y = state->cursor_y;
    runtime->target_fps = state->target_fps;
    runtime->frame_count = state->frame_count;
}

extern "C" int p8p_runtime_save_state(p8p_runtime_t *runtime, void **data,
                                        size_t *size) {
    size_t lua_size;
    size_t audio_size;
    size_t total_size;
    uint8_t *output;
    p8p_runtime_state_header header;
    p8p_runtime_fixed_state *fixed;
    const char *lua_blob;

    if (!runtime || !runtime->lua || !data || !size)
        return -1;
    *data = NULL;
    *size = 0;
    if (runtime->cart_halted) {
        snprintf(runtime->error, sizeof(runtime->error), "save state: cart handoff pending");
        return -1;
    }
    cartdata_flush(runtime);
    active_runtime = runtime;
    lua_sethook(runtime->lua, NULL, 0, 0);
    lua_rawgeti(runtime->lua, LUA_REGISTRYINDEX, runtime->persist_ref);
    int persist_status = lua_pcall(runtime->lua, 0, 1, 0);
    install_service_hook(runtime);
    if (persist_status != LUA_OK) {
        set_error(runtime, "save state");
        return -2;
    }
    lua_blob = lua_tolstring(runtime->lua, -1, &lua_size);
    if (!lua_blob) {
        lua_pop(runtime->lua, 1);
        snprintf(runtime->error, sizeof(runtime->error), "save state: no Lua data");
        return -3;
    }
    audio_size = p8p_audio_state_size();
    if (lua_size > UINT32_MAX || audio_size > UINT32_MAX ||
        lua_size > SIZE_MAX - sizeof(header) - sizeof(*fixed) - audio_size - sizeof(runtime->params)) {
        lua_pop(runtime->lua, 1);
        snprintf(runtime->error, sizeof(runtime->error), "save state: too large");
        return -4;
    }
    total_size = sizeof(header) + sizeof(*fixed) + audio_size + sizeof(runtime->params) + lua_size;
    output = (uint8_t *)malloc(total_size);
    if (!output) {
        lua_pop(runtime->lua, 1);
        snprintf(runtime->error, sizeof(runtime->error), "save state: out of memory");
        return -5;
    }
    memcpy(header.magic, runtime_state_magic, sizeof(header.magic));
    header.version = 2;
    header.fixed_size = (uint32_t)sizeof(*fixed);
    header.audio_size = (uint32_t)audio_size;
    header.lua_size = (uint32_t)lua_size;
    fixed = (p8p_runtime_fixed_state *)(output + sizeof(header));
    capture_fixed_state(runtime, fixed);
    memcpy(output, &header, sizeof(header));
    if (p8p_audio_save_state(runtime->audio,
                             output + sizeof(header) + sizeof(*fixed),
                             audio_size) != 0) {
        free(output);
        lua_pop(runtime->lua, 1);
        return -6;
    }
    memcpy(output + sizeof(header) + sizeof(*fixed) + audio_size,
           runtime->params, sizeof(runtime->params));
    memcpy(output + sizeof(header) + sizeof(*fixed) + audio_size + sizeof(runtime->params),
           lua_blob, lua_size);
    lua_pop(runtime->lua, 1);
    *data = output;
    *size = total_size;
    return 0;
}

/* V1 carts could save aliases of native resume/pcall. Upgrade just those
 * read-side aliases during unpersist; v2 wrapper upvalues still require the
 * original native functions, and the old source compiler stays untouched. */
static void map_legacy_handoff_aliases(lua_State *lua, int native_values) {
    lua_getglobal(lua, "eris");
    lua_getfield(lua, -1, "__p8p_perm");
    lua_getfield(lua, -2, "__p8p_unperm");
    lua_getfield(lua, -3, "__p8p_original");
    const char *names[] = {"coresume", "pcall", "xpcall"};
    for (const char *name : names) {
        lua_getfield(lua, -1, name);
        lua_getupvalue(lua, -1, 1);
        lua_pushvalue(lua, -1);
        lua_rawget(lua, -6);
        int id = (int)lua_tointeger(lua, -1);
        lua_pop(lua, 1);
        lua_pushvalue(lua, native_values ? -1 : -2);
        lua_rawseti(lua, -5, id);
        lua_pop(lua, 2);
    }
    lua_pop(lua, 4);
}

extern "C" int p8p_runtime_load_state(p8p_runtime_t *runtime,
                                        const void *data, size_t size) {
    p8p_runtime_state_header header;
    const uint8_t *input = (const uint8_t *)data;
    const p8p_runtime_fixed_state *fixed;
    const uint8_t *audio;
    const uint8_t *lua_blob;
    size_t expected;

    if (!runtime || !runtime->lua || !data || size < sizeof(header))
        return -1;
    memcpy(&header, input, sizeof(header));
    if (memcmp(header.magic, runtime_state_magic, sizeof(header.magic)) != 0 ||
        (header.version != 1 && header.version != 2) || header.fixed_size != sizeof(*fixed) ||
        header.audio_size != p8p_audio_state_size()) {
        snprintf(runtime->error, sizeof(runtime->error), "load state: incompatible data");
        return -2;
    }
    expected = sizeof(header) + (size_t)header.fixed_size +
               (size_t)header.audio_size +
               (header.version == 2 ? sizeof(runtime->params) : 0);
    if (header.lua_size > SIZE_MAX - expected ||
        expected + (size_t)header.lua_size != size) {
        snprintf(runtime->error, sizeof(runtime->error), "load state: truncated data");
        return -3;
    }
    fixed = (const p8p_runtime_fixed_state *)(input + sizeof(header));
    audio = input + sizeof(header) + header.fixed_size;
    const char *saved_params = (const char *)(audio + header.audio_size);
    if (header.version == 2 && !memchr(saved_params, 0, sizeof(runtime->params))) {
        snprintf(runtime->error, sizeof(runtime->error), "load state: invalid parameters");
        return -3;
    }
    lua_blob = audio + header.audio_size + (header.version == 2 ? sizeof(runtime->params) : 0);
    active_runtime = runtime;
    lua_sethook(runtime->lua, NULL, 0, 0);
    if (header.version == 1)
        map_legacy_handoff_aliases(runtime->lua, 0);
    lua_rawgeti(runtime->lua, LUA_REGISTRYINDEX, runtime->restore_ref);
    lua_pushlstring(runtime->lua, (const char *)lua_blob, header.lua_size);
    int restore_status = lua_pcall(runtime->lua, 1, 0, 0);
    if (header.version == 1)
        map_legacy_handoff_aliases(runtime->lua, 1);
    install_service_hook(runtime);
    if (restore_status != LUA_OK) {
        set_error(runtime, "load state");
        return -4;
    }
    if (header.version == 2)
        memcpy(runtime->params, saved_params, sizeof(runtime->params));
    else
        runtime->params[0] = 0;
    int retire_thread = runtime->cart_halted;
    runtime->cart_request_pending = 0;
    runtime->cart_halted = 0;
    runtime->restart_requested = 0;
    restore_fixed_state(runtime, fixed);
    if (p8p_audio_load_state(runtime->audio, runtime->ram, audio,
                             header.audio_size) != 0)
        return -5;
    /* A frame suspended in flip() belongs to the pre-load state; start the
     * next frame afresh instead of resuming it. */
    if (runtime->cart_thread_active &&
        (runtime->cart_thread_kind == 2 || retire_thread))
        release_cart_thread(runtime);
    return 0;
}

extern "C" const uint8_t *p8p_runtime_framebuffer(p8p_runtime_t *runtime) {
    if (!runtime)
        return NULL;
    if (runtime->draw_target == 0x6000)
        return runtime->framebuffer;
    /* Drawing is redirected; present the real screen memory. */
    for (int i = 0; i < 128 * 64; ++i) {
        uint8_t packed = runtime->ram[0x6000 + i];
        runtime->display[i * 2] = packed & 15;
        runtime->display[i * 2 + 1] = packed >> 4;
    }
    return runtime->display;
}

extern "C" const uint8_t *p8p_runtime_screen_palette(p8p_runtime_t *runtime) {
    return runtime ? runtime->screen_palette : NULL;
}

extern "C" void p8p_runtime_audio_render(p8p_runtime_t *runtime,
                                          int16_t *stereo, size_t frames) {
    p8p_audio_render(runtime ? runtime->audio : NULL, stereo, frames);
}

extern "C" void p8p_runtime_audio_skip(p8p_runtime_t *runtime, size_t frames) {
    p8p_audio_skip(runtime ? runtime->audio : NULL, frames);
}

extern "C" const char *p8p_runtime_menu_item(const p8p_runtime_t *runtime,
                                              int slot) {
    if (!runtime || slot < 1 || slot > 5 || !runtime->menu_labels[slot - 1][0])
        return NULL;
    return runtime->menu_labels[slot - 1];
}

extern "C" int p8p_runtime_menu_select(p8p_runtime_t *runtime, int slot,
                                        int buttons) {
    /* O/X select (PICO-8 passes 16|32|64 for them) and close the menu unless
     * the callback returns true; left/right only notify the callback. */
    int choose = (buttons & 0x70) != 0;
    int filter;
    int keep_open;
    if (!p8p_runtime_menu_item(runtime, slot) || runtime->menu_ref == LUA_NOREF)
        return choose ? 0 : 1;
    filter = runtime->menu_filters[slot - 1];
    if (filter && !(filter & buttons))
        return choose ? 0 : 1;
    active_runtime = runtime;
    lua_rawgeti(runtime->lua, LUA_REGISTRYINDEX, runtime->menu_ref);
    lua_rawgeti(runtime->lua, -1, slot);
    lua_remove(runtime->lua, -2);
    if (!lua_isfunction(runtime->lua, -1)) {
        lua_pop(runtime->lua, 1);
        return choose ? 0 : 1;
    }
    /* Called from the host loop, outside any C function, so use the checked
     * API push rather than push_int. */
    if (runtime->cart_halted) {
        lua_pop(runtime->lua, 1);
        return 0;
    }
    lua_State *thread = lua_newthread(runtime->lua);
    int reference = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_xmove(runtime->lua, thread, 1);
    lua_pushnumber(thread, fix32(buttons));
    int status = lua_resume(thread, runtime->lua, 1);
    /* Menu callbacks formerly ran on the main thread, where flip() and the
     * cooperative stubs returned immediately. Drain ordinary yields here while
     * still stopping at a cart handoff; bound explicit yield loops. */
    for (int resumes = 1; status == LUA_YIELD &&
         !runtime->cart_request_pending && resumes < 1024; ++resumes) {
        lua_settop(thread, 0);
        status = lua_resume(thread, runtime->lua, 0);
    }
    if (runtime->cart_request_pending) keep_open = 0;
    else if (status == LUA_YIELD) {
        snprintf(runtime->error, sizeof(runtime->error),
                 "menu item: callback yielded too many times");
        keep_open = -1;
    } else if (status != LUA_OK) {
        set_thread_error(runtime, "menu item", thread);
        keep_open = -1;
    } else keep_open = !choose ||
        (lua_gettop(thread) > 0 && lua_toboolean(thread, 1));
    luaL_unref(runtime->lua, LUA_REGISTRYINDEX, reference);
    return keep_open;
}

extern "C" int p8p_runtime_target_fps(const p8p_runtime_t *runtime) {
    return runtime ? runtime->target_fps : 0;
}

extern "C" const char *p8p_runtime_error(const p8p_runtime_t *runtime) {
    return runtime ? runtime->error : "no runtime";
}

#ifdef P8P_RUNTIME_DEBUG
extern "C" int p8p_runtime_debug_eval_int(p8p_runtime_t *runtime,
                                            const char *expression,
                                            int *value) {
    char source[256];
    if (!runtime || !runtime->lua || !expression || !value)
        return -1;
    snprintf(source, sizeof(source), "return %s", expression);
    active_runtime = runtime;
    if (luaL_dostring(runtime->lua, source) != LUA_OK) {
        if (lua_gettop(runtime->lua) > 0)
            lua_pop(runtime->lua, 1);
        return -2;
    }
    *value = (int)lua_tonumber(runtime->lua, -1);
    lua_pop(runtime->lua, 1);
    return 0;
}
#endif
