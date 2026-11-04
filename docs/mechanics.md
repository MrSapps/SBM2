# Stage mechanics (normal game, worlds 1-5)

What the special tiles do, which routine handles them, which RAM changes,
and a recipe a bot can follow. Code is described in words; names are in
`tools/sbm2/symbols.txt`. Enemies (including the moving platforms `$DC`,
the track blocks `$DD`, the furnace `$DF` and the jump-pad objects
`$E0`/`$A0`) are in `docs/enemies.md`.

Claims marked **[probe]** were checked in Mesen with an input script and a
per-frame log of P1's cell, every TileMap change and the PCs that wrote
it (start states `build/states/*.mss` and `build/lib/stage_XX.mss`). The
rest comes from reading the code.

Notation: cell `(r,c)` = row r, column c; map index = `r*$80 + c*2`.
"type" = `tile & $3F`. `$1A` = `world<<4 | (stage-1)` with world **1-based**
(`$20` is stage 2-1, `$52` is stage 5-3).

## 0. General rules that explain most gimmicks

* **Solid** = TileMap bit 7 set (`$80`-`$FF`): `$C2` soft block, `$C3`-`$CF`
  hard, `$D0` water (3-x), `$D3` hole, `$E2` push block, `$E7` furnace
  cell, `$F5` hidden push block, magnets `$D2/$D5/$E0/$E1`, `$CD` track
  block. A blast stops on any solid cell, and also on types `$04`, `$0B`
  and `$26` (stairs `$66`).
* **TileMap vs CellItems.** `$7FD000` (CellItems) is the "floor
  underneath". When a blast's flames are cleaned up (`FlameCleanup`
  C0:E14F, loop at C0:E51F), every flamed cell gets **TileMap := CellItems**
  and TileGfxMap := TileGfxMap2, and then `SwitchPanelToggle` and
  `GoldPanelToggle` run for that cell. So "bombing" a tile turns it into
  whatever CellItems holds there: `$6B` -> `$72`, `$68` -> `$6F`,
  `$69` -> `$70` **[probe]**. To predict the result of bombing a cell,
  read CellItems at that cell.
* **Per-tile player handler.** `PlayerTileHandler` C1:4031 runs every
  frame for P1 and jumps through table C1:58DE by type. Only these types
  do anything:

  | type | tiles seen | routine | effect |
  |---|---|---|---|
  | `$09` | `$49` | `PlayerOnExitTile` C1:41A8 | open exit gate |
  | `$0B`, `$23` | `$4B`, `$63` | `PlayerOnTrap` C1:40B9 | trap: P1 loses a heart or dies (section 3) |
  | `$0C` | `$4C` | `PlayerOnSpring` C1:427C -> `SpringTile` C0:46E3 | spring pad (section 5) |
  | `$17`-`$1A` | (none in any normal stage) | C0:53E1 / 5461 / 54E1 / 5561 | conveyor (section 7) |
  | `$25` | `$25`, `$65` | `PlayerOnCrumble` C1:408C | crumbling floor (section 2) |

  Everything else (`$00`, `$0E`, `$36`, `$4D`, `$66`, `$68`, `$69`, `$6B`,
  `$74` ...) is plain walkable floor for P1.
* **Per-world routine.** P1's update also calls `WorldStageUpdate`
  C1:4281, which jumps through table C5:D452 by world: world 1
  C1:4474 (magnets), 2 C1:486A (furnace/fuse, hidden push block), 3
  C1:4A02, 4 C1:4A16 (spawns a `$DC` platform), 5 C1:4B06 (crumble,
  traps). Battle mode uses a different object loop and tile table
  (`BattleTileHandler` C0:4337, table C5:EBBF); none of that runs in the
  normal game.

## 1. Push blocks `$E2` and holes `$D3` (worlds 2 and 5)

Rows of `$E2`/`$D3` that separate map areas are a Sokoban puzzle, not a
switch.

