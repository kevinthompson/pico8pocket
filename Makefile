PROJECT_ROOT := $(abspath $(CURDIR))
override SDK_ROOT := $(abspath $(if $(strip $(SDK_ROOT)),$(SDK_ROOT),$(PROJECT_ROOT)/.deps/openfpgaos-sdk))
FAKE08_ROOT  := $(PROJECT_ROOT)/.deps/fake-08
APP_DIR      := $(PROJECT_ROOT)/src/app
BUILD_DIR    := $(PROJECT_ROOT)/build
TEST_BIN     := $(BUILD_DIR)/tests/pico8pocket_tests
RUNTIME_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_runtime_tests
DELEGATE_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_delegate_tests
COMPAT_SCAN_BIN := $(BUILD_DIR)/tools/pico8pocket_compat_scan
APP_LOAD_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_app_load_tests
APP_MULTICART_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_app_multicart_tests
APP_LOAD_TEST_OBJECTS := $(addprefix $(BUILD_DIR)/tests/app-load/,$(addsuffix .o,main state_store settings menu system_input scheduler multicart))
BBS_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_bbs_tests
MULTICART_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_multicart_tests
MULTICART_RUNTIME_TEST_BIN := $(BUILD_DIR)/tests/pico8pocket_multicart_runtime_tests
LEGACY_RUNTIME_STATE := $(BUILD_DIR)/tests/runtime-v1.state
LEGACY_ALIAS_STATE := $(BUILD_DIR)/tests/runtime-v1-aliases.state
BUNDLE_LOCAL_CARTS ?= 0
export BUNDLE_LOCAL_CARTS
Z8LUA_ROOT   := $(FAKE08_ROOT)/libs/z8lua
MINIZ_ROOT   := $(FAKE08_ROOT)/libs/miniz

include $(PROJECT_ROOT)/mk/z8lua.mk

HOST_RUNTIME_SOURCES := src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c \
	src/runtime/cart_png.c $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c
HOST_RUNTIME_FLAGS := -x c++ -std=c++17 -O2 -fwrapv -Wall -Wextra \
	-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
	-Iinclude -I$(Z8LUA_ROOT) -I$(MINIZ_ROOT)
