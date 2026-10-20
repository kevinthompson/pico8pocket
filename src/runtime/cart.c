#include "p8p/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hash_content_bytes(uint64_t *a, uint64_t *b,
                               const uint8_t *data, size_t size) {
    while (size--) {
        *a ^= *data;
        *a *= UINT64_C(1099511628211);
        *b ^= (uint64_t)(*data++ + 0x9d);
        *b *= UINT64_C(14029467366897019727);
        *b ^= *b >> 29;
    }
}

void p8p_cart_content_hash(const p8p_cart_t *cart, p8p_cart_hash_t *hash) {
    uint64_t a = UINT64_C(14695981039346656037);
    uint64_t b = UINT64_C(7809847782465536322);
    if (!hash)
        return;
    memset(hash, 0, sizeof(*hash));
    if (!cart)
        return;
    hash_content_bytes(&a, &b, cart->rom, sizeof(cart->rom));
    if (cart->lua)
        hash_content_bytes(&a, &b, (const uint8_t *)cart->lua, cart->lua_size);
    /* Preserve the save-state identity's little-endian byte layout. */
    for (unsigned i = 0; i < 8; ++i) {
        hash->bytes[i] = (uint8_t)(a >> (i * 8));
        hash->bytes[i + 8] = (uint8_t)(b >> (i * 8));
    }
}

const p8p_cart_incompatibility_t *p8p_cart_find_incompatibility(
    const p8p_cart_hash_t *hash) {
    static const p8p_cart_incompatibility_t entries[] = {
#include "p8p/incompatible_carts.inc"
        {{0}, NULL, NULL, NULL}
    };
    if (!hash)
        return NULL;
    for (size_t i = 0; entries[i].title; ++i)
        if (memcmp(hash->bytes, entries[i].hash, sizeof(hash->bytes)) == 0)
            return &entries[i];
    return NULL;
}

static const uint8_t png_signature[8] = {
    0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
};

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/*
 * PICO-8 writes .p8 source as UTF-8: control codes and characters 127-255
 * appear as glyphs such as U+2076 or U+25DD, which the console turns
 * back into single P8SCII bytes when it loads the cart.  .p8.png carts store
 * the raw bytes.  Charmap from zepto8 via Fake-08 (WTFPL).
 */
typedef struct p8scii_glyph {
    uint8_t code;
    const char *utf8;
} p8scii_glyph_t;

