/* Drive the real app loop on a virtual clock to show which button timelines
 * reach btnp().  Audio and presentation take simulated time, as they do on a
 * slow frame.  main.c is compiled separately with -Dmain=p8p_app_main. */
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

typedef struct press {
    uint32_t start_us;
    uint32_t end_us;
} press_t;

enum { MAX_PRESSES = 64 };

static press_t presses[MAX_PRESSES];
static unsigned press_count;
static uint32_t audio_cost_us;
static uint32_t present_cost_us;
static uint32_t run_until_us;
static uint32_t now_us;
static uint32_t time_per_read_us;
static int counted_presses;
/* The Pocket's audio queue on the virtual clock: 48 frames drain per ms. */
static uint32_t queue_frames, queue_checked_us;
static int queue_started, queue_ran_dry;
static uint8_t screen[128 * 128];
static jmp_buf app_exit;

static void drain_audio_queue(void) {
    uint32_t drained = (now_us - queue_checked_us) * 48u / 1000u;
    queue_checked_us = now_us;
    if (queue_started && drained >= queue_frames)
        queue_ran_dry = 1;
    queue_frames = drained >= queue_frames ? 0 : queue_frames - drained;
}

static int button_is_down(void) {
    for (unsigned i = 0; i < press_count; ++i)
        if (now_us >= presses[i].start_us && now_us < presses[i].end_us)
            return 1;
    return 0;
}

extern "C" {
int p8p_platform_init(void) { return 0; }
uint8_t *p8p_platform_framebuffer(void) { return screen; }
void p8p_platform_present(const uint8_t *pixels, const uint8_t *) {
    now_us += present_cost_us;
    for (int x = 0; x < 128; ++x)
        if (pixels[x] == 7)
            counted_presses = x;
    if (now_us >= run_until_us)
        longjmp(app_exit, 1);
}
void p8p_platform_audio_pump(struct p8p_runtime *) {
    drain_audio_queue();
    now_us += audio_cost_us;
    drain_audio_queue();
    queue_frames = 2048;
    queue_started = 1;
}
int p8p_platform_audio_queued_frames(void) {
    drain_audio_queue();
    return (int)queue_frames;
}
void p8p_platform_audio_set_paused(int) {}
void p8p_platform_audio_set_volume(unsigned) {}
void p8p_platform_audio_debug(unsigned *, unsigned *, unsigned *, unsigned *) {}
unsigned p8p_platform_set_scale(unsigned scale) { return scale; }
uint8_t p8p_platform_dim_color(uint8_t color) { return color; }
void p8p_platform_poll_input(p8p_physical_input_t *input) {
    memset(input, 0, sizeof *input);
    if (button_is_down())
        input->held = P8P_PHYS_B;
}
uint16_t p8p_platform_poll_physical(void) { return 0; }
uint8_t p8p_platform_poll_buttons(void) { return 0; }
void p8p_platform_wait_frame(void) {}
uint32_t p8p_platform_time_us(void) { return now_us += time_per_read_us; }
void p8p_platform_sleep_us(uint32_t duration) { now_us += duration; }
void p8p_platform_exit(void) { longjmp(app_exit, 1); }
int p8p_platform_savestate_enable(uint32_t) { return -1; }
int p8p_platform_savestate_set_id(const void *, uint32_t) { return -1; }
int p8p_platform_savestate_poll(void) { return P8P_SAVESTATE_NONE; }
int p8p_platform_savestate_save(const void *, uint32_t) { return -1; }
int p8p_platform_savestate_load(void *, uint32_t) { return -1; }
void p8p_platform_savestate_finish(int) {}
}

static int count_presses(char *cart_path) {
    char executable[] = "app-input-test";
    char *app_argv[] = {executable, cart_path, NULL};
    now_us = 0;
    counted_presses = 0;
    queue_frames = queue_checked_us = 0;
    queue_started = queue_ran_dry = 0;
    if (setjmp(app_exit) == 0) {
        p8p_app_main(2, app_argv);
        assert(!"app loop returned before the timeline ended");
    }
    return counted_presses;
}

int main(void) {
    char temporary[] = "/tmp/p8p-app-input-XXXXXX";
    char cart_path[PATH_MAX];
    assert(mkdtemp(temporary));
    assert(snprintf(cart_path, sizeof cart_path, "%s/count.p8", temporary) > 0);
    FILE *file = fopen(cart_path, "wb");
    assert(file);
    assert(fputs("pico-8 cartridge // http://www.pico-8.com\nversion 42\n"
                 "__lua__\nr=0 function _update60()\n"
                 "if btnp(4) then r+=1 end end\n"
                 "function _draw() cls() pset(r,0,7) end\n", file) >= 0);
    assert(fclose(file) == 0);
    assert(chdir(temporary) == 0);
    time_per_read_us = 1;

    /* Taps of 20 ms at every phase of a frame whose audio and presentation
     * each take 14 ms: all of them are longer than any unpolled stretch. */
    audio_cost_us = present_cost_us = 14000;
    press_count = 40;
    for (unsigned i = 0; i < press_count; ++i) {
        presses[i].start_us = 100000u + i * 207000u;
        presses[i].end_us = presses[i].start_us + 20000u;
    }
    run_until_us = presses[press_count - 1].end_us + 200000u;
    int taps = count_presses(cart_path);
    if (taps != (int)press_count) {
        fprintf(stderr, "FAIL short taps: %d of %u reached btnp\n",
                taps, press_count);
        return 1;
    }

    /* Release and press again between two steps of a 74 ms frame, at every
     * phase.  Each gap is visible only to a poll made during the frame. */
    audio_cost_us = present_cost_us = 37000;
    press_count = 24;
    for (unsigned i = 0; i < press_count; i += 2) {
        uint32_t first_us = 100000u + i * 311000u;
        presses[i] = (press_t){first_us, first_us + 200000u};
        presses[i + 1] = (press_t){first_us + 240000u, first_us + 400000u};
    }
    run_until_us = presses[press_count - 1].end_us + 400000u;
    int repeats = count_presses(cart_path);
    if (repeats != (int)press_count) {
        fprintf(stderr, "FAIL quick re-press: %d of %u reached btnp\n",
                repeats, press_count);
        return 1;
    }

    /* A step that stays in Lua for about 100 ms, three times the time the
     * queue holds, must not let audio run dry.  Each clock read stands for
     * the Lua work between two service hooks. */
    file = fopen(cart_path, "wb");
    assert(file);
    assert(fputs("pico-8 cartridge // http://www.pico-8.com\nversion 42\n"
                 "__lua__\nfunction _update60()\n"
                 "for j=1,25 do for i=1,20000 do end end end\n"
                 "function _draw() end\n", file) >= 0);
    assert(fclose(file) == 0);
    audio_cost_us = 3000;
    present_cost_us = 3000;
    time_per_read_us = 400;
    press_count = 0;
    run_until_us = 1000000u;
    count_presses(cart_path);
    if (queue_ran_dry) {
        fprintf(stderr, "FAIL audio queue ran dry during a long Lua step\n");
        return 1;
    }

    assert(unlink(cart_path) == 0);
    assert(chdir("/") == 0);
    assert(rmdir(temporary) == 0);
    puts("PASS taps survive slow frames and long Lua steps keep audio fed");
    return 0;
}
