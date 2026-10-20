#include "p8p/cart.h"
#include "p8p/runtime.h"
#include "p8p/multicart.h"

#include <array>
#include <map>
#include <string>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static uint64_t lua_service_hooks;

static void count_lua_service_hook(void *) {
    ++lua_service_hooks;
}

static uint32_t framebuffer_hash(p8p_runtime_t *runtime) {
    const uint8_t *framebuffer = p8p_runtime_framebuffer(runtime);
    uint32_t hash = 2166136261u;
    for (int pixel = 0; pixel < 128 * 128; ++pixel) {
        hash ^= framebuffer[pixel];
        hash *= 16777619u;
    }
    return hash;
}

static uint8_t scripted_input(unsigned frame, uint32_t *rng) {
    if (frame < 90)
        return 0;
    if (frame == 90 || frame == 150 || frame == 210)
        return (1u << 4) | (1u << 6);

    *rng = *rng * 1664525u + 1013904223u;
    uint8_t buttons = 0;
    unsigned direction = (*rng >> 28) & 7u;
    if (direction < 4)
        buttons |= (uint8_t)(1u << direction);
    if (((*rng >> 8) & 15u) == 0)
        buttons |= 1u << 4;
    if (((*rng >> 16) & 15u) == 0)
        buttons |= 1u << 5;
    if (frame % 257u == 0)
        buttons |= 1u << 6;
    if (buttons & (1u << 4))
        buttons |= 1u << 6;
    return buttons;
}


static void content_identity(const p8p_cart_t &cart, char output[33]) {
    p8p_cart_hash_t hash;
    p8p_cart_content_hash(&cart, &hash);
    for (unsigned i = 0; i < sizeof hash.bytes; ++i)
        std::snprintf(output + i * 2, 3, "%02x", hash.bytes[i]);
}

/* Order matters: the manager borrows both the runtime and active cart. */
struct ScanSession {
    p8p_cart_t cart = {};
    p8p_runtime_t *runtime = nullptr;
    p8p_multicart_t *multicart = nullptr;
    unsigned loads = 0;
    std::map<std::string, std::array<uint8_t, 256>> cartdata;

    static int load_cartdata(void *userdata, const char *id, uint8_t *data, size_t size) {
        auto &stored = static_cast<ScanSession *>(userdata)->cartdata;
        auto found = stored.find(id);
        if (found == stored.end() || size != found->second.size()) return -1;
        std::memcpy(data, found->second.data(), size);
        return 0;
    }

    static int save_cartdata(void *userdata, const char *id, const uint8_t *data, size_t size) {
        auto &stored = static_cast<ScanSession *>(userdata)->cartdata;
        if (size != 256) return -1;
        std::memcpy(stored[id].data(), data, size);
        return 0;
    }

    ~ScanSession() {
        p8p_multicart_destroy(multicart);
        p8p_runtime_destroy(runtime);
        p8p_cart_destroy(&cart);
    }

    int poll() {
        for (unsigned count = 0; count < 32; ++count) {
            const auto *pending = p8p_runtime_cart_request(runtime);
            if (!pending) return 0;
            p8p_runtime_cart_request_t request = *pending;
            char before[33], after[33], error[256];
            content_identity(cart, before);
            if (p8p_multicart_poll(multicart, error, sizeof error) != 1) {
                std::fprintf(stderr, "handoff id=%s failed: %s\n", request.id, error);
                return 6;
            }
            content_identity(cart, after);
            if (request.kind == P8P_RUNTIME_CART_REQUEST_LOAD) ++loads;
            std::printf("handoff kind=%s id=%s from=%s to=%s breadcrumb=%s\n",
                request.kind == P8P_RUNTIME_CART_REQUEST_LOAD ? "load" : "back",
                request.id, before, after, p8p_runtime_breadcrumb(runtime));
        }
        std::fprintf(stderr, "handoff chain exceeds 32 immediate requests\n");
        return 6;
    }