static const p8scii_glyph_t p8scii_glyphs[] = {
    {0x01, "\xc2\xb9"}, /* ctrl 1 */
    {0x02, "\xc2\xb2"}, /* ctrl 2 */
    {0x03, "\xc2\xb3"}, /* ctrl 3 */
    {0x04, "\xe2\x81\xb4"}, /* ctrl 4 */
    {0x05, "\xe2\x81\xb5"}, /* ctrl 5 */
    {0x06, "\xe2\x81\xb6"}, /* ctrl 6 */
    {0x07, "\xe2\x81\xb7"}, /* ctrl 7 */
    {0x08, "\xe2\x81\xb8"}, /* ctrl 8 */
    {0x0b, "\xe1\xb5\x87"}, /* ctrl 11 */
    {0x0c, "\xe1\xb6\x9c"}, /* ctrl 12 */
    {0x0e, "\xe1\xb5\x89"}, /* ctrl 14 */
    {0x0f, "\xe1\xb6\xa0"}, /* ctrl 15 */
    {0x10, "\xe2\x96\xae"}, /* ctrl 16 */
    {0x11, "\xe2\x96\xa0"}, /* ctrl 17 */
    {0x12, "\xe2\x96\xa1"}, /* ctrl 18 */
    {0x13, "\xe2\x81\x99"}, /* ctrl 19 */
    {0x14, "\xe2\x81\x98"}, /* ctrl 20 */
    {0x15, "\xe2\x80\x96"}, /* ctrl 21 */
    {0x16, "\xe2\x97\x80"}, /* ctrl 22 */
    {0x17, "\xe2\x96\xb6"}, /* ctrl 23 */
    {0x18, "\xe3\x80\x8c"}, /* ctrl 24 */
    {0x19, "\xe3\x80\x8d"}, /* ctrl 25 */
    {0x1a, "\xc2\xa5"}, /* ctrl 26 */
    {0x1b, "\xe2\x80\xa2"}, /* ctrl 27 */
    {0x1c, "\xe3\x80\x81"}, /* ctrl 28 */
    {0x1d, "\xe3\x80\x82"}, /* ctrl 29 */
    {0x1e, "\xe3\x82\x9b"}, /* ctrl 30 */
    {0x1f, "\xe3\x82\x9c"}, /* ctrl 31 */
    {0x7f, "\xe2\x97\x8b"}, /* ○ */
    {0x80, "\xe2\x96\x88"}, /* █ */
    {0x81, "\xe2\x96\x92"}, /* ▒ */
    {0x82, "\xf0\x9f\x90\xb1"}, /* 🐱 */
    {0x83, "\xe2\xac\x87\xef\xb8\x8f"}, /* ⬇️ */
    {0x84, "\xe2\x96\x91"}, /* ░ */
    {0x85, "\xe2\x9c\xbd"}, /* ✽ */
    {0x86, "\xe2\x97\x8f"}, /* ● */
    {0x87, "\xe2\x99\xa5"}, /* ♥ */
    {0x88, "\xe2\x98\x89"}, /* ☉ */
    {0x89, "\xec\x9b\x83"}, /* 웃 */
    {0x8a, "\xe2\x8c\x82"}, /* ⌂ */
    {0x8b, "\xe2\xac\x85\xef\xb8\x8f"}, /* ⬅️ */
    {0x8c, "\xf0\x9f\x98\x90"}, /* 😐 */
    {0x8d, "\xe2\x99\xaa"}, /* ♪ */
    {0x8e, "\xf0\x9f\x85\xbe\xef\xb8\x8f"}, /* 🅾️ */
    {0x8f, "\xe2\x97\x86"}, /* ◆ */
    {0x90, "\xe2\x80\xa6"}, /* … */
    {0x91, "\xe2\x9e\xa1\xef\xb8\x8f"}, /* ➡️ */
    {0x92, "\xe2\x98\x85"}, /* ★ */
    {0x93, "\xe2\xa7\x97"}, /* ⧗ */
    {0x94, "\xe2\xac\x86\xef\xb8\x8f"}, /* ⬆️ */
    {0x95, "\xcb\x87"}, /* ˇ */
    {0x96, "\xe2\x88\xa7"}, /* ∧ */
    {0x97, "\xe2\x9d\x8e"}, /* ❎ */
    {0x98, "\xe2\x96\xa4"}, /* ▤ */
    {0x99, "\xe2\x96\xa5"}, /* ▥ */
    {0x9a, "\xe3\x81\x82"}, /* あ */
    {0x9b, "\xe3\x81\x84"}, /* い */
    {0x9c, "\xe3\x81\x86"}, /* う */
    {0x9d, "\xe3\x81\x88"}, /* え */
    {0x9e, "\xe3\x81\x8a"}, /* お */
    {0x9f, "\xe3\x81\x8b"}, /* か */
    {0xa0, "\xe3\x81\x8d"}, /* き */
    {0xa1, "\xe3\x81\x8f"}, /* く */
    {0xa2, "\xe3\x81\x91"}, /* け */
    {0xa3, "\xe3\x81\x93"}, /* こ */
    {0xa4, "\xe3\x81\x95"}, /* さ */
    {0xa5, "\xe3\x81\x97"}, /* し */
    {0xa6, "\xe3\x81\x99"}, /* す */
    {0xa7, "\xe3\x81\x9b"}, /* せ */
    {0xa8, "\xe3\x81\x9d"}, /* そ */
    {0xa9, "\xe3\x81\x9f"}, /* た */
    {0xaa, "\xe3\x81\xa1"}, /* ち */
    {0xab, "\xe3\x81\xa4"}, /* つ */
    {0xac, "\xe3\x81\xa6"}, /* て */
    {0xad, "\xe3\x81\xa8"}, /* と */
    {0xae, "\xe3\x81\xaa"}, /* な */
    {0xaf, "\xe3\x81\xab"}, /* に */
    {0xb0, "\xe3\x81\xac"}, /* ぬ */
    {0xb1, "\xe3\x81\xad"}, /* ね */
    {0xb2, "\xe3\x81\xae"}, /* の */
    {0xb3, "\xe3\x81\xaf"}, /* は */
    {0xb4, "\xe3\x81\xb2"}, /* ひ */
    {0xb5, "\xe3\x81\xb5"}, /* ふ */
    {0xb6, "\xe3\x81\xb8"}, /* へ */
    {0xb7, "\xe3\x81\xbb"}, /* ほ */
    {0xb8, "\xe3\x81\xbe"}, /* ま */
    {0xb9, "\xe3\x81\xbf"}, /* み */
    {0xba, "\xe3\x82\x80"}, /* む */
    {0xbb, "\xe3\x82\x81"}, /* め */
    {0xbc, "\xe3\x82\x82"}, /* も */
    {0xbd, "\xe3\x82\x84"}, /* や */
    {0xbe, "\xe3\x82\x86"}, /* ゆ */
    {0xbf, "\xe3\x82\x88"}, /* よ */
    {0xc0, "\xe3\x82\x89"}, /* ら */
    {0xc1, "\xe3\x82\x8a"}, /* り */
    {0xc2, "\xe3\x82\x8b"}, /* る */
    {0xc3, "\xe3\x82\x8c"}, /* れ */
    {0xc4, "\xe3\x82\x8d"}, /* ろ */
    {0xc5, "\xe3\x82\x8f"}, /* わ */
    {0xc6, "\xe3\x82\x92"}, /* を */
    {0xc7, "\xe3\x82\x93"}, /* ん */
    {0xc8, "\xe3\x81\xa3"}, /* っ */
    {0xc9, "\xe3\x82\x83"}, /* ゃ */
    {0xca, "\xe3\x82\x85"}, /* ゅ */
    {0xcb, "\xe3\x82\x87"}, /* ょ */
    {0xcc, "\xe3\x82\xa2"}, /* ア */
    {0xcd, "\xe3\x82\xa4"}, /* イ */
    {0xce, "\xe3\x82\xa6"}, /* ウ */
    {0xcf, "\xe3\x82\xa8"}, /* エ */
    {0xd0, "\xe3\x82\xaa"}, /* オ */
    {0xd1, "\xe3\x82\xab"}, /* カ */
    {0xd2, "\xe3\x82\xad"}, /* キ */
    {0xd3, "\xe3\x82\xaf"}, /* ク */
    {0xd4, "\xe3\x82\xb1"}, /* ケ */
    {0xd5, "\xe3\x82\xb3"}, /* コ */
    {0xd6, "\xe3\x82\xb5"}, /* サ */
    {0xd7, "\xe3\x82\xb7"}, /* シ */
    {0xd8, "\xe3\x82\xb9"}, /* ス */
    {0xd9, "\xe3\x82\xbb"}, /* セ */
    {0xda, "\xe3\x82\xbd"}, /* ソ */
    {0xdb, "\xe3\x82\xbf"}, /* タ */
    {0xdc, "\xe3\x83\x81"}, /* チ */
    {0xdd, "\xe3\x83\x84"}, /* ツ */
    {0xde, "\xe3\x83\x86"}, /* テ */
    {0xdf, "\xe3\x83\x88"}, /* ト */
    {0xe0, "\xe3\x83\x8a"}, /* ナ */
    {0xe1, "\xe3\x83\x8b"}, /* ニ */
    {0xe2, "\xe3\x83\x8c"}, /* ヌ */
    {0xe3, "\xe3\x83\x8d"}, /* ネ */
    {0xe4, "\xe3\x83\x8e"}, /* ノ */
    {0xe5, "\xe3\x83\x8f"}, /* ハ */
    {0xe6, "\xe3\x83\x92"}, /* ヒ */
    {0xe7, "\xe3\x83\x95"}, /* フ */
    {0xe8, "\xe3\x83\x98"}, /* ヘ */
    {0xe9, "\xe3\x83\x9b"}, /* ホ */
    {0xea, "\xe3\x83\x9e"}, /* マ */
    {0xeb, "\xe3\x83\x9f"}, /* ミ */
    {0xec, "\xe3\x83\xa0"}, /* ム */
    {0xed, "\xe3\x83\xa1"}, /* メ */
    {0xee, "\xe3\x83\xa2"}, /* モ */
    {0xef, "\xe3\x83\xa4"}, /* ヤ */
    {0xf0, "\xe3\x83\xa6"}, /* ユ */
    {0xf1, "\xe3\x83\xa8"}, /* ヨ */
    {0xf2, "\xe3\x83\xa9"}, /* ラ */
    {0xf3, "\xe3\x83\xaa"}, /* リ */
    {0xf4, "\xe3\x83\xab"}, /* ル */
    {0xf5, "\xe3\x83\xac"}, /* レ */
    {0xf6, "\xe3\x83\xad"}, /* ロ */
    {0xf7, "\xe3\x83\xaf"}, /* ワ */
    {0xf8, "\xe3\x83\xb2"}, /* ヲ */
    {0xf9, "\xe3\x83\xb3"}, /* ン */
    {0xfa, "\xe3\x83\x83"}, /* ッ */
    {0xfb, "\xe3\x83\xa3"}, /* ャ */
    {0xfc, "\xe3\x83\xa5"}, /* ュ */
    {0xfd, "\xe3\x83\xa7"}, /* ョ */
    {0xfe, "\xe2\x97\x9c"}, /* ◜ */
    {0xff, "\xe2\x97\x9d"}, /* ◝ */
};

