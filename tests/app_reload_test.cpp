/* The Pocket swaps the file behind the cart slot while the app runs, without
 * restarting the core.  Drive the production app loop, replace the cart file
 * under it and check that the new cart runs with its own Memory identity, and
 * that an unreadable replacement shows the error screen. */
extern "C" {
#include "p8p/platform.h"
int p8p_app_main(int argc, char **argv);
}
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <setjmp.h>
#include <string>
#include <unistd.h>

static uint8_t screen[128 * 128];
static uint8_t identities[2][16];
static unsigned identity_count, presents;
static uint32_t now_us;
static jmp_buf done;
static char cart_path[PATH_MAX];
static int stage;
static unsigned stage_started;

static const char header[] =
    "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n";

static void write_cart(const std::string &text) {
    FILE *file = fopen(cart_path, "wb");
    assert(file);
    assert(fwrite(text.data(), 1, text.size(), file) == text.size());
    assert(fclose(file) == 0);
}

extern "C" {
int p8p_platform_init(void) { return 0; }
uint8_t *p8p_platform_framebuffer(void) { return screen; }
void p8p_platform_present(const uint8_t *pixels, const uint8_t *) {
    ++presents;
    if (stage == 0 && presents == 3) {
        assert(pixels[0] == 3);
        write_cart(std::string(header) +
                   "-- a longer replacement cart\n"
                   "function _draw() cls(12) end\n");
        stage = 1;
        stage_started = presents;
    } else if (stage == 1 && pixels[0] == 12) {
        assert(identity_count == 2);
        assert(memcmp(identities[0], identities[1], 16) != 0);
        write_cart("not a cartridge");
        stage = 2;
        stage_started = presents;
    } else if (stage == 2 && pixels[0] == 8 && pixels[127 * 128 + 127] == 8) {
        longjmp(done, 1);
    }
    /* Each change must show within about two seconds of frames. */
    assert(presents - stage_started < 150);
}
void p8p_platform_audio_pump(struct p8p_runtime *) {}
void p8p_platform_audio_set_paused(int) {}
void p8p_platform_audio_set_volume(unsigned) {}
unsigned p8p_platform_set_scale(unsigned scale) { return scale; }
uint8_t p8p_platform_dim_color(uint8_t color) { return color; }
void p8p_platform_poll_input(p8p_physical_input_t *input) {
    memset(input, 0, sizeof(*input));
}
uint16_t p8p_platform_poll_physical(void) { return 0; }
uint8_t p8p_platform_poll_buttons(void) { return 0; }
void p8p_platform_wait_frame(void) {}
uint32_t p8p_platform_time_us(void) { return now_us += 100; }
void p8p_platform_sleep_us(uint32_t value) { now_us += value; }
void p8p_platform_exit(void) { longjmp(done, 2); }
int p8p_platform_savestate_enable(uint32_t) { return 0; }
int p8p_platform_savestate_set_id(const void *id, uint32_t size) {
    assert(size == 16 && identity_count < 2);
    memcpy(identities[identity_count++], id, 16);
    return 0;
}
int p8p_platform_savestate_poll(void) { return P8P_SAVESTATE_NONE; }
int p8p_platform_savestate_save(const void *, uint32_t) { return -1; }
int p8p_platform_savestate_load(void *, uint32_t) { return -1; }
void p8p_platform_savestate_finish(int) {}
}

int main(void) {
    char original[PATH_MAX];
    char temporary[] = "/tmp/p8p-app-reload-XXXXXX";
    assert(getcwd(original, sizeof(original)));
    assert(mkdtemp(temporary));
    assert(chdir(temporary) == 0);
    assert(snprintf(cart_path, sizeof(cart_path), "%s/slot.p8", temporary) > 0);
    write_cart(std::string(header) + "function _draw() cls(3) end\n");
    char app[] = "app-reload-test";
    char *app_argv[] = {app, cart_path, NULL};
    int outcome = setjmp(done);
    if (!outcome) {
        p8p_app_main(2, app_argv);
        assert(!"app loop returned");
    }
    assert(outcome == 1);
    assert(unlink(cart_path) == 0);
    assert(chdir(original) == 0);
    assert(rmdir(temporary) == 0);
    puts("app cart slot replacement, new Memory identity and unreadable cart passed");
    return 0;
}
