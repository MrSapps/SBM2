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
  mode), and set `"AllowIoOsAccess": true` in that copy.

## Usage

    make            # assemble build/sbm2.sfc and compare it to baserom.sfc
    make coverage   # headless attract-mode run → coverage/attract_<N>f.cdl
    make disasm     # regenerate src/ from ROM + hints + all coverage CDLs

`tools/sbm2/trace.py baserom.sfc hints.txt [cdl]` prints coverage stats,
unresolved indirect jumps, and width conflicts.

## Notes so far

- HiROM + FastROM, 8 Mbit, no SRAM. RESET $00:FA00, NMI $00:FD84.
- Main loop: `$CC:003C`. It waits for the NMI counter at `$0036`, then dispatches the
  current scene through a 24-bit pointer at `$7F0000` → `$6A`, via `jml [$006A]` at `$CC:03E1`.

## RAM map (so far)

| Address | Meaning |
|---|---|
| `$0036` | NMI frame counter (main loop waits on it) |
| `$C3/$C4` | pad 1 raw (`$4218/$4219`); `$C5..$C8` pads 2-4 |
| `$7E8000`+`$100*n` | player structs, n=0..3 (`+$00` = `$9xxx` when live) |
| `$7E8400`-`$7E8FFF` | enemy structs, same layout |
| struct `+$10` / `+$18` | X / Y in pixels (fractions at `+$12` / `+$1A`) |
| struct `+$52` bit 0 | no control (dying, warping in) |
| struct `+$6E` | filtered input word (Up `$0800`, Down `$0400`, Left `$0200`, Right `$0100`) |
| struct `+$E1` | fire range (guess) · `+$F1` bombs left to place |
| `$7FC000` | tile map: 16 cols × 14 rows, 2 bytes/cell, rows `$80` apart |

Tile codes: `$00` empty, `$01-$3F` flame, `$40-$4F` bomb, `$C2` soft block,
`$C5/$C6` hard wall; `$66 $69 $C9 $D2` unknown so far.
Tile (c,r) is centred at pixel (c·16, r·16+8).

## Bot

    SBM2_LOAD=state.mss bot/run.sh <outdir> <inputs> <shot frames>   # scripted input
    cd tools/mesen2 && SBM2_OUT=... SBM2_FRAMES=... SBM2_LOAD=... \
        ./Mesen --testrunner ../../baserom.sfc ../../bot/bot.lua    # bot v0

Each run writes `run.cdl`; copy it into `coverage/` and run `make disasm`.