/* Emoji glyphs end in U+FE0F; some editors drop it, so accept both forms. */
static const char variation_selector[] = "\xef\xb8\x8f";

static size_t p8scii_from_utf8(const uint8_t *in, size_t length, char *out) {
    size_t written = 0;
    for (size_t i = 0; i < length;) {
        size_t best_length = 0;
        uint8_t best_code = 0;
        if (in[i] >= 0x80) {
            for (size_t g = 0; g < sizeof(p8scii_glyphs) / sizeof(p8scii_glyphs[0]); ++g) {
                const char *glyph = p8scii_glyphs[g].utf8;
                size_t glyph_length = strlen(glyph);
                size_t candidates[2] = { glyph_length, 0 };
                if (glyph_length > 3 &&
                    memcmp(glyph + glyph_length - 3, variation_selector, 3) == 0)
                    candidates[1] = glyph_length - 3;
                for (int c = 0; c < 2; ++c) {
                    size_t n = candidates[c];
                    if (n > best_length && n <= length - i &&
                        memcmp(in + i, glyph, n) == 0) {
                        best_length = n;
                        best_code = p8scii_glyphs[g].code;
                    }
                }
            }
        }
        if (best_length) {
            out[written++] = (char)best_code;
            i += best_length;
        } else {
            out[written++] = (char)in[i++];
        }
    }
    return written;
}

