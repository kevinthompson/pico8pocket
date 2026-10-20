#include "p8p/multicart.h"
#include "miniz.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static void put32(unsigned char *out, unsigned value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(value >> (i * 8));
}

static std::string text(const char *body) {
    return std::string("pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n") + body;
}

static void write_pack(const char *path) {
    std::string carts[] = {
        text("function _init() cls(stat(6)=='hello' and 7 or 8) end\nfunction _update() if btnp(5) then load('#grandchild','back child','next') end end\n"),
        text("function _init() cls(stat(6)=='next' and 11 or 8) end\n"),
        text("function _init() error('broken target') end\n"),
    };
    const char *ids[] = {"child", "grandchild", "broken"};
    unsigned char header[16] = {'P','8','P','B','B','S','1',0};
    put32(header + 8, 3);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(header, 1, sizeof header, file) == sizeof header);
    unsigned offset = 16 + 3 * 64;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned char record[64] = {};
        strcpy((char *)record, ids[i]);
        put32(record + 48, offset);
        put32(record + 52, (unsigned)carts[i].size());
        put32(record + 56, (unsigned)mz_crc32(0, (const unsigned char *)carts[i].data(), carts[i].size()));
        assert(fwrite(record, 1, sizeof record, file) == sizeof record);
        offset += (unsigned)carts[i].size();
    }
    for (const auto &cart : carts) assert(fwrite(cart.data(), 1, cart.size(), file) == cart.size());
    assert(fclose(file) == 0);
}

static p8p_cart_t source_cart(const char *body) {
    std::string data = text(body); p8p_cart_t cart = {};
    assert(p8p_cart_load_text_memory((const uint8_t *)data.data(), data.size(), &cart) == 0);
    return cart;
}

static void request(p8p_runtime_t *runtime, const char *id) {
    char expression[200]; int result;
    snprintf(expression, sizeof expression, "(function() load('#%s','return','hello') return 99 end)()", id);
    /* debug evaluation is protected on the main thread; the queued request
       still crosses the same host boundary as a cartridge callback. */
    (void)p8p_runtime_debug_eval_int(runtime, expression, &result);
    assert(p8p_runtime_cart_request(runtime));
}

int main() {
    char path[] = "/tmp/p8p-multicart-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); write_pack(path);
    p8p_cart_t active = source_cart("function _init() cls(3) end\nfunction _update() if btnp(5) then load('#child','back parent','hello') end end\n");
    p8p_cart_hash_t parent_hash, current_hash;
    p8p_cart_content_hash(&active, &parent_hash);
    p8p_runtime_t *runtime = p8p_runtime_create(); assert(runtime);
    p8p_multicart_t *session = p8p_multicart_create(runtime, &active, path); assert(session);
    assert(p8p_runtime_load(runtime, &active) == 0);
    assert(p8p_runtime_framebuffer(runtime)[0] == 3);
    char error[256] = {};
    assert(p8p_multicart_poll(session, error, sizeof error) == 0);
    assert(p8p_runtime_step(runtime, 32) == 0);
    assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    assert(p8p_runtime_framebuffer(runtime)[0] == 7);
    assert(strcmp(p8p_runtime_breadcrumb(runtime), "back parent") == 0);
    p8p_cart_content_hash(&active, &current_hash);
    assert(memcmp(&parent_hash, &current_hash, sizeof parent_hash) != 0);
    assert(p8p_multicart_restart(session) == 0);
    assert(p8p_runtime_framebuffer(runtime)[0] == 7);
    assert(p8p_runtime_step(runtime, 0) == 0);
    assert(p8p_runtime_step(runtime, 32) == 0);
    assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    assert(p8p_runtime_framebuffer(runtime)[0] == 11);
    assert(p8p_runtime_request_back(runtime) == 0);
    assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    assert(p8p_runtime_framebuffer(runtime)[0] == 7);
    assert(p8p_runtime_request_back(runtime) == 0);
    assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    p8p_cart_content_hash(&active, &current_hash);
    assert(memcmp(&parent_hash, &current_hash, sizeof parent_hash) == 0);
    assert(p8p_runtime_framebuffer(runtime)[0] == 3);
    assert(p8p_runtime_breadcrumb(runtime)[0] == '\0');

    request(runtime, "missing");
    assert(p8p_multicart_poll(session, error, sizeof error) == -1);
    assert(error[0]);
    assert(!p8p_runtime_cart_request(runtime));
    assert(p8p_multicart_restart(session) == 0);
    request(runtime, "broken");
    assert(p8p_multicart_poll(session, error, sizeof error) == -1);
    assert(strstr(error, "broken target"));
    assert(p8p_runtime_parameters(runtime)[0] == '\0');
    assert(p8p_runtime_breadcrumb(runtime)[0] == '\0');
    assert(p8p_multicart_restart(session) == 0);
    assert(p8p_runtime_framebuffer(runtime)[0] == 3);

    /* A full history rejects the ninth load without losing any parent. */
    for (unsigned i = 0; i < 8; ++i) {
        request(runtime, "child");
        assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    }
    request(runtime, "child");
    assert(p8p_multicart_poll(session, error, sizeof error) == -1);
    assert(strstr(error, "history is full"));
    assert(p8p_multicart_restart(session) == 0);
    for (unsigned i = 0; i < 8; ++i) {
        assert(p8p_runtime_request_back(runtime) == 0);
        assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    }
    assert(p8p_runtime_breadcrumb(runtime)[0] == '\0');
    p8p_cart_content_hash(&active, &current_hash);
    assert(memcmp(&parent_hash, &current_hash, sizeof parent_hash) == 0);

    /* Loading without a return label discards the whole prior history. */
    request(runtime, "child");
    assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    int ignored;
    (void)p8p_runtime_debug_eval_int(runtime, "(function() load('#grandchild') return 0 end)()", &ignored);
    assert(p8p_multicart_poll(session, error, sizeof error) == 1);
    assert(p8p_runtime_request_back(runtime) == -1);

    p8p_multicart_destroy(session);
    session = p8p_multicart_create(runtime, &active, NULL); assert(session);
    request(runtime, "child");
    assert(p8p_multicart_poll(session, error, sizeof error) == -1);
    assert(strstr(error, "pack unavailable"));
    assert(p8p_multicart_restart(session) == 0);
    p8p_multicart_destroy(session);
    p8p_runtime_destroy(runtime);
    p8p_cart_destroy(&active);
    assert(unlink(path) == 0);
    puts("multicart manager tests passed");
}
