#include "p8p/audio.h"
#include "p8p/cart.h"
#include "p8p/menu.h"
#include "p8p/platform.h"
#include "p8p/runtime.h"
#include "p8p/state_store.h"
#include "miniz.h"
#include "../src/runtime/runtime_internal.h"
#include <lauxlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Platform hooks used by the system menu. */
void p8p_platform_audio_set_volume(unsigned) {}
unsigned p8p_platform_set_scale(unsigned scale) { return scale; }
uint8_t p8p_platform_dim_color(uint8_t color) { return color; }

static int failures;
static int service_hook_calls;
static int inject_pause;
static int inject_live = -1;
static p8p_runtime_t *service_hook_runtime;
static int profile_event_calls[4];

static void count_service_hook(void *) {
    ++service_hook_calls;
    if (inject_pause && service_hook_runtime)
        p8p_runtime_set_live_buttons(service_hook_runtime, 1u << 6);
    if (inject_live >= 0 && service_hook_runtime)
        p8p_runtime_set_live_buttons(service_hook_runtime, (uint8_t)inject_live);
}

static uint32_t last_audio_hash;

/* Zero crossings of the left channel over the next `frames` samples; also
 * records a hash of the samples in last_audio_hash. */
static int audio_crossings(p8p_runtime_t *runtime, int frames) {
    static int16_t buffer[9600 * 2];
    int crossings = 0;
    p8p_runtime_audio_render(runtime, buffer, (size_t)frames);
    last_audio_hash = 2166136261u;
    for (int i = 0; i < frames; ++i) {
        last_audio_hash = (last_audio_hash ^ (uint16_t)buffer[i * 2]) * 16777619u;
        if (i && (buffer[(i - 1) * 2] < 0) != (buffer[i * 2] < 0))
            ++crossings;
    }
    return crossings;
}

