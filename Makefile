ASAR := tools/asar
ROM  := build/sbm2.sfc

.PHONY: all compare disasm coverage clean verify trace verify-hybrid
all: compare

$(ROM): $(wildcard src/*.asm)
	@mkdir -p build
	head -c 1048576 /dev/zero > $@.tmp
	$(ASAR) --no-title-check src/main.asm $@.tmp
	mv $@.tmp $@

compare: $(ROM)
	@cmp $(ROM) baserom.sfc && echo "OK: $(ROM) matches baserom.sfc"

# Regenerates src/ from the ROM; overwrites any hand edits.
disasm:
	python3 -I tools/sbm2/disasm.py baserom.sfc tools/sbm2/hints.txt src $(wildcard coverage/*.cdl)

clean:
	rm -rf build

# Headless attract-mode run; writes a CDL into coverage/.
FRAMES ?= 1800
coverage:
	cd tools/mesen2 && SBM2_FRAMES=$(FRAMES) SBM2_CDL_OUT=$(CURDIR)/coverage/attract_$(FRAMES)f.cdl \
		./Mesen --testrunner --timeout=86400 $(CURDIR)/baserom.sfc $(CURDIR)/bot/cdl_dump.lua

# C port runtime: 65816 interpreter + bus, checked against a Mesen WRAM trace.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
PORT_INPUT  ?= port/input.txt
PORT_TRACE  ?= build/trace/ref.bin
PORT_FRAMES ?= 6000
build/port/verify: port/verify.c port/snes.c port/cpu.c port/ppu.c port/snes.h port/cpu.h
	@mkdir -p build/port
	$(CC) $(CFLAGS) -o $@ port/verify.c port/snes.c port/cpu.c port/ppu.c

# Mesen reference: WRAM at each logic frame for PORT_INPUT.
trace: $(PORT_TRACE)
$(PORT_TRACE): $(PORT_INPUT) port/trace.lua
	@mkdir -p $(dir $@)
	port/trace.sh $(PORT_INPUT) $@ $(PORT_FRAMES)

verify: build/port/verify $(PORT_TRACE)
	build/port/verify baserom.sfc $(PORT_INPUT) $(PORT_TRACE) $(VERIFY_FLAGS)

# Hybrid port: interpreter vs interpreter + native routines, frame-step mode.
HYBRID_SRC := port/hybrid.c port/hook.c port/game.c port/snes.c port/cpu.c port/ppu.c
build/port/hybrid: $(HYBRID_SRC) port/hook.h port/snes.h port/cpu.h port/ppu.h
	@mkdir -p build/port
	$(CC) $(CFLAGS) -o $@ $(HYBRID_SRC)

# Front-end: the hybrid with the PPU drawing (SDL2 window if available).
PLAY_SRC := port/play.c port/ppu.c port/hook.c port/game.c port/snes.c port/cpu.c
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
build/port/play: $(PLAY_SRC) port/hook.h port/snes.h port/cpu.h port/ppu.h
	@mkdir -p build/port
	$(CC) $(CFLAGS) $(if $(SDL_LIBS),-DHAVE_SDL $(SDL_CFLAGS)) -o $@ $(PLAY_SRC) $(SDL_LIBS)

# Same as port/input.txt but places bombs (A) in game, so blasts get exercised.
HYBRID_INPUT ?= port/input_hybrid.txt
verify-hybrid: build/port/hybrid
	build/port/hybrid baserom.sfc $(HYBRID_INPUT) $(HYBRID_FLAGS)
