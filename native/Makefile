# native/Makefile — Dokuro-chan script editor (Dear ImGui + DX11, Win7, 32-bit PE)
#
#   nix develop --command make -C native tests    # host test binary -> build/run_tests
#   nix develop --command make -C native          # cross exe -> build/dokuro-editor.exe
#   nix develop --command make -C native selftest # build + run exe --selftest (needs Windows host or wine)
#
# 32-bit PE mirrors tools/res_explorer in ../OpenSummoners (runs on Win7 32/64).
# IMGUI_DIR comes from the nix flake (pkgs.imgui sources), same as res_explorer.
#
# Staleness hardening (do not remove — this is load-bearing):
#   1. CONFIG_STAMP — every object depends on a fingerprint of the toolchain
#      paths/versions, IMGUI_DIR, and ALL compile/link flags. Any change
#      invalidates every object -> full rebuild. Objects compiled against
#      different headers/configs can never be mixed into one link again
#      (that mixed-build state caused the editor's startup crash and the
#      imgui "Mismatched version string!" assert).
#   2. -MMD -MP header deps — a header edit rebuilds every object that
#      includes it (the old Makefile had NO header deps at all; stale
#      objects after the UndoOp/project.h ABI changes were the crash).
#   3. IMGUI_DIR guard — running make outside `nix develop` fails loudly
#      instead of silently building against a wrong/absent imgui tree.

ifeq ($(strip $(IMGUI_DIR)),)
$(error IMGUI_DIR is not set. Run inside the flake dev shell: nix develop --command make -C native)
endif

# Disable make's built-in implicit rules (MAKEFLAGS -r). The built-in
# `%: %.o` rule makes a missing auto-dep file (obj/win/foo.d) look like it
# can be "remade" from foo.d.o — which our `%.o` pattern rules then try to
# compile from a nonexistent foo.d.cpp, spamming failed targets and
# read-only-store rm/unlink noise. All compile/link rules are explicit.
MAKEFLAGS += -r

WINCC   := i686-w64-mingw32-gcc
WINCXX  := i686-w64-mingw32-g++
CC      := gcc
CXX     := g++
OBJ     := obj

CDEFS    := -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE
# -MMD -MP: emit header dependency files next to each object (see DEPS below).
DEPFLAGS := -MMD -MP
CFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Icore -Icore/vendor $(DEPFLAGS)
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Icore -Icore/vendor $(DEPFLAGS)

# -fstack-protector-all: stack canaries in the shipped exe (mingw has no
# ASan runtime in this nixpkgs; host ASan coverage comes via `make asan`).
WCFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fstack-protector-all -I. -Icore -Icore/vendor $(CDEFS) $(DEPFLAGS)
WCXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fstack-protector-all \
             -I. -Icore -Icore/vendor -Iapp -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMGUI_DIR)/misc/cpp $(CDEFS) $(DEPFLAGS)
WLDFLAGS  := -mwindows -static -static-libgcc -static-libstdc++
WLIBS     := -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 -lkernel32 -lwinmm \
             -lcomdlg32 -lshell32 -lole32 -ldwmapi

# ---- build-config fingerprint (stale-object insurance) ----
# md5 of every input that affects code generation: resolved tool paths +
# versions, IMGUI_DIR, all flags, and env vars that can leak include/lib
# paths into the toolchain. Written to obj/.build_config; when the content
# changes the stamp's mtime advances and make rebuilds every object.
BUILD_FINGERPRINT := $(shell printf '%s\n' \
  "WINCC=$(WINCC) @$(shell command -v $(WINCC) 2>/dev/null) $(shell $(WINCC) --version 2>/dev/null | head -1)" \
  "WINCXX=$(WINCXX) @$(shell command -v $(WINCXX) 2>/dev/null) $(shell $(WINCXX) --version 2>/dev/null | head -1)" \
  "CC=$(CC) @$(shell command -v $(CC) 2>/dev/null) $(shell $(CC) --version 2>/dev/null | head -1)" \
  "CXX=$(CXX) @$(shell command -v $(CXX) 2>/dev/null) $(shell $(CXX) --version 2>/dev/null | head -1)" \
  "IMGUI_DIR=$(IMGUI_DIR)" \
  "CDEFS=$(CDEFS)" "CFLAGS=$(CFLAGS)" "CXXFLAGS=$(CXXFLAGS)" \
  "WCFLAGS=$(WCFLAGS)" "WCXXFLAGS=$(WCXXFLAGS)" \
  "WLDFLAGS=$(WLDFLAGS)" "WLIBS=$(WLIBS)" \
  "ENV CPATH=$(CPATH) CPLUS_INCLUDE_PATH=$(CPLUS_INCLUDE_PATH) LIBRARY_PATH=$(LIBRARY_PATH) NIX_CFLAGS_COMPILE=$(NIX_CFLAGS_COMPILE) NIX_LDFLAGS=$(NIX_LDFLAGS)" \
  | md5sum | cut -d' ' -f1)