HOST_CART_HEADERS := src/runtime/runtime_internal.h $(wildcard include/p8p/*.h) include/p8p/incompatible_carts.inc include/p8p/bbs_catalog.inc

.DEFAULT_GOAL := test

deps:
	@./scripts/fetch-deps.sh

$(TEST_BIN): tests/test_main.c src/app/system_input.c src/app/scheduler.c src/runtime/cart.c src/runtime/cart_png.c \
		src/runtime/display.c include/p8p/cart.h include/p8p/display.h \
		$(MINIZ_ROOT)/miniz.c include/p8p/incompatible_carts.inc
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-strict-prototypes -pedantic \
		-Iinclude -I$(MINIZ_ROOT) \
		tests/test_main.c src/app/system_input.c src/app/scheduler.c src/runtime/cart.c src/runtime/cart_png.c \
		src/runtime/display.c $(MINIZ_ROOT)/miniz.c -o $@

$(RUNTIME_TEST_BIN): tests/runtime_test.cpp src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c src/app/state_store.c src/app/settings.c src/app/menu.c \
		src/runtime/cart_png.c include/p8p/runtime.h src/runtime/runtime_internal.h include/p8p/cart.h \
		$(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c include/p8p/incompatible_carts.inc
	@mkdir -p $(dir $@)
	$(CXX) -x c++ -std=c++17 -O2 -fwrapv -Wall -Wextra \
		-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
		-Iinclude -I$(Z8LUA_ROOT) -I$(MINIZ_ROOT) \
		tests/runtime_test.cpp src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c src/app/state_store.c src/app/settings.c src/app/menu.c \
		src/runtime/cart_png.c $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c -lm -o $@

$(COMPAT_SCAN_BIN): tools/compat_scan.cpp src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c \
		src/runtime/cart_png.c include/p8p/runtime.h src/runtime/runtime_internal.h include/p8p/cart.h \
		src/runtime/bbs.c src/app/multicart.c $(HOST_CART_HEADERS) $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c
	@mkdir -p $(dir $@)
	$(CXX) -x c++ -std=c++17 -O2 -fwrapv -DP8P_RUNTIME_DEBUG -Wall -Wextra \
		-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
		-Iinclude -I$(Z8LUA_ROOT) -I$(MINIZ_ROOT) \
		tools/compat_scan.cpp src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c \
		src/runtime/cart_png.c src/runtime/bbs.c src/app/multicart.c $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c -lm -o $@

compatibility-check:
	python3 scripts/incompatible-carts.py --check
	python3 scripts/offline-carts.py --check
	python3 -m unittest discover -s tests -p 'test_compatibility_registry.py'
	python3 -m unittest discover -s tests -p 'test_offline_catalog.py'

$(BBS_TEST_BIN): tests/bbs_test.c src/runtime/bbs.c src/runtime/cart.c src/runtime/cart_png.c $(HOST_CART_HEADERS) $(MINIZ_ROOT)/miniz.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -I$(MINIZ_ROOT) \
		tests/bbs_test.c src/runtime/bbs.c src/runtime/cart.c src/runtime/cart_png.c $(MINIZ_ROOT)/miniz.c -o $@

$(MULTICART_RUNTIME_TEST_BIN): tests/multicart_runtime_test.cpp $(HOST_RUNTIME_SOURCES) $(HOST_CART_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_RUNTIME_FLAGS) -DP8P_RUNTIME_DEBUG tests/multicart_runtime_test.cpp $(HOST_RUNTIME_SOURCES) -lm -o $@

$(BUILD_DIR)/tests/%.state: tests/fixtures/%.state.z
	@mkdir -p $(dir $@)
	python3 -c 'import pathlib,zlib; pathlib.Path("$@").write_bytes(zlib.decompress(pathlib.Path("$<").read_bytes()))'

$(MULTICART_TEST_BIN): tests/multicart_test.cpp src/app/multicart.c src/runtime/bbs.c $(HOST_RUNTIME_SOURCES) $(HOST_CART_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_RUNTIME_FLAGS) -DP8P_RUNTIME_DEBUG tests/multicart_test.cpp \
		src/app/multicart.c src/runtime/bbs.c $(HOST_RUNTIME_SOURCES) -lm -o $@

$(BUILD_DIR)/tests/app-load/%.o: src/app/%.c $(wildcard include/p8p/*.h)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Iinclude -I$(MINIZ_ROOT) \
		$(if $(filter main,$*),-DOF_PC -Dmain=p8p_app_main) -c $< -o $@

$(APP_LOAD_TEST_BIN): tests/app_load_test.cpp $(APP_LOAD_TEST_OBJECTS) \
		src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c src/runtime/cart_png.c \
		src/runtime/bbs.c $(HOST_CART_HEADERS) $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c
	@mkdir -p $(dir $@)
	$(CXX) -x c++ -std=c++17 -O2 -fwrapv -Wall -Wextra \
		-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
		-Iinclude -I$(Z8LUA_ROOT) -I$(MINIZ_ROOT) \
		tests/app_load_test.cpp src/runtime/runtime.cpp src/runtime/palette_fast.cpp src/runtime/vm_delegate.cpp src/runtime/audio.c src/runtime/cart.c \
		src/runtime/cart_png.c src/runtime/bbs.c $(Z8LUA_SOURCES) $(MINIZ_ROOT)/miniz.c \
		-x none $(APP_LOAD_TEST_OBJECTS) -lm -o $@

$(APP_MULTICART_TEST_BIN): tests/app_multicart_test.cpp $(APP_LOAD_TEST_OBJECTS) src/runtime/bbs.c $(HOST_RUNTIME_SOURCES) $(HOST_CART_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_RUNTIME_FLAGS) tests/app_multicart_test.cpp src/runtime/bbs.c $(HOST_RUNTIME_SOURCES) \
		-x none $(APP_LOAD_TEST_OBJECTS) -lm -o $@

$(DELEGATE_TEST_BIN): tests/delegate_test.cpp tests/delegate_thunk.cpp src/runtime/vm_delegate.cpp $(Z8LUA_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) -x c++ -std=c++17 -O2 -fwrapv -Wall -Wextra \
		-Wno-deprecated-declarations -Wno-unused-function -Wno-unused-parameter \
		-I$(Z8LUA_ROOT) tests/delegate_test.cpp tests/delegate_thunk.cpp src/runtime/vm_delegate.cpp $(Z8LUA_SOURCES) -lm -o $@


test: deps compatibility-check $(DELEGATE_TEST_BIN) $(TEST_BIN) $(RUNTIME_TEST_BIN) $(APP_LOAD_TEST_BIN) \
	$(BBS_TEST_BIN) $(MULTICART_RUNTIME_TEST_BIN) $(MULTICART_TEST_BIN) $(APP_MULTICART_TEST_BIN) $(COMPAT_SCAN_BIN) $(LEGACY_RUNTIME_STATE) $(LEGACY_ALIAS_STATE)
	$(TEST_BIN)
	$(RUNTIME_TEST_BIN)
	$(DELEGATE_TEST_BIN)
	$(APP_LOAD_TEST_BIN)
	$(BBS_TEST_BIN)
	$(MULTICART_RUNTIME_TEST_BIN) $(LEGACY_RUNTIME_STATE) $(LEGACY_ALIAS_STATE)
	$(MULTICART_TEST_BIN)
	$(APP_MULTICART_TEST_BIN)
	$(APP_MULTICART_TEST_BIN) --missing
	python3 -m unittest discover -s tests -p 'test_compat_scan_multicart.py'

compat-scan: deps compatibility-check $(COMPAT_SCAN_BIN)

# Priority regression check on local Celeste carts (assets/cards/celeste*).
celeste: compat-scan
	@./scripts/celeste-check.sh

celeste-baseline: compat-scan
	@./scripts/celeste-check.sh --update

pocket-elf: deps compatibility-check
	$(MAKE) -C $(APP_DIR) \
		PROJECT_ROOT=$(PROJECT_ROOT) \
		SDK_ROOT=$(SDK_ROOT) \
		FAKE08_ROOT=$(FAKE08_ROOT)

pocket: pocket-elf
	@SDK_ROOT=$(SDK_ROOT) ./scripts/assemble-pocket.sh

validate: pocket
	@./scripts/validate-pocket.sh

package: validate
	@./scripts/package-pocket.sh

# Explicit opt-in for private test builds.  Public packages never include
# locally supplied cartridges.
package-local:
	$(MAKE) package BUNDLE_LOCAL_CARTS=1

clean:
	$(MAKE) -C $(APP_DIR) clean PROJECT_ROOT=$(PROJECT_ROOT) SDK_ROOT=$(SDK_ROOT) 2>/dev/null || true
	rm -rf $(BUILD_DIR) $(PROJECT_ROOT)/.obj

.PHONY: deps test compatibility-check compat-scan celeste celeste-baseline pocket-elf pocket validate package package-local clean

# Host-side offline collection tools; no private cartridge dependency.
offline-plugin-test:
	cargo test --locked --manifest-path tools/pocket-sync/Cargo.toml
	cargo clippy --locked --manifest-path tools/pocket-sync/Cargo.toml --all-targets -- -D warnings

offline-plugin-tools:
	./scripts/build-tools.sh

.PHONY: offline-plugin-test offline-plugin-tools
