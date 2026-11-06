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

`bot/test/test_models.lua` (also run by `make bot-test`) pins the world
models one rule at a time, each traced from a no-rewind death: pierce and
barrel blasts, enemy bombs, magnet pulls, 2-8 drop markers, boss window
merging, the 1-8 hands, `$B2` shots, `$91` neighbours, `$86` dashes and
which flashing enemies stay harmful. Invented maps inline, WRAM poked.

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

## Scenarios (the whole bot, on the port)

`bot/scenario.py run` plays each `scenarios/*.txt` on `build/port/botrun`
(the bot on the C port, no Mesen; `make build/port/botrun`): a stage start
from `build/lib` (or `build/libp`), the pads of an earlier run up to the
moment under test (text, run-length encoded per logic frame), then the bot
for `budget` frames; it passes when the bot log matches `pass`. Each takes
seconds to a minute; `-j` runs them in parallel.

`bot/scenario.py make NAME RUN_DIR FRAME --pass RE` cuts one from any bench
run (Mesen or port: `RUN_DIR/run.inputs`). Probing from such a moment:
`SBM2_SAVE_AT=<frame>:<path.mss> bot/scenario.py run NAME -b <frame+1>`
writes a port savestate there; `SBM2_LOAD=<path.mss> build/port/botrun
baserom.sfc <probe.lua>` runs any Lua script from it.

botrun runs the game in the port's timed mode (Mesen's lag frames) by
default, `SBM2_TIMED=0` for frame-step; `bot/bench.py -p` and
`SBM2_PORT=1 bot/drive.py` use it for whole runs (start states replayed
from their pad logs once and cached as `<state>.port`).