CONFIG_STAMP := $(OBJ)/.build_config

FORCE:

$(CONFIG_STAMP): FORCE
	@mkdir -p $(OBJ)
	@printf '%s\n' '$(BUILD_FINGERPRINT)' > $@.tmp
	@if cmp -s $@ $@.tmp 2>/dev/null; then rm -f $@.tmp; \
	else mv -f $@.tmp $@; echo "build config changed -> full rebuild ($(BUILD_FINGERPRINT))"; fi

CORE_C    := core/vendor/miniz.c core/vendor/miniz_tdef.c core/vendor/miniz_tinfl.c
CORE_CPP  := core/utf8.cpp core/cp932.cpp core/text.cpp core/uni2.cpp core/stcm2.cpp \
             core/project.cpp core/asmfmt.cpp core/undo.cpp core/iso.cpp
APP_CPP   := app/main.cpp app/app.cpp app/win32_util.cpp app/settings.cpp \
             app/project_ui.cpp app/editor_ui.cpp app/asm_ui.cpp app/selftest.cpp \
             app/uitest.cpp
TESTS_CPP := tests/test_main.cpp

IMGUI_CPP := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
             $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
             $(IMGUI_DIR)/misc/cpp/imgui_stdlib.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_win32.cpp $(IMGUI_DIR)/backends/imgui_impl_dx11.cpp

HOST_COBJ  := $(OBJ)/host/vendor_miniz.o $(OBJ)/host/miniz_tdef.o $(OBJ)/host/miniz_tinfl.o
HOST_CXXOBJ:= $(addprefix $(OBJ)/host/, $(notdir $(CORE_CPP:.cpp=.o)))
TEST_OBJ   := $(addprefix $(OBJ)/host/, $(notdir $(TESTS_CPP:.cpp=.o)))
WIN_COBJ   := $(OBJ)/win/vendor_miniz.o $(OBJ)/win/miniz_tdef.o $(OBJ)/win/miniz_tinfl.o
WIN_CXXOBJ := $(addprefix $(OBJ)/win/, $(notdir $(CORE_CPP:.cpp=.o)) $(notdir $(APP_CPP:.cpp=.o)))
WIN_IMGUI  := $(addprefix $(OBJ)/win/, $(notdir $(IMGUI_CPP:.cpp=.o)))

# The first rule in the file is the default goal; the vendor_miniz rule below
# would otherwise make bare `make` build only that one object. Force `all`.
.DEFAULT_GOAL := all

ALL_OBJ := $(HOST_COBJ) $(HOST_CXXOBJ) $(TEST_OBJ) $(WIN_COBJ) $(WIN_CXXOBJ) $(WIN_IMGUI)

# Every object is invalidated by any build-config change (see CONFIG_STAMP).
$(ALL_OBJ): $(CONFIG_STAMP)

# Auto-generated header dependencies (-MMD -MP). Included at parse time; if a
# listed header disappears (e.g. a nix store path rotated), the rule below
# makes make treat it as absent instead of erroring — the config stamp has
# already forced a full rebuild in that case, so deps regenerate before use.
DEPS := $(ALL_OBJ:.o=.d)
-include $(DEPS)
/nix/store/%: ;

$(OBJ)/host/vendor_miniz.o: core/vendor/miniz.c
	@mkdir -p $(OBJ)/host
	$(CC) $(CFLAGS) -c core/vendor/miniz.c -o $@
$(OBJ)/host/%.o: core/vendor/%.c
	@mkdir -p $(OBJ)/host
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/win/vendor_miniz.o: core/vendor/miniz.c
	@mkdir -p $(OBJ)/win
	$(WINCC) $(WCFLAGS) -c core/vendor/miniz.c -o $@
$(OBJ)/win/%.o: core/vendor/%.c
	@mkdir -p $(OBJ)/win
	$(WINCC) $(WCFLAGS) -c $< -o $@

OUT_DIR := ../build
OUT     := $(OUT_DIR)/dokuro-editor.exe
TESTS   := $(OUT_DIR)/run_tests

.PHONY: all clean tests asan selftest sign

all: $(OUT)

tests: $(TESTS)

