#include "p8p/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static int eval(p8p_runtime_t *r, const char *s) {
    int value = -999;
    CHECK(p8p_runtime_debug_eval_int(r, s, &value) == 0);
    return value;
}
static int load(p8p_runtime_t *r, const char *lua) {
    p8p_cart_t cart = {};
    cart.lua = const_cast<char *>(lua);
    cart.lua_size = strlen(lua);
    return p8p_runtime_load(r, &cart);
}
static void noncontinuation() {
    struct Case { const char *source; int steps; bool menu; };
    const Case cases[] = {
        {"after=0 load('#target','parent','hello') after=1", 0, false},
        {"after=0 function _init() load('#target','parent','hello') after=1 end", 0, false},
        {"after=0 function _update() load('#target','parent','hello') after=1 end function _draw() after=2 end", 1, false},
        {"after=0 function _draw() load('#target','parent','hello') after=1 end", 1, false},
        {"after=0 menuitem(1,'next',function() load('#target','parent','hello') after=1 end)", 0, true},
        {"after=0 menuitem(1,'next',function() flip() load('#target','parent','hello') after=1 end)", 0, true},
        {"after=0 menuitem(1,'next',function() mkdir('unused') load('#target','parent','hello') after=1 end)", 0, true},
        {"after=0 menuitem(1,'next',function() yield(true,false) flip() load('#target','parent','hello') after=1 end)", 0, true},
        {"after=0 flip() load('#target','parent','hello') after=1", 1, false},
        {"after=0 function _init() flip() load('#target','parent','hello') after=1 end", 1, false},
        {"after=0 function _update() flip() load('#target','parent','hello') after=1 end", 2, false},
        {"after=0 function _draw() flip() load('#target','parent','hello') after=1 end", 2, false},
        {"after=0 function _update() pcall(function() load('#target','parent','hello') end) after=1 end", 1, false},
        {"after=0 menuitem(1,'next',function() pcall(function() load('#target','parent','hello') end) after=1 end)", 0, true},
        {"after=0 coresume(cocreate(function() load('#target','parent','hello') end)) after=1", 0, false},
        {"after=0 function _update() coroutine.resume(coroutine.create(function() load('#target','parent','hello') end)) after=1 end", 1, false},
        {"after=0 function _draw() coroutine.wrap(function() load('#target','parent','hello') end)() after=1 end", 1, false},
        {"after=0 menuitem(1,'next',function() coresume(cocreate(function() load('#target','parent','hello') end)) after=1 end)", 0, true},
        {"after=0 coresume(cocreate(function() coresume(cocreate(function() load('#target','parent','hello') end)) after=1 end)) after=1", 0, false},
        {"after=0 function _update() pcall(function() table.sort({2,1},function(a,b) load('#target','parent','hello') return a<b end) end) after=1 end", 1, false},
        {"after=0 function _update() pcall(function() string.gsub('a','a',function() load('#target','parent','hello') return 'b' end) end) after=1 end", 1, false},
        {"after=0 function _update() xpcall(function() table.sort({2,1},function(a,b) load('#target','parent','hello') return a<b end) end,function(e) return e end) after=1 end", 1, false},
        {"after=0 table.sort({2,1},function(a,b) load('#target','parent','hello') return a<b end) after=1", 0, false},
        {"after=0 function _init() table.sort({2,1},function(a,b) load('#target','parent','hello') return a<b end) after=1 end", 0, false},
        {"after=0 function _draw() table.sort({2,1},function(a,b) load('#target','parent','hello') return a<b end) after=1 end", 1, false},
        {"after=0 function _update() xpcall(function() table.sort({2,1},function(a,b) load('#target','parent','hello') end) end,function(e) after=77 load('#wrong') return e end) after=1 end", 1, false}
    };
    for (const auto &test : cases) {
        p8p_runtime_t *r = p8p_runtime_create();
        CHECK(load(r, test.source) == 0);
        for (int step = 0; step < test.steps; ++step)
            CHECK(p8p_runtime_step(r, 0) == 0);
        if (test.menu) CHECK(p8p_runtime_menu_select(r, 1, 112) == 0);
        CHECK(eval(r, "after") == 0);
        const auto *request = p8p_runtime_cart_request(r);
        CHECK(request && request->kind == P8P_RUNTIME_CART_REQUEST_LOAD);
        CHECK(request && strcmp(request->id, "target") == 0);
        CHECK(request && strcmp(request->breadcrumb, "parent") == 0);
        CHECK(request && strcmp(request->params, "hello") == 0);
        void *data = NULL; size_t size = 0;
        CHECK(p8p_runtime_save_state(r, &data, &size) != 0);
        CHECK(p8p_runtime_step(r, 0) == 0);
        CHECK(eval(r, "after") == 0);
        p8p_runtime_clear_cart_request(r);
        CHECK(p8p_runtime_cart_request(r) == NULL);
        // Host failure recovery must never resume the suspended statement.
        CHECK(p8p_runtime_step(r, 0) == 0);
        CHECK(eval(r, "after") != 1);
        p8p_runtime_destroy(r);
    }
}

