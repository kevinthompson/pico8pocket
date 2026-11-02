#include "p8p/scheduler.h"

int p8p_render_divisor_for_load(int target_fps, uint32_t full_frame_us,
                                uint32_t update_only_us, uint32_t audio_us,
                                uint32_t present_us) {
    /* A 30 fps frame lasts 33.3 ms; 32 ms leaves room for input, the
     * overlay and pacing.  At 30 ms Celeste 2's tower rooms and grapple
     * dropped to divisor 2 although their frames fit. */
    uint32_t full_rate_budget_us = target_fps == 30 ? 32000u : 15000u;
    /* Once frames are skipped, a late drawn frame is repaid by the cheap ones
     * that follow it, so a 60 fps cart can use nearly all of its 16.7 ms.  At
     * 15 ms a cart needing 16 ms per frame fell from 30 to 20 visible fps. */
    uint32_t skipping_budget_us = target_fps == 30 ? 32000u : 16500u;
    int maximum_divisor = target_fps == 30 ? 2 : 4;
    uint32_t update_us = update_only_us ? update_only_us : full_frame_us / 4u;
    uint32_t draw_us = full_frame_us > update_us ?
                       full_frame_us - update_us : 0;
    uint32_t fixed_us = update_us + audio_us;
    uint32_t render_us = draw_us + present_us;

    for (int divisor = 1; divisor <= maximum_divisor; ++divisor) {
        uint32_t estimated_us = fixed_us +
            (render_us + (uint32_t)divisor - 1u) / (uint32_t)divisor;
        if (estimated_us <= (divisor == 1 ? full_rate_budget_us :
                             skipping_budget_us))
            return divisor;
    }

    /* Nothing fits, so the cart will run slow at any divisor.  Skipping more
     * draws raises the logical rate less and less while it destroys the
     * visible rate.  BAS Escape spends almost all of its frame in _update and
     * gains very little from rendering one frame in four; UFO's draw pass
     * dominates, so a larger divisor still protects game speed.  Take the
     * lowest divisor within a fifth of the fastest achievable step. */
    uint32_t fastest_us = fixed_us +
        (render_us + (uint32_t)maximum_divisor - 1u) /
        (uint32_t)maximum_divisor;
    for (int divisor = 1; divisor < maximum_divisor; ++divisor) {
        uint32_t estimated_us = fixed_us +
            (render_us + (uint32_t)divisor - 1u) / (uint32_t)divisor;
        if ((uint64_t)estimated_us * 100u <= (uint64_t)fastest_us * 120u)
            return divisor;
    }
    return maximum_divisor;
}
