#include "p8p/audio.h"
#include "p8p/cart.h"
#include "p8p/menu.h"
#include "p8p/platform.h"
#include "p8p/runtime.h"
#include "p8p/state_store.h"
#include "miniz.h"

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

int main(void) {
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
        for (int check = 1; check <= 18; ++check) {
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
                "if run({1,2,3,4,5,6,7,8},function(t,v) if v==5 then t[4]=nil t[8]=nil end end)=='1234567' then pset(6,0,7) end end\n";
            p8p_cart_t all_cart = {};
            CHECK(p8p_cart_load_text_memory((const uint8_t *)all_source,
                                            sizeof(all_source) - 1,
                                            &all_cart) == 0);
            CHECK(p8p_runtime_load(runtime, &all_cart) == 0);
            CHECK(p8p_runtime_step(runtime, 0) == 0);
            for (int check = 0; check < 7; ++check) {
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