static void ordinary_menu_yields() {
    struct Case { const char *callback; int keep_open; };
    const Case cases[] = {
        {"flip() after=1 return true", 1},
        {"mkdir('unused') after=1 return false", 0},
        {"flip() mkdir('unused') after=1", 0},
        {"after=1", 0},
        {"after=1 return true,false", 1},
        {"after=1 return false,true", 0},
        {"for i=1,10 do yield(true,false) end after=1 return true", 1}
    };
    for (const auto &test : cases) {
        auto r = p8p_runtime_create();
        std::string source = std::string("after=0 menuitem(1,'ordinary',function(buttons) seen=buttons ") +
            test.callback + " end)";
        CHECK(load(r, source.c_str()) == 0);
        CHECK(p8p_runtime_menu_select(r, 1, 112) == test.keep_open);
        CHECK(eval(r, "after") == 1);
        CHECK(eval(r, "seen") == 112);
        CHECK(p8p_runtime_cart_request(r) == NULL);
        CHECK(p8p_runtime_error(r)[0] == '\0');
        p8p_runtime_destroy(r);
    }
    auto r = p8p_runtime_create();
    CHECK(load(r, "after=0 menuitem(1,'bounded',function() while true do after+=1 yield(true) end end)") == 0);
    CHECK(p8p_runtime_menu_select(r, 1, 112) == -1);
    CHECK(eval(r, "after>1 and after<2000 and 1 or 0") == 1);
    CHECK(strstr(p8p_runtime_error(r), "menu item") != NULL);
    CHECK(p8p_runtime_cart_request(r) == NULL);
    // A discarded ordinary callback cannot resume during a later game frame.
    int stopped = eval(r, "after");
    CHECK(p8p_runtime_step(r, 0) == 0);
    CHECK(eval(r, "after") == stopped);
    p8p_runtime_destroy(r);
}

