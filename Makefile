# Jurassic Park: The Game on Recomp-Framework (framework/, a git submodule): the 32-bit Intel Mac release statically
# recompiled to native arm64 (framework/mac32).
#   make full-gen          translate the whole executable (orig.bin -> gen_full/)
#   make -j8 full app      build build/JurassicPark
#   make bundle GAME=...   build/Jurassic Park The Game.app (icon from the user's copy at GAME)
#   make memcheck          a build that reports guest writes to a chosen range (RT32_MC_LO/HI/ARM)
#   make test-gen && make && tests/run.sh   Lua regression test on a partial translation (gen/)
FW          = framework
PYTHON      = .venv/bin/python
BUILD       = build
GAME_BIN    = orig.bin
TEXT_LO     = 2600
TEXT_HI     = 49d140
GEN_DIR     = gen_full
HLE         = tools/hle.txt
APP_NAME    = JurassicPark
BUNDLE_NAME = Jurassic Park The Game
INFO_PLIST  = packaging/Info.plist
GAME       ?= $(HOME)/Library/Application Support/Steam/steamapps/common/JurassicPark/JurassicPark100.app
GAME_ICON   = $(GAME)/Contents/Resources/GameApp.icns
GAME_SRC    = src/game.c src/engine.c

all: $(BUILD)/test_lua

include $(FW)/mac32/mac32.mk

full-gen: gen
full: $(BUILD)/libgame32.a
.PHONY: all full-gen full test-gen test

# ---- Lua regression test: the game's embedded zlib + Lua 5.1 VM, translated on their own ----
TEST_LO = b1cf2
TEST_HI = d5c66
TEST_SRC = $(wildcard gen/code_*.c) gen/tables.c
TEST_OBJ = $(patsubst %.c,$(BUILD)/test/%.o,$(TEST_SRC))
TEST_RT  = $(patsubst %.c,$(BUILD)/m32/%.o,rt.c shims.c sys.c cxx.c libc2.c cf.c steam.c fmod.c vorbis.c)
test-gen:
	$(PYTHON) $(M32)/tools/recomp32.py $(GAME_BIN) $(TEST_LO) $(TEST_HI) gen --hle $(HLE)
$(BUILD)/test/%.o: %.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_lua: $(TEST_OBJ) $(TEST_RT) $(BUILD)/game/src/game.o $(BUILD)/test/tests/test_lua.o
	$(CC) $(CFLAGS) $^ -lz -lc++ -framework CoreFoundation -framework ApplicationServices -framework AudioToolbox \
	    -framework CoreAudio -o $@
test: $(BUILD)/test_lua
	tests/run.sh
