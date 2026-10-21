#define _POSIX_C_SOURCE 200809L
#include "p8p/bbs.h"
#include "miniz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const uint8_t text_cart[] = "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\nprint('bundle fixture')\n";
static char path[] = "/tmp/p8p-bbs-XXXXXX";
static unsigned checks;

typedef struct catalog_entry { const char *id; uint32_t feature; } catalog_entry_t;
static const catalog_entry_t catalog[] = {
#include "p8p/bbs_catalog.inc"
    {NULL, 0}
};

static void le32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static void be32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static uint32_t fixture_crc(const uint8_t *data, size_t size) {
    return (uint32_t)mz_crc32(0, data, size);
}
static void save(const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    CHECK(f); CHECK(fwrite(data, 1, size, f) == size); CHECK(fclose(f) == 0);
}
static uint8_t *bundle(const char **ids, size_t count, const uint8_t *payload,
                       size_t size, size_t *length) {
    size_t cursor = 16 + count * 64;
    uint8_t *data;
    *length = cursor + count * size;
    data = (uint8_t *)calloc(*length, 1); CHECK(data);
    memcpy(data, "P8PBBS1", 7); le32(data + 8, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        uint8_t *entry = data + 16 + 64 * i;
        CHECK(strlen(ids[i]) <= 47); strcpy((char *)entry, ids[i]);
        le32(entry + 48, (uint32_t)cursor); le32(entry + 52, (uint32_t)size);
        le32(entry + 56, fixture_crc(payload, size));
        memcpy(data + cursor, payload, size); cursor += size;
    }
    return data;
}
static p8p_bbs_pack_t *open_ok(const uint8_t *data, size_t size) {
    char error[128] = "old error";
    p8p_bbs_pack_t *pack;
    save(data, size); pack = p8p_bbs_pack_open(path, error, sizeof(error));
    if (!pack) fprintf(stderr, "unexpected pack failure: %s\n", error);
    CHECK(pack); CHECK(error[0] == 0); ++checks; return pack;
}
static void reject(const uint8_t *data, size_t size, const char *reason) {
    char error[128] = {0};
    save(data, size); CHECK(p8p_bbs_pack_open(path, error, sizeof(error)) == NULL);
    CHECK(strstr(error, reason)); ++checks;
}
static size_t chunk(uint8_t *png, size_t pos, const char *type,
                     const uint8_t *data, size_t size) {
    be32(png + pos, (uint32_t)size); memcpy(png + pos + 4, type, 4);
    if (size) memcpy(png + pos + 8, data, size);
    be32(png + pos + 8 + size, fixture_crc(png + pos + 4, size + 4));
    return pos + 12 + size;
}
static uint8_t *make_png(size_t *size) {
    const size_t stride = 160 * 4 + 1, raw_size = stride * 205;
    uint8_t *raw = (uint8_t *)calloc(raw_size, 1);
    mz_ulong compressed_size = mz_compressBound(raw_size);
    uint8_t *compressed = (uint8_t *)malloc(compressed_size);
    uint8_t *png = (uint8_t *)malloc(compressed_size + 57);
    uint8_t ihdr[13] = {0};
    static const uint8_t source[] = "print('png fixture')\n";
    CHECK(raw && compressed && png);
    /* PICO-8 packs each byte into the low two bits of RGBA. */
    for (size_t i = 0; i < sizeof(source) - 1; ++i) {
        size_t index = 0x4300 + i, pos = (index / 160) * stride + 1 + (index % 160) * 4;
        raw[pos] = (source[i] >> 4) & 3; raw[pos + 1] = (source[i] >> 2) & 3;
        raw[pos + 2] = source[i] & 3; raw[pos + 3] = source[i] >> 6;
    }
    CHECK(mz_compress(compressed, &compressed_size, raw, raw_size) == MZ_OK);
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    be32(ihdr, 160); be32(ihdr + 4, 205); ihdr[8] = 8; ihdr[9] = 6;
    *size = chunk(png, 8, "IHDR", ihdr, sizeof(ihdr));
    *size = chunk(png, *size, "IDAT", compressed, compressed_size);
    *size = chunk(png, *size, "IEND", NULL, 0);
    free(raw); free(compressed); return png;
}
static void test_valid_and_load(void) {
    const char *ids[] = {"fixture_1-test"};
    size_t size; uint8_t *data = bundle(ids, 1, text_cart, sizeof(text_cart) - 1, &size);
    p8p_bbs_pack_t *pack = open_ok(data, size);
    p8p_cart_t cart = {{0}, NULL, 0}; char error[128];
    CHECK(p8p_bbs_pack_features(pack) == 0);
    CHECK(p8p_bbs_pack_load(pack, "#FIXTURE_1-TEST", &cart, error, sizeof(error)) == 0);
    CHECK(strcmp(cart.lua, "print('bundle fixture')\n") == 0); CHECK(error[0] == 0);
    p8p_cart_destroy(&cart);
    { const char *bad[] = {"", "#", "##fixture_1-test", "../fixture_1-test", "fixture/test", "with.dot", "\x80", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
      for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
          CHECK(p8p_bbs_pack_load(pack, bad[i], &cart, error, sizeof(error)) == -1);
          CHECK(strstr(error, "id")); ++checks;
      }
    }
    CHECK(p8p_bbs_pack_load(pack, "missing", &cart, error, sizeof(error)) == -1);
    CHECK(strstr(error, "not installed"));
    CHECK(p8p_bbs_pack_load(NULL, "fixture", &cart, NULL, 0) == -1);
    CHECK(p8p_bbs_pack_load(pack, "fixture_1-test", NULL, NULL, 0) == -1);
    p8p_bbs_pack_close(pack); free(data);
    data = bundle(ids, 0, text_cart, 0, &size); pack = open_ok(data, size);
    CHECK(p8p_bbs_pack_features(pack) == 0); p8p_bbs_pack_close(pack); free(data);
    CHECK(p8p_bbs_pack_features(NULL) == 0); p8p_bbs_pack_close(NULL);
    { size_t png_size; uint8_t *png = make_png(&png_size);
      data = bundle(ids, 1, png, png_size, &size); pack = open_ok(data, size);
      CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == 0);
      CHECK(strcmp(cart.lua, "print('png fixture')\n") == 0);
      p8p_cart_destroy(&cart); p8p_bbs_pack_close(pack); free(png); free(data);
    }
}
static void test_invalid_index(void) {
    const char *ids[] = {"first", "second"}; size_t size;
    uint8_t *data = bundle(ids, 2, text_cart, sizeof(text_cart) - 1, &size);
    uint8_t *bad = (uint8_t *)malloc(size + 1); CHECK(bad);
#define BAD_COPY() memcpy(bad, data, size)
    for (size_t n = 0; n < 16; ++n) reject(data, n, "header");
    BAD_COPY(); bad[6] = '2'; reject(bad, size, "version");
    BAD_COPY(); bad[0] = 'X'; reject(bad, size, "header");
    BAD_COPY(); bad[7] = 1; reject(bad, size, "header");
    BAD_COPY(); le32(bad + 8, 257); reject(bad, size, "count");
    BAD_COPY(); le32(bad + 12, 1); reject(bad, size, "reserved");
    reject(data, 16 + 127, "index");
    BAD_COPY(); memset(bad + 16, 'a', 48); reject(bad, size, "id");
    BAD_COPY(); bad[16] = '#'; reject(bad, size, "id");
    BAD_COPY(); bad[16] = 'F'; reject(bad, size, "id");
    BAD_COPY(); bad[16] = 0; reject(bad, size, "id");
    BAD_COPY(); bad[22] = 'x'; reject(bad, size, "padding");
    BAD_COPY(); memcpy(bad + 80, bad + 16, 48); reject(bad, size, "duplicate");
    BAD_COPY(); le32(bad + 16 + 60, 1); reject(bad, size, "reserved");
    BAD_COPY(); le32(bad + 16 + 48, 143); reject(bad, size, "contiguous");
    BAD_COPY(); le32(bad + 80 + 48, 144); reject(bad, size, "contiguous");
    BAD_COPY(); le32(bad + 16 + 48, 0xffffffffu); reject(bad, size, "contiguous");
    BAD_COPY(); le32(bad + 16 + 52, 2 * 1024 * 1024 + 1); reject(bad, size, "size");
    BAD_COPY(); le32(bad + 16 + 52, 0); reject(bad, size, "size");
    reject(data, size - 1, "payload");
    BAD_COPY(); bad[size] = 0; reject(bad, size + 1, "trailing");
    { FILE *f = fopen(path, "wb"); char error[128]; CHECK(f);
      CHECK(fseek(f, 64 * 1024 * 1024, SEEK_SET) == 0); CHECK(fputc(0, f) == 0); fclose(f);
      CHECK(!p8p_bbs_pack_open(path, error, sizeof(error))); CHECK(strstr(error, "size"));
    }
    { char error[2] = {'x', 'x'}; save(data, 1);
      CHECK(!p8p_bbs_pack_open(path, error, sizeof(error))); CHECK(error[1] == 0);
      CHECK(!p8p_bbs_pack_open(path, NULL, 0)); CHECK(!p8p_bbs_pack_open(NULL, NULL, 0));
    }
    free(bad); free(data);
#undef BAD_COPY
}
static void test_invalid_payload(void) {
    const char *ids[] = {"fixture"}; size_t size; char error[128];
    uint8_t *data = bundle(ids, 1, text_cart, sizeof(text_cart) - 1, &size);
    p8p_cart_t cart = {{0}, NULL, 0}; p8p_bbs_pack_t *pack;
    data[size - 1] ^= 1; pack = open_ok(data, size);
    CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == -1);
    CHECK(strstr(error, "CRC")); CHECK(cart.lua == NULL); p8p_bbs_pack_close(pack); free(data);
    data = bundle(ids, 1, (const uint8_t *)"123456789", 9, &size);
    /* Standard published CRC-32 check value, independent of reader. */
    CHECK(fixture_crc(data + 80, 9) == 0xcbf43926u); pack = open_ok(data, size);
    CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == -1);
    CHECK(strstr(error, "cart")); p8p_bbs_pack_close(pack); free(data);
    { static const uint8_t invalid_png[] = "\x89PNG\r\n\x1a\ninvalid";
      data = bundle(ids, 1, invalid_png, sizeof(invalid_png) - 1, &size); pack = open_ok(data, size);
      CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == -1);
      CHECK(strstr(error, "cart")); p8p_bbs_pack_close(pack); free(data);
    }
    { size_t png_size; uint8_t *png = make_png(&png_size);
      /* Keep the valid IHDR; damage the IDAT zlib header to reach the decoder. */
      png[41] = 0;
      data = bundle(ids, 1, png, png_size, &size); pack = open_ok(data, size);
      memset(cart.rom, 0x5a, sizeof(cart.rom));
      CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == -1);
      CHECK(strstr(error, "decoding")); CHECK(cart.lua == NULL);
      for (size_t i = 0; i < sizeof(cart.rom); ++i) CHECK(cart.rom[i] == 0x5a);
      p8p_bbs_pack_close(pack); free(png); free(data);
    }
}
static void test_contract_limits(void) {
    const char *ids[256]; char names[256][48]; size_t size;
    uint8_t *data; p8p_bbs_pack_t *pack; p8p_cart_t cart = {{0}, NULL, 0};
    char error[128];
    for (size_t i = 0; i < 256; ++i) {
        snprintf(names[i], sizeof(names[i]), "fixture_%03u", (unsigned)i);
        ids[i] = names[i];
    }
    memset(names[0], 'a', 47); names[0][47] = 0;
    data = bundle(ids, 256, text_cart, sizeof(text_cart) - 1, &size);
    pack = open_ok(data, size);
    CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == 0);
    p8p_cart_destroy(&cart);
    CHECK(p8p_bbs_pack_load(pack, ids[255], &cart, error, sizeof(error)) == 0);
    p8p_cart_destroy(&cart); p8p_bbs_pack_close(pack); free(data);
    { const size_t limit = 2 * 1024 * 1024;
      uint8_t *payload = (uint8_t *)malloc(limit); CHECK(payload);
      memset(payload, ' ', limit); memcpy(payload, text_cart, sizeof(text_cart) - 1);
      data = bundle(ids, 1, payload, limit, &size); pack = open_ok(data, size);
      CHECK(p8p_bbs_pack_load(pack, ids[0], &cart, error, sizeof(error)) == 0);
      p8p_cart_destroy(&cart); p8p_bbs_pack_close(pack); free(payload); free(data);
    }
}
static void test_features(void) {
    const char *ids[256]; size_t count = 0, size; uint8_t *data; p8p_bbs_pack_t *pack;
    uint32_t expected = 0;
    for (size_t i = 0; catalog[i].id; ++i) { ids[count++] = catalog[i].id; expected |= catalog[i].feature; }
    CHECK(count > 1 && count <= 256);
    data = bundle(ids, count, text_cart, sizeof(text_cart) - 1, &size); pack = open_ok(data, size);
    CHECK(p8p_bbs_pack_features(pack) == expected); p8p_bbs_pack_close(pack); free(data);
    for (size_t omitted = 0; omitted < count; ++omitted) {
        const char *partial[256]; size_t n = 0;
        for (size_t i = 0; i < count; ++i) if (i != omitted) partial[n++] = ids[i];
        data = bundle(partial, n, text_cart, sizeof(text_cart) - 1, &size); pack = open_ok(data, size);
        CHECK(p8p_bbs_pack_features(pack) == (expected & ~catalog[omitted].feature));
        p8p_bbs_pack_close(pack); free(data);
    }
}
int main(void) {
    int fd = mkstemp(path); CHECK(fd >= 0); CHECK(close(fd) == 0);
    test_valid_and_load(); test_invalid_index(); test_invalid_payload();
    test_contract_limits(); test_features();
    CHECK(unlink(path) == 0); printf("bbs tests passed (%u checked cases)\n", checks); return 0;
}