static int context(p8p_runtime_t *r, const char *lua, const char *params, const char *back) {
    p8p_cart_t cart = {}; cart.lua = const_cast<char *>(lua); cart.lua_size = strlen(lua);
    return p8p_runtime_load_context(r, &cart, params, back);
}
static void context_and_back() {
    auto r = p8p_runtime_create();
    CHECK(load(r, "ok=stat(100)==nil and stat(6)==''") == 0);
    CHECK(eval(r, "ok and 1 or 0") == 1);
    CHECK(p8p_runtime_request_back(r) == -1);
    const char *lua = "seen=stat(6)=='hello' and stat(100)=='launcher' "
        "menuitem(1,'back',function() extcmd('go_back') seen=false end)";
    CHECK(context(r, lua, "hello", "launcher") == 0);
    CHECK(eval(r, "seen and 1 or 0") == 1);
    CHECK(strcmp(p8p_runtime_breadcrumb(r), "launcher") == 0);
    CHECK(strcmp(p8p_runtime_parameters(r), "hello") == 0);
    CHECK(p8p_runtime_menu_select(r, 1, 112) == 0);
    CHECK(eval(r, "seen and 1 or 0") == 1);
    const auto *back = p8p_runtime_cart_request(r);
    CHECK(back && back->kind == P8P_RUNTIME_CART_REQUEST_BACK);
    p8p_runtime_clear_cart_request(r);
    CHECK(p8p_runtime_request_back(r) == 0);
    p8p_runtime_clear_cart_request(r);
    CHECK(load(r, "compiled=load('return 31') value=compiled() other=loadstring('return 9')() invalid=loadstring('#target')==nil extcmd('reset')") == 0);
    CHECK(eval(r, "value+other") == 40);
    CHECK(eval(r, "invalid and 1 or 0") == 1);
    CHECK(p8p_runtime_cart_request(r) == NULL);
    // A restart with no parameters preserves both parts of the launch context.
    CHECK(context(r, "function _update() run() end", "hello", "launcher") == 0);
    CHECK(p8p_runtime_step(r, 0) == 0);
    CHECK(eval(r, "stat(6)=='hello' and stat(100)=='launcher' and 1 or 0") == 1);
    CHECK(context(r, "function _update() run('changed') end", "hello", "launcher") == 0);
    CHECK(p8p_runtime_step(r, 0) == 0);
    CHECK(eval(r, "stat(6)=='changed' and stat(100)=='launcher' and 1 or 0") == 1);
    CHECK(context(r, "ok=1", p8p_runtime_parameters(r), p8p_runtime_breadcrumb(r)) == 0);
    CHECK(eval(r, "stat(6)=='changed' and stat(100)=='launcher' and 1 or 0") == 1);
    p8p_runtime_destroy(r);
}
struct Header { unsigned char magic[8]; uint32_t version, fixed, audio, lua; };
static void state_context(const char *legacy) {
    auto r = p8p_runtime_create();
    const char *lua = "native=load back=extcmd compiled=load('return 31') value=compiled() after=0 function _update() after+=1 end";
    CHECK(context(r, lua, "hello", "launcher") == 0);
    void *data = NULL; size_t size = 0;
    int saved = p8p_runtime_save_state(r, &data, &size);
    if (saved) fprintf(stderr, "state error: %s\n", p8p_runtime_error(r));
    CHECK(saved == 0);
    if (saved || !data) { p8p_runtime_destroy(r); return; }
    CHECK(data && static_cast<Header *>(data)->version == 2);
    CHECK(context(r, lua, "other", "current parent") == 0);
    Header *saved_header = static_cast<Header *>(data);
    size_t params_offset = sizeof(Header) + saved_header->fixed + saved_header->audio;
    std::vector<unsigned char> malformed(static_cast<unsigned char *>(data),
                                         static_cast<unsigned char *>(data) + size);
    memset(malformed.data() + params_offset, 'x', 256);
    CHECK(p8p_runtime_load_state(r, malformed.data(), malformed.size()) != 0);
    memcpy(malformed.data(), data, size);
    reinterpret_cast<Header *>(malformed.data())->lua = UINT32_MAX;
    CHECK(p8p_runtime_load_state(r, malformed.data(), malformed.size()) != 0);
    CHECK(p8p_runtime_load_state(r, data, size - 1) != 0);
    CHECK(p8p_runtime_load_state(r, data, size) == 0);
    CHECK(eval(r, "stat(6)=='hello' and stat(100)=='current parent' and 1 or 0") == 1);
    CHECK(eval(r, "native==load and back==extcmd and compiled()==31 and 1 or 0") == 1);
    Header *header = static_cast<Header *>(data);
    size_t context_offset = sizeof(Header) + header->fixed + header->audio;
    std::vector<unsigned char> v1(size - 256);
    memcpy(v1.data(), data, context_offset);
    memcpy(v1.data() + context_offset, static_cast<unsigned char *>(data) + context_offset + 256,
           header->lua);
    reinterpret_cast<Header *>(v1.data())->version = 1;
    CHECK(p8p_runtime_load_state(r, v1.data(), v1.size()) == 0);
    CHECK(eval(r, "stat(6)=='' and stat(100)=='current parent' and 1 or 0") == 1);
    CHECK(load(r, lua) == 0);
    CHECK(p8p_runtime_load_state(r, data, size) == 0);
    CHECK(eval(r, "stat(6)=='hello' and stat(100)==nil and 1 or 0") == 1);
    CHECK(eval(r, "load('function _update() load(\"#restored\") after=99 end')()==nil and 1 or 0") == 1);
    CHECK(p8p_runtime_step(r, 0) == 0);
    const auto *request = p8p_runtime_cart_request(r);
    CHECK(request && strcmp(request->id, "restored") == 0);
    CHECK(eval(r, "after") == 0);
    free(data);
    // Actual pre-change binary writes this fixture privately; no game assets involved.
    if (legacy) {
        FILE *f = fopen(legacy, "rb"); CHECK(f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END); long length=ftell(f); rewind(f);
            std::vector<unsigned char> bytes((size_t)length);
            CHECK(fread(bytes.data(), 1, bytes.size(), f) == bytes.size()); fclose(f);
            CHECK(context(r, lua, "discard", "launcher") == 0);
            CHECK(p8p_runtime_load_state(r, bytes.data(), bytes.size()) == 0);
            CHECK(eval(r, "stat(6)=='' and stat(100)=='launcher' and native('return 5')()==5 and compiled()==31 and 1 or 0") == 1);
        }
    }
    p8p_runtime_destroy(r);
}