* `$E2` = **push block** (solid). `$D3` = **hole** (solid, impassable
  for P1, and the blast stops on it). Neither changes on its own.
* **A blast that reaches an `$E2` pushes it one cell in the blast's
  direction** (BlastResolveUp/Down/Left/Right C0:EAA7..., on type `$22`,
  call `PushBlockStart` C1:2811). The flame stops at the block.
* The push only happens if the target cell (`PushBlockTargetOk`
  C1:27D5) is `$D3`, or is `$00` with no bomb (list `$7F0300`) and no
  enemy (`$7E26A0`) on it. Otherwise nothing moves.
* While it moves, a block object (`+$7E = $E2`, code `PushBlockObject` C1:A6F3) stamps
  `$C5` on both the source and the target cell. Afterwards the source
  becomes `$00`. The target becomes **`$00` if it was a hole `$D3`** (the
  block fills it), or `$E2` again if it was floor. One push = exactly one
  cell.
* Timing **[probe]** (5-3, `c_w5.mss`): the bomb at (14,4) exploded at
  f238. (13,4) `$E2` and (12,4) `$D3` both became `$C5` at f239, the
  source became `$00` at f261 and the filled hole became `$00` at f396.
  So wait about 160 frames after the explosion before walking over it.

**Bot recipe.** For every `$E2` next to a `$D3`, put a bomb on the cell on
the **opposite** side of the block (bomb, block and hole in a straight
line, bomb adjacent to the block). Walk out of the blast and wait for both
cells to read `$00`. Plan with the map: pushing a block onto floor in the
wrong direction can make a hole unfillable (there are fewer `$E2` than
`$D3` in 5-4). Examples: 5-3 (13,4) up into (12,4) **[probe]**; 5-2 row 7
`E2 D3 D3 E2`: push (7,9) right and (7,12) left; 2-2 row 5: bomb at (5,17)
pushes (5,18) right into (5,19), and the panel at (5,20) becomes
reachable; 2-2 row 11: bomb at (11,6) pushes (11,5) left into (11,4).

### Hidden push block `$F5` and button `$6B` (world 2)

`World2ButtonCheck` C1:4897. At stage init the single `$6B` cell is
listed in `$7E5AB4` (+2 cell, +4 gfx index) and the single `$F5` in
`$7E5ABC`. Each frame, if the graphics at the `$6B` cell equal `$1CC8`
(the "pressed" art kept in TileGfxMap2), the `$F5` cell becomes `$E2`
(push block, gfx `$1C0E`) and `$7E5E30` bit 0 is set (once).

**Bot recipe.** Get a flame onto the `$6B` cell. Bombs can't be placed
on `$6B`, so use a bomb on an adjacent cell. When the flames clear, `$6B`
becomes `$72` and `$F5` becomes `$E2` **[probe]** (2-2: bomb at (9,23),
`$6B` (10,23), `$F5` (7,27) -> `$E2` at f243). Then push it: in 2-2,
bomb at (8,27) pushes (7,27) up into the hole (6,27), and the gold panel
at (5,27) becomes reachable.

## 2. Crumbling floor `$25` -> `$65` -> `$D3` (world 5)

