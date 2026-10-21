/* Exercise the production C app loop, including active Memory identity. */
#include "p8p/cart.h"
#include "miniz.h"
extern "C" {
#include "p8p/platform.h"
int p8p_app_main(int argc, char **argv);
}
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <setjmp.h>
#include <string>
#include <unistd.h>

static uint8_t screen[128 * 128];
static uint8_t identities[3][16];
static unsigned identity_count, presents, pauses;
static bool missing_target;
static uint32_t now_us;
static jmp_buf done;

extern "C" {
int p8p_platform_init(void) { return 0; }
uint8_t *p8p_platform_framebuffer(void) { return screen; }
void p8p_platform_present(const uint8_t *pixels, const uint8_t *) {
    if (presents == 0) assert(pixels[0] == 3);
    if (missing_target) {
        if (presents == 2) {
            assert(pixels[0] == 8 && pixels[127 * 128 + 127] == 8);
            assert(identity_count == 1);
        }
        if (presents == 3) { assert(pixels[0] != 8); assert(pauses >= 2); }
        if (++presents > 12) longjmp(done, 3);
        return;
    }
    if (presents == 2) { assert(pixels[0] == 7); assert(identity_count == 2); }
    if (presents == 3) { assert(pixels[0] != 7); assert(pauses > 0); }
    if (presents >= 8) { assert(pixels[0] == 3); assert(identity_count == 3); }
    if (++presents == 11) longjmp(done, 1);
}
void p8p_platform_audio_pump(struct p8p_runtime *) {}
void p8p_platform_audio_set_paused(int paused) { if (paused) ++pauses; }
void p8p_platform_audio_set_volume(unsigned) {}
unsigned p8p_platform_set_scale(unsigned s) { return s; }
uint8_t p8p_platform_dim_color(uint8_t c) { return c; }
void p8p_platform_poll_input(p8p_physical_input_t *input) {
    static const uint16_t route[] = {
        0, P8P_PHYS_A, 0, P8P_PHYS_SELECT, 0, P8P_PHYS_DOWN, 0,
        P8P_PHYS_A, P8P_PHYS_A, P8P_PHYS_A, 0
    };
    static const uint16_t missing_route[] = {
        0, P8P_PHYS_A, 0, P8P_PHYS_SELECT, 0, P8P_PHYS_UP, 0,
        P8P_PHYS_A, 0, P8P_PHYS_A
    };
    memset(input, 0, sizeof(*input));
    if (missing_target && presents < sizeof(missing_route) / sizeof(missing_route[0]))
        input->held = input->pressed = missing_route[presents];
    else if (!missing_target && presents < sizeof(route) / sizeof(route[0]))
        input->held = input->pressed = route[presents];
}
uint16_t p8p_platform_poll_physical(void) { return 0; }
uint8_t p8p_platform_poll_buttons(void) { return 0; }
void p8p_platform_wait_frame(void) {}
uint32_t p8p_platform_time_us(void) { return ++now_us; }
void p8p_platform_sleep_us(uint32_t value) { now_us += value; }
void p8p_platform_exit(void) { longjmp(done, 2); }
int p8p_platform_savestate_enable(uint32_t) { return -1; }
int p8p_platform_savestate_set_id(const void *id, uint32_t size) {
    assert(size == 16 && identity_count < 3);
    memcpy(identities[identity_count++], id, 16); return 0;
}
int p8p_platform_savestate_poll(void) { return P8P_SAVESTATE_NONE; }
int p8p_platform_savestate_save(const void *, uint32_t) { return -1; }
int p8p_platform_savestate_load(void *, uint32_t) { return -1; }
void p8p_platform_savestate_finish(int) {}
}

static void put32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

int main(int argc, char **argv) {
    missing_target = argc == 2 && strcmp(argv[1], "--missing") == 0;
    char original[PATH_MAX]; assert(getcwd(original, sizeof(original)));
    char temp[] = "/tmp/p8p-app-multicart-XXXXXX"; assert(mkdtemp(temp));
    assert(chdir(temp) == 0);
    static const char header[] = "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n";
    std::string root = std::string(header) +
        "function _update() if btnp(5) then load('#" +
        (missing_target ? "missing" : "child") +
        "','back parent','message') end end\nfunction _draw() cls(3) end\n";
    std::string child = std::string(header) +
        "function _draw() cls(stat(6)=='message' and 7 or 8) end\n";
    FILE *file = fopen("root.p8", "wb"); assert(file);
    assert(fwrite(root.data(), 1, root.size(), file) == root.size()); fclose(file);
    uint8_t index[80] = {'P','8','P','B','B','S','1',0};
    put32(index + 8, 1); strcpy((char *)index + 16, "child");
    put32(index + 16 + 48, 80); put32(index + 16 + 52, (uint32_t)child.size());
    put32(index + 16 + 56, (uint32_t)mz_crc32(0, (const uint8_t *)child.data(), child.size()));
    file = fopen("bbs.p8pack", "wb"); assert(file);
    assert(fwrite(index, 1, sizeof(index), file) == sizeof(index));
    assert(fwrite(child.data(), 1, child.size(), file) == child.size()); fclose(file);
    assert(setenv("P8P_BBS_PACK", "bbs.p8pack", 1) == 0);
    char app[] = "app-multicart-test", cart[] = "root.p8";
    char *app_argv[] = {app, cart, NULL};
    int outcome = setjmp(done);
    if (!outcome) { p8p_app_main(2, app_argv); assert(false); }
    if (missing_target) {
        assert(outcome == 2 && presents == 9);
        assert(identity_count == 1);
    } else {
        assert(outcome == 1);
        assert(identity_count == 3);
        assert(memcmp(identities[0], identities[1], 16) != 0);
        assert(memcmp(identities[0], identities[2], 16) == 0);
    }
    unlink("root.p8"); unlink("bbs.p8pack");
    assert(chdir(original) == 0); assert(rmdir(temp) == 0);
    puts(missing_target ? "app missing-target error, Select-menu recovery and exit passed" :
         "app multicart handoff, parameters, Memory IDs, return and input suppression passed");
}