static int audio_crossings_for(p8p_runtime_t *runtime, const char *init) {
    /* SFX 0: square C-2 instrument, looping.  SFX 1: custom instrument 0 at
     * C-3.  SFX 2: plain square C-3.  SFX 3: SFX 0 an octave up, used as
     * instrument by SFX 4 at C-3.  SFX 5: SFX 2 with detune. */
    static const char header[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n";
    /* Each entry: 4-byte header (filters, speed, loop start/end) and the
     * first note (key, waveform + custom bit, volume, effect). */
    static const char *const sfx_lines[][2] = {
        {"00100001", "18350"}, {"00100000", "24870"}, {"00100000", "24370"},
        {"00100001", "24350"}, {"00100000", "24b70"}, {"08100000", "24370"},
    };
    char sfx[2048] = "__sfx__\n";
    for (size_t i = 0; i < sizeof(sfx_lines) / sizeof(sfx_lines[0]); ++i) {
        strcat(sfx, sfx_lines[i][0]);
        strcat(sfx, sfx_lines[i][1]);
        for (int note = 1; note < 32; ++note)
            strcat(sfx, "00000");
        strcat(sfx, "\n");
    }
    static char source[4096];
    p8p_cart_t cart = {};
    snprintf(source, sizeof(source), "%sfunction _init() %s end\n%s",
             header, init, sfx);
    if (p8p_cart_load_text_memory((const uint8_t *)source, strlen(source),
                                  &cart) != 0)
        return -1;
    int result = -1;
    if (p8p_runtime_load(runtime, &cart) == 0 &&
        p8p_runtime_step(runtime, 0) == 0)
        result = audio_crossings(runtime, 9600);
    p8p_cart_destroy(&cart);
    return result;
}

static void count_profile_event(void *, p8p_runtime_profile_event_t event) {
    if ((unsigned)event < 4)
        ++profile_event_calls[event];
}

static int test_cartdata_load(void *userdata, const char *id,
                              uint8_t *data, size_t size) {
    return p8p_cartdata_load_file((const char *)userdata, id, data, size);
}

static int test_cartdata_save(void *userdata, const char *id,
                              const uint8_t *data, size_t size) {
    return p8p_cartdata_save_file((const char *)userdata, id, data, size);
}

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static uint32_t frame_hash(p8p_runtime_t *runtime) {
    const uint8_t *framebuffer = p8p_runtime_framebuffer(runtime);
    uint32_t hash = 2166136261u;
    for (int pixel = 0; pixel < 128 * 128; ++pixel)
        hash = (hash ^ framebuffer[pixel]) * 16777619u;
    return hash;
}

/* Start presses, then deterministic pseudo-random play: walking, jumping
 * (O) and dashing (X). */
static uint8_t celeste_input(int frame) {
    if (frame < 60)
        return 0;
    if (frame % 60 < 4 && frame < 240)
        return (1u << 4) | (1u << 5);
    uint32_t r = (uint32_t)frame / 12u * 2654435761u;
    uint8_t buttons = (uint8_t)(1u << ((r >> 28) & 3u));
    if (((r >> 20) & 7u) == 0)
        buttons |= 1u << 4;
    if (((r >> 12) & 15u) == 0)
        buttons |= 1u << 5;
    return buttons;
}

/*
 * Celeste Classic (1 or 2) is the priority acceptance cart.  The cart is
 * supplied locally (assets/cards/, never committed).  It must load at
 * 30 FPS, play 600 frames of scripted input without errors while the
 * picture changes and sound plays, and a state saved mid-game must replay
 * the following frames exactly.
 */
static void celeste_acceptance(p8p_runtime_t *runtime, const char *path,
                               p8p_cart_t *cart) {
    enum { PLAY = 600, REPLAY = 90 };
    uint32_t expected[REPLAY];
    uint32_t previous = 0;
    int changes = 0;
    static int16_t audio[1600 * 2];
    int64_t audio_energy = 0;
    void *state = NULL;
    size_t state_size = 0;
    int loaded = p8p_runtime_load(runtime, cart);
    if (loaded != 0)
        fprintf(stderr, "%s load: %s\n", path, p8p_runtime_error(runtime));
    CHECK(loaded == 0);
    if (loaded != 0)
        return;
    CHECK(p8p_runtime_target_fps(runtime) == 30);
    for (int frame = 0; frame < PLAY; ++frame) {
        if (p8p_runtime_step(runtime, celeste_input(frame)) != 0) {
            fprintf(stderr, "%s frame %d: %s\n", path, frame,
                    p8p_runtime_error(runtime));
            CHECK(!"Celeste runtime error");
            return;
        }
        p8p_runtime_audio_render(runtime, audio, 1600);
        for (int i = 0; i < 1600; ++i)
            audio_energy += audio[i * 2] < 0 ? -audio[i * 2] : audio[i * 2];
        uint32_t hash = frame_hash(runtime);
        changes += hash != previous;
        previous = hash;
    }
    CHECK(changes > PLAY / 4);
    CHECK(audio_energy > 0);

    CHECK(p8p_runtime_save_state(runtime, &state, &state_size) == 0);
    for (int frame = 0; frame < REPLAY; ++frame) {
        CHECK(p8p_runtime_step(runtime, celeste_input(PLAY + frame)) == 0);
        expected[frame] = frame_hash(runtime);
    }
    CHECK(p8p_runtime_load_state(runtime, state, state_size) == 0);
    int mismatches = 0;
    for (int frame = 0; frame < REPLAY; ++frame) {
        CHECK(p8p_runtime_step(runtime, celeste_input(PLAY + frame)) == 0);
        mismatches += frame_hash(runtime) != expected[frame];
    }
    printf("%s: %d/%d frames changed, state %zu bytes, replay mismatches %d\n",
           path, changes, PLAY, state_size, mismatches);
    CHECK(mismatches == 0);
    free(state);
}

#define TEST_NOTES_4 "21070210702107021070"
#define TEST_NOTES_32 \
    TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4 \
    TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4

/* Private cartridges are optional; never copy them into the public fixtures. */
static void test_incompatible_carts(void) {
    static const struct { const char *env; const char *title; } cases[] = {
        {"P8P_TEST_PICO_ARCADE", "Pico Arcade"},
        {"P8P_TEST_TRAIL_OF_THE_SORCERER", "Trail of the Sorcerer"},
        {"P8P_TEST_PICO_BACKROOMS", "Pico Backrooms"},
        {"P8P_TEST_PERAL", "Peral"},
        {"P8P_TEST_PICO_NIGHT_FUNKIN", "Pico Night Funkin"},
    };
    for (const auto &item : cases) {
        const char *path = getenv(item.env);
        if (!path) {
            printf("Compatibility rejection test skipped (%s unset)\n", item.env);
            continue;
        }
        p8p_cart_t cart = {};
        CHECK(p8p_cart_load_file(path, &cart) == 0);
        if (!cart.lua)
            continue;
        p8p_cart_hash_t hash;
        p8p_cart_content_hash(&cart, &hash);
        printf("Compatibility fixture %s: ", item.title);
        for (uint8_t byte : hash.bytes)
            printf("%02x", byte);
        printf("\n");
        p8p_runtime_t *runtime = p8p_runtime_create();
        CHECK(runtime != NULL);
        if (runtime) {
            int result = p8p_runtime_load(runtime, &cart);
            CHECK(result == P8P_RUNTIME_INCOMPATIBLE_CART);
            if (result != 0) {
                CHECK(strstr(p8p_runtime_error(runtime), item.title) != NULL);
                CHECK(strstr(p8p_runtime_error(runtime), "incompatible") != NULL);
                CHECK(p8p_runtime_step(runtime, 0) != 0);
                CHECK(p8p_runtime_load(runtime, &cart) != 0);
                p8p_settings_t settings;
                p8p_settings_defaults(&settings);
                p8p_menu_t *menu = p8p_menu_create(&settings, &hash, "p8.png");
                CHECK(menu != NULL);
                if (menu) {
                    p8p_menu_open(menu, runtime, 0, 0);
                    CHECK(p8p_menu_framebuffer(menu) != NULL);
                    p8p_menu_destroy(menu);
                }
                /* A revised cart must not inherit this exact version's block. */
                cart.rom[P8P_CART_ROM_SIZE - 1] ^= 1;
                CHECK(p8p_runtime_load(runtime, &cart) == 0);
            }
            p8p_runtime_destroy(runtime);
        }
        p8p_cart_destroy(&cart);
    }
}

static void audio_chunk_boundaries(void) {
    enum { FRAMES = 4099 };
    static uint8_t ram[0x10000];
    static int16_t expected[FRAMES * 2], actual[FRAMES * 2 + 2];
    const size_t chunks[] = {0, 1, 2, 3, 7, 16, 31, 64, 257, 1600};
    uint8_t pcm[257];
    for (size_t i = 0; i < sizeof(pcm); ++i)
        pcm[i] = (uint8_t)(i * 37);
    for (unsigned waveform = 0; waveform < 8; ++waveform) {
        memset(ram, 0, sizeof(ram));
        for (unsigned channel = 0; channel < 4; ++channel) {
            uint8_t *sfx = ram + 0x3200 + channel * 68;
            unsigned wave = (waveform + channel) & 7;
            for (unsigned note = 0; note < 32; ++note) {
                sfx[note * 2] = (uint8_t)((24 + note % 12) | ((wave & 3) << 6));
                sfx[note * 2 + 1] = (uint8_t)((wave >> 2) | 14 | ((note % 6) << 4));
            }
            sfx[64] = (uint8_t)(waveform * 8);
            sfx[65] = 1;
            sfx[67] = 32;
        }
        for (size_t skipped = 0; skipped < 4; ++skipped) {
            p8p_audio_t *reference = p8p_audio_create(ram);
            p8p_audio_t *batched = p8p_audio_create(ram);
            CHECK(reference && batched);
            if (!reference || !batched) {
                p8p_audio_destroy(reference);
                p8p_audio_destroy(batched);
                continue;
            }
            for (int channel = 0; channel < 4; ++channel) {
                p8p_audio_sfx(reference, channel, channel, 0, 32);
                p8p_audio_sfx(batched, channel, channel, 0, 32);
            }
            CHECK(p8p_audio_pcm_push(reference, pcm, sizeof(pcm)) == sizeof(pcm));
            CHECK(p8p_audio_pcm_push(batched, pcm, sizeof(pcm)) == sizeof(pcm));
            p8p_audio_skip(reference, skipped);
            p8p_audio_skip(batched, skipped);
            for (size_t frame = 0; frame < FRAMES; ++frame)
                p8p_audio_render(reference, expected + frame * 2, 1);
            actual[0] = actual[FRAMES * 2 + 1] = 12345;
            size_t rendered = 0;
            for (size_t part = 0; rendered < FRAMES; ++part) {
                size_t count = chunks[part % (sizeof(chunks) / sizeof(chunks[0]))];
                if (count > FRAMES - rendered) count = FRAMES - rendered;
                p8p_audio_render(batched, actual + 1 + rendered * 2, count);
                p8p_audio_render(batched, NULL, 1);
                rendered += count;
            }
            CHECK(memcmp(expected, actual + 1, sizeof(expected)) == 0);
            CHECK(actual[0] == 12345 && actual[FRAMES * 2 + 1] == 12345);
            CHECK(p8p_audio_pcm_queued(reference) == p8p_audio_pcm_queued(batched));
            /* Compare the next output too: interpolation phase is unsaved. */
            p8p_audio_skip(reference, 3);
            p8p_audio_skip(batched, 3);
            p8p_audio_render(reference, expected, FRAMES);
            p8p_audio_render(batched, actual + 1, FRAMES);
            CHECK(memcmp(expected, actual + 1, sizeof(expected)) == 0);
            p8p_audio_destroy(reference);
            p8p_audio_destroy(batched);
        }
    }
}

/* PICO-8 chooses 30 or 60 fps once _init has run, so carts may install
 * _update60 from _init.  A 30 fps cart that later swaps _update for
 * _update60 gets two _update60 calls per frame.  Measured in PICO-8 0.2.7. */
static void update_callbacks_follow_init(void) {
    static const struct {
        const char *name;
        const char *source;
        int steps;
        int fps;
        int count;
    } cases[] = {
        {"_update60 set in _init",
         "function _init() _update60=function() n+=1 end end\n", 3, 60, 3},
        {"_init adds _update60 beside _update",
         "function _update() n+=2 end\n"
         "function _init() _update60=function() n+=1 end end\n", 3, 60, 3},
        {"_init removes _update60",
         "function _update60() n+=1 end\n"
         "function _init() _update60=nil _update=function() n+=2 end end\n",
         3, 30, 6},
        {"_init yields before setting _update60",
         "function _init() flip() _update60=function() n+=1 end end\n",
         4, 60, 3},
        {"_update swapped for _update60 mid-run",
         "function _update() n+=1\n"
         "if n==2 then _update60=_update _update=nil end end\n", 4, 30, 6},
    };
    for (const auto &test : cases) {
        char source[512];
        snprintf(source, sizeof(source),
                 "pico-8 cartridge // http://www.pico-8.com\nversion 42\n"
                 "__lua__\nn=0\n%s"
                 "function _draw() cls() pset(n,0,7) end\n", test.source);
        p8p_cart_t cart = {};
        CHECK(p8p_cart_load_text_memory((const uint8_t *)source,
                                        strlen(source), &cart) == 0);
        p8p_runtime_t *runtime = p8p_runtime_create();
        CHECK(runtime != NULL);
        CHECK(p8p_runtime_load(runtime, &cart) == 0);
        for (int step = 0; step < test.steps; ++step)
            CHECK(p8p_runtime_step(runtime, 0) == 0);
        const uint8_t *fb = p8p_runtime_framebuffer(runtime);
        int matches = p8p_runtime_target_fps(runtime) == test.fps &&
                      fb[test.count] == 7;
        if (!matches)
            fprintf(stderr, "FAIL %s: fps %d, pixel %d at n=%d\n", test.name,
                    p8p_runtime_target_fps(runtime), fb[test.count],
                    test.count);
        CHECK(matches);
        p8p_runtime_destroy(runtime);
        p8p_cart_destroy(&cart);
    }
}

static void draw_palette_transitions(void) {
    struct PaletteCase {
        const char *name;
        const char *setup;
        uint8_t zero_pixel, one_pixel, fifteen_pixel, screen_one, screen_fifteen;
    };
    static const PaletteCase cases[] = {
        {"screen scalar", "pal(1,8,1)", 6,1,15,8,15},
        {"screen table", "pal({[1]=8,[15]=139},1)", 6,1,15,8,139},
        {"scalar restore", "pal(1,4) pal(1,1)", 6,1,15,1,15},
        {"table restore", "pal({[1]=4,[15]=0}) pal({[1]=1,[15]=15})", 6,1,15,1,15},
        {"draw remap", "pal(1,4)", 6,4,15,1,15},
        {"remap replacement", "pal(1,4) pal(1,5)", 6,5,15,1,15},
        {"reset both", "pal(1,4) pal(1,8,1) pal()", 6,1,15,1,15},
        {"unchanged entries", "pal(1,1) pal(1,1) pal({[1]=1}) pal(1,4)", 6,4,15,1,15},
        {"screen and draw", "pal(1,8,1) pal(1,4)", 6,4,15,8,15},
        {"poke remap", "poke(0x5f01,4)", 6,4,15,1,15},
        {"poke restore", "poke(0x5f01,4) poke(0x5f01,1)", 6,1,15,1,15},
        {"copy remap", "poke(0x4300,4) memcpy(0x5f01,0x4300,1)", 6,4,15,1,15},
        {"copy restore", "poke(0x4300,4) memcpy(0x5f01,0x4300,1) poke(0x4300,1) memcpy(0x5f01,0x4300,1)", 6,1,15,1,15},
        {"screen RAM", "poke(0x5f11,8)", 6,1,15,8,15},
        {"one remap remains", "pal(1,4) pal(15,0) pal(1,1)", 6,1,0,1,15},
        {"opaque zero", "pal(1,8,1) palt(0,false)", 0,1,15,8,15},
        {"transparent nonzero", "pal(1,8,1) palt(1,true)", 6,6,15,8,15},
        {"transparency reset", "pal(1,8,1) palt(1,true) palt()", 6,1,15,8,15},
        {"screen-only reset", "pal({[1]=8,[15]=139},1) pal()", 6,1,15,1,15},
        {"all draw entries", "for i=0,15 do pal(i,(i+1)%16) end", 6,2,0,1,15},
        {"all entries restored", "for i=0,15 do pal(i,(i+1)%16) end for i=0,15 do pal(i,i) end", 6,1,15,1,15},
        {"print memory", "print(chr(6)..'!5f01'..chr(4),0,100)", 6,4,15,1,15},
    };
    for (const PaletteCase &test : cases) {
        char text[2048];
        snprintf(text, sizeof(text),
            "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
            "function _init() for y=0,7 do for x=0,7 do sset(8+x,8+y,(x+y*8)%%16) end end "
            "mset(0,0,17) %s end\n"
            "function _draw() if btn(0) then pal(1,4) elseif btn(1) then pal(1,1) end "
            "cls(6) spr(17,16,16) map(0,0,40,16,1,1) end\n", test.setup);
        p8p_cart_t cart = {};
        p8p_runtime_t *runtime = p8p_runtime_create();
        CHECK(runtime != NULL);
        if (!runtime) continue;
        int loaded = p8p_cart_load_text_memory((const uint8_t *)text, strlen(text), &cart);
        CHECK(loaded == 0);
        if (loaded == 0) loaded = p8p_runtime_load(runtime, &cart);
        CHECK(loaded == 0);
        if (loaded == 0) {
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            void *saved = NULL;
            size_t saved_size = 0;
            CHECK(p8p_runtime_save_state(runtime, &saved, &saved_size) == 0);
            for (int restored = 0; restored < 2; ++restored) {
                const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
                const uint8_t *screen = p8p_runtime_screen_palette(runtime);
                int before = failures;
                for (int tile = 0; tile < 2; ++tile) {
                    int x = tile == 0 ? 16 : 40;
                    CHECK(pixels[16 * 128 + x] == test.zero_pixel);
                    CHECK(pixels[16 * 128 + x + 1] == test.one_pixel);
                    CHECK(pixels[17 * 128 + x + 7] == test.fifteen_pixel);
                }
                CHECK(screen[1] == test.screen_one);
                CHECK(screen[15] == test.screen_fifteen);
                if (failures != before) fprintf(stderr, "palette transition: %s restored=%d\n", test.name, restored);
                if (restored == 0 && saved) {
                    CHECK(p8p_runtime_step(runtime, test.one_pixel == 4 ? 2 : 1) == 0);
                    CHECK(p8p_runtime_load_state(runtime, saved, saved_size) == 0);
                    CHECK(p8p_runtime_step(runtime, 0) == 0);
                }
            }
            free(saved);
        }
        p8p_runtime_destroy(runtime);
        p8p_cart_destroy(&cart);
    }
}

static void test_terminal_nul_source(void) {
    /* Legacy PNGs can count the terminating NUL in their declared code length.
     * Keep those decoded bytes for content/save identity, but compile source
     * without the terminator, as PICO-8 does when it exports the same cart. */
    char source[] = "function _init() cls(3) end\nfunction _draw() cls(9) end\n";
    p8p_cart_t cart = {};
    p8p_cart_hash_t before = {}, after = {}, without_nul = {};
    p8p_runtime_t *runtime = p8p_runtime_create();
    void *state = NULL;
    size_t state_size = 0;
    cart.lua = source;
    cart.lua_size = sizeof(source);
    p8p_cart_content_hash(&cart, &before);
    --cart.lua_size;
    p8p_cart_content_hash(&cart, &without_nul);
    ++cart.lua_size;
    CHECK(memcmp(&before, &without_nul, sizeof(before)) != 0);
    CHECK(runtime != NULL);
    if (!runtime)
        return;
    int loaded = p8p_runtime_load(runtime, &cart);
    CHECK(loaded == 0);
    if (loaded == 0) {
        CHECK(p8p_runtime_framebuffer(runtime)[0] == 3);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[0] == 9);
        CHECK(p8p_runtime_save_state(runtime, &state, &state_size) == 0);
        CHECK(state != NULL && state_size != 0);
        CHECK(p8p_runtime_load(runtime, &cart) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[0] == 3);
        if (state) {
            CHECK(p8p_runtime_load_state(runtime, state, state_size) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 9);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
        }
    }
    CHECK(cart.lua_size == sizeof(source));
    CHECK(cart.lua[cart.lua_size - 1] == '\0');
    p8p_cart_content_hash(&cart, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    char embedded_nul[] = "cls(7)\0cls(9)\n";
    cart.lua = embedded_nul;
    cart.lua_size = sizeof(embedded_nul) - 1;
    CHECK(p8p_runtime_load(runtime, &cart) != 0);
    free(state);
    p8p_runtime_destroy(runtime);
}

static void palette_fast_path_semantics(void) {
    static const char source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function _init() assert(select('#',pal())==0) "
        "assert(pal(1,4)==1) assert(pal(1,1)==4) "
        "pal({[1]=8},1) pal() assert(peek(0x5f11)==1) "
        "palt(0,false) pal() assert(peek(0x5f00)==16) end\n"
        "function _draw() pal() assert(select('#',pal())==0) "
        "assert(peek(0x5f00)==16 and peek(0x5f01)==1) end\n";
    p8p_cart_t cart = {};
    p8p_runtime_t *runtime = p8p_runtime_create();
    CHECK(runtime != NULL);
    if (!runtime) return;
    int loaded = p8p_cart_load_text_memory((const uint8_t *)source,
                                           sizeof(source) - 1, &cart);
    CHECK(loaded == 0);
    if (loaded == 0) loaded = p8p_runtime_load(runtime, &cart);
    CHECK(loaded == 0);
    if (loaded == 0) {
        for (int profiled = 0; profiled < 2; ++profiled) {
            p8p_runtime_set_profile_hook(runtime,
                profiled ? count_profile_event : NULL, NULL);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            p8p_runtime_api_profile_t profile = {};
            p8p_runtime_get_api_profile(runtime, &profile);
            CHECK(profile.calls[P8P_API_DRAW_STATE] == (profiled ? 2u : 0u));
        }
    }
    p8p_runtime_destroy(runtime);
    p8p_cart_destroy(&cart);

    /* A remap in any word must keep both tile APIs on the general path. */
    for (int changed = 0; changed < 16; ++changed) {
        char text[1024];
        snprintf(text, sizeof(text),
            "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
            "function _init() for y=0,7 do for x=0,7 do "
            "sset(8+x,8+y,(x+y*8)%%16) end end "
            "mset(0,0,17) cls(6) pal(1,8,1) pal(%d,%d) end\n"
            "function _draw() spr(17,16,16) map(0,0,40,16,1,1) end\n",
            changed, (changed + 1) % 16);
        p8p_cart_t remapped_cart = {};
        runtime = p8p_runtime_create();
        CHECK(runtime != NULL);
        if (!runtime) continue;
        loaded = p8p_cart_load_text_memory((const uint8_t *)text,
                                           strlen(text), &remapped_cart);
        CHECK(loaded == 0);
        if (loaded == 0) loaded = p8p_runtime_load(runtime, &remapped_cart);
        CHECK(loaded == 0);
        if (loaded == 0) {
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            for (int tile = 0; tile < 2; ++tile) {
                int x = tile == 0 ? 16 : 40;
                CHECK(pixels[16 * 128 + x] == 6);
                for (int color = 1; color < 16; ++color) {
                    int expected = color == changed ? (color + 1) % 16 : color;
                    CHECK(pixels[(16 + color / 8) * 128 + x + color % 8] == expected);
                }
            }
            CHECK(p8p_runtime_screen_palette(runtime)[1] == 8);
        }
        p8p_runtime_destroy(runtime);
        p8p_cart_destroy(&remapped_cart);
    }
}

static void palette_leaf_contracts(void) {
    alignas(4) uint8_t palette[16];
    for (int i = 0; i < 16; ++i) palette[i] = (uint8_t)i;
    CHECK(p8p_draw_palette_identity(palette) == 1);
    for (int i = 0; i < 16; ++i) {
        palette[i] ^= 1;
        CHECK(p8p_draw_palette_identity(palette) == 0);
        palette[i] ^= 1;
    }
    p8p_runtime_t *previous = p8p_active_runtime;
    p8p_runtime_t *runtime = (p8p_runtime_t *)calloc(1, sizeof(*runtime));
    lua_State *lua = luaL_newstate();
    CHECK(runtime && lua);
    if (!runtime || !lua) { free(runtime); if (lua) lua_close(lua); return; }
    p8p_active_runtime = runtime;
    runtime->palettes_default = runtime->transparency_default = 1;
    CHECK(p8p_api_pal(lua) == 0);
    runtime->palettes_default = 0;
    memset(runtime->draw_palette, 3, 16);
    CHECK(p8p_api_pal(lua) == 0);
    CHECK(runtime->palettes_default == 1);
    CHECK(p8p_draw_palette_identity(runtime->draw_palette) == 1);
    runtime->transparency_default = 0;
    memset(runtime->transparent, 1, 16);
    CHECK(p8p_api_pal(lua) == 0);
    CHECK(runtime->transparency_default == 1 && runtime->transparent[0] == 1);
    for (int i = 1; i < 16; ++i) CHECK(runtime->transparent[i] == 0);
    runtime->profile_hook = count_profile_event;
    uint32_t before = runtime->api_profile_calls[P8P_API_DRAW_STATE];
    CHECK(p8p_api_pal(lua) == 0);
    CHECK(runtime->api_profile_calls[P8P_API_DRAW_STATE] == before + 1);
    runtime->profile_hook = NULL;
    lua_pushnumber(lua, 1);
    lua_pushnumber(lua, 4);
    CHECK(p8p_api_pal(lua) == 1);
    CHECK(runtime->draw_palette[1] == 4 && runtime->palettes_default == 0);
    CHECK(lua_tonumber(lua, -1) == 1);
    p8p_active_runtime = previous;
    lua_close(lua);
    free(runtime);
}

/* P8SCII behaviour measured on PICO-8 0.2.7.  Each body draws with print() and
 * asserts through eq(actual, expected) and px(x, y, color). */
struct print_case_t {
    const char *label;
    const char *body;
};

static const print_case_t print_cases[] = {
    {"escapes lex to control codes", R"(eq(ord("\*"),1) eq(ord("\#"),2) eq(ord("\-"),3) eq(ord("\|"),4)
eq(ord("\+"),5) eq(ord("\^"),6) eq(ord("\a"),7) eq(ord("\b"),8) eq(ord("\t"),9)
eq(ord("\v"),11) eq(ord("\f"),12) eq(ord("\r"),13) eq(ord("\014"),14) eq(ord("\015"),15))"},
    {"shifts, home and jump", R"(eq(print("\-ka",8,0),16) eq(print("a\|ga",0,8),8)
print("\+kga",8,8,7) px(12,8,7)
print("ab\^hc\^gd\ne",0,20,7) eq(print("\^j24",0,0),8)
print("ab\rx",0,30,7) px(0,30,7) px(1,31,0))"},
    {"modes toggle and stripe", R"(print("\^t\^=l",0,0,7) px(0,0,7) px(0,1,0) px(0,2,7)
print("\^w\^=l",10,0,7) px(10,0,0) px(11,0,7)
print("\^pl",20,0,7) px(20,0,0) px(21,0,7) px(21,1,0)
eq(print("\^p\^-pl",30,0),34))"},
    {"raw memory writes", R"(print("\^@43000002ABc") eq(peek(0x4300),65) eq(peek(0x4301),66)
local y=peek(0x5f27) print("\^!4310XY") eq(peek(0x4311),89) eq(peek(0x5f27),y))"},
    {"default n and m differ", R"(
local function glyph(c,x,rows) print(c,x,0,7) for y=0,4 do local b=rows[y+1]
for i=0,2 do px(x+i,y,(b>>(2-i))&1==1 and 7 or 0) end end end
glyph("n",0,{6,5,5,5,5}) glyph("m",8,{7,7,5,5,5}) glyph("N",16,{0,6,5,5,5}) glyph("M",24,{0,7,7,5,5}))"},
    {"foreground persists", R"(print("\fca",0,0,7) eq(peek(0x5f25),12)
color(7) print("\f9x",0,10) pset(5,5) px(5,5,9))"},
    {"cls homes cursor", R"(rectfill(0,0,127,127,3) print("\^c1ab",50,50,7)
px(60,60,1) px(0,0,7) px(50,50,1))"},
    {"terminator keeps cursor", R"(cursor(0,0) print("ab\0") eq(peek(0x5f26),8) eq(peek(0x5f27),0)
print("c",7) px(9,0,7)
print("a\0",20,30) eq(peek(0x5f26),24) eq(peek(0x5f27),30)
eq(print("abc\0",0,0),12) print("\0abc",40,40) px(40,40,0))"},
    {"console scrolls below 122", R"(cursor(0,120) pset(5,10,9) print("a",7)
px(5,4,9) px(0,114,7) eq(peek(0x5f27),120))"},
    {"scroll needs no coordinates", R"(pset(5,10,9) print("a",0,120,7)
px(5,10,9) eq(peek(0x5f27),126))"},
    {"scroll threshold", R"(cursor(0,114) pset(5,10,9) print("a",7) px(5,10,9) eq(peek(0x5f27),120))"},
    {"scroll can be disabled", R"(poke(0x5f36,0x40) cursor(0,120) pset(5,10,9) print("a",7)
px(5,10,9) eq(peek(0x5f27),126) poke(0x5f36,0))"},
    {"scroll inside a string", R"(cursor(0,114) print("a\nb\nc",7) eq(peek(0x5f27),120))"},
    {"scroll twice for a line past the end", R"(cursor(0,117) pset(5,10,9) print("a\nb",7) eq(peek(0x5f27),117)
px(0,105,7) px(0,111,7))"},
    {"terminator only skips the final scroll", R"(cursor(0,120) print("a\0",7) eq(peek(0x5f27),120)
cursor(0,126) print("a\0",7) eq(peek(0x5f27),122))"},
    {"scroll stays inside the clip", R"(rectfill(0,0,127,127,5) pset(20,20,8) pset(5,30,8) clip(10,10,50,50) cursor(0,120) print("a",7) clip()
px(20,14,8) px(5,30,8) px(5,24,5))"},
    {"scroll threshold follows the camera", R"(camera(0,10) cursor(0,117) print("a",7) eq(peek(0x5f27),123))"},
    {"wide doubles the current width", R"(eq(print("\^wab",0,0),16) eq(print("\^x5\^wab",0,0),20)
eq(print("\^w\^x5ab",0,0),20) eq(print("\^w\^-wa",0,0),4))"},
    {"high characters advance", R"(eq(print("\130",0,0),8) eq(print("\#8\130",0,0),8)
eq(print("\^i\130",0,0),8) eq(print("\^w\130",0,0),16) eq(print("\^x5\130",0,0),9))"},
    {"custom widths survive modes", R"(poke(0x5600,6,10,8) eq(print("\014a\130",0,0),16)
eq(print("\014\#9a\130",0,0),16) eq(print("\014\^wa",0,0),12)
eq(print("\014\^w\^-wa",0,0),6) print("\014\^ta",0,0) eq(peek(0x5f27),16))"},
    {"tall doubles the current height", R"(print("\^ta\nb",0,0,7) px(0,12,7) print("\^y4\^ta\nb",20,0,7) px(20,8,7))"},
    {"default attribute modes set metrics", R"(poke(0x5f58,5) eq(print("ab",0,0),16)
poke(0x5f58,9) print("a",0,0) eq(peek(0x5f27),12) poke(0x5f58,0))"},
    {"backspace undoes last width", R"(print("\130\b\0",0,0) eq(peek(0x5f26),0)
print("\^wa\b\0",0,0) eq(peek(0x5f26),0))"},
    {"blank print uses character height", R"(print("\^y8",0,0) eq(peek(0x5f27),8))"},
    {"default width and height", R"(poke(0x5f59,0x85) eq(print("ab",0,0),10) eq(peek(0x5f27),8))"},
    {"default high width", R"(poke(0x5f5a,0x06) eq(print("\130",0,0),6) eq(print("a",0,0),4))"},
    {"zero nibbles are ignored", R"(poke(0x5f59,0x05) print("\na",0,0,7) px(0,6,7) eq(print("a",0,0),5))"},
    {"default offsets", R"(poke(0x5f5b,0x21) print("a",0,0,7) px(1,2,7) px(0,0,0))"},
    {"tab reaches the next stop", R"(eq(print("a\tb",0,0),20) eq(print("\tb",0,0),20))"},
    {"tab width counts fours of pixels", R"(eq(print("\^s8a\tb",0,0),36) eq(print("\^s1a\tb",0,0),12)
eq(print("\^s0a\tb",0,0),20))"},
    {"default tab width", R"(poke(0x5f5a,0x30) eq(print("a\tb",0,0),20) poke(0x5f5a,0x20) eq(print("a\tb",0,0),16)
poke(0x5f5a,0x10) eq(print("\t\tb",0,0),20))"},
    {"custom font tab width", R"(poke(0x5600,4) poke(0x5606,10) eq(print("\014a\tb",0,0),14)
eq(print("a\tb",0,0),20))"},
    {"tabs relative to home", R"(poke(0x5600,4) poke(0x5606,10) eq(print("\014a\tb",3,0),14)
poke(0x5605,2) eq(print("\014a\tb",3,0),17))"},
    {"background covers the padded cell", R"(print("\#8 ",10,20) px(9,19,8) px(13,24,8) px(13,25,8)
print("\^-b\#9 ",30,20) px(29,20,0) px(30,19,0) px(30,20,9) px(33,25,9) px(34,20,0))"},
    {"attribute padding", R"(poke(0x5f58,1) print("\#a ",50,20) px(49,20,0) px(53,25,10)
poke(0x5f58,3) print("\#a ",60,20) px(59,19,10) poke(0x5f58,0))"},
    {"padding switches back on", R"(print("\^-b\^b\#8 ",10,20) px(9,19,8))"},
    {"invert fills the padded cell", R"(print("\^ix",10,20,7) px(9,19,7) px(10,20,0)
px(11,20,7) px(13,24,7) px(13,25,7))"},
    {"invert without padding", R"(print("\^-b\^ix",30,20,7) px(29,20,0) px(30,19,0) px(31,20,7) px(30,25,7))"},
    {"solid background without a colour", R"(rectfill(0,40,40,60,5) print("\^#x",10,45,7)
px(9,44,0) px(11,45,0) px(10,45,7))"},
    {"background follows the fill pattern", R"(fillp(0x5a5a) print("\#8  ",20,90) fillp()
px(21,90,0) px(20,90,8) px(21,91,8))"},
    {"one-off cell keeps eight rows", R"(print("\#8\^;ff00000000000000",10,60,7) px(12,67,8) px(12,68,0))"},
    {"cls code stays inside the clip", R"(clip(10,10,20,20) print("\^c5",10,10) clip() px(0,0,0) px(15,15,5))"},
    {"high character cell", R"(print("\#8\130",20,50,7) px(19,49,8) px(27,55,8) px(28,50,0))"},
    {"one-off ignores padding and height", R"(print("\#8\^:ff00000000000000",10,60,7)
px(10,60,7) px(9,60,0) px(10,59,0) px(17,67,8) px(18,60,0)
print("\^y3\^:0000000000000001",30,60,7) px(30,67,7))"},
    {"one-off respects padding", R"(print("\#8\^;ff00000000000000",10,60,7) px(10,60,7) px(9,59,8) px(17,67,8))"},
    {"repeat paints background", R"(print("\#8\*3x",10,10,7) px(21,10,8) px(20,10,7))"},
    {"repeat sets line height", R"(print("\^y9\*2a\nb",0,0,7) px(0,9,7))"},
    {"repeat newline", R"(print("\*3\nb",0,0,7) px(0,18,7))"},
    {"decorate repeated character", R"(print("\*2a\v0b",0,20,7) px(2,12,7))"},
    {"decorate one-off character", R"(print("\^:0000000000000000\v0b",10,20,7) px(8,12,7))"},
    {"decoration restores cursor", R"(eq(print("a\v5zb",0,10),8))"},
    {"repeat keeps outline", R"(print("\^o8ff\*2a",10,30,7) px(9,29,8) px(15,31,8))"},
    {"one-off keeps outline", R"(print("\^o8ff\^:0100000000000000",10,50,7) px(10,50,7) px(9,49,8))"},
    {"black outline", R"(rectfill(0,0,30,30,5) print("\^o0ffa",10,10,7) px(9,9,0) px(10,10,7) px(11,11,0))"},
    {"outline in the current colour", R"(print("\^o$ffa",10,40,12) px(9,39,12) print("\f8\^o$ffa",30,40) px(29,39,8))"},
    {"outline without interior", R"(rectfill(10,60,30,80,5) print("\^o!ffa",12,62,7) px(11,61,7) px(12,62,5) px(13,63,7))"},
    {"outline over a background", R"(print("\#9\^o8ff.",10,90,7) px(9,90,9) px(14,90,0) px(10,93,8) px(11,94,7))"},
    {"outline colour is taken when set", R"(print("\^o$ff\f8a",10,40,7) px(9,39,7) px(10,40,8))"},
    {"outline keeps invert", R"(print("\^i\^o8ffa",10,20,7) px(8,18,8) px(9,19,7))"},
    {"underline width and start", R"(print("\^-b\^ua",10,0,7) px(9,6,0) px(14,6,7) print("\^w\^ua",20,20,7) px(18,26,7) px(22,26,7) px(23,26,0))"},
    {"outline stays one pixel thick", R"(print("\^w\^o8ffa",40,10,7) px(39,9,8) px(38,9,0))"},
    {"custom draw offsets", R"(poke(0x5600,8,8,8) poke(0x5600+97*8,1)
poke(0x5603,2) poke(0x5604,3) print("\014a",10,10,7) px(12,13,7) px(10,10,0)
poke(0x5603,0xff) poke(0x5604,0xff) print("\014a",10,20,7) px(9,19,0))"},
    {"custom high characters", R"(poke(0x5600,8,10,8) poke(0x5600+200*8,0x80)
eq(print("\014\200",10,30,7),20) px(17,30,7))"},
    {"custom width adjustments", R"(poke(0x5600,8,8,8) poke(0x5605,1)
poke(0x5608+40,0x70) eq(print("\014a",0,0),7) poke(0x5608+40,0x40) eq(print("\014a",0,0),4)
poke(0x5608+40,0x30) eq(print("\014a",0,0),11) poke(0x5608+41,0x02) eq(print("\014b",0,0),10)
poke(0x5608,0x01) eq(print("\014\16",0,0),9) poke(0x5605,0) eq(print("\014a",0,0),8))"},
    {"custom width adjust scales when wide", R"(poke(0x5600,8,8,8) poke(0x5605,1)
poke(0x5608+40,0x70) eq(print("\014\^wa",0,0),14))"},
    {"custom raised character", R"(poke(0x5600,8,8,8) poke(0x5600+97*8,1) poke(0x5605,1)
poke(0x5608+40,0x80) print("\014a",10,40,7) px(10,39,7) px(10,40,0))"},
    {"wrap moves the overflowing character", R"(print("\^r2abc",0,0,7) px(1,6,7) eq(peek(0x5f27),12))"},
    {"wrap leaves an exact fit alone", R"(print("\^r2ab",0,0,7) eq(peek(0x5f27),6))"},
    {"wrap returns home", R"(print("ab\^h\^r4cde",0,0,7) px(9,6,7))"},
    {"screen wrap flag", R"(print("aaa",120,0,7) px(120,6,0) poke(0x5f36,0x80) print("aaa",120,10,7) px(120,16,7) poke(0x5f36,0))"},
    {"audio plays an existing sfx", R"(poke(0x3200+68*12,0x18,0x0a) poke(0x3200+68*12+65,32)
print("\a12 ab",0,0,7) eq(stat(46),12) px(0,0,7))"},
    {"audio skips note data", R"(print("\accc ab",0,0,7) eq(stat(46),-1) px(0,0,7))"},
    {"underline can be disabled", R"(print("\^ua\^-ub",0,0,7) px(1,6,7) px(5,6,0))"},
    {"second return is the lowest y", R"(local x,y=print("ab\ncd",10,20) eq(y,32)
x,y=print("\|0a",10,20) eq(y,20) x,y=print("abc\0",10,20) eq(y,20))"},
    {"print returns to the margin", R"(print("ab\^hcd",30,40,7) eq(peek(0x5f24),30) eq(peek(0x5f26),38)
print("c") eq(peek(0x5f26),30)
print("ab\0",30,40) print("c") eq(peek(0x5f26),30) cursor(9,0) eq(peek(0x5f24),9))"},
    {"poke command ends the print", R"(print("ab\^!4300x",30,40,7) eq(peek(0x5f26),38) eq(peek(0x5f27),40)
local x,y=print("ab\^!4300x",0,0) eq(x,8) eq(y,0))"},
    {"stripes follow height when tall and wide", R"(print("\^w\^t\^=l",0,0,7) px(0,0,7) px(1,0,7) px(0,1,0))"},
    {"pinball is its own state", R"(eq(print("\^p\^-wa",0,0),8) print("\^p\^-=l",0,0,7) px(0,0,0) px(1,0,7) px(1,1,0))"},
    {"padded cells scale and reach the last row", R"(print("\#8 ",10,20) px(13,25,8) px(14,25,0)
print("\^w\#9 ",30,20) px(28,19,9) px(27,19,0) print("\^t\#a ",50,40) px(49,38,10) px(49,37,0))"},
    {"cls resets clip and ignores pal", R"(clip(10,10,20,20) pal(5,8) cls(5) eq(peek(0x5f20),0) eq(peek(0x5f22),128)
px(0,0,5) pal() rectfill(0,0,3,3,1) px(1,1,1) pal(1,9) print("\^c1",0,0) px(9,9,1))"},
    {"blank lines ignore tall", R"(print("\^t\n\nb",0,0,7) px(0,12,7) print("\^t",0,0) eq(peek(0x5f27),6))"},
    {"print argument conversion", R"(eq(print(nil,0,0),20) eq(print({},0,0),28) print("a",nil) eq(peek(0x5f25),0)
local x,y=print() eq(x,nil))"},
    {"foreground keeps the raw parameter", R"(print("\fga",0,0) eq(peek(0x5f25),16) px(0,0,0))"},
    {"repeat re-runs any code", R"(eq(print("\*3\tb",0,0),52) eq(print("\*2\*3x",0,0),12) print("\*3\f8x",0,0) eq(peek(0x5f25),8))"},
    {"truncated code ends the print", R"(print("\#",10,20) eq(peek(0x5f27),20) print("\-",10,20) eq(peek(0x5f26),250)
print("\^j",10,20) eq(peek(0x5f26),0) eq(peek(0x5f27),0))"},
    {"decoration starts from the last character", R"(local x,y=print("\v5z",-10,-20) eq(x,-1) eq(y,-7)
print("\v5z",10,20,7) px(11,13,0))"},
    {"glyphs are cut to their advance", R"(poke(0x5f5a,1) print("\130",10,0,7) px(11,1,0) px(10,1,7) poke(0x5f5a,0)
poke(0x5600,8,8,8) poke(0x5600+97*8,0x80) poke(0x5605,1) poke(0x5608+40,0x70) print("\014a",20,0,7) px(27,0,0))"},
    {"default attributes skip custom fonts", R"(poke(0x5f59,5) poke(0x5600,6,9,7) eq(print("\014ab",0,0),12) poke(0x5f59,0))"},
    {"attributes stop at the font codes", R"(poke(0x5f59,5) poke(0x5600,6,9,7) print("ab\014\015",0,0) eq(print("\014\015ab",0,0),8))"},
    {"attribute offsets skip custom fonts", R"(poke(0x5f5b,1) poke(0x5600,8,8,8) poke(0x5600+97*8,1) print("\014a",10,10,7) px(10,10,7))"},
    {"attribute font with an empty header", R"(poke(0x5f58,0x81) eq(print("ab",0,0),16) poke(0x5f58,0))"},
    {"underline stops at eight rows", R"(poke(0x5600,8,8,12) print("\014\^ua",0,0,7) px(1,8,7) px(1,12,0))"},
    {"underline follows offsets and raised", R"(poke(0x5f5b,0x10) print("\^ua",0,0,7) px(1,7,7) poke(0x5f5b,0))"},
    {"invert stops at eight rows", R"(print("\^y9\^ia",10,10,7) px(12,17,7) px(12,18,0))"},
    {"wrap uses the unscaled width and never scrolls", R"(eq(print("\^r0ab",10,0),14) poke(0x5f36,0x80) local x=print("\^waa",116,0) eq(x,132) poke(0x5f36,0)
cursor(0,118) print("\^r3a\tb",7) eq(peek(0x5f27),122))"},
    {"decoration uses the position before a wrap", R"(print("\^r3a\v5z",10,20,7) px(11,13,7))"},
    {"audio stops at control characters", R"(eq(print("\a12\nab",0,0),8))"},
    {"truncated one-off and outline", R"(eq(print("\^:",0,0),0) eq(peek(0x5f27),6) print("\^.\1",0,0) eq(peek(0x5f27),0) print("\^o",0,0) eq(peek(0x5f27),6) print("\^o8f",0,0) eq(peek(0x5f27),0))"},
    {"flag 4 keeps the cursor on the line", R"(poke(0x5f36,4) print("ab",10,20) eq(peek(0x5f26),18) eq(peek(0x5f27),20) poke(0x5f36,0))"},
    {"cursor without arguments goes home", R"(cursor(9,9) cursor() eq(peek(0x5f24),0) eq(peek(0x5f26),0) eq(peek(0x5f27),0))"},
    {"explicit nil colour and function names", R"(color(9) print("a",10,20,nil) eq(peek(0x5f25),0) eq(print(print,0,0),40))"},
    {"delays make print return nothing", R"(eq(print("a\^1b",0,0),nil) eq(print("\^d1a",0,0),nil) eq(print("a\^d1",0,0),4)
eq(print("\^d0a",0,0),4) print("a\^1b",10,20,7) px(14,20,7))"},
    {"short hex prints as text", R"(eq(print("\^:ff",0,0),8) print("\^:ff",0,0,7) px(0,1,7))"},
    {"flag 4 skips the final scroll", R"(poke(0x5f36,4) cursor(0,120) print("a",7) eq(peek(0x5f27),120) poke(0x5f36,0))"},
    /* end of print cases */
};

static void p8scii_print_cases(void) {
    static const char cart_format[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function eq(a,b) if a~=b then bad=true end end\n"
        "function px(x,y,c) eq(pget(x,y),c) end\n"
        "function _draw() cls()\n%s\ncls(bad and 8 or 11) end\n";
    p8p_runtime_t *runtime = p8p_runtime_create();
    CHECK(runtime != NULL);
    if (!runtime)
        return;
    for (const print_case_t &print_case : print_cases) {
        char text[4096];
        int length = snprintf(text, sizeof(text), cart_format, print_case.body);
        p8p_cart_t cart = {};
        int ok = length > 0 && (size_t)length < sizeof(text) &&
                 p8p_cart_load_text_memory((const uint8_t *)text, (size_t)length,
                                           &cart) == 0 &&
                 p8p_runtime_load(runtime, &cart) == 0 &&
                 p8p_runtime_step(runtime, 0) == 0 &&
                 p8p_runtime_framebuffer(runtime)[0] == 11;
        if (!ok) {
            fprintf(stderr, "FAIL print case: %s (%s)\n", print_case.label,
                    p8p_runtime_error(runtime));
            ++failures;
        }
        p8p_cart_destroy(&cart);
    }
    p8p_runtime_destroy(runtime);
}

/** @brief Run host runtime contracts and embedded cartridge regression cases.
 * Checks API state and rendered pixel markers, including all(t) traversal
 * through deletion, growth, collection and mixed values. Returns nonzero on
 * failed checks so iterator changes are covered by the normal test target.
 */
int main(void) {
    p8scii_print_cases();
    test_terminal_nul_source();
    test_incompatible_carts();
    palette_leaf_contracts();
    palette_fast_path_semantics();
    draw_palette_transitions();
    update_callbacks_follow_init();
    audio_chunk_boundaries();
    static const uint8_t source[] =
        "pico-8 cartridge // http://www.pico-8.com\n"
        "version 42\n"
        "__lua__\n"
        "x=0\n"
        "local tins=table.insert holder={}\n"
        "function _init() cartdata('runtime-test') saved=dget(0) "
        "dset(0,saved+1) empty={} add(nil,1) del(nil,1) deli(nil) "
        "foreach(nil,function() add(empty,99) end) add(empty,7) "
        "seq={1,2,3} seen=0 foreach(seq,function(v) seen=seen*10+v "
        "if v==2 then del(seq,v) end end) "
        "cls(1) sfx(0) end\n"
        "function _update60() for i=1,16384 do local z=i end "
        "if btn(\x91) then x+=1 tins(holder,x) end end\n"
        "function _draw() local loopn=0 for i=1,10 do loopn+=1 end\n"
        " cls(0) pset(x,3,8) rectfill(10,10,12,12,9) print('a',20,20,7)\n"
        " circfill(30,30,2,10) spr(0,50,50)\n"
        " camera(5,6) clip(40,40,3,3) rectfill(44,45,50,50,11)\n"
        " camera() clip()\n"
        " pset(5,4,12)\n"
        " ovalfill(70,70,74,72,10) sspr(0,0,2,1,80,80,4,2)\n"
        " poke2(0x4300,0x1234) if peek2(0x4300)==0x1234 then pset(90,2,7) end\n"
        " poke4(0x4304,1.5) if peek4(0x4304)==1.5 then pset(91,2,8) end\n"
        " if 5.5/2==2.75 and -5.5/2==-2.75 and 2.75*2==5.5 "
        "then pset(92,2,9) end\n"
        " if seen==123 and #seq==2 and seq[2]==3 then pset(93,2,10) end\n"
        " if peek(0x6102)~=0xc0 then pset(4,4,8) end\n"
        " poke(0x6103,0x34) pal(8,12,1) "
        "pal({[0]=128,[1]=9,[15]=139},1) pset(loopn,6,13)\n"
        "end\n"
        "__gfx__\n"
        "1200000000000000\n"
        "__gff__\n"
        "05\n"
        "__map__\n"
        "01\n"
        "__sfx__\n"
        "00100000" TEST_NOTES_32 "\n";
    p8p_cart_t cart = {};
    p8p_cart_t png_cart = {};
    p8p_cart_t legacy_cart = {};
    p8p_cart_t flip_cart = {};
    p8p_cart_t glyph_cart = {};
    p8p_cart_t nested_short_if_cart = {};
    p8p_cart_t live_input_cart = {};
    p8p_cart_t runaway_cart = {};
    p8p_cart_t run_cart = {};
    p8p_cart_t env_fallback_cart = {};
    p8p_cart_t memory_limits_cart = {};
    p8p_cart_t table_paths_cart = {};
    p8p_cart_t text_engine_cart = {};
    p8p_cart_t tap_cart = {};
    p8p_cart_t late_press_cart = {};
    p8p_cart_t no_repeat_cart = {};
    p8p_runtime_t *runtime;
    const uint8_t *framebuffer;
    const uint8_t *screen_palette;
    int16_t audio_samples[512 * 2];
    void *saved_state = NULL;
    size_t saved_state_size = 0;
    p8p_cart_hash_t cart_hash = {};
    p8p_state_meta_t state_meta = {};
    p8p_runtime_api_profile_t api_profile = {};
    unsigned char config_pattern[32];
    unsigned char config_readback[32];
    char store_path[] = "/tmp/pico8pocket-state-XXXXXX";
    int store_fd = mkstemp(store_path);

    CHECK(store_fd >= 0);
    if (store_fd >= 0)
        close(store_fd);
    for (size_t i = 0; i < sizeof(config_pattern); ++i)
        config_pattern[i] = (unsigned char)(i * 7u + 3u);

    CHECK(p8p_cart_load_text_memory(source, sizeof(source) - 1, &cart) == 0);
    CHECK(cart.lua != NULL);
    CHECK(strstr(cart.lua, "function _update60()") != NULL);
    CHECK(cart.rom[0] == 0x21);
    CHECK(cart.rom[0x3000] == 0x05);
    CHECK(cart.rom[0x2000] == 0x01);
    CHECK(cart.rom[0x3200] == 0x21);
    CHECK(cart.rom[0x3201] == 0x0e);
    CHECK(cart.rom[0x3241] == 0x10);

    CHECK(p8p_cart_load_file(
        ".deps/fake-08/test/carts/cartparsetest.p8.png", &png_cart) == 0);
    CHECK(png_cart.lua != NULL);
    CHECK(strcmp(png_cart.lua, "a=1") == 0 || strcmp(png_cart.lua, "a=1\n") == 0);
    CHECK(png_cart.rom[0] == 0xff);
    CHECK(png_cart.rom[1] == 0x01);
    CHECK(png_cart.rom[0x2000] == 0x01);
    CHECK(png_cart.rom[0x3001] == 0x01);
    CHECK(p8p_cart_load_file(
        ".deps/fake-08/test/carts/test_legacypng_cart.p8.png", &legacy_cart) == 0);
    CHECK(strcmp(legacy_cart.lua, "print(\"0.1.10c\")\n") == 0);
    static const uint8_t flip_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 repeat n+=1 pset(n,1,8) flip() until n==3\n"
        "function _update() n+=1 end\n";
    CHECK(p8p_cart_load_text_memory(flip_source, sizeof(flip_source) - 1,
                                    &flip_cart) == 0);
    static const uint8_t glyph_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "v=0 function _update60() "
        "if btn(\x8b) then v=0 elseif btn(\x91) then v=1 "
        "elseif btn(\x94) then v=2 elseif btn(\x83) then v=3 "
        "elseif btn(\x8e) then v=4 elseif btn(\x97) then v=5 "
        "elseif btn(6) then v=6 end end\n"
        "function _draw() cls() pset(v,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(glyph_source, sizeof(glyph_source) - 1,
                                    &glyph_cart) == 0);
    static const uint8_t nested_short_if_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 co=cocreate(function() yield() n+=4 end)\n"
        "function _update60()\n"
        "if(false) if(true) n=99\n"
        "n+=1 if(costatus(co)~='dead') coresume(co)\nend\n"
        "function _draw() cls() pset(n,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(nested_short_if_source,
                                    sizeof(nested_short_if_source) - 1,
                                    &nested_short_if_cart) == 0);
    static const uint8_t live_input_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 function _update60() while not btn(6) do n+=1 end end\n"
        "function _draw() cls() pset(1,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(live_input_source,
                                    sizeof(live_input_source) - 1,
                                    &live_input_cart) == 0);
    static const uint8_t runaway_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 while true do n+=1 end\n";
    CHECK(p8p_cart_load_text_memory(runaway_source,
                                    sizeof(runaway_source) - 1,
                                    &runaway_cart) == 0);
    static const uint8_t run_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function _init() cartdata('run-test') n=dget(0) dset(0,n+1) end\n"
        "function _update() if n==0 then run() end end\n"
        "function _draw() cls() pset(n,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(run_source, sizeof(run_source) - 1,
                                    &run_cart) == 0);
    static const uint8_t env_fallback_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function station() return 9 end t={}\n"
        "function sandbox_lookup(_ENV) return type(circfill)=='function' end\n"
        "function _draw() cls() if t.station==nil and sandbox_lookup({}) "
        "then pset(2,0,7) end end\n";
    CHECK(p8p_cart_load_text_memory(env_fallback_source,
                                    sizeof(env_fallback_source) - 1,
                                    &env_fallback_cart) == 0);
    static const uint8_t memory_limits_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function _draw() cls() memset(0,3,20000)\n"
        "local n=select('#',peek(0,20000))\n"
        "local s=chr(peek(0,9000))\n"
        "if n==20000 and #s==9000 and ord(s,9000)==3 then pset(3,0,7) end\n"
        "if debug==nil then pset(4,0,7) end end\n";
    CHECK(p8p_cart_load_text_memory(memory_limits_source,
                                    sizeof(memory_limits_source) - 1,
                                    &memory_limits_cart) == 0);
    static const uint8_t table_paths_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function _draw() cls() local ok=0\n"
        "local log={} local p=setmetatable({},{__index=function(t,k) return 'i'..k end,"
        "__newindex=function(t,k,v) rawset(t,k,v*2) log[#log+1]=k end})\n"
        "if p.a=='ia' then ok+=1 end\n"
        "p.b=3 if p.b==6 and #log==1 then ok+=1 end\n"
        "p.b=5 if p.b==5 and #log==1 then ok+=1 end\n"
        "local t={10,20,[0]=5,[1.5]=7,x=1} t[2]=21 t.x+=1\n"
        "if t[1]==10 and t[2]==21 and t[0]==5 and t[1.5]==7 and t[1.25]==nil "
        "and t.x==2 and t.y==nil then ok+=1 end\n"
        "local str='abc' if str[2]=='b' and str[-1]=='c' then ok+=1 end\n"
        "local o={v=4} function o:get() return self.v end if o:get()==4 then ok+=1 end\n"
        "local function inenv(_ENV) return circfill~=nil and q==nil end\n"
        "if inenv({}) then ok+=1 end\n"
        "glob=1 glob+=1 if glob==2 then ok+=1 end\n"
        "pset(ok,1,7) end\n";
    CHECK(p8p_cart_load_text_memory(table_paths_source,
                                    sizeof(table_paths_source) - 1,
                                    &table_paths_cart) == 0);
    /* .p8 source stores P8SCII as UTF-8 glyphs: U+25DD is 255 and the
     * down-arrow emoji (with or without U+FE0F) is 131. */
    static const uint8_t text_engine_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function _draw() cls()\n"
        "local s=\"\xe2\x97\x9d" "\xe2\xac\x87\xef\xb8\x8f" "\xe2\xac\x87\"\n"
        "if #s==3 and ord(s,1)==255 and ord(s,2)==131 and ord(s,3)==131 then pset(1,2,7) end\n"
        "palt(2) if peek(0x5f0e)&16>0 and peek(0x5f00)&16==0 then pset(2,2,7) end palt()\n"
        "if print(\"ab\",10,100)==18 then pset(3,2,7) end\n"
        "print(\"\\fcx\",30,100,7) local hit=0\n"
        "for y=100,104 do for x=30,33 do if pget(x,y)==12 then hit+=1 end end end\n"
        "if hit>0 then pset(4,2,7) end\n"
        "print(\"\\6!4300AB\") if peek(0x4300)==65 and peek(0x4301)==66 then pset(5,2,7) end\n"
        "poke(0x5f55,0) pset(1,1,9) poke(0x5f55,0x60)\n"
        "if sget(1,1)==9 and pget(1,1)==0 then pset(6,2,7) end\n"
        "palt(2) rectfill(60,60,75,67,3) spr(-1,60,60,2,1) palt()\n"
        "if pget(60,60)==3 and pget(75,67)==3 then pset(7,2,7) end\n"
        "poke(0x5f55,0) rectfill(0,0,3,3,5) sset(1,1,9) mset(0,32,0x77)\n"
        "local redirected=sget(1,1)==9 and sget(0,1)==5 and sget(0,64)==7 and sget(1,64)==7\n"
        "poke(0x5f55,0x60) if redirected and sget(1,1)==9 then pset(8,2,7) end\n"
        "rectfill(40,40,90,90,0) clip(140,0,20,128) circfill(130,64,40,8) rectfill(0,50,200,60,8)\n"
        "clip() if pget(64,64)==0 and pget(64,55)==0 then pset(9,2,7) end\n"
        "if \x81==0x5a5a.8 and \x80==0 and \x99==0x5555.8 and \x8e==4 then pset(10,2,7) end\n"
        "color(6) ovalfill(40,40,50,50,9) rect(40,40,41,41) if pget(40,40)==9 and peek(0x5f25)==9 then pset(11,2,7) end\n"
        "palt(9,true) pal() if peek(0x5f09)==9 and peek(0x5f00)==0x10 then pset(12,2,7) end\n"
        "cstore() if trace(\"here\")==\"here\" and trace()==\"\" then pset(13,2,7) end\n"
        "if tostr(1.5)..\",\"..-32768 ..\",\"..(1/3)..\",\"..0x0.0001 ..\",\"..-0x0.0001 ..\",\"..32767.99999 ..\",\"..-2.25 ==\"1.5,-32768,0.3333,0,-0,32768,-2.25\" then pset(15,2,7) end\n"
        "local n=0 for v in all(split(false,\",\")) do n+=1 end if n==0 and split(nil)==nil and #split(\"1,2\")==2 and split(12)[1]==12 then pset(16,2,7) end\n"
        "local m={__eq=function() return true end} local ta,tb=setmetatable({},m),setmetatable({},m) local sa=\"ab\" local sb=\"a\"..\"b\"\n"
        "if 1==1.0 and 2~=3 and sa==sb and sa~=\"ac\" and nil~=false and false==false and true~=false and ta==tb and {}~={} and ta==ta then pset(17,2,7) end\n"
        "local A={x=1} local B=setmetatable({y=2},{__index=A}) local C=setmetatable({},{__index=B})\n"
        "local F=setmetatable({},{__index=function(t,k) return k..\"!\" end})\n"
        "local H=setmetatable({[0.5]=\"half\"},{__index={[0.5]=\"wrong\"}})\n"
        "local D=C for i=1,6 do D=setmetatable({},{__index=D}) end\n"
        "local E=setmetatable({},{__index=_ENV}) local function inenv() local _ENV=E return x==nil and pset~=nil end\n"
        "if C.x==1 and C.y==2 and C.z==nil and F.q==\"q!\" and H[0.5]==\"half\" and D.x==1 and inenv() then pset(18,2,7) end\n"
        "lc=5 do local lc*=2 local ls=\"a\" local ls..=\"b\" if lc==10 and ls==\"ab\" then pset(14,2,7) end end\n"
        "if lc~=5 then pset(14,2,0) end\n"
        "print(\"\\6y7a\\6y3\\n\\n\\f8b\",40,100,7) local row=0\n"
        "for x=40,43 do if pget(x,110)==8 then row+=1 end end if row>0 then pset(19,2,7) end\n"
        "end\n";
    CHECK(p8p_cart_load_text_memory(text_engine_source,
                                    sizeof(text_engine_source) - 1,
                                    &text_engine_cart) == 0);
    /* A: tap released mid-frame before the read.  B: press arriving after
     * the btnp() check.  C: 0x5f5c=255 disables btnp() repeat. */
    static const uint8_t tap_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 function _update60() for i=1,30000 do end\n"
        "if btn(4) and btnp(4) then n+=1 end end\n"
        "function _draw() cls() pset(n,0,7) end\n";
    static const uint8_t late_press_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "m=0 function _update60() if btnp(4) then m+=1 end\n"
        "for i=1,30000 do end end\n"
        "function _draw() cls() pset(m,0,7) end\n";
    static const uint8_t no_repeat_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "r=0 function _init() poke(0x5f5c,255) end\n"
        "function _update60() if btnp(4) then r+=1 end end\n"
        "function _draw() cls() pset(r,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(tap_source, sizeof(tap_source) - 1,
                                    &tap_cart) == 0);
    CHECK(p8p_cart_load_text_memory(late_press_source,
                                    sizeof(late_press_source) - 1,
                                    &late_press_cart) == 0);
    CHECK(p8p_cart_load_text_memory(no_repeat_source,
                                    sizeof(no_repeat_source) - 1,
                                    &no_repeat_cart) == 0);
    runtime = p8p_runtime_create();
    CHECK(runtime != NULL);
    if (runtime) {
        p8p_runtime_set_cartdata_hooks(runtime, test_cartdata_load,
                                       test_cartdata_save, store_path);
        int loaded = p8p_runtime_load(runtime, &cart);
        if (loaded != 0)
            fprintf(stderr, "runtime load: %s\n", p8p_runtime_error(runtime));
        CHECK(loaded == 0);
        CHECK(p8p_runtime_target_fps(runtime) == 60);
        p8p_runtime_flush_cartdata(runtime);
        unsigned char cartdata_readback[256] = {};
        CHECK(p8p_cartdata_load_file(store_path, "runtime-test",
                                     cartdata_readback,
                                     sizeof(cartdata_readback)) == 0);
        CHECK(cartdata_readback[2] == 1);
        p8p_runtime_set_service_hook(runtime, count_service_hook, NULL);
        memset(profile_event_calls, 0, sizeof(profile_event_calls));
        p8p_runtime_set_profile_hook(runtime, count_profile_event, NULL);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(service_hook_calls > 0);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_BEGIN] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_END] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_BEGIN] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_END] == 1);
        p8p_runtime_get_api_profile(runtime, &api_profile);
        CHECK(api_profile.calls[P8P_API_SPRITE] > 0);
        CHECK(api_profile.calls[P8P_API_GRAPHICS] > 0);
        CHECK(api_profile.calls[P8P_API_MEMORY] > 0);
        CHECK(api_profile.calls[P8P_API_DRAW_STATE] > 0);
        CHECK(api_profile.calls[P8P_API_TEXT] > 0);
        CHECK(api_profile.calls[P8P_API_INPUT] > 0);
        p8p_runtime_set_profile_hook(runtime, NULL, NULL);
        memset(profile_event_calls, 0, sizeof(profile_event_calls));
        p8p_runtime_set_profile_hook(runtime, count_profile_event, NULL);
        CHECK(p8p_runtime_step_with_draw(runtime, 0, 0) == 0);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_BEGIN] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_END] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_BEGIN] == 0);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_END] == 0);
        p8p_runtime_get_api_profile(runtime, &api_profile);
        CHECK(api_profile.calls[P8P_API_INPUT] > 0);
        CHECK(api_profile.calls[P8P_API_SPRITE] == 0);
        CHECK(api_profile.calls[P8P_API_GRAPHICS] == 0);
        CHECK(api_profile.calls[P8P_API_MEMORY] == 0);
        CHECK(api_profile.calls[P8P_API_DRAW_STATE] == 0);
        CHECK(api_profile.calls[P8P_API_TEXT] == 0);
        p8p_runtime_set_profile_hook(runtime, NULL, NULL);
        framebuffer = p8p_runtime_framebuffer(runtime);
        CHECK(framebuffer != NULL);
        CHECK(framebuffer[3 * 128 + 1] == 8);
        CHECK(framebuffer[10 * 128 + 10] == 9);
        CHECK(framebuffer[12 * 128 + 12] == 9);
        CHECK(framebuffer[20 * 128 + 21] == 7);
        CHECK(framebuffer[28 * 128 + 30] == 10);
        CHECK(framebuffer[29 * 128 + 29] == 10);
        CHECK(framebuffer[29 * 128 + 28] == 0);
        CHECK(framebuffer[30 * 128 + 32] == 10);
        CHECK(framebuffer[50 * 128 + 50] == 1);
        CHECK(framebuffer[50 * 128 + 51] == 2);
        CHECK(framebuffer[40 * 128 + 40] == 11);
        CHECK(framebuffer[42 * 128 + 42] == 11);
        CHECK(framebuffer[43 * 128 + 42] == 0);
        CHECK(framebuffer[4 * 128 + 4] == 0);
        CHECK(framebuffer[4 * 128 + 5] == 12);
        CHECK(framebuffer[4 * 128 + 6] == 4);
        CHECK(framebuffer[4 * 128 + 7] == 3);
        CHECK(framebuffer[6 * 128 + 10] == 13);
        CHECK(framebuffer[70 * 128 + 72] == 10);
        CHECK(framebuffer[80 * 128 + 80] == 1);
        CHECK(framebuffer[80 * 128 + 81] == 1);
        CHECK(framebuffer[80 * 128 + 82] == 2);
        CHECK(framebuffer[2 * 128 + 90] == 7);
        CHECK(framebuffer[2 * 128 + 91] == 8);
        CHECK(framebuffer[2 * 128 + 92] == 9);
        CHECK(framebuffer[2 * 128 + 93] == 10);
        screen_palette = p8p_runtime_screen_palette(runtime);
        CHECK(screen_palette != NULL);
        CHECK(screen_palette[0] == 128);
        CHECK(screen_palette[1] == 9);
        CHECK(screen_palette[8] == 12);
        CHECK(screen_palette[15] == 139);
        memset(audio_samples, 0, sizeof(audio_samples));
        p8p_runtime_audio_render(runtime, audio_samples, 512);
        int audible_samples = 0;
        for (int sample = 0; sample < 512; ++sample) {
            CHECK(audio_samples[sample * 2] == audio_samples[sample * 2 + 1]);
            audible_samples += audio_samples[sample * 2] != 0;
        }
        CHECK(audible_samples > 400);

        CHECK(p8p_runtime_save_state(runtime, &saved_state,
                                     &saved_state_size) == 0);
        CHECK(saved_state != NULL);
        CHECK(saved_state_size > 65536);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_load_state(runtime, saved_state,
                                     saved_state_size) == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        framebuffer = p8p_runtime_framebuffer(runtime);
        CHECK(framebuffer[3 * 128 + 1] == 8);
        CHECK(framebuffer[3 * 128 + 2] == 0);
        free(saved_state);
        saved_state = NULL;

        p8p_cart_content_hash(&cart, &cart_hash);
        CHECK(p8p_store_write_config_file(store_path, config_pattern,
                                          sizeof(config_pattern)) == 0);
        CHECK(p8p_state_save_file(store_path, &cart_hash, runtime) == 0);
        memset(config_readback, 0, sizeof(config_readback));
        CHECK(p8p_store_read_config_file(store_path, config_readback,
                                         sizeof(config_readback)) == 0);
        CHECK(memcmp(config_pattern, config_readback,
                     sizeof(config_pattern)) == 0);
        CHECK(p8p_state_get_meta_file(store_path, &cart_hash,
                                      &state_meta) == 0);
        CHECK(state_meta.exists == 1);
        CHECK(state_meta.raw_size > 65536);
        CHECK(state_meta.compressed_size < state_meta.raw_size);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_state_load_file(store_path, &cart_hash, runtime) == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        framebuffer = p8p_runtime_framebuffer(runtime);
        CHECK(framebuffer[3 * 128 + 1] == 8);
        CHECK(framebuffer[3 * 128 + 2] == 0);
        CHECK(p8p_state_delete_file(store_path, &cart_hash) == 0);
        CHECK(p8p_state_get_meta_file(store_path, &cart_hash,
                                      &state_meta) == 0);
        CHECK(state_meta.exists == 0);

        {
            /* Native (Pocket) save states round-trip through one blob and
             * refuse corrupt data or another cart's state. */
            uint8_t *blob = NULL;
            uint32_t blob_size = 0;
            p8p_cart_hash_t other_hash = cart_hash;
            other_hash.bytes[0] ^= 0xffu;
            CHECK(p8p_state_serialize(&cart_hash, runtime, &blob,
                                      &blob_size) == 0);
            CHECK(blob != NULL);
            CHECK(blob_size > 40u && blob_size <= P8P_STATE_BLOB_MAX);
            CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
            CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
            /* Another cart's state is told apart from a damaged one, so
             * the player can be told why instead of a bare failure. */
            CHECK(p8p_state_deserialize(&other_hash, runtime, blob,
                                        blob_size) == P8P_STATE_OTHER_CART);
            CHECK(strcmp(p8p_state_load_notice(P8P_STATE_OTHER_CART),
                         "Memory is from another cart") == 0);
            CHECK(p8p_state_deserialize(&cart_hash, runtime, blob, 39u) != 0);
            CHECK(p8p_state_deserialize(&cart_hash, runtime, blob, 39u) !=
                  P8P_STATE_OTHER_CART);
            blob[blob_size - 1] ^= 0x5au;
            CHECK(p8p_state_deserialize(&cart_hash, runtime, blob,
                                        blob_size) != 0);
            CHECK(p8p_state_deserialize(&other_hash, runtime, blob,
                                        blob_size) != P8P_STATE_OTHER_CART);
            CHECK(p8p_state_load_notice(-6) == NULL);
            CHECK(p8p_state_load_notice(0) == NULL);
            blob[blob_size - 1] ^= 0x5au;
            CHECK(p8p_state_deserialize(&cart_hash, runtime, blob,
                                        blob_size) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            framebuffer = p8p_runtime_framebuffer(runtime);
            CHECK(framebuffer[3 * 128 + 1] == 8);
            CHECK(framebuffer[3 * 128 + 2] == 0);
            free(blob);
        }

        {
            /* A state whose raw size exceeds the 256 KiB slot (Moss Moss
             * saves ~500 KB) must still save when it compresses to fit. */
            static const char big_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "big={} for i=1,12000 do big[i]=\"item \"..i end\n"
                "function _draw() cls() if #big==12000 and big[777]==\"item 777\" then pset(0,0,7) end end\n";
            p8p_cart_t big_cart = {};
            p8p_cart_hash_t big_hash = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)big_source,
                                            sizeof(big_source) - 1,
                                            &big_cart) == 0);
            p8p_cart_content_hash(&big_cart, &big_hash);
            CHECK(p8p_runtime_load(runtime, &big_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_state_save_file(store_path, &big_hash, runtime) == 0);
            CHECK(p8p_state_get_meta_file(store_path, &big_hash,
                                          &state_meta) == 0);
            CHECK(state_meta.exists == 1);
            CHECK(state_meta.raw_size > 256u * 1024u);
            CHECK(p8p_runtime_load(runtime, &big_cart) == 0);
            CHECK(p8p_state_load_file(store_path, &big_hash, runtime) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
            CHECK(p8p_state_delete_file(store_path, &big_hash) == 0);
            p8p_cart_destroy(&big_cart);
        }

        loaded = p8p_runtime_load(runtime, &flip_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1 * 128 + 1] == 8);
        CHECK(p8p_runtime_step_with_draw(runtime, 0, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1 * 128 + 2] == 8);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1 * 128 + 3] == 8);

        loaded = p8p_runtime_load(runtime, &glyph_cart);
        CHECK(loaded == 0);
        for (int button = 0; button < 7; ++button) {
            CHECK(p8p_runtime_step(runtime, (uint8_t)(1u << button)) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[button] == 7);
        }
        CHECK(p8p_runtime_step_with_draw(runtime, 1u << 0, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[6] == 7);
        CHECK(p8p_runtime_framebuffer(runtime)[0] == 0);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        CHECK(p8p_runtime_framebuffer(runtime)[6] == 0);

        loaded = p8p_runtime_load(runtime, &nested_short_if_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[6] == 7);

        loaded = p8p_runtime_load(runtime, &live_input_cart);
        CHECK(loaded == 0);
        service_hook_runtime = runtime;
        inject_pause = 1;
        service_hook_calls = 0;
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(service_hook_calls > 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        inject_pause = 0;
        service_hook_runtime = NULL;

        loaded = p8p_runtime_load(runtime, &runaway_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);

        loaded = p8p_runtime_load(runtime, &run_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);

        loaded = p8p_runtime_load(runtime, &env_fallback_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[2] == 7);

        loaded = p8p_runtime_load(runtime, &memory_limits_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[3] == 7);
        CHECK(p8p_runtime_framebuffer(runtime)[4] == 7);

        loaded = p8p_runtime_load(runtime, &table_paths_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[128 + 8] == 7);

        loaded = p8p_runtime_load(runtime, &text_engine_cart);
        if (loaded != 0)
            fprintf(stderr, "text engine load: %s\n", p8p_runtime_error(runtime));
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        for (int check = 1; check <= 19; ++check) {
            if (p8p_runtime_framebuffer(runtime)[2 * 128 + check] != 7)
                fprintf(stderr, "text engine check %d failed\n", check);
            CHECK(p8p_runtime_framebuffer(runtime)[2 * 128 + check] == 7);
        }

        service_hook_runtime = runtime;
        loaded = p8p_runtime_load(runtime, &tap_cart);
        CHECK(loaded == 0);
        inject_live = 0;
        CHECK(p8p_runtime_step(runtime, 1u << 4) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);

        loaded = p8p_runtime_load(runtime, &late_press_cart);
        CHECK(loaded == 0);
        inject_live = 1u << 4;
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
        CHECK(p8p_runtime_step(runtime, 1u << 4) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        inject_live = -1;
        service_hook_runtime = NULL;

        loaded = p8p_runtime_load(runtime, &no_repeat_cart);
        CHECK(loaded == 0);
        for (int frame = 0; frame < 40; ++frame)
            CHECK(p8p_runtime_step(runtime, 1u << 4) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);

        {
            int plain = audio_crossings_for(runtime, "sfx(2)");
            uint32_t plain_hash = last_audio_hash;
            int custom = audio_crossings_for(runtime, "sfx(1)");
            int octave = audio_crossings_for(runtime, "sfx(4)");
            int detuned = audio_crossings_for(runtime, "sfx(5)");
            uint32_t detuned_hash = last_audio_hash;
            fprintf(stderr, "audio crossings: plain=%d custom=%d octave=%d "
                    "detuned=%d\n", plain, custom, octave, detuned);
            /* One note at speed 16 lasts ~0.13 s; C-3 (~523 Hz) gives
             * ~139 crossings.  The custom instrument plays C-2 transposed by
             * the parent's C-3, so it matches; an instrument note an octave
             * higher doubles it.  Detune must change the waveform. */
            CHECK(plain > 125 && plain < 150);
            CHECK(custom > plain - 4 && custom < plain + 4);
            CHECK(octave > 2 * plain - 8 && octave < 2 * plain + 8);
            CHECK(detuned > 0 && detuned_hash != plain_hash);
        }

        {
            /* Saved states keep 48 kHz units while voices run at 24 kHz: a
             * looping note must continue at the same pitch after a save and
             * load (SFX 0, key 24 ~262 Hz: ~105 crossings in 0.2 s). */
            audio_crossings_for(runtime, "sfx(0)");
            void *audio_state = NULL;
            size_t audio_state_size = 0;
            CHECK(p8p_runtime_save_state(runtime, &audio_state,
                                         &audio_state_size) == 0);
            int before = audio_crossings(runtime, 9600);
            CHECK(p8p_runtime_load_state(runtime, audio_state,
                                         audio_state_size) == 0);
            int after = audio_crossings(runtime, 9600);
            fprintf(stderr, "audio state round trip: %d -> %d crossings\n",
                    before, after);
            CHECK(before > 95 && before < 115);
            CHECK(after >= before - 2 && after <= before + 2);
            free(audio_state);
        }

        {
            /* With no wall clock, stat(80..85) and stat(90..95) still report a
             * valid date: month-table lookups and date arithmetic at cart load
             * must not hit nil or a zero month. */
            static const char date_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "day=stat(80)%100*365+split\"0,31,59\"[stat(81)]+stat(82)\n"
                "function _draw() cls()\n"
                "if day==25551 and stat(90)==stat(80) and stat(91)==stat(81)\n"
                "and stat(92)==stat(82) then pset(0,0,7) end\n"
                "if stat(83)+stat(84)+stat(85)+stat(93)+stat(94)+stat(95)==0\n"
                "then pset(1,0,7) end end\n";
            p8p_cart_t date_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)date_source,
                                            sizeof(date_source) - 1,
                                            &date_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &date_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
            p8p_cart_destroy(&date_cart);
        }

        {
            static const char rrect_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls() rrectfill(50,80,40,30,2,8)\n"
                "rrect(2,2,10,10,2,9) rrectfill(0,40,0,5,1,7) end\n";
            p8p_cart_t rrect_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)rrect_source,
                                            sizeof(rrect_source) - 1,
                                            &rrect_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &rrect_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            /* The manual's example: radius 2 leaves 3 pixels missing per corner. */
            CHECK(pixels[80 * 128 + 50] == 0);
            CHECK(pixels[80 * 128 + 51] == 0);
            CHECK(pixels[80 * 128 + 52] == 8);
            CHECK(pixels[81 * 128 + 50] == 0);
            CHECK(pixels[81 * 128 + 51] == 8);
            CHECK(pixels[82 * 128 + 50] == 8);
            CHECK(pixels[95 * 128 + 70] == 8);
            CHECK(pixels[109 * 128 + 89] == 0);
            CHECK(pixels[109 * 128 + 87] == 8);
            CHECK(pixels[110 * 128 + 70] == 0);
            /* The outline follows the same corner shape and leaves the inside. */
            CHECK(pixels[2 * 128 + 3] == 0);
            CHECK(pixels[2 * 128 + 4] == 9);
            CHECK(pixels[3 * 128 + 3] == 9);
            CHECK(pixels[4 * 128 + 2] == 9);
            CHECK(pixels[4 * 128 + 3] == 0);
            CHECK(pixels[6 * 128 + 6] == 0);
            CHECK(pixels[6 * 128 + 11] == 9);
            CHECK(pixels[11 * 128 + 6] == 9);
            /* Zero width draws nothing. */
            CHECK(pixels[42 * 128 + 0] == 0);
            p8p_cart_destroy(&rrect_cart);
        }

        {
            /* Pixels measured from PICO-8 0.2.7: each corner follows PICO-8's
             * own circ() of radius r + 1, and the radius is limited to half
             * the shorter side minus one. */
            static const char corner_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls() rrectfill(0,0,24,12,4,7)\n"
                "rrectfill(30,0,24,12,5,7) rrectfill(60,0,24,18,8,7)\n"
                "rrectfill(90,0,4,10,5,7) rrectfill(100,0,10,3,1,7)\n"
                "rrectfill(0,20,6,6,3,7) rrect(30,20,24,12,5,7) end\n";
            p8p_cart_t corner_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)corner_source,
                                            sizeof(corner_source) - 1,
                                            &corner_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &corner_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            const auto first_lit = [&](int left, int top, int row) {
                int inset = 0;
                while (inset < 24 && !pixels[(top + row) * 128 + left + inset])
                    ++inset;
                return inset;
            };
            static const int radius_4[] = {3, 2, 1, 0};
            for (int row = 0; row < 4; ++row)
                CHECK(first_lit(0, 0, row) == radius_4[row]);
            static const int radius_5[] = {4, 3, 2, 1, 0};
            for (int row = 0; row < 5; ++row)
                CHECK(first_lit(30, 0, row) == radius_5[row]);
            static const int radius_8[] = {7, 5, 3, 2, 2, 1, 1, 0};
            for (int row = 0; row < 8; ++row)
                CHECK(first_lit(60, 0, row) == radius_8[row]);
            /* Width 4 limits radius 5 to 1, and height 3 removes rounding. */
            CHECK(first_lit(90, 0, 0) == 1);
            CHECK(first_lit(90, 0, 1) == 0);
            CHECK(first_lit(100, 0, 0) == 0);
            static const int six_by_six[] = {2, 1, 0};
            for (int row = 0; row < 3; ++row)
                CHECK(first_lit(0, 20, row) == six_by_six[row]);
            /* The outline keeps the one-pixel diagonal of radius 5. */
            CHECK(pixels[21 * 128 + 33] == 7);
            CHECK(pixels[21 * 128 + 34] == 0);
            CHECK(pixels[21 * 128 + 35] == 0);
            p8p_cart_destroy(&corner_cart);
        }

        {
            /* pal(p) resets one palette and leaves the other two alone. */
            static const char pal_reset_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls() palt(5,true)\n"
                "pal(8,9) pal(3,4,1) pal(0) pset(0,0,8) pset(1,0,5)\n"
                "pal(7,6,1) pal(8,9) pal(1) pset(2,0,8)\n"
                "if pget(0,0)==8 then pset(3,0,7) end\n"
                "if peek(0x5f13)==3 then pset(4,0,7) end\n"
                "if peek(0x5f05)&16==16 then pset(5,0,7) end\n"
                "pal(0) end\n";
            p8p_cart_t pal_reset_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)pal_reset_source,
                                            sizeof(pal_reset_source) - 1,
                                            &pal_reset_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &pal_reset_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            CHECK(pixels[0] == 8);
            /* pal(1) kept the remapped draw palette. */
            CHECK(pixels[2] == 9);
            CHECK(pixels[3] == 7);
            CHECK(pixels[4] == 7);
            /* pal(0) resets transparency along with the draw palette, which
             * PICO-8 0.2.7 does although the manual does not mention it. */
            CHECK(pixels[5] == 0);
            CHECK(p8p_runtime_screen_palette(runtime)[3] == 3);
            CHECK(p8p_runtime_screen_palette(runtime)[7] == 7);
            p8p_cart_destroy(&pal_reset_cart);
        }

        {
            /* palt() returns the previous state, as in PICO-8 0.2.7: a boolean
             * for one colour, and otherwise the old bitfield. */
            static const char source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls() palt()\n"
                "a=palt(5,true) b=palt(5,false)\n"
                "if a==false and b==true then pset(0,0,7) end\n"
                "palt(0,false) palt(5,true)\n"
                "if palt(0x1234)==1024 then pset(1,0,7) end\n"
                "palt(0,false) palt(5,true)\n"
                "if palt()==5684 then pset(2,0,7) end\n"
                "palt(0xffff)\n"
                "if palt()==-1 then pset(3,0,7) end\n"
                "palt() end\n";
            p8p_cart_t cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)source,
                                            sizeof(source) - 1, &cart) == 0);
            CHECK(p8p_runtime_load(runtime, &cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            for (int x = 0; x < 4; ++x)
                CHECK(pixels[x] == 7);
            p8p_cart_destroy(&cart);
        }

        {
            /* An explicit nil colour maps to 0, as in PICO-8 0.2.7. */
            static const char source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls()\n"
                "pal(8,9) r=pal(8,nil)\n"
                "if r==9 and peek(0x5f08)==0 then pset(0,0,7) end\n"
                "pal() pal(8,9,1) pal(8,nil,1)\n"
                "if peek(0x5f18)==0 then pset(1,0,7) end\n"
                "pal() end\n";
            p8p_cart_t cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)source,
                                            sizeof(source) - 1, &cart) == 0);
            CHECK(p8p_runtime_load(runtime, &cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            CHECK(pixels[0] == 7);
            CHECK(pixels[1] == 7);
            p8p_cart_destroy(&cart);
        }

        {
            /* pal(-1), and pal() given one value that is not a number, reset
             * both palettes and transparency, as PICO-8 0.2.7 does. */
            static const char source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls()\n"
                "pal(8,9) pal(3,4,1) palt(5,true) pal(-1)\n"
                "if peek(0x5f08)==8 then pset(0,0,7) end\n"
                "if peek(0x5f13)==3 then pset(1,0,7) end\n"
                "if peek(0x5f05)==5 then pset(2,0,7) end\n"
                "pal(8,9) pal(3,4,1) pal(\"1\")\n"
                "if peek(0x5f08)==8 and peek(0x5f13)==3 then pset(3,0,7) end\n"
                "pal(8,9) pal(3,4,1) pal(-2)\n"
                "if peek(0x5f08)==9 and peek(0x5f13)==4 then pset(4,0,7) end\n"
                "pal() end\n";
            p8p_cart_t cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)source,
                                            sizeof(source) - 1, &cart) == 0);
            CHECK(p8p_runtime_load(runtime, &cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            for (int x = 0; x < 5; ++x)
                CHECK(pixels[x] == 7);
            p8p_cart_destroy(&cart);
        }

        {
            /* spr() sizes are in tiles but may be fractional: w and h set
             * the size in pixels as w*8 and h*8. */
            static const char fractional_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls()\n"
                "for y=0,15 do for x=0,15 do sset(8+x,y,7) end end\n"
                "sset(8,0,9)\n"
                "spr(1,10,10,.875,1)\n"
                "spr(1,30,10,.5,.5)\n"
                "spr(1,50,10,.5,.5,true)\n"
                "spr(1,70,10,1.5,1)\n"
                "end\n";
            p8p_cart_t fractional_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)fractional_source,
                                            sizeof(fractional_source) - 1,
                                            &fractional_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &fractional_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            CHECK(pixels[10 * 128 + 10] == 9);
            CHECK(pixels[17 * 128 + 16] == 7);
            CHECK(pixels[17 * 128 + 17] == 0);
            CHECK(pixels[13 * 128 + 33] == 7);
            CHECK(pixels[13 * 128 + 34] == 0);
            CHECK(pixels[14 * 128 + 33] == 0);
            /* Flipping mirrors within the fractional width. */
            CHECK(pixels[10 * 128 + 50] == 7);
            CHECK(pixels[10 * 128 + 53] == 9);
            CHECK(pixels[17 * 128 + 81] == 7);
            CHECK(pixels[17 * 128 + 82] == 0);
            p8p_cart_destroy(&fractional_cart);
        }

        {
            /* PICO-8 0.2.7 applies the 1x1 default only with three arguments
             * or fewer: once w is passed, a missing or nil size is 0. */
            static const char size_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls() sset(8,0,7)\n"
                "spr(1,0,0) spr(1,10,0,1) spr(1,20,0,nil,nil,true)\n"
                "spr(1,30,0,1,nil) spr(1,40,0,nil,1) spr(1,50,0,1,1) end\n";
            p8p_cart_t size_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)size_source,
                                            sizeof(size_source) - 1,
                                            &size_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &size_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *pixels = p8p_runtime_framebuffer(runtime);
            CHECK(pixels[0] == 7);
            CHECK(pixels[10] == 0);
            CHECK(pixels[27] == 0);
            CHECK(pixels[30] == 0);
            CHECK(pixels[40] == 0);
            CHECK(pixels[50] == 7);
            p8p_cart_destroy(&size_cart);
        }

        {
            /* stat(26) counts 183/22050 s ticks into the music pattern:
             * 9600 samples at 48 kHz are 24 ticks. */
            audio_crossings_for(runtime,
                                "music(0) end function _update() "
                                "t=stat(26) end function _draw() "
                                "if t==24 then pset(0,0,7) end");
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
        }

        {
            /* stat(57) is a boolean: true while music plays, including a
             * fade-out, and false once it stops (PICO-8 0.2.7). */
            static const char music_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "r={} a={0,-1,0,-1} f={0,0,0,500}\n"
                "function _update() add(r,stat(57)) local n=#r\n"
                "if a[n] then music(a[n],f[n]) end end\n"
                "function _draw() cls() for i,v in pairs(r) do\n"
                "pset(i,0,v==true and 7 or v==false and 8 or 1) end end\n"
                "__sfx__\n"
                "00100000183501835018350183501835018350183501835018350183501835018350183501835018350"
                "1835018350183501835018350183501835018350183501835018350183501835018350183501835018350\n"
                "__music__\n"
                "00 00424344\n";
            p8p_cart_t music_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)music_source,
                                            sizeof(music_source) - 1,
                                            &music_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &music_cart) == 0);
            for (int frame = 0; frame < 5; ++frame)
                CHECK(p8p_runtime_step(runtime, 0) == 0);
            const uint8_t *fb = p8p_runtime_framebuffer(runtime);
            /* Stopped, playing, stopped, playing, fading out. */
            CHECK(fb[1] == 8);
            CHECK(fb[2] == 7);
            CHECK(fb[3] == 8);
            CHECK(fb[4] == 7);
            CHECK(fb[5] == 7);
            p8p_cart_destroy(&music_cart);
        }

        {
            /* Muted output skips synthesis but must keep time: 9600 skipped
             * frames advance the music by the same 24 ticks as rendering. */
            audio_crossings_for(runtime,
                                "music(0) end function _update() "
                                "t=stat(26) end function _draw() "
                                "if t==48 then pset(0,0,7) end");
            p8p_runtime_audio_skip(runtime, 9600);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
        }

        {
            /* serial(0x808) queues 8-bit PCM; stat(108) counts what is left
             * and rendering drains it at 5512.5 Hz (100 samples ~ 871). */
            static const char pcm_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "q=0 function _init() memset(0x4300,200,100)\n"
                "n=serial(0x808,0x4300,100) q=stat(108) end\n"
                "function _draw() cls() if n==100 and q==100 then pset(0,0,7) end\n"
                "if stat(108)==0 then pset(1,0,7) end end\n";
            p8p_cart_t pcm_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)pcm_source,
                                            sizeof(pcm_source) - 1,
                                            &pcm_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &pcm_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 0);
            audio_crossings(runtime, 1000);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
            /* Muted, the queue drains at the same rate. */
            CHECK(p8p_runtime_load(runtime, &pcm_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 0);
            p8p_runtime_audio_skip(runtime, 869);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 0);
            p8p_runtime_audio_skip(runtime, 131);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
            p8p_cart_destroy(&pcm_cart);
        }

        {
            /* exit() is not PICO-8 API, but Yarn calls it while drawing its
             * score panel; it must be a harmless no-op. */
            static const char exit_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function _draw() cls() for i=1,2 do exit() end pset(0,0,7) end\n";
            p8p_cart_t exit_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)exit_source,
                                            sizeof(exit_source) - 1,
                                            &exit_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &exit_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 7);
            p8p_cart_destroy(&exit_cart);
        }

        {
            /* flip() inside _update ends the frame; the loop sees the next
             * step's input, as carts like Explorers expect. */
            static const char flip_loop_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "n=0 m=0 function _update60()\n"
                "if btn(5) then n+=1 while btn(5) do flip() end m+=1 end end\n"
                "function _draw() cls() pset(n,0,7) pset(m,1,7) end\n";
            p8p_cart_t flip_loop_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)flip_loop_source,
                                            sizeof(flip_loop_source) - 1,
                                            &flip_loop_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &flip_loop_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 1u << 5) == 0);
            CHECK(p8p_runtime_step(runtime, 1u << 5) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
            CHECK(p8p_runtime_framebuffer(runtime)[128 + 1] == 7);
            p8p_cart_destroy(&flip_loop_cart);
        }

        {
            /* Loading a state discards a frame suspended in flip(): the
             * resumed frame would see t == 1 against the restored a == 1. */
            static const char flip_state_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "a=0 function _update()\n"
                "local t=a a+=1 flip() if t~=a-1 then pset(5,5,8) end end\n"
                "function _draw() end\n";
            p8p_cart_t flip_state_cart = {};
            void *state = NULL;
            size_t state_size = 0;
            CHECK(p8p_cart_load_text_memory((const uint8_t *)flip_state_source,
                                            sizeof(flip_state_source) - 1,
                                            &flip_state_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &flip_state_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_save_state(runtime, &state, &state_size) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_load_state(runtime, state, state_size) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[5 * 128 + 5] != 8);
            free(state);
            p8p_cart_destroy(&flip_state_cart);
        }

        {
            /* all(): deleting the current or other items and adding during
             * iteration, empty and non-table arguments. */
            static const char all_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function run(t,f) local s=\"\" for v in all(t) do f(t,v) s..=v end return s end\n"
                "function _draw() cls()\n"
                "if run({1,2,3,4},function(t,v) if v==2 then del(t,v) end end)==\"1234\" then pset(0,0,7) end\n"
                "if run({1,2,3},function(t,v) del(t,v) end)==\"123\" then pset(1,0,7) end\n"
                "if run({1,2},function(t,v) if v==1 then add(t,9) end end)==\"129\" then pset(2,0,7) end\n"
                "if run({5,6,7},function(t,v) if v==5 then deli(t,3) end end)==\"56\" then pset(3,0,7) end\n"
                "local n=0 for v in all({}) do n+=1 end for v in all(nil) do n+=1 end\n"
                "if n==0 then pset(4,0,7) end\n"
                "local t={} for i=1,20 do t['key'..i]=i end t[1]=1 t[2]=2 if run(t,function() end)=='12' then pset(5,0,7) end\n"
                "if run({1,2,3,4,5,6,7,8},function(t,v) if v==5 then t[4]=nil t[8]=nil end end)=='1234567' then pset(6,0,7) end\n"
                "local t={1,2} local sum=0 for v in all(t) do if v==1 then for i=3,128 do t[i]=i end collectgarbage() end sum+=v end if sum==8256 then pset(7,0,7) end\n"
                "local a={} local t={a,false,3,'x'} local n=0 for v in all(t) do n+=1 if n==1 then del(t,a) collectgarbage() end end if n==4 and #t==3 then pset(8,0,7) end\n"
                "local traps=0 local t=setmetatable({1,2},{__len=function() traps+=1 return 100 end,__index=function() traps+=1 end}) if run(t,function() end)=='12' and traps==0 then pset(9,0,7) end\n"
                "local t={[1]=1,[2]=2,[3]=3} if run(t,function() end)=='123' then pset(10,0,7) end\n"
                "local a={n=1} local b={n=2} local s=0 for v in all({a,a,b}) do s=s*10+v.n end if s==112 then pset(11,0,7) end\n"
                "local a={n=1} local t={a,{n=2}} local n=0 local s=0 for v in all(t) do n+=1 s+=v.n if n==1 then del(t,a) for i=3,64 do add(t,{n=i}) end collectgarbage() end end if n==64 and s==2080 then pset(12,0,7) end\n"
                "local n=0 local s=0 for v in all({{n=1},{n=2},false,7}) do n+=1 if type(v)=='table' then s+=v.n else s+=1 end end if n==4 and s==5 then pset(13,0,7) end end\n";
            p8p_cart_t all_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)all_source,
                                            sizeof(all_source) - 1,
                                            &all_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &all_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            for (int check = 0; check < 14; ++check) {
                if (p8p_runtime_framebuffer(runtime)[check] != 7)
                    fprintf(stderr, "all() check %d failed\n", check);
                CHECK(p8p_runtime_framebuffer(runtime)[check] == 7);
            }
            p8p_cart_destroy(&all_cart);
        }

        {
            /* foreach(): callbacks can resize either Lua stack or table,
             * mutate traversal, collect, recurse, or raise an error. */
            static const char foreach_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "function walk(t,f) local s='' foreach(t,function(v) s..=tostr(v) f(t,v) end) return s end\n"
                "function _draw() cls()\n"
                "if walk({1,2,3,4},function(t,v) if v==2 then del(t,v) end end)=='1234' then pset(0,0,7) end\n"
                "if walk({1,2,3},function(t,v) del(t,v) end)=='123' then pset(1,0,7) end\n"
                "if walk({1,2},function(t,v) if v==1 then add(t,9) end end)=='129' then pset(2,0,7) end\n"
                "if walk({1,2,3},function(t,v) if v==1 then deli(t,3) end end)=='12' then pset(3,0,7) end\n"
                "if walk({1,2},function(t,v) if v==1 then t[1]=9 end end)=='192' then pset(4,0,7) end\n"
                "local once=false if walk({2,2,3},function(t,v) if not once then deli(t,1) once=true end end)=='23' then pset(5,0,7) end\n"
                "local t={1,2,3,4,5,6,7,8} if walk(t,function(t,v) if v==5 then t[4]=nil t[8]=nil end end)=='12345' then pset(6,0,7) end\n"
                "local s=0 foreach({1,2},function(v) foreach({3,4},function(w) s+=v*w return 9,8,7 end) end) if s==21 then pset(7,0,7) end\n"
                "local function grow(n) if n==0 then collectgarbage() return 1 end return 1+grow(n-1) end\n"
                "local t={1,2,3} local s=0 foreach(t,function(v) if v==1 then for i=4,512 do t[i]=i end for i=512,4,-1 do t[i]=nil end end s+=v+grow(80) end) if s==249 then pset(8,0,7) end\n"
                "local t={{n=1},{n=2}} local s=0 foreach(t,function(v) del(t,v) collectgarbage() s+=v.n end) if s==3 and #t==0 then pset(9,0,7) end\n"
                "local traps=0 local t=setmetatable({1,2},{__len=function() traps+=1 return 100 end,__index=function() traps+=1 end}) local s=0 foreach(t,function(v) s+=v end) if s==3 and traps==0 then pset(10,0,7) end\n"
                "local ok,err=pcall(function() foreach({1},function() error('foreach-boom') end) end) local s=0 foreach({1,2},function(v) s+=v end) if not ok and string.find(err,'foreach-boom',1,true) and s==3 then pset(11,0,7) end\n"
                "local co=cocreate(function() foreach({1},function() yield() end) end) local ok,err=coresume(co) if not ok and string.find(err,'yield') then pset(12,0,7) end\n"
                "local calls=0 local f=setmetatable({},{__call=function() calls+=1 end}) foreach(nil,function() calls+=1 end) foreach({},function() calls+=1 end) foreach({1},f) if calls==0 then pset(13,0,7) end\n"
                "local traps=0 local mt={__eq=function() traps+=1 return true end} local a=setmetatable({},mt) local b=setmetatable({},mt) local t={a} local n=0 foreach(t,function(v) n+=1 if n==1 then t[1]=b end end) if n==2 and traps==0 then pset(14,0,7) end\n"
                "local t={} for i=1,20 do t['key'..i]=i end t[1]=1 t[2]=2 local n=0 foreach(t,function(v) n+=v end) if n==3 then pset(15,0,7) end\n"
                "local a={} local t={a,'same',3,true} local n=0 foreach(t,function(v) n+=1 if n==1 then t[1]=false elseif n==2 then t[1]=a end end) if n==6 then pset(16,0,7) end\n"
                "end\n";
            p8p_cart_t foreach_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)foreach_source,
                                            sizeof(foreach_source) - 1,
                                            &foreach_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &foreach_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            for (int check = 0; check < 17; ++check) {
                if (p8p_runtime_framebuffer(runtime)[check] != 7)
                    fprintf(stderr, "foreach() check %d failed: %s\n", check,
                            p8p_runtime_error(runtime));
                CHECK(p8p_runtime_framebuffer(runtime)[check] == 7);
            }
            p8p_cart_destroy(&foreach_cart);
        }

        {
            /* menuitem(): slots 1-5, removal, the button filter in bits 8-15,
             * O/X closing unless the callback returns true, and errors. */
            static const char menu_source[] =
                "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
                "s=0 last=-1\n"
                "function _init()\n"
                "menuitem(1,\"Music: on\",function(b) last=b if b&3>0 then s+=1 return true end menuitem(1,\"Music: off\") end)\n"
                "menuitem(2,\"gone\") menuitem(2)\n"
                "menuitem(3|0x100,\"left \x8e/\x97\",function(b) last=b end)\n"
                "menuitem(4,\"err\",function() error(\"boom\") end)\n"
                "menuitem(9,\"bad\")\n"
                "end\n"
                "function _draw() cls() pset(0,0,s) if last==112 then pset(1,0,7) end end\n";
            p8p_cart_t menu_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)menu_source,
                                            sizeof(menu_source) - 1,
                                            &menu_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &menu_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            const char *label = p8p_runtime_menu_item(runtime, 1);
            CHECK(label && strcmp(label, "Music: on") == 0);
            CHECK(p8p_runtime_menu_item(runtime, 2) == NULL);
            label = p8p_runtime_menu_item(runtime, 3);
            CHECK(label && strcmp(label, "left O/X") == 0);
            CHECK(p8p_runtime_menu_item(runtime, 0) == NULL);
            CHECK(p8p_runtime_menu_item(runtime, 6) == NULL);
            CHECK(p8p_runtime_menu_select(runtime, 1, 2) == 1);
            CHECK(p8p_runtime_menu_select(runtime, 3, 2) == 1);
            CHECK(p8p_runtime_menu_select(runtime, 1, 112) == 0);
            /* Relabelling from the callback keeps the callback. */
            label = p8p_runtime_menu_item(runtime, 1);
            CHECK(label && strcmp(label, "Music: off") == 0);
            CHECK(p8p_runtime_menu_select(runtime, 1, 2) == 1);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 2);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 0);
            CHECK(p8p_runtime_menu_select(runtime, 4, 112) == -1);
            CHECK(strstr(p8p_runtime_error(runtime), "boom") != NULL);

            /* The system menu lists cart entries after RESUME: right on
             * entry 1 keeps the menu open, A on it closes the menu. */
            CHECK(p8p_runtime_load(runtime, &menu_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            /* The menu reads state slots ("save:N") from the working
             * directory; keep them out of the source tree. */
            char menu_dir[] = "/tmp/pico8pocket-menu-XXXXXX";
            char previous_dir[1024];
            CHECK(getcwd(previous_dir, sizeof(previous_dir)) != NULL);
            CHECK(mkdtemp(menu_dir) != NULL && chdir(menu_dir) == 0);
            p8p_settings_t menu_settings;
            p8p_cart_hash_t menu_hash = {};
            p8p_settings_defaults(&menu_settings);
            p8p_menu_t *menu = p8p_menu_create(&menu_settings, &menu_hash, "P8");
            CHECK(menu != NULL);
            p8p_menu_open(menu, runtime, 0, 1);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_DOWN) == P8P_MENU_NONE);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_RIGHT) == P8P_MENU_NONE);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_A) == P8P_MENU_CLOSE);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[0] == 1);
            CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
            /* Entry 4 raises an error: two rows below entry 1. */
            p8p_menu_open(menu, runtime, 0, 1);
            for (int i = 0; i < 3; ++i)
                p8p_menu_update(menu, runtime, 0, P8P_PHYS_DOWN);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_A) == P8P_MENU_CART_ERROR);
            /* After a runtime error the cart entries are hidden. */
            p8p_menu_open(menu, runtime, 0, 0);
            p8p_menu_update(menu, runtime, 0, P8P_PHYS_DOWN);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_A) == P8P_MENU_NONE);
            /* X confirms like A, as both are PICO-8's X button in play:
             * it resumes, opens STATES (B then leaves it, and B again
             * closes the menu) and confirms a restart. */
            p8p_menu_open(menu, runtime, 0, 0);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_X) == P8P_MENU_CLOSE);
            p8p_menu_open(menu, runtime, 0, 0);
            for (int i = 0; i < 3; ++i)
                p8p_menu_update(menu, runtime, 0, P8P_PHYS_DOWN);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_X) == P8P_MENU_NONE);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_B) == P8P_MENU_NONE);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_B) == P8P_MENU_CLOSE);
            p8p_menu_open(menu, runtime, 0, 0);
            for (int i = 0; i < 4; ++i)
                p8p_menu_update(menu, runtime, 0, P8P_PHYS_DOWN);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_X) == P8P_MENU_NONE);
            CHECK(p8p_menu_update(menu, runtime, 0, P8P_PHYS_X) == P8P_MENU_RESTART);
            p8p_menu_destroy(menu);
            p8p_cart_destroy(&menu_cart);
            unlink("save:0");
            CHECK(chdir(previous_dir) == 0);
            rmdir(menu_dir);
        }

        {
            static const char *const celeste_paths[] = {
                "assets/cards/celeste.p8.png",
                "assets/cards/celeste_classic_2.p8.png",
            };
            int found = 0;
            for (size_t i = 0; i < sizeof(celeste_paths) / sizeof(celeste_paths[0]); ++i) {
                p8p_cart_t celeste_cart = {};
                if (p8p_cart_load_file(celeste_paths[i], &celeste_cart) != 0)
                    continue;
                ++found;
                CHECK(celeste_cart.lua_size > 20000);
                celeste_acceptance(runtime, celeste_paths[i], &celeste_cart);
                p8p_cart_destroy(&celeste_cart);
            }
            if (!found)
                puts("Celeste acceptance test skipped (no local cart in assets/cards)");
        }

        p8p_runtime_destroy(runtime);
    }
    p8p_cart_destroy(&cart);
    p8p_cart_destroy(&png_cart);
    p8p_cart_destroy(&legacy_cart);
    p8p_cart_destroy(&flip_cart);
    p8p_cart_destroy(&glyph_cart);
    p8p_cart_destroy(&nested_short_if_cart);
    p8p_cart_destroy(&live_input_cart);
    p8p_cart_destroy(&runaway_cart);
    p8p_cart_destroy(&run_cart);
    p8p_cart_destroy(&env_fallback_cart);
    p8p_cart_destroy(&memory_limits_cart);
    p8p_cart_destroy(&table_paths_cart);
    p8p_cart_destroy(&text_engine_cart);
    p8p_cart_destroy(&tap_cart);
    p8p_cart_destroy(&late_press_cart);
    p8p_cart_destroy(&no_repeat_cart);
    if (store_fd >= 0)
        unlink(store_path);

    if (failures) {
        fprintf(stderr, "%d runtime test(s) failed\n", failures);
        return 1;
    }
    puts("z8lua runtime smoke test passed");
    return 0;
}

#undef TEST_NOTES_32
#undef TEST_NOTES_4
