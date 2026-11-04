/* The Pocket's slot reader against a fake openfpgaOS async read.  The OS
 * copies a finished read into its destination from the completion IRQ, even
 * after the reader has stopped waiting for it, so a read that times out must
 * not leave a caller's buffer as its destination. */
#define OF_PC 1

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint8_t slot_bytes[200000];
static uint32_t fake_now_us;
static int stall_reads;
static int next_token;

static struct pending_read {
    int active;
    int token;
    uint32_t offset;
    uint8_t *dest;
    uint32_t length;
    void (*callback)(int token, int result);
} pending;

static uint32_t of_time_us(void) {
    return fake_now_us += 1000;
}

static uint32_t of_file_async_max_read(void) {
    return 65536u;
}

static void finish_pending_read(void) {
    if (!pending.active)
        return;
    pending.active = 0;
    memcpy(pending.dest, slot_bytes + pending.offset, pending.length);
    pending.callback(pending.token, 0);
}

static int of_file_async_poll(void) {
    if (stall_reads || !pending.active)
        return 0;
    finish_pending_read();
    return 1;
}

static int of_file_read_async(int slot, uint32_t offset, void *dest,
                              uint32_t length,
                              void (*callback)(int token, int result)) {
    (void)slot;
    if (pending.active)
        return -1;
    pending = (struct pending_read){1, next_token++, offset, (uint8_t *)dest,
                                    length, callback};
    return pending.token;
}

#include "../src/platform/openfpgaos/slot_read.c"

static void reads_a_cart_across_chunks(void) {
    static uint8_t cart[150000];
    assert(p8p_platform_read_slot(4, 1000, cart, sizeof(cart)) == 0);
    assert(memcmp(cart, slot_bytes + 1000, sizeof(cart)) == 0);
}

static void late_completion_leaves_the_caller_buffer_alone(void) {
    uint8_t tail[512];
    memset(tail, 0xa5, sizeof(tail));
    stall_reads = 1;
    assert(p8p_platform_read_slot(4, 0, tail, sizeof(tail)) < 0);

    /* The caller has returned and reused its buffer before the bridge
     * finishes the read. */
    memset(tail, 0x11, sizeof(tail));
    finish_pending_read();
    for (unsigned index = 0; index < sizeof(tail); ++index)
        assert(tail[index] == 0x11);
    stall_reads = 0;
}

static void a_late_completion_is_not_taken_for_the_next_read(void) {
    uint8_t first[64], second[64];
    stall_reads = 1;
    assert(p8p_platform_read_slot(4, 0, first, sizeof(first)) < 0);
    stall_reads = 0;

    /* The stalled read still owns the bridge, so the next read fails
     * instead of returning the stalled read's bytes. */
    assert(p8p_platform_read_slot(4, 5000, second, sizeof(second)) < 0);
    finish_pending_read();
    assert(p8p_platform_read_slot(4, 5000, second, sizeof(second)) == 0);
    assert(memcmp(second, slot_bytes + 5000, sizeof(second)) == 0);
}

int main(void) {
    for (unsigned index = 0; index < sizeof(slot_bytes); ++index)
        slot_bytes[index] = (uint8_t)(index * 31 + (index >> 8));
    reads_a_cart_across_chunks();
    late_completion_leaves_the_caller_buffer_alone();
    a_late_completion_is_not_taken_for_the_next_read();
    return 0;
}