static int has_text_header(const uint8_t *data, size_t size) {
    static const char header[] = "pico-8 cartridge // http://www.pico-8.com";
    size_t offset = 0;

    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        offset = 3;

    return size - offset >= sizeof(header) - 1 &&
           memcmp(data + offset, header, sizeof(header) - 1) == 0;
}

int p8p_cart_probe_memory(const uint8_t *data, size_t size,
                          uint64_t file_size, p8p_cart_info_t *out) {
    p8p_cart_info_t info;

    if (!data || !out)
        return -1;

    memset(&info, 0, sizeof(info));
    info.file_size = file_size;

    if (has_text_header(data, size)) {
        info.kind = P8P_CART_TEXT;
        info.valid = 1;
    } else if (size >= 24 && memcmp(data, png_signature, sizeof(png_signature)) == 0 &&
               memcmp(data + 12, "IHDR", 4) == 0) {
        info.kind = P8P_CART_PNG;
        info.png_width = read_be32(data + 16);
        info.png_height = read_be32(data + 20);
        info.valid = info.png_width == 160 && info.png_height == 205;
    } else if (size >= 4 && data[0] == 'P' && data[1] == 'K' &&
               ((data[2] == 3 && data[3] == 4) ||
                (data[2] == 5 && data[3] == 6) ||
                (data[2] == 7 && data[3] == 8))) {
        info.kind = P8P_CART_ZIP;
        info.valid = 1;
    }

    *out = info;
    return 0;
}

int p8p_cart_probe_file(const char *path, p8p_cart_info_t *out) {
    uint8_t prefix[128];
    long length;
    size_t read_count;
    FILE *file;

    if (!path || !out)
        return -1;

    file = fopen(path, "rb");
    if (!file)
        return -2;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -3;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }

    read_count = fread(prefix, 1, sizeof(prefix), file);
    fclose(file);
    return p8p_cart_probe_memory(prefix, read_count, (uint64_t)length, out);
}

