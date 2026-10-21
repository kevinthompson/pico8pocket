#ifndef P8P_MULTICART_H
#define P8P_MULTICART_H

#include "p8p/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p8p_multicart p8p_multicart_t;

/* Borrows runtime and active cart; owns the pack and saved parent carts.
 * A missing optional pack is allowed and grants no collection capabilities. */
p8p_multicart_t *p8p_multicart_create(p8p_runtime_t *runtime,
                                     p8p_cart_t *active, const char *pack_path);
void p8p_multicart_destroy(p8p_multicart_t *session);
/* Service one queued VM request outside Lua: 1 switched, 0 idle, -1 failed.
 * On failure the caller must stop stepping the VM and display error. */
int p8p_multicart_poll(p8p_multicart_t *session, char *error, size_t error_size);
int p8p_multicart_restart(p8p_multicart_t *session);

#ifdef __cplusplus
}
#endif
#endif
