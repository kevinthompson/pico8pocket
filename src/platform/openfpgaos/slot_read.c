#include "p8p/platform.h"

#ifndef OF_PC
#include "of.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_READ_TIMEOUT_US 2000000u
#define SLOT_READ_DEFAULT_CHUNK 65536u

/*
 * The OS copies a finished read into its destination from the completion
 * IRQ, whether or not anyone is still waiting for it.  A read that outlasts
 * the timeout (the bridge stalls while the Pocket writes a screenshot, for
 * example) would otherwise land later in a caller's stack or freed heap.  So
 * every read targets this staging buffer, which nothing else ever uses, and
 * only a completed read is copied out.
 */
static uint8_t *g_staging;
static uint32_t g_staging_size;
static volatile int g_completed_token = -1;
static volatile int g_completed_result;

static void slot_read_complete(int token, int result) {
    g_completed_result = result;
    g_completed_token = token;
}

static int ensure_staging(void) {
    uint32_t size;
    if (g_staging)
        return 0;
    size = of_file_async_max_read();
    if (!size)
        size = SLOT_READ_DEFAULT_CHUNK;
    if (!(g_staging = (uint8_t *)malloc(size)))
        return -1;
    g_staging_size = size;
    return 0;
}

int p8p_platform_read_slot(unsigned slot, uint32_t offset, void *dest,
                           uint32_t length) {
    uint8_t *bytes = (uint8_t *)dest;
    if (ensure_staging() != 0)
        return -1;
    while (length) {
        uint32_t chunk = length < g_staging_size ? length : g_staging_size;
        uint32_t start;
        int token;
        g_completed_token = -1;
        /* Fails while an earlier, abandoned read still owns the bridge. */
        token = of_file_read_async((int)slot, offset, g_staging, chunk,
                                   slot_read_complete);
        if (token < 0)
            return -1;
        /* Completion arrives from the data-slot IRQ; polling is optional
         * on the Pocket but harmless, and the timeout bounds a lost IRQ. */
        start = of_time_us();
        while (g_completed_token != token) {
            if (of_time_us() - start > SLOT_READ_TIMEOUT_US)
                return -2;
            of_file_async_poll();
        }
        if (g_completed_result < 0)
            return -3;
        memcpy(bytes, g_staging, chunk);
        bytes += chunk;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}