# ---- sanitizer build of the host test suite (Linux) ----
# Catches memory bugs in core code fast: -fsanitize=address,undefined. The
# object dir is separate (obj/asan) so the normal build is untouched.
ASAN_OBJ  := obj/asan
# Same stale-object insurance as the main build: -MMD -MP header deps + the
# config stamp below. Without them, `make asan` after a core/test edit
# silently links stale objects (false confidence in a sanitizer run).
ASAN_FLAGS := -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter -Icore -Icore/vendor $(DEPFLAGS)
ASAN_SRCS := $(CORE_CPP) $(TESTS_CPP) core/vendor/miniz.c core/vendor/miniz_tdef.c core/vendor/miniz_tinfl.c
ASAN_OBJS := $(patsubst %.cpp,$(ASAN_OBJ)/%.o,$(patsubst %.c,$(ASAN_OBJ)/%.o,$(ASAN_SRCS)))
ASAN_TESTS := $(OUT_DIR)/run_tests_asan

-include $(ASAN_OBJS:.o=.d) # header deps for the sanitizer objects (see DEPS above)

$(ASAN_OBJS): $(CONFIG_STAMP)

$(ASAN_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ASAN_FLAGS) -c $< -o $@
$(ASAN_OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ASAN_FLAGS) -c $< -o $@

asan: $(ASAN_TESTS)

$(ASAN_TESTS): $(ASAN_OBJS)
	@mkdir -p $(OUT_DIR)
	$(CXX) $(ASAN_FLAGS) -o $@ $(ASAN_OBJS)

selftest: $(OUT)
	$(OUT) --selftest ../game-files/SCRIPT.UNI

# Authenticode-signs the exe with a self-signed cert (generated on first run).
# A signature — even self-signed — suppresses the Windows 7 "Open File -
# Security Warning" dialog for files with a Mark-of-the-Web zone identifier.
# (Win8+/SmartScreen still shows its blue "unknown publisher" prompt once per
# machine; a real code-signing cert from a CA is needed to remove that.)
CERT := $(OUT_DIR)/dokuro_signing.crt
KEY  := $(OUT_DIR)/dokuro_signing.key

sign: $(OUT)
	@test -f $(CERT) -a -f $(KEY) || \
		openssl req -x509 -newkey rsa:2048 -keyout $(KEY) -out $(CERT) \
			-days 3650 -nodes -subj "/CN=Dokuro Translation Toolkit" \
			-addext "extendedKeyUsage=codeSigning" 2>/dev/null
	osslsigncode sign -certs $(CERT) -key $(KEY) \
		-n "Dokuro-chan Script Editor" -t http://timestamp.digicert.com \
		-in $(OUT) -out $(OUT).signed
	mv $(OUT).signed $(OUT)
	@echo "signed: $(OUT)"

# ---- host test binary (Linux, exercises the same core sources) ----
$(OBJ)/host/%.o: core/%.cpp
	@mkdir -p $(OBJ)/host
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/host/%.o: core/%.c
	@mkdir -p $(OBJ)/host
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/host/%.o: tests/%.cpp
	@mkdir -p $(OBJ)/host
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TESTS): $(HOST_COBJ) $(HOST_CXXOBJ) $(TEST_OBJ)
	@mkdir -p $(OUT_DIR)
	$(CXX) -o $@ $(HOST_COBJ) $(HOST_CXXOBJ) $(TEST_OBJ)

# ---- Windows exe (i686 mingw cross) ----
$(OBJ)/win/%.o: core/%.cpp
	@mkdir -p $(OBJ)/win
	$(WINCXX) $(WCXXFLAGS) -c $< -o $@
$(OBJ)/win/%.o: core/%.c
	@mkdir -p $(OBJ)/win
	$(WINCC) $(WCFLAGS) -c $< -o $@
$(OBJ)/win/%.o: app/%.cpp
	@mkdir -p $(OBJ)/win
	$(WINCXX) $(WCXXFLAGS) -c $< -o $@
$(OBJ)/win/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(OBJ)/win
	$(WINCXX) $(WCXXFLAGS) -c $< -o $@
$(OBJ)/win/%.o: $(IMGUI_DIR)/misc/cpp/%.cpp
	@mkdir -p $(OBJ)/win
	$(WINCXX) $(WCXXFLAGS) -c $< -o $@
$(OBJ)/win/%.o: $(IMGUI_DIR)/backends/%.cpp
	@mkdir -p $(OBJ)/win
	$(WINCXX) $(WCXXFLAGS) -c $< -o $@

$(OUT): $(WIN_COBJ) $(WIN_CXXOBJ) $(WIN_IMGUI)
	@mkdir -p $(OUT_DIR)
	$(WINCXX) $(WLDFLAGS) -o $@ $(WIN_COBJ) $(WIN_CXXOBJ) $(WIN_IMGUI) $(WLIBS)

clean:
	rm -rf $(OBJ) $(OUT) $(TESTS)