    int open(const char *path) {
        if (p8p_cart_load_file(path, &cart) != 0) {
            std::fprintf(stderr, "cannot load %s\n", path);
            return 2;
        }
        runtime = p8p_runtime_create();
        if (runtime)
            multicart = p8p_multicart_create(runtime, &cart, std::getenv("P8P_BBS_PACK"));
        if (!runtime || !multicart) {
            std::fprintf(stderr, "load error: out of memory\n");
            return 3;
        }
        p8p_runtime_set_cartdata_hooks(runtime, load_cartdata, save_cartdata, this);
        if (p8p_runtime_load(runtime, &cart) != 0 && !p8p_runtime_cart_request(runtime)) {
            std::fprintf(stderr, "load error: %s\n", p8p_runtime_error(runtime));
            return 3;
        }
        return poll();
    }

    int step(uint8_t buttons, unsigned frame) {
        if (p8p_runtime_step(runtime, buttons) != 0 && !p8p_runtime_cart_request(runtime)) {
            std::fprintf(stderr, "frame=%u error=%s\n", frame, p8p_runtime_error(runtime));
            return 4;
        }
        return poll();
    }
};

#ifdef P8P_RUNTIME_DEBUG
static int apply_setup(ScanSession &session, const char *expression) {
    int value = 0;
    int result = p8p_runtime_debug_eval_int(session.runtime, expression, &value);
    int pending = p8p_runtime_cart_request(session.runtime) != nullptr;
    if (result != 0 && !pending) {
        std::fprintf(stderr, "setup expression failed: %s\n", expression);
        return 5;
    }
    std::printf("setup=%s value=%d pending=%d\n", expression, value, pending);
    return session.poll();
}

static int run_handoff(const char *path, const char *expression, unsigned frames,
                       uint8_t launch_buttons) {
    ScanSession session;
    int result = session.open(path);
    if (result) return result;
    /* Large launchers may still be in a cooperatively sliced _init(). */
    for (unsigned frame = 0; frame < 120; ++frame) {
        result = session.step(0, frame);
        if (result) return result;
    }
    char initial[33], current[33];
    content_identity(session.cart, initial);
    unsigned initial_loads = session.loads;
    result = apply_setup(session, expression);
    if (result) return result;
    if (launch_buttons) {
        result = session.step(launch_buttons, 0);
        if (result) return result;
    }
    for (unsigned frame = 0; frame < frames; ++frame) {
        result = session.step(0, frame);
        if (result) return result;
    }
    content_identity(session.cart, current);
    if (session.loads == initial_loads || std::strcmp(initial, current) == 0) {
        std::fprintf(stderr, "diagnostic did not load a different cartridge\n");
        return 7;
    }
    std::printf("target identity=%s frames=%u framebuffer=%08x\n",
                current, frames, framebuffer_hash(session.runtime));
    if (!p8p_runtime_breadcrumb(session.runtime)[0]) {
        if (p8p_runtime_request_back(session.runtime) == 0) {
            std::fprintf(stderr, "standalone target unexpectedly allows return\n");
            return 7;
        }
        std::printf("handoff verified standalone=%s return=unavailable\n", current);
        return 0;
    }
    for (unsigned depth = 0; depth < 8; ++depth) {
        if (p8p_runtime_request_back(session.runtime) != 0) {
            std::fprintf(stderr, "diagnostic target has no return breadcrumb\n");
            return 7;
        }
        result = session.poll();
        if (result) return result;
        content_identity(session.cart, current);
        if (std::strcmp(initial, current) == 0) {
            std::printf("handoff verified return=%s\n", current);
            return 0;
        }
    }
    std::fprintf(stderr, "diagnostic return did not reach the initial cartridge\n");
    return 7;
}
#endif

