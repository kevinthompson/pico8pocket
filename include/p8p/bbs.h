#ifndef P8P_BBS_H
#define P8P_BBS_H

#include "p8p/cart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p8p_bbs_pack p8p_bbs_pack_t;

/* Open validates the bounded index; payloads are read and verified on demand. */
p8p_bbs_pack_t *p8p_bbs_pack_open(const char *path, char *error,
                                size_t error_size);
void p8p_bbs_pack_close(p8p_bbs_pack_t *pack);
uint32_t p8p_bbs_pack_features(const p8p_bbs_pack_t *pack);
/* Accepts an optional # prefix and ASCII uppercase. On failure out is untouched.
 * On success the caller owns out and must call p8p_cart_destroy. */
int p8p_bbs_pack_load(p8p_bbs_pack_t *pack, const char *id, p8p_cart_t *out,
                     char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
