#ifndef P8P_RUNTIME_INTERNAL_H
#define P8P_RUNTIME_INTERNAL_H
#include "p8p/runtime.h"
#include "p8p/audio.h"
#include <lua.h>
#include <stddef.h>
#include <stdint.h>
struct p8p_runtime {
    lua_State *lua;
    p8p_audio_t *audio;
    char *cart_lua;
    size_t cart_lua_size;
    uint8_t ram[0x10000];
    uint8_t cart_rom[P8P_CART_ROM_SIZE];
    /* One byte per logical pixel is the fast drawing surface.  PICO-8's
     * packed 4-bpp screen RAM is synchronized only when a memory API touches
     * it, avoiding a read-modify-write for every rendered pixel.  Word
     * aligned so fill patterns can write four pixels at a time. */
    alignas(4) uint8_t framebuffer[128 * 128];
    uint8_t screen_ram_dirty;
    /* RAM address the framebuffer mirrors: the screen at 0x6000 unless the
     * cart redirects drawing with 0x5f55 (e.g. into the sprite sheet). */
    uint16_t draw_target;
    /* Unpacked screen RAM for presentation while drawing is redirected. */
    uint8_t display[128 * 128];
    /* The screen after the 0x5f2c transformation, when one is selected. */
    uint8_t transformed[128 * 128];
    uint8_t draw_palette[16];
    uint8_t screen_palette[16];
    uint8_t transparent[16];
    /* Derived fast-path state.  These flags are deliberately not serialized:
     * they are cheap to rebuild after loading a state or draw-state RAM. */
    uint8_t palettes_default;
    uint8_t transparency_default;
    uint8_t buttons;
    uint8_t previous_buttons;
    /* Buttons sampled at the start of the current frame; live updates
     * during the frame may only add to them. */
    uint8_t frame_buttons;
    uint16_t held_frames[7];
    uint32_t rng[2];
    int camera_x;
    int camera_y;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
    int draw_color;
    uint16_t fill_pattern;
    uint8_t fill_pattern_transparent;
    int cursor_x;
    int cursor_y;
    int target_fps;
    uint8_t draw_frame;
    uint32_t frame_count;
    int persist_ref;
    int restore_ref;
    int frame_ref;  /* Lua dispatcher that runs _update/_draw (registry) */
    int cart_thread_ref;
    lua_State *cart_thread;
    uint8_t cart_thread_active;
    uint8_t cart_thread_kind;
    uint8_t restart_requested;
    uint32_t cart_features;
    uint8_t cart_request_pending;
    uint8_t cart_halted;
    p8p_runtime_cart_request_t cart_request;
    char params[256];
    char breadcrumb[64];
    /* menuitem() entries 1-5: label ("" when empty), button filter from
     * bits 8-15 of the index, and a registry table of callbacks. */
    char menu_labels[5][17];
    uint8_t menu_filters[5];
    int menu_ref;
    p8p_runtime_service_fn service_hook;
    void *service_userdata;
    p8p_runtime_profile_fn profile_hook;
    void *profile_userdata;
    uint32_t api_profile_calls[P8P_API_CATEGORY_COUNT];
    uint16_t cart_instruction_slices;
    p8p_runtime_cartdata_load_fn cartdata_load;
    p8p_runtime_cartdata_save_fn cartdata_save;
    void *cartdata_userdata;
    char cartdata_id[65];
    uint8_t cartdata_active;
    uint8_t cartdata_dirty;
    char error[256];
};

extern "C" {
extern p8p_runtime_t *p8p_active_runtime;
int p8p_api_pal(lua_State *lua);
int p8p_api_pal_full(lua_State *lua);
int p8p_draw_palette_identity(const uint8_t *palette);
}
#endif