const char *p8p_cart_kind_name(p8p_cart_kind_t kind) {
    switch (kind) {
    case P8P_CART_TEXT: return "p8";
    case P8P_CART_PNG: return "p8.png";
    case P8P_CART_ZIP: return "zip";
    default: return "unknown";
    }
}

typedef enum p8p_section {
    P8P_SECTION_NONE = 0,
    P8P_SECTION_LUA,
    P8P_SECTION_GFX,
    P8P_SECTION_GFF,
    P8P_SECTION_MAP,
    P8P_SECTION_SFX,
    P8P_SECTION_MUSIC
} p8p_section_t;

static int hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_byte(const uint8_t *p) {
    int high = hex_nibble(p[0]);
    int low = hex_nibble(p[1]);
    return high < 0 || low < 0 ? -1 : (high << 4) | low;
}

static p8p_section_t section_from_line(const uint8_t *line, size_t length) {
    struct section_name {
        const char *name;
        p8p_section_t section;
    };
    static const struct section_name names[] = {
        {"__lua__", P8P_SECTION_LUA},
        {"__gfx__", P8P_SECTION_GFX},
        {"__gff__", P8P_SECTION_GFF},
        {"__map__", P8P_SECTION_MAP},
        {"__sfx__", P8P_SECTION_SFX},
        {"__music__", P8P_SECTION_MUSIC}
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t name_length = strlen(names[i].name);
        if (length == name_length && memcmp(line, names[i].name, length) == 0)
            return names[i].section;
    }
    return P8P_SECTION_NONE;
}

static void parse_hex_stream(const uint8_t *line, size_t length,
                             uint8_t *destination, size_t capacity,
                             size_t *position, int swap_nibbles) {
    int first = -1;

    for (size_t i = 0; i < length && *position < capacity; ++i) {
        int nibble = hex_nibble(line[i]);
        if (nibble < 0)
            continue;
        if (first < 0) {
            first = nibble;
        } else {
            destination[(*position)++] = (uint8_t)(swap_nibbles
                ? first | (nibble << 4)
                : (first << 4) | nibble);
            first = -1;
        }
    }
}

static void parse_music_line(const uint8_t *line, size_t length,
                             uint8_t *rom, size_t index) {
    int flags;
    int channels[4];

    if (index >= 64 || length < 11)
        return;
    flags = hex_byte(line);
    for (int i = 0; i < 4; ++i)
        channels[i] = hex_byte(line + 3 + i * 2);
    if (flags < 0 || channels[0] < 0 || channels[1] < 0 ||
        channels[2] < 0 || channels[3] < 0)
        return;

    rom[0x3100 + index * 4 + 0] = (uint8_t)(channels[0] | ((flags & 1) << 7));
    rom[0x3100 + index * 4 + 1] = (uint8_t)(channels[1] | ((flags & 2) << 6));
    rom[0x3100 + index * 4 + 2] = (uint8_t)(channels[2] | ((flags & 4) << 5));
    rom[0x3100 + index * 4 + 3] = (uint8_t)(channels[3] | ((flags & 8) << 4));
}

static void parse_sfx_line(const uint8_t *line, size_t length,
                           uint8_t *rom, size_t index) {
    uint8_t *sfx;

    if (index >= 64 || length < 168)
        return;
    sfx = rom + 0x3200 + index * 68;
    for (int note = 0; note < 32; ++note) {
        size_t offset = 8 + (size_t)note * 5;
        int key = hex_byte(line + offset);
        int waveform = hex_nibble(line[offset + 2]);
        int volume = hex_nibble(line[offset + 3]);
        int effect = hex_nibble(line[offset + 4]);
        if (key < 0 || waveform < 0 || volume < 0 || effect < 0)
            continue;
        sfx[note * 2] = (uint8_t)((key & 0x3f) | ((waveform & 3) << 6));
        sfx[note * 2 + 1] = (uint8_t)(((waveform >> 2) & 1) |
            ((volume & 7) << 1) | ((effect & 7) << 4) |
            ((waveform > 7 ? 1 : 0) << 7));
    }
    sfx[64] = (uint8_t)hex_byte(line);
    sfx[65] = (uint8_t)hex_byte(line + 2);
    sfx[66] = (uint8_t)hex_byte(line + 4);
    sfx[67] = (uint8_t)hex_byte(line + 6);
}

