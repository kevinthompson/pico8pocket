#include "p8p/platform.h"

#ifndef OF_PC
#include "of.h"
#endif

#include <stdint.h>

static volatile int g_slot_read_done;
static volatile int g_slot_read_result;

static void slot_read_complete(int token, int result) {
    (void)token;
    g_slot_read_result = result;
    g_slot_read_done = 1;
}

int p8p_platform_read_slot(unsigned slot, uint32_t offset, void *dest,
                           uint32_t length) {
    uint8_t *bytes = (uint8_t *)dest;
    uint32_t chunk_limit = of_file_async_max_read();
    if (!chunk_limit)
        chunk_limit = 65536u;
    while (length) {
        uint32_t chunk = length < chunk_limit ? length : chunk_limit;
        uint32_t start;
        g_slot_read_done = 0;
        g_slot_read_result = 0;
        if (of_file_read_async((int)slot, offset, bytes, chunk,
                               slot_read_complete) < 0)
            return -1;
        /* Completion arrives from the data-slot IRQ; polling is optional
         * on the Pocket but harmless, and the timeout bounds a lost IRQ. */
        start = of_time_us();
        while (!g_slot_read_done) {
            if (of_time_us() - start > 2000000u)
                return -2;
            of_file_async_poll();
        }
        if (g_slot_read_result < 0)
            return -3;
        bytes += chunk;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}
