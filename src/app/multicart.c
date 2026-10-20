#include "p8p/multicart.h"
#include "p8p/bbs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARENT_LIMIT 8

typedef struct parent_cart {
    p8p_cart_t *cart;
    char params[256];
    char breadcrumb[64];
} parent_cart_t;

struct p8p_multicart {
    p8p_runtime_t *runtime;
    p8p_cart_t *active;
    p8p_bbs_pack_t *pack;
    parent_cart_t parents[PARENT_LIMIT];
    unsigned depth;
    char pack_error[192];
};

static int fail(char *error, size_t size, const char *message) {
    if (error && size)
        snprintf(error, size, "%s", message);
    return -1;
}

static void discard_parents(p8p_multicart_t *session) {
    while (session->depth) {
        parent_cart_t *parent = &session->parents[--session->depth];
        p8p_cart_destroy(parent->cart);
        free(parent->cart);
        parent->cart = NULL;
    }
}

p8p_multicart_t *p8p_multicart_create(p8p_runtime_t *runtime,
                                     p8p_cart_t *active, const char *pack_path) {
    p8p_multicart_t *session;
    if (!runtime || !active)
        return NULL;
    session = (p8p_multicart_t *)calloc(1, sizeof(*session));
    if (!session)
        return NULL;
    session->runtime = runtime;
    session->active = active;
    if (pack_path)
        session->pack = p8p_bbs_pack_open(pack_path, session->pack_error,
                                          sizeof(session->pack_error));
    p8p_runtime_set_cart_features(runtime, p8p_bbs_pack_features(session->pack));
    return session;
}

void p8p_multicart_destroy(p8p_multicart_t *session) {
    if (!session)
        return;
    discard_parents(session);
    p8p_bbs_pack_close(session->pack);
    free(session);
}

int p8p_multicart_restart(p8p_multicart_t *session) {
    if (!session)
        return -1;
    /* A restored state may have a different parameter string. Use the runtime
     * context for restart; the manager keeps only saved parent contexts. */
    return p8p_runtime_load_context(session->runtime, session->active,
                                    p8p_runtime_parameters(session->runtime),
                                    p8p_runtime_breadcrumb(session->runtime));
}

int p8p_multicart_poll(p8p_multicart_t *session, char *error, size_t error_size) {
    const p8p_runtime_cart_request_t *pending;
    p8p_runtime_cart_request_t request;
    p8p_cart_t target;
    p8p_cart_t *saved_parent = NULL;
    int result;
    if (!session)
        return fail(error, error_size, "Offline cartridge session unavailable.");
    pending = p8p_runtime_cart_request(session->runtime);
    if (!pending)
        return 0;
    request = *pending;
    p8p_runtime_clear_cart_request(session->runtime);
    if (error && error_size)
        error[0] = '\0';
    if (request.kind == P8P_RUNTIME_CART_REQUEST_BACK) {
        parent_cart_t *parent;
        if (!session->depth)
            return fail(error, error_size, "No previous cartridge is available.");
        parent = &session->parents[session->depth - 1];
        result = p8p_runtime_load_context(session->runtime, parent->cart,
                                          parent->params, parent->breadcrumb);
        if (result != 0)
            return fail(error, error_size, p8p_runtime_error(session->runtime));
        p8p_cart_destroy(session->active);
        *session->active = *parent->cart;
        free(parent->cart);
        parent->cart = NULL;
        --session->depth;
        return 1;
    }
    if (!session->pack)
        return fail(error, error_size,
                    "Offline pack unavailable. Install the collection with Pocket Sync.");
    memset(&target, 0, sizeof(target));
    if (p8p_bbs_pack_load(session->pack, request.id, &target, error, error_size) != 0)
        return -1;
    if (request.breadcrumb[0]) {
        if (session->depth == PARENT_LIMIT) {
            p8p_cart_destroy(&target);
            return fail(error, error_size, "Offline return history is full (8 cartridges).");
        }
        saved_parent = (p8p_cart_t *)malloc(sizeof(*saved_parent));
        if (!saved_parent) {
            p8p_cart_destroy(&target);
            return fail(error, error_size, "Not enough memory for cartridge return.");
        }
        *saved_parent = *session->active;
        snprintf(session->parents[session->depth].params,
                 sizeof(session->parents[session->depth].params), "%s",
                 p8p_runtime_parameters(session->runtime));
        snprintf(session->parents[session->depth].breadcrumb,
                 sizeof(session->parents[session->depth].breadcrumb), "%s",
                 p8p_runtime_breadcrumb(session->runtime));
    }
    result = p8p_runtime_load_context(session->runtime, &target,
                                      request.params, request.breadcrumb);
    if (result != 0) {
        free(saved_parent); /* active still owns this cart's Lua allocation */
        p8p_cart_destroy(&target);
        return fail(error, error_size, p8p_runtime_error(session->runtime));
    }
    if (saved_parent) {
        parent_cart_t *parent = &session->parents[session->depth++];
        parent->cart = saved_parent;
    } else {
        discard_parents(session);
        p8p_cart_destroy(session->active);
    }
    *session->active = target;
    return 1;
}
