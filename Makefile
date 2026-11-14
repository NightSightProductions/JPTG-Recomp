CC      = clang
CFLAGS  = -O1 -g -arch arm64 -mmacosx-version-min=11.0 -Wno-unused-variable -Wno-unused-but-set-variable -Wno-deprecated-declarations
GEN_SRC = $(wildcard gen/code_*.c) gen/tables.c
RT_SRC  = runtime/rt.c runtime/shims.c runtime/sys.c runtime/cxx.c runtime/libc2.c runtime/platform.c runtime/steam.c runtime/fmod.c runtime/vorbis.c
OBJ     = $(patsubst %.c,build/%.o,$(GEN_SRC) $(RT_SRC))

all: build/test_lua

build/%.o: %.c runtime/rt.h runtime/tables.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/test_lua: $(OBJ) build/runtime/test_lua.o
	$(CC) $(CFLAGS) $^ -lz -lc++ -framework CoreFoundation -framework ApplicationServices -framework AudioToolbox -framework CoreAudio -o $@

gen:
	.venv/bin/python tools/recomp.py b1cf2 d5c66 gen

clean:
	rm -rf build

.PHONY: all gen clean

# ---- whole-binary build ----
FULL_SRC = $(wildcard gen_full/code_*.c) gen_full/tables.c
FULL_OBJ = $(patsubst %.c,build/%.o,$(FULL_SRC))

full-gen:
	.venv/bin/python tools/recomp.py 2600 49d140 gen_full

build/libjp.a: $(FULL_OBJ)
	ar rcs $@ $^

full: build/libjp.a

.PHONY: full full-gen

build/boot: build/libjp.a $(patsubst %.c,build/%.o,$(RT_SRC)) build/runtime/boot.o
	$(CC) $(CFLAGS) build/runtime/boot.o $(patsubst %.c,build/%.o,$(RT_SRC)) build/libjp.a -lz -lc++ -framework CoreFoundation -framework ApplicationServices -framework AudioToolbox -framework CoreAudio -o $@

# ---- native app ----
HOST_SRC = $(RT_SRC) runtime/gl.c gen_host/gl_gen.c
HOST_OBJ = $(patsubst %.c,build/%.o,$(HOST_SRC)) build/runtime/app.o
FRAMEWORKS = -framework CoreFoundation -framework ApplicationServices -framework Cocoa -framework OpenGL -framework AudioToolbox -framework CoreAudio

gen_host/gl_gen.c: tools/glgen.py
	@mkdir -p gen_host
	.venv/bin/python tools/glgen.py $@

build/runtime/app.o: runtime/app.m runtime/rt.h runtime/tables.h
	@mkdir -p build/runtime
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@

build/JurassicPark: build/libjp.a $(HOST_OBJ)
	$(CC) $(CFLAGS) $(HOST_OBJ) build/libjp.a -lz -lc++ $(FRAMEWORKS) -o $@

app: build/JurassicPark
.PHONY: app

# ---- debug build with guest write checking (JP_MC_LO/HI/ARM) ----
MC_OBJ = $(patsubst %.c,build-mc/%.o,$(FULL_SRC) $(HOST_SRC))
build-mc/%.o: %.c runtime/rt.h runtime/tables.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMEMCHECK -c $< -o $@
build-mc/runtime/app.o: runtime/app.m runtime/rt.h runtime/tables.h
	@mkdir -p build-mc/runtime
	$(CC) $(CFLAGS) -DMEMCHECK -fobjc-arc -c $< -o $@
build-mc/JurassicPark: $(MC_OBJ) build-mc/runtime/app.o
	$(CC) $(CFLAGS) $^ -lz -lc++ $(FRAMEWORKS) -o $@
memcheck: build-mc/JurassicPark
.PHONY: memcheck

# ---- double-clickable app bundle (uses the user's own game install for data and icon) ----
GAME ?= $(HOME)/Library/Application Support/Steam/steamapps/common/JurassicPark/JurassicPark100.app
bundle: build/JurassicPark packaging/Info.plist
	rm -rf "build/Jurassic Park The Game.app"
	mkdir -p "build/Jurassic Park The Game.app/Contents/MacOS" "build/Jurassic Park The Game.app/Contents/Resources"
	cp build/JurassicPark "build/Jurassic Park The Game.app/Contents/MacOS/JurassicPark"
	cp packaging/Info.plist "build/Jurassic Park The Game.app/Contents/Info.plist"
	-cp "$(GAME)/Contents/Resources/GameApp.icns" "build/Jurassic Park The Game.app/Contents/Resources/"
	codesign --force --sign - "build/Jurassic Park The Game.app"
.PHONY: bundle
