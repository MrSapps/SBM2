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