static void legacy_aliases(const char *path) {
    if (!path) return;
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file) return;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);
    std::vector<unsigned char> bytes((size_t)length);
    CHECK(fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
    fclose(file);
    auto r = p8p_runtime_create();
    CHECK(load(r, "after=0") == 0);
    const char *updates[] = {
        "function _update() resume_alias(cocreate(function() load('#target','parent','hello') end)) after=77 end",
        "function _update() protected_alias(function() table.sort({2,1},function(a,b) load('#target','parent','hello') end) end) after=77 end",
        "function _update() xprotected_alias(function() table.sort({2,1},function(a,b) load('#target','parent','hello') end) end,function(e) after=88 return e end) after=77 end"
    };
    for (const char *update : updates) {
        CHECK(p8p_runtime_load_state(r, bytes.data(), bytes.size()) == 0);
        CHECK(eval(r, "resume_alias==coresume and protected_alias==pcall and xprotected_alias==xpcall and 1 or 0") == 1);
        std::string expression = std::string("load([=[") + update + "]=])()==nil and 1 or 0";
        CHECK(eval(r, expression.c_str()) == 1);
        CHECK(p8p_runtime_step(r, 0) == 0);
        CHECK(eval(r, "after") == 0);
        const auto *request = p8p_runtime_cart_request(r);
        CHECK(request && strcmp(request->id, "target") == 0);
        CHECK(request && strcmp(request->breadcrumb, "parent") == 0);
        CHECK(request && strcmp(request->params, "hello") == 0);
        p8p_runtime_clear_cart_request(r);
    }
    p8p_runtime_destroy(r);
}
static void ordinary_coroutines_round_trip() {
    auto r = p8p_runtime_create();
    const char *lua = "resume_alias=coresume "
        "wrapped=coroutine.wrap(function(v) yield(v+1,nil,7) return 8 end) "
        "a,b,c=wrapped(3) "
        "ok,result=coroutine.resume(cocreate(function() return 17 end)) "
        "bad,message=pcall(coroutine.wrap(function() error('broken') end)) "
        "protected=coroutine.wrap(function() local ok,v=pcall(function() yield(12) return 18 end) return ok and v end) "
        "before=protected() "
        "xprotected=coroutine.wrap(function() local ok,v=xpcall(function() yield(13) return 19 end,function(e) return e end) return ok and v end) "
        "xbefore=xprotected()";
    CHECK(load(r, lua) == 0);
    CHECK(eval(r, "a==4 and b==nil and c==7 and ok and result==17 and not bad and 1 or 0") == 1);
    void *data=NULL; size_t size=0;
    int saved = p8p_runtime_save_state(r, &data, &size);
    if (saved) fprintf(stderr, "coroutine state error: %s\n", p8p_runtime_error(r));
    CHECK(saved == 0);
    if (!saved) {
        CHECK(load(r, lua) == 0);
        CHECK(p8p_runtime_load_state(r, data, size) == 0);
        CHECK(eval(r, "resume_alias==coresume and wrapped()==8 and protected()==18 and before==12 and xprotected()==19 and xbefore==13 and 1 or 0") == 1);
    }
    free(data);
    p8p_runtime_destroy(r);
}
static void restore_retires_pending_top_level() {
    auto r = p8p_runtime_create();
    const char *lua = "after=0 if stat(6)=='trigger' then load('#target') after=1 end";
    CHECK(context(r, lua, "", "parent") == 0);
    void *data = NULL; size_t size = 0;
    CHECK(p8p_runtime_save_state(r, &data, &size) == 0);
    CHECK(context(r, lua, "trigger", "parent") == 0);
    CHECK(p8p_runtime_cart_request(r) != NULL);
    CHECK(p8p_runtime_load_state(r, data, size) == 0);
    CHECK(p8p_runtime_cart_request(r) == NULL);
    CHECK(p8p_runtime_step(r, 0) == 0);
    CHECK(eval(r, "after") == 0);
    free(data);
    p8p_runtime_destroy(r);
}
static void back_boundaries() {
    const char *sources[] = {
        "after=0 extcmd('go_back') after=1",
        "after=0 function _init() extcmd('go_back') after=1 end",
        "after=0 function _update() extcmd('go_back') after=1 end",
        "after=0 function _draw() extcmd('go_back') after=1 end"
    };
    for (int i = 0; i < 4; ++i) {
        auto r = p8p_runtime_create();
        CHECK(context(r, sources[i], "", "parent") == 0);
        if (i >= 2) CHECK(p8p_runtime_step(r, 0) == 0);
        const auto *request = p8p_runtime_cart_request(r);
        CHECK(request && request->kind == P8P_RUNTIME_CART_REQUEST_BACK);
        CHECK(eval(r, "after") == 0);
        p8p_runtime_destroy(r);
    }
}
static void bounded_requests() {
    auto r = p8p_runtime_create();
    std::string id(64, 'a'), params(256, 'p');
    CHECK(load(r, ("load('#"+id+"')").c_str()) != 0);
    CHECK(p8p_runtime_cart_request(r) == NULL);
    CHECK(load(r, ("load('#target','','"+params+"')").c_str()) != 0);
    CHECK(p8p_runtime_cart_request(r) == NULL);
    CHECK(context(r, "", params.c_str(), "") != 0);
    CHECK(context(r, "", "", id.c_str()) != 0);
    id.resize(63); params.resize(255);
    CHECK(load(r, ("load('#"+id+"','"+id+"','"+params+"')").c_str()) == 0);
    auto q = p8p_runtime_cart_request(r);
    CHECK(q && strlen(q->id)==63 && strlen(q->breadcrumb)==63 && strlen(q->params)==255);
    p8p_runtime_destroy(r);
}