`World5Update` C1:4B06 lists every `$25` in `$7E59AA` (`CrumbleList`, 8-byte
entries: +0 state, +2 cell, +4 gfx index). `PlayerOnCrumble` C1:408C sets
bit 15 of the entry under P1. `CrumbleUpdate` C1:4B43 acts on an entry
once P1 has **left** the cell (cell != P1's cell, tile type still `$25`):

* first time: the tile gets bit 6 (`$25` -> `$65`, cracked) **[probe]**.
* second time: the tile becomes **`$D3`, a hole** (solid, permanent). The
  entry is retired (bit 0).

**Bot recipe.** `$25` and `$65` are walkable, but a `$65` cell turns into
a wall as soon as you step off it. Treat `$65` as a cell you can cross
only once, and never route through a `$25` twice if the second pass is
needed later. A crumbled hole can be refilled with a push block
(section 1).

## 3. Traps `$63` and `$4B` (world 5)

* Types `$0B`/`$23` share `PlayerOnTrap` C1:40B9: unless P1 is locked, it
  clears CellItems, starts a "break" animation on the cell (list `$7F0000`,
  kind 4) and calls `PlayerTakeHit` C1:415A. With a heart (`+$EF` != 0,
  values `$80`+ are endless) P1 loses it and flashes (`+4` bit 5,
  `+$B9` = `$C0`). With no heart, P1 **dies** (jump to C0:A675).
  **[probe]**: 5-3, P1 walked onto `$63` at (8,4), and C1:40B9,
  C1:412F and C0:A675 ran on that frame. Lives went 3 -> 2. The tile
  showed `$41` and then became `$00`, so a sprung trap is gone for good.
* `$63` looks like floor. `TrapUpdate` C1:4C4A (list `$7E5928` from
  `FindTraps` C1:4D5E) makes a trap blink only while P1 is on one of the 4
  neighbouring cells. **A blast clears a `$63`** **[probe]**: 5-5
  (`stage_54.mss`, bot seed 0), the bomb at (4,10) with fire 2 went off
  at f115; the flame wrote `$01` on (4,10), (5,10) and (6,10), and the
  trap at (5,10) read `$00` from then on (P1 walked over it at f223 with
  its heart kept). (An earlier note here said flames leave `$63` alone;
  the blast code for type `$23` is not traced yet.)
* `$4B` (type `$0B`) is the visible kind. A blast destroys it like a soft
  block (break kind 4, the flame stops there) and leaves floor.

* `PlayerTakeHit` does nothing (carry clear, no heart used) while P1
  flashes (`+4` bit 5): after a heart is spent `+$B9` = `$C0` counts down
  once a frame (`ObjFlashTick` C0:A570), so for about 190 frames every
  further trap P1 walks onto is sprung (and becomes `$00`) for free.
* **[probe]** A death does **not** keep the trap sprung. 5-3
  (`stage_52.mss`), P1 put on the trap (8,2) with no heart: the tile went
  `$41` at once and `$00` 60 frames later, but after the death animation
  (about 300 frames) the whole map was cleared and the stage reloaded:
  row 8 all `$63` again, 14 soft blocks again, lives 2 -> 1. `FindTraps`
  (C1:4D6C) rebuilds `TrapList` from the map at stage load. (The earlier
  probe only watched the frames before the reload.)
* So a trap crossing only sticks with a heart (or during the flash after
  one). Hearts come from heart items only: no other item flashes P1.

### Items and hearts

Items hidden in soft blocks are listed at `$7E3C48` (4-byte entries
`{cell, kind}`, `$FFFF` ends; bit 14 of kind is set when the block breaks
and `sub_C1223D` writes tile `$44` and the item's art from table C5:F205).
Picking an item up jumps through table C5:F145 by the low byte of the
cell's `TileGfxMap` (`$7FA000`). Kind 11 (art `$1640`, handler C0:9C5B)
is the **heart**: `+$EF` += 1. Kinds 0/2 raise `+$E1`/`+$E2`, 1/5/9/10 set
`+$F3` (remote and other bomb kinds), 12/13/7/8 count up `+$FB/$FA/$FE/$FF`,
14/15 add score. Bench start states: 5-2 hides a heart at (26,11) and 5-6
one at (13,4); 5-3 and 5-5 hide none (5-3 starts with 0 hearts, 5-5 with
1). 5-3 still clears without crossing (bench seed 1, frame 14230); 5-5's
corridors to the holes at column 25 would need about 8 separate
crossings on foot, but bombs clear the traps, so no heart is needed.

**Bot recipe.** Never step on `$63` or `$4B` without a heart (treat them
as solid). Bomb them: a blast clears both. Walking on one with a heart (or
during the flash after one) also clears it, but costs the heart; the
planner only does that when no bomb will. Chain traps that lie close
together inside one flash. In 5-3 (`c_w5`) row 8 is a full line of `$63`;
in 5-5 the corridors to the holes at column 25 hold 8 traps between block
pushes (`bot/test/maps/5-5_start.map`).

## 4. Gold panels and the light switch

* Mode 0 (`$69`, worlds 1-2): bombing a panel makes it `$70` (from
  CellItems). The exit opens when every listed cell reads `$70`
  (`CheckGoldPanels` C1:2335).
* Mode 1 (`$74`/`$73`, worlds 3-5): **every flame cleanup over a panel
  toggles it** (`GoldPanelToggle` C1:2282, called from the flame cleanup
  loop): state bit 0 flips, gfx `$1246` <-> `$1248`. The exit opens when
  every entry has bit 0 set. Bombing the same panel twice undoes it. Two
  bombs whose flames both cover a panel toggle it twice. So which spots
  to bomb is a parity puzzle (XOR over the panels each spot's blast
  covers). In 4-2 every spot next to (26,12) also covers a finished
  panel, but (26,9) and (28,9) each reach only one.
* Light switch `$64` (or `$6E`), world 5: `InitSwitchFlag` C1:2609 sets
  `$7E5AAC` = 1 (for either tile), or `$FFFF` when there is none. Each flame
  cleanup over the switch cell (`$7E5AAE`) flips bit 0
  (`SwitchPanelToggle` C1:22F3). Stepping on it does nothing **[probe]**.
  It doesn't change the map. It changes the palette, and the speed and
  AI of enemies `$8A`/`$97` (see `docs/enemies.md`).

## 5. Spring pads `$4C` (world 3)

`$4C` (type `$0C`) is a spring. In 3-1, two `$4C` connect the outer ring
to the walled centre, which holds the gold panel and the `$68`.

* Stepping on the pad centre: `SpringTile` C0:46E3 locks P1
  (`+$52 = $0801`) for about 20 frames. Then `SpringLaunch` C0:4937 takes
  the direction **held when the lock ends** (the run where Down was
  released before the launch bounced in place) and P1 flies along a ballistic arc (x speed 1.19 px/f, it ignores walls
  and water), and lands on the cell it reaches. `+$52` = `$1801` during
  the flight.
* **[probe]** 3-1 (`stage_30.mss`): pad (6,4), holding Right -> landed on
  the pad at (6,9) at f150 (5 cells, over `$C5`/`$68`/`$C5`). Still
  holding Right, it bounced again and landed at (6,14). With nothing
  held, P1 bounced straight up and came back down on the same pad.
* Landing on a pad launches again in the held direction.
* Distances (`SpringLaunch` C0:4937 picks the routine from the held
  direction): Up (C0:4A95) and Down (C0:4A0D) set the target y to the
  start -/+ `$30`, so they land **exactly 3 cells** away. Left (C0:4B16)
  and Right (C0:4B91) fly at `$1.3000` px/f along an arc: about 5 cells
  (the 3-1 probe above). Nothing held: C0:499A, a bounce in place. In 3-1
  pad (9,6) Down lands on (9,9), inside the walled centre with the panel.
* **[probe]** 3-6 (`stage_35.mss`): after a flight `+$52` reads `$0809`
  for about 20 frames while P1 settles (on or next to the target pad),
  then `$0801` on the pad or clear. A death is `$0801` off a pad, so
  check bit 3 before calling a lock a death. Pads there are 3-5 cells
  apart vertically. (10,24) Up chained to (10,18), then (10,14), (10,10)
  and (10,6), which climbs from the start area (rows 24-27) to the upper
  area with the soft blocks and the gate. A jump from (5,24) Up drowned.

**Bot recipe.** Walk onto the `$4C` and hold the direction you want to
fly until P1 is airborne (about 20 frames after arriving). The landing
cell is 5 cells away (observed for Right). Treat a pad as an edge to the
cell 5 cells away in each direction. Whether P1 can leave a pad it
landed on without being launched again (by releasing the pad) was not
tested. Enemies near the landing cell are dangerous: in the run with
nothing held, P1 died to an enemy right after landing.

## 5b. Submarines `$DA`/`$DB` (3-5)

The two `$66` stairs on 3-5's top corridor, (10,4) and (20,4), each hold a
purple submarine object (enemy-slot type `$DA` code C2:84FC, `$DB` code
C2:85E4). Walking onto one boards it (P1 `+2` bit 15 set, as on a world 4
platform) and it carries P1 along a fixed path through the `$D0` water to
the island's stairs, (12,10) or (18,10), where P1 steps off **[probe]**.
Holding Down on the island beside the docked sub boards it again and it
goes back: from (18,9), (18,12) at +30 frames, (23,11) at +90, (21,6) at
+170. So the island in the middle (gold panels at (13,7) and (17,7)) is
reached only by sub.

P1 walks rows at y = r\*16+7 (origin 0), not +8: off the sub at (18,9)
P1 stood at y 152, and holding Right moved it to 150 and then 151 as it
walked. Steering that wants the cross axis within 1 px of +8 fights this
nudge (`steer` in bot.lua allows 2).

## 6. Crystals `$68` (world 3)

Bombing a `$68` makes it `$6F` (CellItems). When every `$68` of the stage
is `$6F`, all `$8F` enemies (otherwise immune) die (`Check8FPanels`
C1:239D, `KillAllType8F` C1:2413; details in `docs/enemies.md`). Stepping
on `$68` does nothing **[probe]**. `$66` (stairs) is walkable floor that
stops blasts **[probe]**.

## 7. Conveyors (types `$17`-`$1A`)

The handlers exist (C0:53E1, C0:5461, C0:54E1, C0:5561). Each clears
P1's speed and pushes P1 at about 0.56 px/f (`$8FFF`/65536), or about
2.3 px/f when `$7E5CBE` bit 0 is set. The direction comes from `+$28`,
set as pad bits: type `$17` right (`$0100`), `$18` left (`$0200`), `$19`
up (`$0800`), `$1A` down (`$0400`). `$7E5CBE` bit 1 reverses it. In the
normal game `$7E5CBE` is only ever cleared (C1:577C), so there is no
fast or reversed mode. Bits 0 and 1 are toggled only by the
battle-mode lever tiles (types `$12`/`$13` in the battle table, C0:5288
/ C0:52B6).

**No normal-game stage dumped here has a type `$17`-`$1A` tile**
(the start maps of 1-1, 1-2, 2-1..2-6, 3-1, 4-2, 4-3, 4-4, 5-2..5-6).
What looks like conveyors in world 4 is the following:

* `$4D` = track (walkable), `$CD` = the cell the track block `$DD` sits on
  right now (solid). The block shuttles along the track at 0.5 px/f,
  32 frames per cell **[probe]** (4-2: tracks on columns 4 and 8, rows
  6-10), and pushes P1 standing on the track ahead of it. Bot: treat
  `$CD` as solid and predict it moving one cell every ~32 frames.
* `$36` cells sit at the two ends of lanes of solid cells (4-2: (9,12)
  and (9,16) around the solid lane (9,13)-(9,15); (8,17) and (4,17)
  under the exit (3,17)). The `$DC` platform runs along those lanes, and
  P1 can ride it (`docs/enemies.md`). The `$36` cells are
  the boarding points that join otherwise unconnected areas: they are
  the "warp rooms" of 4-2. No code compares a tile with `$36`.
  **[probe]** 4-2 (`stage_41.mss`, bot log): standing on the `$36` does
  nothing. Stepping toward the lane while the `$DC` waits on the first
  lane cell boards it. P1 `+2` bit 15 is set while riding, and P1 goes
  where the platform goes (back and forth) until it steps off onto the
  far `$36` while the platform waits there. (12,9) to (16,9) and (17,8)
  to (17,4) took 100 frames each. `World4Update`
  C1:4A16 spawns one more `$DC` when the `$6A` cell's art becomes
  `$0B26` (after bombing `$6A`, position per stage from table C1:CEC6).

## 8. Magnets (world 1, tiles `$D2` `$D5` `$E0` `$E1`)

`InitMagnets` C1:476A lists up to 4 magnets in `$7E5828` (`$40` bytes
each: +2 cell, +4 gfx index, +6 tile, +8 dir). The tile gives the
facing: `$D2` up (dir 0), `$D5` right (2), `$E0` down (4), `$E1` left
(6). Cell deltas: -$80, +2, +$80, -2.

* `MagnetUpdate` C1:44F6, every frame per magnet: walk up to 4 cells
  from the magnet in its facing direction. Stop at the first cell whose
  tile has bit 6 or 7 set (wall, soft block, bomb ...). If that cell is a
  bomb (`$43`) that is still ticking (fuse >= 3, not exploding, not
  already moving), the magnet **pulls it**: the bomb gets `+0` bit 12
  (sliding) and a carrier object (`+$7E = $E9`) slides it toward the
  magnet. An object standing on the scanned line with the right flags is
  caught the same way.
* A blast that reaches a magnet (`MagnetBlastRotate` C1:46E9, types
  `$12`/`$15`/`$20`/`$21`) **rotates it 90 degrees clockwise** (dir + 2)
  and rewrites its tile and art. The flame stops there.

**Bot recipe.** A bomb placed in a magnet's line of sight, within 4
cells with nothing solid in between, does not stay where it was placed:
it slides up to the magnet. Predict its blast from the cell next to the
magnet, or avoid those lines. To aim a magnet, bomb it (each hit turns
it a quarter turn clockwise).

## 9. Other world 1 / world 2 tiles

* `$14` (world 1) = **power spot**. `FindPowerSpots` C0:3761 lists them in
  `$7E2FF0`. A bomb placed while P1 stands on one gets range 9
  (`BombSetRange` C0:3583). Useful for reaching far targets.
* `$D6` (type `$16`, 1-2) = **barrel**. A blast reaching it
  (`BarrelExplode` C1:26AA) clears it to `$00` and sets off 5 range-1
  explosions: on the barrel cell and on the cells 2 away in each
  direction (skipped where the tile has bit 6 set). Keep 3+ cells away
  when a barrel is in a blast's path.
* `$0E` (world 2) = fuse line (drawn as a dotted line). Walkable, bombs
  can be placed on it, and a blast over it just sets `BlastHitSpecial`
  **[probe]**. `World2Update` lists the `$E7` furnace cells (`$7E2FF0`)
  and the `$0E` cells (`FuseCells` `$7E30F0`). When a bomb **explodes
  while its cell is an `$E7` cell**, `World2MachineState` `$7E5D5C` bit 0
  is set. 64 frames later, and then every 640 frames, every fuse cell
  gets TileMap = CellItems = `$01` (`World2FuseIgnite` C1:4922, sound
  `$6A`). `BombUpdate` explodes any bomb lying on a `$01` cell
  immediately. This was not observed. Getting a bomb into the furnace
  cell involves the `$DF` furnace object (see `docs/enemies.md`); it is
  not needed to clear 2-2, which only needs the gold panels (sections 1
  and 4).
* Soft blocks in world 2 reappear about 190 frames after being blown up
  (seen on cells patrolled by enemy `$86`) **[probe]**.

## 10. Remote-control bombs (all worlds)

A bomb's `+0` bit 0 (copied from P1 `+$F3` bit 0 by `BombPlace`) stops its
fuse (`BombUpdate` C0:E980). The bomb sits forever until **B** is pressed,
and each B press sets off the **oldest** remote bomb, one per press
**[probe]** (2-2 state `k_w2`: P1 `+$F3 = 1`, 4 unexploded bombs, `+$F1`
= 0 bombs left, so `a` did nothing until B freed a slot). A bot that
picked up the remote must press B, about 20 frames apart per bomb, after
retreating.

## 10b. Glove: lift and throw

* The glove item (cell graphics `$26`, handler at C0:9BF6 via the table at
  C5:F145) adds 1 to P1 `+$FE`.
* **Keeping it.** Stage start (C1:3B9B) copies P1's stats from the entry
  copy at `$7E5ECE` once `$7E5DCC` bit 15 is set, so a death goes back to
  what P1 entered the stage with: a glove carried into a stage stays
  through its deaths, one picked up there is lost by a death before the
  clear. The first time (bit 15 clear) passwords 5-9 (`PasswordIndex`
  `$1C6A` >= 5: the same worlds as 0-4) give bombs 6, fire 5, the remote
  and the glove; passwords 0-4 give nothing.
* **5-8 needs one carried in.** Form 2 is hurt only by thrown bombs and the
  5-8 arena has no soft blocks (so no items); the routine CC:CCDE that runs
  as form 1 dies only fills effect slots (`$7F1000`, CC:E998), it drops
  nothing. 5-7 hides a glove at (13,10). A 5-8 start state without a glove
  (as made from a password 0-4 run that never picked one up) cannot be won.
* **Lift** (`BombButton` C0:3082): pressing the bomb button (an edge:
  `+$0E` bit 10) while standing on a bomb (tile under P1 `+$B4` has bit 6
  and `& $3F` = 3) with `+$FE` ≠ 0 and `+$50` ≠ 8 calls C0:2F6E. `+$52`
  becomes 3, then 6 while the bomb is held overhead (about 14 frames).
* **Throw**: releasing the button while `+$52` bit 2 is set calls C0:2CAF.
  It creates a bomb object (`+$7E` = `$0100`) 16 px above P1 with `+$52`
  = `$10`, flying in the facing direction (`+$5E`). **[probe]** in 5-8:
  down from P1 y 176: y 160, 160, 172, 182, 191, 199, 206, 212, 217, 220,
  then it lands (~3 cells). Left from x 128: x drops 4 px/frame for 12
  frames while y arcs from −14 to −35 and back to −12. Then it is a
  normal bomb again.
* A held bomb is thrown by itself when P1 dies (`+$52` bit 1 at the death
  code C0:A675 -> C0:A743, same object, random direction).
* Only these objects (`+$52` bit 4) hurt bosses that use
  `BossBombObjHit` (CC:96D9): 5-8 form 2 (box ±24, CC:97E8). Bombs placed
  normally (bit 5) are swallowed by it.

## 11. Stage notes

* **2-2** (`k_w2`, `$21`): three `$69` panels at (5,20), (5,27), (10,3).
  (5,20): push (5,18) right (bomb at (5,17)). (5,27): bomb next to `$6B`
  (10,23), then push the new `$E2` (7,27) up (bomb at (8,27)). (10,3):
  push (11,5) left (bomb at (11,6)), clear soft (11,3), bomb the panel
  from (11,3). Then the exit (4,16) opens.
* **4-2** (`$41`): exit (3,17) is above a solid lane (4..7,17). Reach it
  through the `$36`/`$DC` platform lanes (section 7). The start area
  (columns 2-12) joins the rest only through the lane (9,12)-(9,16). The
  middle (columns 15-19) and right (21-28) areas are joined by row 8.
  The exit is reached by the lane (8,17)-(4,17).
* **5-2** (`$51`): rows of `E2 D3` pairs (section 1), traps `$4B` row 12.
* **5-3** (`c_w5`, `$52`): push (13,4) up into (12,4) **[probe]**, then
  go up column 4: (10,4) is crumble (once). Row 8 is all `$63` traps
  (section 3), and the exit (3,4) is beyond it.
* **5-4** (`$53`): row 11 is all holes `$D3`; the only push block (10,14)
  must be pushed down (bomb at (9,14)) to fill (11,14) and reach the
  `$74` panels on row 12.
