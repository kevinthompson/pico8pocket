/* Drive the real app loop with host platform hooks after an initial load
 * rejection. main.c is compiled separately as C with -Dmain=p8p_app_main. */
#include "p8p/cart.h"
#include "p8p/runtime.h"
extern "C" {
#include "p8p/platform.h"
int p8p_app_main(int argc, char **argv);
}

#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t screen[128 * 128];
static uint8_t first_frame[sizeof screen];
static unsigned presents;
static unsigned pause_calls;
static unsigned audio_calls;
static uint32_t now_us;
static jmp_buf app_exit;

static void capture(const uint8_t *pixels, const char *name) {
    const char *directory = getenv("P8P_APP_CAPTURE_DIR");
    if (!directory) return;
    static const uint8_t palette[16][3] = {
        {0,0,0}, {29,43,83}, {126,37,83}, {0,135,81},
        {171,82,54}, {95,87,79}, {194,195,199}, {255,241,232},
        {255,0,77}, {255,163,0}, {255,236,39}, {0,228,54},
        {41,173,255}, {131,118,156}, {255,119,168}, {255,204,170}
    };
    char path[PATH_MAX];
    assert(snprintf(path, sizeof path, "%s/%s.ppm", directory, name) > 0);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "P6\n128 128\n255\n") > 0);
    for (size_t i = 0; i < sizeof screen; ++i)
        assert(fwrite(palette[pixels[i] & 15], 1, 3, file) == 3);
    assert(fclose(file) == 0);
}

extern "C" {
int p8p_platform_init(void) { return 0; }
uint8_t *p8p_platform_framebuffer(void) { return screen; }
void p8p_platform_present(const uint8_t *pixels, const uint8_t *palette) {
    assert(!palette);
    if (presents == 0) {
        /* The actual error screen has a red border and visible text. */
        assert(pixels[0] == 8 && pixels[127 * 128 + 127] == 8);
        unsigned text_pixels = 0;
        for (size_t i = 0; i < sizeof screen; ++i)
            text_pixels += pixels[i] == 7;
        assert(text_pixels > 0);
        memcpy(first_frame, pixels, sizeof first_frame);
        capture(pixels, "load-error");
    } else if (presents == 1) {
        assert(pause_calls == 1);
        /* Opening the menu must also make it visible, not just pause audio. */
        assert(memcmp(first_frame, pixels, sizeof first_frame) != 0);
        assert(pixels[0] != 8);
        capture(pixels, "select-menu");
    } else if (presents == 5) {
        capture(pixels, "exit-confirmation");
    }
    if (++presents > 12) longjmp(app_exit, 2);
}
void p8p_platform_audio_pump(struct p8p_runtime *) { ++audio_calls; }
void p8p_platform_audio_set_paused(int paused) {
    if (paused) ++pause_calls;
}
void p8p_platform_audio_set_volume(unsigned) {}
unsigned p8p_platform_set_scale(unsigned scale) { return scale; }
uint8_t p8p_platform_dim_color(uint8_t color) { return color; }
void p8p_platform_poll_input(p8p_physical_input_t *input) {
    /* Select, release, Up wraps to EXIT, then confirm the exit dialog. */
    static const uint16_t route[] = {
        0, P8P_PHYS_SELECT, 0, P8P_PHYS_UP, 0, P8P_PHYS_A, 0,
        P8P_PHYS_A
    };
    memset(input, 0, sizeof *input);
    if (presents < sizeof route / sizeof route[0])
        input->held = input->pressed = route[presents];
}
uint16_t p8p_platform_poll_physical(void) { return 0; }
uint8_t p8p_platform_poll_buttons(void) { return 0; }
void p8p_platform_wait_frame(void) {}
uint32_t p8p_platform_time_us(void) { return ++now_us; }
void p8p_platform_sleep_us(uint32_t duration) { now_us += duration; }
void p8p_platform_exit(void) { longjmp(app_exit, 1); }
int p8p_platform_savestate_enable(uint32_t) { return -1; }
int p8p_platform_savestate_set_id(const void *, uint32_t) { return -1; }
int p8p_platform_savestate_poll(void) { return P8P_SAVESTATE_NONE; }
int p8p_platform_savestate_save(const void *, uint32_t) { return -1; }
int p8p_platform_savestate_load(void *, uint32_t) { return -1; }
void p8p_platform_savestate_finish(int) {}
}

int main(int argc, char **argv) {
    char original[PATH_MAX];
    char cart_path[PATH_MAX];
    char temporary[] = "/tmp/p8p-app-load-XXXXXX";
    assert(getcwd(original, sizeof original));
    if (argc > 1) assert(realpath(argv[1], cart_path));
    assert(mkdtemp(temporary));
    assert(chdir(temporary) == 0);
    if (argc <= 1) {
        assert(snprintf(cart_path, sizeof cart_path, "%s/error.p8", temporary) > 0);
        FILE *file = fopen(cart_path, "wb");
        assert(file);
        assert(fputs("pico-8 cartridge // http://www.pico-8.com\nversion 42\n"
                     "__lua__\nfunction _init() error('load-test') end\n", file) >= 0);
        assert(fclose(file) == 0);
    }
    /* Prove the fixture reaches initial load rejection, not a later step. */
    p8p_cart_t cart = {};
    assert(p8p_cart_load_file(cart_path, &cart) == 0);
    p8p_runtime_t *runtime = p8p_runtime_create();
    assert(runtime);
    assert(p8p_runtime_load(runtime, &cart) != 0);
    assert(p8p_runtime_error(runtime)[0]);
    p8p_runtime_destroy(runtime);
    p8p_cart_destroy(&cart);

    char executable[] = "app-load-test";
    char *app_argv[] = {executable, cart_path, NULL};
    int outcome = setjmp(app_exit);
    if (outcome == 0) {
        p8p_app_main(2, app_argv);
        assert(!"app loop returned before exit");
    }
    assert(outcome == 1);
    assert(presents == 7 && pause_calls == 1 && audio_calls == 0);
    assert(chdir(original) == 0);
    if (argc <= 1) assert(unlink(cart_path) == 0);
    assert(rmdir(temporary) == 0);
    puts("PASS initial load rejection shows error, Select menu and confirmed exit");
    return 0;
}
