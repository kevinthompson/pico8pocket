/* Kept outside LTO so fast-path bodies do not enter its global inline budget. */
#include "runtime_internal.h"
#include <lstate.h>
#include <string.h>
#if defined(__riscv)
#define P8P_PALETTE_LEAF __attribute__((section(".app_fasttext"), noinline, optimize("align-functions=2", "align-loops=2")))
#define P8P_PALETTE_COMPACT __attribute__((optimize("Os")))
#else
#define P8P_PALETTE_COMPACT
#define P8P_PALETTE_LEAF
#endif
static_assert(offsetof(p8p_runtime, draw_palette) % 4 == 0, "palette word alignment");
extern "C" P8P_PALETTE_LEAF int p8p_api_pal(lua_State *lua) {
    p8p_runtime_t *runtime = p8p_active_runtime;
    if (lua->top == lua->ci->func + 1 && runtime && !runtime->profile_hook &&
        runtime->palettes_default && runtime->transparency_default)
        return 0;
    return p8p_api_pal_full(lua);
}
/* A four-iteration leaf keeps this check within the production BRAM budget. */
extern "C" P8P_PALETTE_LEAF P8P_PALETTE_COMPACT
int p8p_draw_palette_identity(const uint8_t *palette) {
    alignas(4) static const uint8_t first[4] = {0,1,2,3};
    uint32_t expected;
    __builtin_memcpy(&expected, first, 4);
    const uint8_t *aligned = static_cast<const uint8_t *>(__builtin_assume_aligned(palette, 4));
#if defined(__riscv) && defined(__GNUC__)
#pragma GCC unroll 1
#endif
    for (int i = 0; i < 4; ++i) {
        uint32_t actual;
        __builtin_memcpy(&actual, aligned + i * 4, 4);
        if (actual != expected) return 0;
        expected += UINT32_C(0x04040404);
    }
    return 1;
}