static void failed_load_preserves_context() {
    auto r = p8p_runtime_create();
    for (const char *bad : {"function _init() error('broken') end", "function broken("}) {
        CHECK(context(r, "after=0 function _update() after=1 end", "original", "parent") == 0);
        CHECK(context(r, bad, "wrong parameters", "wrong parent") != 0);
        CHECK(strcmp(p8p_runtime_parameters(r), "original") == 0);
        CHECK(strcmp(p8p_runtime_breadcrumb(r), "parent") == 0);
        CHECK(p8p_runtime_cart_request(r) == NULL);
        CHECK(p8p_runtime_step(r, 0) == 0);
        CHECK(p8p_runtime_error(r)[0] != '\0');
    }
    p8p_runtime_destroy(r);
}
static void capability_fixtures() {
    const char *keys[] = {"P8P_TEST_PICO_ARCADE", "P8P_TEST_TRAIL_OF_THE_SORCERER",
                          "P8P_TEST_PICO_BACKROOMS", "P8P_TEST_PERAL",
                          "P8P_TEST_PICO_NIGHT_FUNKIN"};
    for (const char *key : keys) {
        const char *path = getenv(key);
        if (!path || !*path) continue;
        p8p_cart_t cart = {};
        CHECK(p8p_cart_load_file(path, &cart) == 0);
        p8p_cart_hash_t hash;
        p8p_cart_content_hash(&cart, &hash);
        auto incompatible = p8p_cart_find_incompatibility(&hash);
        CHECK(incompatible != NULL);
        if (incompatible) {
            auto r = p8p_runtime_create();
            CHECK(p8p_runtime_load(r, &cart) == P8P_RUNTIME_INCOMPATIBLE_CART);
            p8p_runtime_set_cart_features(r, ~incompatible->required_features);
            CHECK(p8p_runtime_load(r, &cart) == P8P_RUNTIME_INCOMPATIBLE_CART);
            p8p_runtime_set_cart_features(r, UINT32_MAX);
            CHECK(p8p_runtime_load(r, &cart) == (incompatible->required_features ? 0 : P8P_RUNTIME_INCOMPATIBLE_CART));
            p8p_runtime_destroy(r);
        }
        p8p_cart_destroy(&cart);
    }
}
int main(int argc, char **argv) {
    noncontinuation(); context_and_back(); state_context(argc>1?argv[1]:NULL);
    bounded_requests(); back_boundaries(); restore_retires_pending_top_level(); capability_fixtures();
    ordinary_coroutines_round_trip(); ordinary_menu_yields(); failed_load_preserves_context();
    legacy_aliases(argc > 2 ? argv[2] : NULL);
    if (!failures) puts("multicart runtime tests passed");
    return failures ? 1 : 0;
}
