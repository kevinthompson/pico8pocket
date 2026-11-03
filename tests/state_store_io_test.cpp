/* How much of the 256 KB store one cartdata save touches.  The store lives in
 * memory the CPU reads word by word with interrupts off, so a save during
 * play must stay far below the frame budget.  The source is included to count
 * its file calls and to write a record in the format of older releases. */
#include <stdio.h>

static size_t transferred_bytes;

static size_t counted_fread(void *data, size_t size, size_t count, FILE *file) {
    size_t items = fread(data, size, count, file);
    transferred_bytes += items * size;
    return items;
}

static size_t counted_fwrite(const void *data, size_t size, size_t count,
                             FILE *file) {
    size_t items = fwrite(data, size, count, file);
    transferred_bytes += items * size;
    return items;
}

#define fread counted_fread
#define fwrite counted_fwrite
#include "../src/app/state_store.c"
#undef fread
#undef fwrite

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

enum { SAVE_DURING_PLAY_MAX_BYTES = 4096 };

static void fill(uint8_t *data, uint8_t seed) {
    for (unsigned i = 0; i < CARTDATA_SIZE; ++i)
        data[i] = (uint8_t)(seed + i * 7);
}

static void expect_cartdata(const char *path, const char *id, uint8_t seed) {
    uint8_t expected[CARTDATA_SIZE];
    uint8_t actual[CARTDATA_SIZE];
    fill(expected, seed);
    assert(p8p_cartdata_load_file(path, id, actual, sizeof actual) == 0);
    assert(memcmp(expected, actual, sizeof actual) == 0);
}

static void save_cartdata(const char *path, const char *id, uint8_t seed) {
    uint8_t data[CARTDATA_SIZE];
    fill(data, seed);
    assert(p8p_cartdata_save_file(path, id, data, sizeof data) == 0);
}

/* The record older releases wrote: deflated, so its size varied with the
 * values and it could not be replaced where it stood. */
static void save_deflated_cartdata(const char *path, const char *id,
                                   uint8_t seed) {
    uint8_t data[CARTDATA_SIZE];
    uint8_t *store = NULL;
    p8p_cart_hash_t hash;
    fill(data, seed);
    cartdata_hash(id, &hash);
    assert(load_store(path, &store) == 0);
    uint32_t used = read_u32(store + 12);
    mz_ulong compressed_size = mz_compressBound(sizeof data);
    assert(mz_compress2(store + used + RECORD_HEADER_SIZE, &compressed_size,
                        data, sizeof data, MZ_BEST_SPEED) == MZ_OK);
    memset(store + used, 0, RECORD_HEADER_SIZE);
    write_u32(store + used, RECORD_MAGIC);
    write_u32(store + used + 4, RECORD_HEADER_SIZE + (uint32_t)compressed_size);
    write_u32(store + used + 8, sizeof data);
    write_u32(store + used + 12, (uint32_t)compressed_size);
    write_u32(store + used + 16, (uint32_t)mz_crc32(0, data, sizeof data));
    write_u64(store + used + 20, read_u64(store + 16));
    memcpy(store + used + 32, hash.bytes, sizeof hash.bytes);
    write_u32(store + used + 52, RECORD_TYPE_CARTDATA);
    write_u32(store + 12, used + RECORD_HEADER_SIZE + (uint32_t)compressed_size);
    write_u64(store + 16, read_u64(store + 16) + 1);
    assert(save_store(path, store) == 0);
    free(store);
}

int main(void) {
    char path[] = "/tmp/p8p-store-io-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);

    save_cartdata(path, "first", 1);
    save_cartdata(path, "second", 2);
    save_cartdata(path, "third", 3);

    transferred_bytes = 0;
    save_cartdata(path, "second", 9);
    if (transferred_bytes > SAVE_DURING_PLAY_MAX_BYTES) {
        fprintf(stderr, "FAIL cartdata save moved %zu bytes of the store\n",
                transferred_bytes);
        return 1;
    }
    expect_cartdata(path, "first", 1);
    expect_cartdata(path, "second", 9);
    expect_cartdata(path, "third", 3);

    /* The newest record stays newest for eviction after an in-place save. */
    uint8_t *store = NULL;
    p8p_cart_hash_t hash;
    uint32_t offset, size;
    assert(load_store(path, &store) == 0);
    cartdata_hash("second", &hash);
    assert(find_record(store, &hash, RECORD_TYPE_CARTDATA, &offset, &size));
    uint64_t saved_sequence = read_u64(store + offset + 20);
    assert(read_u64(store + 16) == saved_sequence + 1);
    free(store);

    /* Records from older releases still load and move to the new form. */
    save_deflated_cartdata(path, "legacy", 4);
    expect_cartdata(path, "legacy", 4);
    save_cartdata(path, "legacy", 5);
    expect_cartdata(path, "legacy", 5);
    transferred_bytes = 0;
    save_cartdata(path, "legacy", 6);
    assert(transferred_bytes <= SAVE_DURING_PLAY_MAX_BYTES);
    expect_cartdata(path, "legacy", 6);
    expect_cartdata(path, "third", 3);

    assert(unlink(path) == 0);
    puts("PASS cartdata saves stay small during play");
    return 0;
}
