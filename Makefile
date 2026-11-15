ASAR := tools/asar
ROM  := build/sbm2.sfc

.PHONY: all compare disasm coverage clean verify trace verify-hybrid bot-test planner-test
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

# Standalone Lua 5.4 (Mesen's bundled sources) for the bot's offline tests.
LUA_SRC := tools/mesen2-src/Lua
LUA_CORE := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate \
	lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loslib \
	lstrlib ltablib lutf8lib loadlib linit
build/tools/lua: tools/lua/main.c $(LUA_CORE:%=$(LUA_SRC)/%.c)
	@mkdir -p build/tools
	$(CC) -O2 -std=gnu99 -DLUA_USE_LINUX -I$(LUA_SRC) -o $@ $^ -lm -ldl

# The bot on the port (frame-step, no Mesen): bot/drive.py --port.
# (the core as one unit, port/core_unity.c: the CPU's bus calls inline)
BOTRUN_SRC := port/botrun.c port/core_unity.c
build/port/botrun: $(BOTRUN_SRC) port/snes.c port/cpu.c port/ppu.c port/snes.h port/cpu.h port/ppu.h port/path_pad.h $(LUA_CORE:%=$(LUA_SRC)/%.c)
	@mkdir -p build/port
	$(CC) -O2 -std=gnu99 -DLUA_USE_LINUX -I$(LUA_SRC) -Iport -o $@ $(BOTRUN_SRC) $(LUA_CORE:%=$(LUA_SRC)/%.c) -lm -ldl -lpthread

# botrun with the C++ agent built in (emu.agent): bot/agent.lua drives the
# game with planner/agent.cpp. C objects via $(CC), linked with $(CXX).
AGENT_CXX := $(filter-out planner/test/%,$(wildcard planner/*.cpp))
AGENT_OBJ := build/port/agentobj
# The oracle cache's key (SBM2_AGCACHE): whatever decides a run's outcome.
AGENT_CORE_ID = $(shell cat port/snes.c port/cpu.c port/ppu.c port/*.h port/core_unity.c port/botrun.c planner/agent_api.h | md5sum | cut -c1-16)
build/port/agentrun: $(BOTRUN_SRC) port/snes.c port/cpu.c port/ppu.c port/*.h planner/*.cpp planner/*.hpp planner/agent_api.h
	@mkdir -p $(AGENT_OBJ)
	$(CC) -O2 -std=gnu99 -DLUA_USE_LINUX -DSBM2_AGENT -DSBM2_CORE_ID=\"$(AGENT_CORE_ID)\" -I$(LUA_SRC) -Iport -Iplanner -c port/botrun.c -o $(AGENT_OBJ)/botrun.o
	$(CC) -O2 -std=gnu99 -Iport -c port/core_unity.c -o $(AGENT_OBJ)/core.o
	@for f in $(LUA_CORE); do [ $(AGENT_OBJ)/lua_$$f.o -nt $(LUA_SRC)/$$f.c ] || \
	  $(CC) -O2 -std=gnu99 -DLUA_USE_LINUX -I$(LUA_SRC) -c $(LUA_SRC)/$$f.c -o $(AGENT_OBJ)/lua_$$f.o || exit 1; done
	$(CXX) -O2 -g -std=c++17 -Wall -Wextra -Iplanner -o $@ $(AGENT_OBJ)/*.o $(AGENT_CXX) -lm -ldl -lpthread

# New C++ planner (planner/): unit tests on world view snapshots
# (docs/worldview.md), no emulator.
PLANNER_SRC := $(wildcard planner/*.cpp)
build/planner/test: $(PLANNER_SRC) planner/*.hpp planner/test/*.cpp port/path_pad.h
	@mkdir -p build/planner
	$(CXX) -O2 -g -std=c++17 -Wall -Wextra -Iplanner -Iport -o $@ $(PLANNER_SRC) planner/test/*.cpp

planner-test: build/planner/test build/planner/plan
	build/planner/test planner/test/worlds

# plan <snapshot> [goal]: the goal layer's plan for one world view.
build/planner/plan: $(PLANNER_SRC) planner/*.hpp planner/tools/plan_main.cpp
	@mkdir -p build/planner
	$(CXX) -O2 -g -std=c++17 -Wall -Wextra -Iplanner -o $@ $(PLANNER_SRC) planner/tools/plan_main.cpp

# Planner / steering unit tests on text maps (bot/test/maps), no emulator.
bot-test: build/tools/lua
	build/tools/lua bot/test/test_planner.lua
	build/tools/lua bot/test/test_models.lua
