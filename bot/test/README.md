# Bot unit tests (no emulator)

`make bot-test` builds a standalone Lua 5.4 from Mesen's bundled sources
(`tools/lua/main.c` + `tools/mesen2-src/Lua`) and runs
`bot/test/test_planner.lua`, about 2 s.

Each test loads `bot/bot.lua` with a stand-in `emu` (`emu.lua`) over a WRAM
image built from a text map in `maps/` (format at the top of `maps.lua`).
With `BOT_TEST` set, bot.lua returns its planner pieces (`bfs`, `stagePlan`,
`regionOf`, `steer`, ...) at the end of the file. Memory is static: no
CPU runs, so memory callbacks never fire and savestates are empty.

Maps are either invented (to pin one mechanic) or text extracts of a real
stage state. No binary or ROM-derived blobs are committed.

## Tools

* `think.lua <file.wram> [frames] [KEY=VALUE...]`: run the whole bot on
  one frozen frame and print its log (what it would do there).
* `map.lua <file.wram>`: print a WRAM image's map with P1, enemies, the
  gate and unfinished panels marked.
* `Maps.fromWram` (`maps.lua`): turn a WRAM image into a `.map` file.

WRAM images come from `port/hybrid`, which replays any pad log without
Mesen: `build/port/hybrid baserom.sfc RUN.inputs -N -D end:out.wram` (or
`-D N:out.wram` after logic frame N). A bench run's `run.inputs` ends where
the bot finished, rollbacks cut out. To probe a moment the run rolled back
from, rerun the stage with `SBM2_SAVE_AT=<frame>:<path.mss>` and play inputs
from that state with `bot/run.sh` (`SBM2_LOAD`).