static int run_scan(const char *path, unsigned frames, uint32_t seed,
                    int draw_frame, const char *setup_expression,
                    int service_hooks) {
    ScanSession session;
    int opened = session.open(path);
    if (opened) return opened;
    p8p_runtime_t *runtime = session.runtime;
    const p8p_cart_t &cart = session.cart;
    uint64_t elapsed_ns = 0;
    uint64_t slowest_ns = 0;
    const char *every_frame_env = std::getenv("P8P_EVERY_FRAME");
    int every_frame = every_frame_env && every_frame_env[0] == '1';

#ifdef P8P_RUNTIME_DEBUG
    if (setup_expression) {
        int result = apply_setup(session, setup_expression);
        if (result) return result;
    }
#else
    (void)setup_expression;
#endif
    std::printf("cart=%s fps=%d lua=%zu frames=%u seed=%u draw=%d hook=%d\n", path,
                p8p_runtime_target_fps(runtime), cart.lua_size, frames, seed,
                draw_frame, service_hooks);
    lua_service_hooks = 0;
    if (service_hooks)
        p8p_runtime_set_service_hook(runtime, count_lua_service_hook, nullptr);
    for (unsigned frame = 0; frame < frames; ++frame) {
        uint8_t buttons = setup_expression ? 0 : scripted_input(frame, &seed);
        auto start = std::chrono::steady_clock::now();
        int result = p8p_runtime_step_with_draw(runtime, buttons, draw_frame);
        auto end = std::chrono::steady_clock::now();
        uint64_t frame_ns = (uint64_t)std::chrono::duration_cast<
            std::chrono::nanoseconds>(end - start).count();
        elapsed_ns += frame_ns;
        if (frame_ns > slowest_ns)
            slowest_ns = frame_ns;
        if (result != 0 && !p8p_runtime_cart_request(runtime)) {
            std::fprintf(stderr, "frame=%u buttons=%02x error=%s\n", frame,
                         buttons, p8p_runtime_error(runtime));
            return 4;
        }
        int handoff = session.poll();
        if (handoff) return handoff;
        /* P8P_EVERY_FRAME=1 prints every frame's hash, to diff two builds
         * frame by frame (see .claude/skills/pico8pocket-compat). */
        if (every_frame)
            std::printf("f%u %08x\n", frame, framebuffer_hash(runtime));
        if ((frame + 1) % 100000u == 0)
            std::printf("frame=%u hash=%08x avg=%.3fms max=%.3fms\n",
                        frame + 1, framebuffer_hash(runtime),
                        (double)elapsed_ns / (double)(frame + 1) / 1000000.0,
                        (double)slowest_ns / 1000000.0);
    }
    std::printf("ok hash=%08x avg=%.3fms max=%.3fms "
                "lua_hooks=%llu lua_kins/frame=%.1f\n",
                framebuffer_hash(runtime),
                frames ? (double)elapsed_ns / (double)frames / 1000000.0 : 0.0,
                (double)slowest_ns / 1000000.0,
                (unsigned long long)lua_service_hooks,
                frames ? (double)lua_service_hooks * 8.192 / (double)frames : 0.0);
    return 0;
}

static int run_input_probe(const char *path) {
    for (int button = 0; button < 6; ++button) {
        ScanSession control_session, probe_session;
        int result = control_session.open(path);
        if (!result) result = probe_session.open(path);
        if (result) return result;
        p8p_runtime_t *control = control_session.runtime;
        p8p_runtime_t *probe = probe_session.runtime;
        int first_difference = -1;
        int different_frames = 0;
        for (unsigned frame = 0; frame < 500; ++frame) {
            uint8_t shared = 0;
            uint8_t tested = shared;
            if (button < 4)
                tested |= ((frame / 30u) & 1u) ?
                          (uint8_t)(1u << button) : 0;
            else if (frame % 45u == 0)
                tested |= (uint8_t)(1u << button);
            int result = control_session.step(shared, frame);
            if (!result) result = probe_session.step(tested, frame);
            if (result) return result;
            if (framebuffer_hash(control) != framebuffer_hash(probe)) {
                if (first_difference < 0)
                    first_difference = (int)frame;
                ++different_frames;
            }
        }
        std::printf("button=%d first_difference=%d different_frames=%d\n",
                    button, first_difference, different_frames);
    }
    return 0;
}

#ifdef P8P_RUNTIME_DEBUG
static int run_eval(const char *path, unsigned frames, uint8_t buttons,
                    const char *expression) {
    ScanSession session;
    int result = session.open(path);
    if (result) return result;
    for (unsigned frame = 0; frame < frames; ++frame) {
        result = session.step(buttons, frame);
        if (result) return result;
    }
    int value = 0;
    result = p8p_runtime_debug_eval_int(session.runtime, expression, &value);
    std::printf("eval result=%d value=%d expression=%s\n", result, value, expression);
    if (p8p_runtime_cart_request(session.runtime)) return session.poll();
    return result == 0 ? 0 : 5;
}
#endif

