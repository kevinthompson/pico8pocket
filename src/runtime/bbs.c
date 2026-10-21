#include "p8p/bbs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BBS_MAX_ENTRIES 256u
#define BBS_MAX_FILE_SIZE (64u * 1024u * 1024u)
#define BBS_MAX_CART_SIZE (2u * 1024u * 1024u)
#define BBS_HEADER_SIZE 16u
#define BBS_ENTRY_SIZE 64u
#define BBS_ID_SIZE 48u

typedef struct bbs_entry {
    char id[BBS_ID_SIZE];
    uint32_t offset;
    uint32_t size;
    uint32_t crc;
} bbs_entry_t;

struct p8p_bbs_pack {
    FILE *file;
    bbs_entry_t *entries;
    uint32_t count;
    uint32_t features;
};

typedef struct bbs_catalog_entry {
    const char *id;
    uint32_t feature;
} bbs_catalog_entry_t;

static const bbs_catalog_entry_t catalog[] = {
#include "p8p/bbs_catalog.inc"
    {NULL, 0}
};

static void set_error(char *error, size_t size, const char *message) {
    if (error && size)
        snprintf(error, size, "%s", message);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int id_character(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static const bbs_entry_t *find_entry(const p8p_bbs_pack_t *pack,
                                    const char *id) {
    for (uint32_t i = 0; i < pack->count; ++i)
        if (strcmp(pack->entries[i].id, id) == 0)
            return &pack->entries[i];
    return NULL;
}

static uint32_t collection_features(const p8p_bbs_pack_t *pack) {
    uint32_t present = 0;
    uint32_t missing = 0;
    for (size_t i = 0; catalog[i].id; ++i) {
        present |= catalog[i].feature;
        if (!find_entry(pack, catalog[i].id))
            missing |= catalog[i].feature;
    }
    return present & ~missing;
}

void p8p_bbs_pack_close(p8p_bbs_pack_t *pack) {
    if (!pack)
        return;
    if (pack->file)
        fclose(pack->file);
    free(pack->entries);
    free(pack);
}

static p8p_bbs_pack_t *open_failure(p8p_bbs_pack_t *pack, char *error,
                                   size_t error_size, const char *message) {
    set_error(error, error_size, message);
    p8p_bbs_pack_close(pack);
    return NULL;
}

p8p_bbs_pack_t *p8p_bbs_pack_open(const char *path, char *error,
                                size_t error_size) {
    uint8_t header[BBS_HEADER_SIZE];
    uint32_t count;
    uint64_t expected_offset;
    long file_size;
    p8p_bbs_pack_t *pack;
    set_error(error, error_size, "");
    if (!path) {
        set_error(error, error_size, "Missing BBS pack path");
        return NULL;
    }
    pack = (p8p_bbs_pack_t *)calloc(1, sizeof(*pack));
    if (!pack) {
        set_error(error, error_size, "Cannot allocate BBS pack index");
        return NULL;
    }
    pack->file = fopen(path, "rb");
    if (!pack->file)
        return open_failure(pack, error, error_size, "Cannot open BBS pack");
    if (fseek(pack->file, 0, SEEK_END) != 0 ||
        (file_size = ftell(pack->file)) < 0 ||
        (uint64_t)file_size > BBS_MAX_FILE_SIZE ||
        fseek(pack->file, 0, SEEK_SET) != 0)
        return open_failure(pack, error, error_size, "Invalid BBS pack file size");
    if (fread(header, 1, sizeof(header), pack->file) != sizeof(header))
        return open_failure(pack, error, error_size, "Truncated BBS pack header");
    if (memcmp(header, "P8PBBS", 6) == 0 && header[6] != '1')
        return open_failure(pack, error, error_size, "Unsupported BBS pack version");
    if (memcmp(header, "P8PBBS1\0", 8) != 0)
        return open_failure(pack, error, error_size, "Invalid BBS pack header");
    count = read_le32(header + 8);
    if (count > BBS_MAX_ENTRIES)
        return open_failure(pack, error, error_size, "BBS pack entry count exceeds limit");
    if (read_le32(header + 12) != 0)
        return open_failure(pack, error, error_size, "Invalid BBS pack header reserved field");
    expected_offset = BBS_HEADER_SIZE + count * BBS_ENTRY_SIZE;
    if (expected_offset > (uint64_t)file_size)
        return open_failure(pack, error, error_size, "Truncated BBS pack index");
    if (count) {
        pack->entries = (bbs_entry_t *)calloc(count, sizeof(*pack->entries));
        if (!pack->entries)
            return open_failure(pack, error, error_size, "Cannot allocate BBS pack index");
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t record[BBS_ENTRY_SIZE];
        size_t id_length = 0;
        bbs_entry_t *entry = &pack->entries[i];
        if (fread(record, 1, sizeof(record), pack->file) != sizeof(record))
            return open_failure(pack, error, error_size, "Cannot read BBS pack index");
        while (id_length < BBS_ID_SIZE && record[id_length]) {
            if (!id_character(record[id_length]))
                return open_failure(pack, error, error_size, "Invalid BBS pack id");
            ++id_length;
        }
        if (id_length == 0 || id_length == BBS_ID_SIZE)
            return open_failure(pack, error, error_size, "Invalid or unterminated BBS pack id");
        for (size_t n = id_length + 1; n < BBS_ID_SIZE; ++n)
            if (record[n])
                return open_failure(pack, error, error_size, "Invalid BBS pack id padding");
        memcpy(entry->id, record, BBS_ID_SIZE);
        if (find_entry(pack, entry->id))
            return open_failure(pack, error, error_size, "BBS pack contains duplicate id");
        if (read_le32(record + 60) != 0)
            return open_failure(pack, error, error_size, "Invalid BBS pack entry reserved field");
        entry->offset = read_le32(record + 48);
        entry->size = read_le32(record + 52);
        entry->crc = read_le32(record + 56);
        if (entry->size == 0 || entry->size > BBS_MAX_CART_SIZE)
            return open_failure(pack, error, error_size, "BBS cart payload size exceeds limit or is empty");
        if (entry->offset != expected_offset)
            return open_failure(pack, error, error_size, "BBS pack payloads are not contiguous in index order");
        expected_offset += entry->size;
        if (expected_offset > (uint64_t)file_size || expected_offset > (uint64_t)LONG_MAX)
            return open_failure(pack, error, error_size, "Truncated or out-of-range BBS pack payload");
        ++pack->count;
    }
    if (expected_offset != (uint64_t)file_size)
        return open_failure(pack, error, error_size, "BBS pack contains trailing bytes");
    pack->features = collection_features(pack);
    return pack;
}

uint32_t p8p_bbs_pack_features(const p8p_bbs_pack_t *pack) {
    return pack ? pack->features : 0;
}

static int normalize_id(const char *input, char id[BBS_ID_SIZE]) {
    size_t n = 0;
    if (!input)
        return -1;
    if (*input == '#')
        ++input;
    while (*input) {
        unsigned char c = (unsigned char)*input++;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + ('a' - 'A'));
        if (n >= BBS_ID_SIZE - 1 || !id_character(c))
            return -1;
        id[n++] = (char)c;
    }
    id[n] = '\0';
    return n ? 0 : -1;
}

static uint32_t payload_crc32(const uint8_t *data, size_t size) {
    static const uint32_t table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return crc ^ 0xffffffffu;
}

int p8p_bbs_pack_load(p8p_bbs_pack_t *pack, const char *input, p8p_cart_t *out,
                     char *error, size_t error_size) {
    char id[BBS_ID_SIZE];
    const bbs_entry_t *entry;
    uint8_t *data;
    p8p_cart_info_t info;
    p8p_cart_t cart;
    int result = -1;
    set_error(error, error_size, "");
    if (!pack || !out) {
        set_error(error, error_size, "Missing BBS pack or cart output");
        return -1;
    }
    if (normalize_id(input, id) != 0) {
        set_error(error, error_size, "Invalid BBS cart id");
        return -1;
    }
    entry = find_entry(pack, id);
    if (!entry) {
        set_error(error, error_size, "BBS cart is not installed");
        return -1;
    }
    data = (uint8_t *)malloc(entry->size);
    if (!data) {
        set_error(error, error_size, "Cannot allocate BBS cart payload");
        return -1;
    }
    if (fseek(pack->file, (long)entry->offset, SEEK_SET) != 0 ||
        fread(data, 1, entry->size, pack->file) != entry->size) {
        set_error(error, error_size, "Cannot read BBS cart payload");
        free(data);
        return -1;
    }
    if (payload_crc32(data, entry->size) != entry->crc) {
        set_error(error, error_size, "BBS cart CRC mismatch");
        free(data);
        return -1;
    }
    memset(&cart, 0, sizeof(cart));
    if (p8p_cart_probe_memory(data, entry->size, entry->size, &info) == 0 && info.valid) {
        if (info.kind == P8P_CART_PNG)
            result = p8p_cart_load_png_memory(data, entry->size, &cart);
        else if (info.kind == P8P_CART_TEXT)
            result = p8p_cart_load_text_memory(data, entry->size, &cart);
    }
    free(data);
    if (result != 0) {
        p8p_cart_destroy(&cart);
        set_error(error, error_size, "Invalid BBS cart data or decoding failed");
        return -1;
    }
    *out = cart;
    return 0;
}
