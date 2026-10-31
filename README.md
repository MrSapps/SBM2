# Super Bomberman 2 (USA) disassembly → C port

Goal: a readable C version of SBM2. Steps:

1. **Disassembly**: `src/*.asm`, which rebuilds to the identical ROM. This is the ground truth.
2. **Coverage**: Mesen 2 runs the game headless (eventually driven by a bot)
   and its code/data log tells the tracer what is code and the register widths
   (M/X) it ran with.
3. **C port**: routines are rewritten by hand and checked frame by frame
   against the original's RAM.

## Setup

Put your ROM at `baserom.sfc`:
SHA-1 `14e4d0b3d00fd04f996eea86daa485a35e501853`.

Tools (not in git):

- asar v1.91: `git clone --branch v1.91 https://github.com/RPGHacker/asar tools/asar-src`,
  build with `cmake -S src -B build -DASAR_GEN_LIB=OFF && cmake --build build`
  inside it, then `ln -s asar-src/build/asar/bin/asar tools/asar`.
- Mesen 2.1.1: unzip `Mesen_2.1.1_Linux_x64.zip` into `tools/mesen2/`, run it
  once, copy `~/.config/Mesen2/settings.json` next to the binary (portable
  mode), and set `"AllowIoOsAccess": true` and `"ScriptTimeout": 10` in that copy
  (the bot can exceed the default 1 s per-frame Lua budget on wide maps).

## Usage

    make            # assemble build/sbm2.sfc and compare it to baserom.sfc
    make coverage   # headless attract-mode run → coverage/attract_<N>f.cdl
    make disasm     # regenerate src/ from ROM + hints + all coverage CDLs

`tools/sbm2/trace.py baserom.sfc hints.txt [cdl]` prints coverage stats,
unresolved indirect jumps, and width conflicts.

## Port runtime

`port/` holds the C side: a 65816 interpreter (`cpu.c`), the SNES bus with a
timing model and a fake APU (`snes.c`), and `verify.c`, which runs the ROM
with an input script and compares all of WRAM with a Mesen trace at the start
of every logic frame (each exit from an NMI wait loop, e.g. `$CC:0054`).

    make trace     # Mesen: port/input.txt -> build/trace/ref.bin (+ .txt)
    make verify    # interpreter vs trace; VERIFY_FLAGS="-p 40 -k" etc.

`build/port/verify` with no arguments lists its options (instruction history,
write watch, per-frame timing, per-instruction clocks).

Ported routines live in `port/game.c` and replace the ROM code at their
address through `port/hook.c`. `make verify-hybrid` runs two machines in
frame-step mode (one NMI per wait loop, no timing), one interpreted and one
with the C routines, and compares them after every logic frame;
`HYBRID_FLAGS=-c` also checks every native call against the interpreter
(registers, flags, WRAM), `-N` disables the hooks, `-o/-x NAME` picks them.

Mesen runs can be replayed there: `bot/inputlog.lua` logs every pad read and
NMI from power-on (`.inputs`; savestates get a sidecar with the log up to
them, as `bot/harness.lua`, `port/trace.lua` and `bot/world_states.py` do;
`SBM2_WATCH_LUA=bot/inputlog_wrap.lua` adds it to bot.lua). Then
`build/port/hybrid baserom.sfc run.inputs [-R trace.bin] [-c]` replays it;
frame-step WRAM then equals Mesen's at every logic frame.

## Notes so far

- HiROM + FastROM, 8 Mbit, no SRAM. RESET $00:FA00, NMI $00:FD84.
- Main loop: `$CC:003C`. It waits for the NMI counter at `$0036`, then dispatches the
  current scene through a 24-bit pointer at `$7F0000` → `$6A`, via `jml [$006A]` at `$CC:03E1`.

## RAM map (so far)

Names used in the asm live in `tools/sbm2/symbols.txt`.

| Address | Meaning |
|---|---|
| `$0036` | NMI frame counter (main loop waits on it) |
| `$C3/$C4` | pad 1 raw (`$4218/$4219`); `$C5..$C8` pads 2-4 |
| `$7E8000`+`$100*n` | player structs, n=0..3 (`+$00` = `$9xxx` when live) |
| `$7E8400`-`$7E8FFF` | enemy structs, same layout; `$7E8400` is the exit gate object in castle stages |
| struct `+$10` / `+$18` | X / Y in pixels (fractions at `+$12` / `+$1A`) |
| struct `+$52` bit 0 | no control (dying, warping in, entering the exit) |
| struct `+$6E` | filtered input word (Up `$0800`, Down `$0400`, Left `$0200`, Right `$0100`) |
| struct `+$A8` | map index of the tile under the object |
| struct `+$E1` | fire as shown on the HUD · `+$E2` blast length − 1 · `+$F1` bombs left to place |
| `$7F2000` | bombs, 40 × `$100`: `+1` flags (`$80` live, `$40` exploding), `+2` cell (r·64 + c·2), `+4` fuse (×2 frames), `+7` blast length − 1, `+$A` map index |
| `$7FC000` | tile map: 32 cols × 14 rows used, 2 bytes/cell, rows `$80` apart |
| `$7E5EFB` / `$7E5F01` | stage origin Y / X: tile = (pos + origin) / 16 |
| `$7E5B56` | gold panel list (8-byte entries, `$FFFF` end) |
| `$7E5D94` | panel state: `$FFFF` no panels, bit 15 set once every panel is lit (`$69` → `$70`) |
| `$7E5D72` | map index of the exit gate (tile `$C9` closed, `$49` open) |
| `$7E5828` | rotating magnets, 4 × `$40` (tiles `$D2/$D5/$E0/$E1` = 4 directions) |
| `$001C6A` | password index matched on the password screen |
| `$7F0028`-`$7F002E` | password digits being entered (one word each) |

Tile codes: `$00` empty, `$01-$3F` flame, `$40-$43` bomb, `$44` item,
`$49` open exit gate, `$66` stairs, `$69` gold panel (unlit; a blast lights
it → `$70`), `$C2` soft block, `$C5/$C6` hard wall, `$C9` closed gate,
`$D0-$EF` magnets etc. The tile under the player is dispatched through the
table at C1:58DE (`tile & $3F`); `$09` (open gate) ends the stage.

Clearing a stage: kill every enemy, light every gold panel; the gate opens.

## Bot

    SBM2_LOAD=state.mss bot/run.sh <outdir> <inputs> <shot frames>   # scripted input
    cd tools/mesen2 && SBM2_OUT=... SBM2_FRAMES=... SBM2_LOAD=... \
        ./Mesen --testrunner ../../baserom.sfc ../../bot/bot.lua    # bot v0

    python3 bot/drive.py <outdir> <start.mss> <frames> [shot frames]   # bot with crash resume
    python3 bot/world_states.py      # build/states/world1..5.mss via the passwords

Each run writes `run.cdl`; copy it into `coverage/` and run `make disasm`.
Mesen's test runner quits after 100 s unless given `--timeout=<s>` (the
scripts pass one).