int main(int argc, char **argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        std::fprintf(stderr,
                     "usage: %s CART [FRAMES [SEED [nodraw]]] | "
                     "CART --bench EXPR FRAMES [SEED [nodraw [nohook]]] | "
                     "CART --probe | CART --identity | CART --source\n"
                     "       CART --handoff EXPR [FRAMES] | CART --arcade-launch INDEX [FRAMES]\n"
                     "       CART --eval FRAMES BUTTONS EXPR\n"
                     "Set P8P_BBS_PACK=/path/to/bbs.p8pack for offline loads.\n"
                     "--handoff verifies identity and return, or no return for a standalone load.\n"
                     "--arcade-launch uses the real Arcade X-button route; INDEX is 1..35.\n"
                     "Diagnostics warm up 120 frames, then run FRAMES target frames (default 120).\n",
                     argv[0]);
        return argc < 2 ? 1 : 0;
    }
    if (argc >= 3 && std::strcmp(argv[2], "--probe") == 0)
        return run_input_probe(argv[1]);
    if (argc >= 3 && std::strcmp(argv[2], "--identity") == 0) {
        p8p_cart_t cart = {};
        if (p8p_cart_load_file(argv[1], &cart) != 0)
            return 2;
        p8p_cart_hash_t hash;
        p8p_cart_content_hash(&cart, &hash);
        for (uint8_t byte : hash.bytes)
            std::printf("%02x", byte);
        const p8p_cart_incompatibility_t *entry =
            p8p_cart_find_incompatibility(&hash);
        std::printf(" %s\n", entry ? entry->title : "unlisted");
        p8p_cart_destroy(&cart);
        return 0;
    }
#ifdef P8P_RUNTIME_DEBUG
    if (argc >= 4 && std::strcmp(argv[2], "--handoff") == 0)
        return run_handoff(argv[1], argv[3], argc >= 5 ?
            (unsigned)std::strtoul(argv[4], nullptr, 0) : 120u, 0);
    if (argc >= 4 && std::strcmp(argv[2], "--arcade-launch") == 0) {
        unsigned index = (unsigned)std::strtoul(argv[3], nullptr, 0);
        if (index < 1 || index > 35) {
            std::fprintf(stderr, "Arcade index must be 1..35\n");
            return 1;
        }
        char expression[220];
        std::snprintf(expression, sizeof expression,
            "(function() assert(cartname and #cartname==35 and game and gs_showcarts) "
            "cartnum=%u game.state=gs_showcarts return cartnum end)()", index);
        return run_handoff(argv[1], expression, argc >= 5 ?
            (unsigned)std::strtoul(argv[4], nullptr, 0) : 120u, 1u << 5);
    }
    if (argc >= 6 && std::strcmp(argv[2], "--eval") == 0)
        return run_eval(argv[1],
                        (unsigned)std::strtoul(argv[3], nullptr, 0),
                        (uint8_t)std::strtoul(argv[4], nullptr, 0), argv[5]);
#endif
    if (argc >= 3 && std::strcmp(argv[2], "--source") == 0) {
        p8p_cart_t cart = {};
        if (p8p_cart_load_file(argv[1], &cart) != 0)
            return 2;
        std::fwrite(cart.lua, 1, cart.lua_size, stdout);
        p8p_cart_destroy(&cart);
        return 0;
    }
#ifdef P8P_RUNTIME_DEBUG
    if (argc >= 5 && std::strcmp(argv[2], "--bench") == 0) {
        unsigned frames = (unsigned)std::strtoul(argv[4], nullptr, 0);
        uint32_t seed = argc >= 6 ?
            (uint32_t)std::strtoul(argv[5], nullptr, 0) : 0x12345678u;
        int draw_frame = !(argc >= 7 &&
                           std::strcmp(argv[6], "nodraw") == 0);
        int service_hooks = !(argc >= 8 &&
                              std::strcmp(argv[7], "nohook") == 0);
        return run_scan(argv[1], frames, seed, draw_frame, argv[3],
                        service_hooks);
    }
#endif
    unsigned frames = argc >= 3 ? (unsigned)std::strtoul(argv[2], nullptr, 0) :
                                  100000u;
    uint32_t seed = argc >= 4 ? (uint32_t)std::strtoul(argv[3], nullptr, 0) :
                                0x12345678u;
    int draw_frame = !(argc >= 5 && std::strcmp(argv[4], "nodraw") == 0);
    int service_hooks = !(argc >= 6 && std::strcmp(argv[5], "nohook") == 0);
    return run_scan(argv[1], frames, seed, draw_frame, nullptr, service_hooks);
}