int p8p_cart_load_text_memory(const uint8_t *data, size_t size,
                              p8p_cart_t *out) {
    p8p_cart_t cart;
    p8p_section_t section = P8P_SECTION_NONE;
    size_t cursor = 0;
    size_t lua_capacity;
    size_t gfx_position = 0;
    size_t gff_position = 0;
    size_t map_position = 0;
    size_t music_line = 0;
    size_t sfx_line = 0;

    if (!data || !out || !has_text_header(data, size))
        return -1;
    memset(&cart, 0, sizeof(cart));
    for (size_t i = 0; i < 64; ++i)
        cart.rom[0x3200 + i * 68 + 65] = 16;
    lua_capacity = size + 1;
    cart.lua = (char *)malloc(lua_capacity);
    if (!cart.lua)
        return -2;
    cart.lua[0] = '\0';

    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        cursor = 3;

    while (cursor < size) {
        size_t line_start = cursor;
        size_t line_length;
        p8p_section_t next_section;

        while (cursor < size && data[cursor] != '\n')
            ++cursor;
        line_length = cursor - line_start;
        if (line_length && data[line_start + line_length - 1] == '\r')
            --line_length;
        if (cursor < size)
            ++cursor;

        next_section = section_from_line(data + line_start, line_length);
        if (next_section != P8P_SECTION_NONE) {
            section = next_section;
            continue;
        }

        switch (section) {
        case P8P_SECTION_LUA:
            if (cart.lua_size + line_length + 2 <= lua_capacity) {
                /* Conversion never lengthens the line. */
                cart.lua_size += p8scii_from_utf8(data + line_start, line_length,
                                                  cart.lua + cart.lua_size);
                cart.lua[cart.lua_size++] = '\n';
                cart.lua[cart.lua_size] = '\0';
            }
            break;
        case P8P_SECTION_GFX:
            parse_hex_stream(data + line_start, line_length, cart.rom,
                             0x2000, &gfx_position, 1);
            break;
        case P8P_SECTION_GFF:
            parse_hex_stream(data + line_start, line_length, cart.rom + 0x3000,
                             0x100, &gff_position, 0);
            break;
        case P8P_SECTION_MAP:
            parse_hex_stream(data + line_start, line_length, cart.rom + 0x2000,
                             0x1000, &map_position, 0);
            break;
        case P8P_SECTION_MUSIC:
            parse_music_line(data + line_start, line_length, cart.rom, music_line++);
            break;
        case P8P_SECTION_SFX:
            parse_sfx_line(data + line_start, line_length, cart.rom, sfx_line++);
            break;
        default:
            break;
        }
    }

    if (cart.lua_size == 0) {
        free(cart.lua);
        return -3;
    }
    *out = cart;
    return 0;
}

int p8p_cart_load_text_file(const char *path, p8p_cart_t *out) {
    FILE *file;
    uint8_t *data;
    long length;
    int result;

    if (!path || !out)
        return -1;
    file = fopen(path, "rb");
    if (!file)
        return -2;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) {
        fclose(file);
        return -4;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return -5;
    }
    fclose(file);
    result = p8p_cart_load_text_memory(data, (size_t)length, out);
    free(data);
    return result;
}

int p8p_cart_load_file(const char *path, p8p_cart_t *out) {
    FILE *file;
    uint8_t *data;
    long length;
    p8p_cart_info_t info;
    int result;

    if (!path || !out)
        return -1;
    file = fopen(path, "rb");
    if (!file)
        return -2;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) {
        fclose(file);
        return -4;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return -5;
    }
    fclose(file);
    if (p8p_cart_probe_memory(data, (size_t)length, (uint64_t)length, &info) != 0) {
        free(data);
        return -6;
    }
    if (info.kind == P8P_CART_TEXT)
        result = p8p_cart_load_text_memory(data, (size_t)length, out);
    else if (info.kind == P8P_CART_PNG)
        result = p8p_cart_load_png_memory(data, (size_t)length, out);
    else
        result = -7;
    free(data);
    return result;
}

void p8p_cart_destroy(p8p_cart_t *cart) {
    if (!cart)
        return;
    free(cart->lua);
    memset(cart, 0, sizeof(*cart));
}
