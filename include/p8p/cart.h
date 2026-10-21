#ifndef P8P_CART_H
#define P8P_CART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum p8p_cart_kind {
    P8P_CART_UNKNOWN = 0,
    P8P_CART_TEXT,
    P8P_CART_PNG,
    P8P_CART_ZIP
} p8p_cart_kind_t;

typedef struct p8p_cart_info {
    p8p_cart_kind_t kind;
    uint64_t file_size;
    uint32_t png_width;
    uint32_t png_height;
    int valid;
} p8p_cart_info_t;

#define P8P_CART_ROM_SIZE 0x4300u

typedef struct p8p_cart {
    uint8_t rom[P8P_CART_ROM_SIZE];
    char *lua;
    size_t lua_size;
} p8p_cart_t;

typedef struct p8p_cart_hash {
    uint8_t bytes[16];
} p8p_cart_hash_t;

enum p8p_cart_feature {
    P8P_CART_FEATURE_OFFLINE_ARCADE = 1u,
    P8P_CART_FEATURE_OFFLINE_SORCERER = 2u
};

typedef struct p8p_cart_incompatibility {
    uint8_t hash[16];
    const char *title;
    const char *reason;
    const char *workaround;
    /* Zero denotes an unconditional blocker. Otherwise all bits are needed. */
    uint32_t required_features;
} p8p_cart_incompatibility_t;

/* Identity of decoded ROM and exact Lua bytes; independent of filename/label. */
void p8p_cart_content_hash(const p8p_cart_t *cart, p8p_cart_hash_t *hash);
const p8p_cart_incompatibility_t *p8p_cart_find_incompatibility(
    const p8p_cart_hash_t *hash);

int p8p_cart_probe_memory(const uint8_t *data, size_t size,
                          uint64_t file_size, p8p_cart_info_t *out);
int p8p_cart_probe_file(const char *path, p8p_cart_info_t *out);
const char *p8p_cart_kind_name(p8p_cart_kind_t kind);
int p8p_cart_load_text_memory(const uint8_t *data, size_t size,
                              p8p_cart_t *out);
int p8p_cart_load_text_file(const char *path, p8p_cart_t *out);
int p8p_cart_load_png_memory(const uint8_t *data, size_t size,
                             p8p_cart_t *out);
int p8p_cart_load_file(const char *path, p8p_cart_t *out);
void p8p_cart_destroy(p8p_cart_t *cart);

#ifdef __cplusplus
}
#endif

#endif
