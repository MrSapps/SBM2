# Enemies (normal game, worlds 1-5)

How the enemies move, turn, attack and kill, written so that a bot can
predict them. The code is described in words; routine names are in
`tools/sbm2/symbols.txt`. Claims marked **[log]** were checked against
per-frame struct dumps from Mesen (idle runs and bot runs from
`build/states/*.mss`). The rest comes from reading the code.

## 1. Where it runs

* `StageFrame` (C0:FCA4) calls `ObjectsUpdate` (C1:52C5) once per frame.
  It walks the live-object pointer list `$7E2620` (`$FFFF`-terminated)
  from index 8. An object is an enemy when `+$7E` has bit 7 set.
  Enemies that are not dormant (`+4` bit 15 clear) get:
  `EnemyDispatch` (C1:61E8), then `EnemyNoteCells` (C1:64FE), then the
  animation step (C1:4E82), the screen wrap (C0:A146) and the `+$0C`
  timer (C1:53D4).
* `EnemyDispatch` runs **only while the screen is at full brightness**:
  INIDISP shadow `$30 & $F == $F` (in world 5, `>= 7`). Enemies freeze
  during fades.
* The per-type routine comes from the 3-byte pointer table C1:623E,
  indexed by `type & $3F`. So `$DF` and `$9F` share an entry, and so do
  `$E0` and `$A0`.
* `EnemyNoteCells` writes the enemy's current cell (`+$A8`) to
  `$7E26A0[i]` and its target cell (`+$4C`) to `$7E2720[i]`. It writes
  0 instead while the enemy is hidden (`+4` bit 6), and while a
  `$90`/`$98` has `+$BE` bit 14 set. Other enemies read these to avoid
  each other.
* `$000400` is a fresh random word every frame (`RandomPerFrame`
  C0:0CAA, which calls `Random` C0:0CDF). All the random enemy choices
  read it (or `$000402`). Several enemies that decide on the same frame
  therefore see the same random value.

### Spawn record

`EnemyInitFromRecord` (C1:2C52) copies an 8-byte record into the
object. The record comes from the pointer table C2:DAC6, indexed by the
spawn object's `+4`. The fields land at `+$40..+$47`:

| record byte | field | meaning |
|---|---|---|
| 0 | `+$40` | `type & $7F` |
| 1 | `+$41` | bit 7 = harmless (skipped by the contact test) |
| 3 | `+$43` | hit points (`& $7F == $7F` = immune) |
| 6 | `+$46` | speed, integer px/frame |
| 7 | `+$48` (high byte) | speed, fraction |

Defaults: almost every type has speed 0.5 px/f and 1 or 2 hp.
Exceptions: `$8A`/`$97` 1.25, `$86` 2.5, `$85` 1.5, and `$DC` 0.406.
`$8F` starts with hp `$FF`.

## 2. Struct fields used by enemy AI

Enemies use the same `$100`-byte layout as the player.

| offset | meaning |
|---|---|
| `+0` | bit 0: x velocity is negative; bit 1: y velocity is negative; `$9000` = live |
| `+4` | bit 15 dormant/inactive; bit 6 hidden (no cell occupancy, no contact); bit 5 flashing (no contact, no flame damage) |
| `+$0C` | animation timer; `$0FFF` = animation finished (`EnemyTimerDone` C1:4ED5) |
| `+$10.+$12` | x (integer.fraction16). Map coordinate = x + `$7E5F01` |
| `+$18.+$1A` | y. Map coordinate = y + `$7E5EFB` |
| `+$14.+$16`, `+$1C.+$1E` | x and y speed, magnitude in px/frame (16.16). `ObjMove` C0:A380 adds or subtracts them |
| `+$43` | hit points (byte); `$FF` = immune |
| `+$46.+$48` | move speed, copied into the velocity each time a move starts |
| `+$4A` | cell step of the current direction: `-$80` up, `+$80` down, `-2` left, `+2` right |
| `+$4C` | target cell (the next cell centre it is heading for) |
| `+$4E` | wait counter (frames before it picks a new direction) |
| `+$6A` | animation index (direction animations are type-specific) |
| `+$6C` | direction bits: 8 up, 4 down, 2 left, 1 right (`< 4` = horizontal) |
| `+$7E` | type |
| `+$A8` | current cell `row*$80 + col*2` (`ObjTileUpdate`) |
| `+$B4` | tile under the enemy (1 = flame) |
| `+$C0` | movement flags: bit 15 moving; bit 12 hurt (frozen); bit 11 resume after hurt |
| `+$C2..+$C8`, `+$D2..+$D8` | neighbour cells and their tiles (up, down, left, right) |
| `+$D0` | chaser state (see 4.2), or a type-specific phase |
| `+$BA/+$BC/+$BE/+$DA/+$DE` | type-specific timers |

**Cell centre** (`CellCentreXY` C0:EE1C):

* x = col·16 − `$5F01` + 8
* y = row·16 − `$5EFB` + 7

Note the **+7** on y. A player standing still sits at +8, one pixel
lower (this matters for the sight test in 4.3).

## 3. Contact death

`PlayerEnemyContact` (C1:5518) runs from the player update (C1:54BE),
right after `PlayerHitCheck`. It is skipped while P1 has `+2` bit 15
(riding/launched), `+0` bit 10, `+2` bit 9 or `+4` bit 5 (flashing
after a heart loss). `EnemyOnPlayerCell` (C1:558E) then looks for an
object in `$7E2620` that meets all of these:

* `+$A8 == P1 +$A8`. **Same cell, nothing else**: the cell that holds
  the enemy's x/y point is the cell that holds the player's point. There
  is no pixel-distance test. **[log]** Placing P1 on a `$94` killed it in
  the same frame.
* `+$7E` bit 7 set and bit 6 clear. Types `$C0-$FF` (gadgets such as
  `$DF`, `$DC`, `$DD`, `$E0`, `$E3`) never kill.
* `type & $7F` not in the list C5:D850 (`$30 $31 $16 $24-$28`).
* `+4 & $60 == 0` (not flashing, not hidden), `+4` bit 15 clear,
  `+$41` bit 7 clear, `+$68` bit 1 clear (not dying).
* `$98`/`$90`: not while `+$BE` bit 15 (`$98` hidden, `$90` turned into
  a mount). `$99`: only while `+$D0` bit 8.

On a hit, P1 loses a heart (`+$EF`; `$80+` = endless) and flashes for
`$C0` frames, or dies (C0:A675: `+$52 = $0801`).

Because enemy x/y moves continuously, the cell changes at the half-way
point between two centres. A moving enemy therefore "occupies" the next
cell as soon as it is 8 px from it.

Some attacks kill through the flame tile instead of contact: `$91` and
`$92` write tile `$01` into P1's cell, `$89` sets off a blast, and the
`$90` shot is a lethal object.

## 4. Movement

### 4.1 Generic walker (`EnemyGeneric` C1:62D7)

Used as-is by types `$83 $84 $8D $8F $94`. Most other types run this
same core, with additions.

```
each frame:
  EnemyFlameCheck   (tile under = 1 -> hurt, hp--)
  EnemyCommonTick   (ObjMove with the velocity, flash, ObjTileUpdate)
  if moving (+$C0 bit 15):                       EnemyMoveStep C1:6366
     snap the cross-axis coordinate to the cell centre
     if tile[+$4C] is solid (bit 7) or $43 (bomb):
        stop: x,y := centre of +$A8 (can jump up to ~8 px), velocity 0,
        +$4E = $20, moving off
     elif +$A8 == +$4C and |x-cx|,|y-cy| < tol (tol = 2,3,4,5,6 for speed 0,1,2,3,4+):
        next = +$4C + +$4A
        if next is not solid, not $43, and not any other enemy's current/target cell:
           +$4C = next                           (keep going straight)
        else stop as above (+$4E = $20)
  else:                                          EnemyPickRandomDir C1:6466
     refresh the neighbour tiles
     if +$4E > 1: +$4E--, wait
     else dir = $000400 & 3   (0 up, 1 down, 2 left, 3 right)
          if that neighbour is solid / $43 / another enemy's cell: retry next frame
          else +$4C = neighbour, start moving at +$46.+$48
```

Rules for prediction:

* **Never turns at junctions.** It goes straight until the cell ahead is
  blocked, then stops at the centre of its cell.
* **Waits exactly 32 frames**, then picks a uniformly random direction
  each frame until one is free. That direction can be the reverse, and
  can be the blocked one again. **[log]** `$94` at 0.5 px/f: stop at
  frame 454, `+$4E` = `$20` counted down, moved again at 493.
* **Blocked by:** solid tiles (`$80+`, including soft blocks `$C2`),
  bombs (tile `$43`), and the current or target cells of other enemies.
  Not blocked by flames or by the player.
* A bomb placed in its target cell while it is moving stops it at once,
  snapped back to the centre of its current cell.
* Speed 0.5 px/f means 32 frames per cell.

### 4.2 Shared chaser (`ChaseUpdate` C1:8843)

Used by `$82` (after a hit), `$85` (after it sees P1), `$89` (always),
`$93`, `$8A`, `$97` and `$91` (after they see P1). State lives in
`+$D0`: bit 7 chase on (never cleared by the chaser itself), bit 6
initial stop done, bit 5 leg planned, bit 15 detour, bit 14 detour
initialised, low nibble = direction to P1.

1. **Initial stop.** Finish the current step to the centre of `+$4C`,
   then stop (C1:6315).
2. **Plan a leg.** `ChaseDirToP1` (C1:906D) compares `x & $1F8` with P1
   x (`$7E8010`) and `y & $1F8` with P1 y (`$7E8018`). The nibble is
   1 = P1 to the right, 2 = left, 4 = below, 8 = above, and it is copied
   to `+$CE`. If the nibble is 0 it uses `$A`. The leg runs along the
   axis with the larger |delta| (ties go horizontal), or the only axis
   that has a component. It starts without checking for walls.
3. **Leg step** (`ChaseStep` C1:8EA9):
   * Target solid or `$43`: stop and start a detour.
   * At each cell centre: if the next cell is free (the generic test,
     including other enemies) **and** the nibble still equals `+$CE`,
     continue straight. Otherwise stop and start a detour.
   * The net effect is that it runs straight at you until it reaches
     your 8-px row or column band, then re-plans on the other axis.
4. **Detour** (`ChaseDetour` C1:8BA2, at most 3 cells, `+$CC = 3`):
   * Preferences (C1:8E05): reverse on the axis it was moving along;
     random (`$000400 & 1` or `$000402 & 1`) on the other axis.
   * First move: perpendicular if free, else back along the axis.
   * At each centre: perpendicular preference first, then along the
     axis. These turn tests only check solid tiles and `$43`, not other
     enemies.
   * After 3 cells, or when both options are blocked, it goes back to
     step 2.

**[log]** world 5 `$8A`: charge, blocked charge, 3-cell detour and
re-charge (frames 386-604) match exactly.

### 4.3 Sight tests

* `SeesP1` (C1:8AF9) checks the same row first: `(y & $1F8)` equal for
  enemy and P1. Failing that, it checks the same column: `(x & $1F8)`
  equal. It then steps up to `$7E5D96` cells toward P1's cell
  `$7E80A8`, stopping at solid or `$43` tiles unless `$7E5D98` bit 15
  is set. On success it sets `$50` to the direction bit.
* `SeesP1XRay` (C1:8AEB) uses range 8 and **ignores walls, soft blocks
  and bombs**.
* Quirk: an enemy's centre y is row·16+7, while P1 at rest is at +8, so
  the two are in different 8-px y bands. **The same-row test fails while
  both are centred.** It fires only while P1's y is between row·16+0 and
  +7 (slightly up, or mid-move vertically). The column test works
  normally. **[log]** k_w5: P1 at y `$B8` next to an enemy at `$B7` was
  never seen.

## 5. Per type

"Generic" means section 4.1. Speeds are the defaults from the spawn
records.

| type | seen in | routine | hp | speed | behaviour |
|---|---|---|---|---|---|
| `$94` | 1-x | C1:62D7 | 1 | 0.5 | generic **[log]** |
| `$8D` | 2-x | C1:62D7 | 1 | 0.5 | generic **[log]** |
| `$8F` | 3-x | C1:62D7 | `$FF` | 0.5 | generic walker, **immune**. Dies only all together, see below **[log]** |
| `$83/$84` | | C1:62D7 | | | generic |
| `$82` | 2-x | C1:8B35 | 2 | 0.5 | generic. After a non-lethal hit it becomes a permanent chaser at **1.0 px/f** |
| `$85` | 5-x | C1:8A8F | 1 | 1.5 | generic until `SeesP1XRay` fires (checked every frame, through walls), then a permanent chaser **[log]** |
| `$86` | 2-x | C1:664B | 1 | 2.5 | does nothing for its first 64 frames. Then walks **at most 4 cells**, stops, rests **128 frames**. New direction: toward P1 if P1 is in sight (range 8, walls block), else random. Stops snap it back to the cell it just entered **[log]** |
| `$87` | 4-x | C1:CD70 | 1 | 0.5 | generic. On every 3rd move that starts after a wait, writes a **soft block `$C2`** into the floor cell it is leaving **[log]** |
| `$88` | | C1:87BB | | | like `$89`, blast every `$140` frames |
| `$89` | 4-x | C1:86EB | 1 | 0.5 | always a chaser. Every **448 frames** (`+$BE` counts to `$1C0`) it stops, crouches (anim `$12`) and sets off a **blast centred on itself** (bomb slot, fuse 1): range 3 when `$1A < $40`, else 9 (full fire). It cannot be hurt during the attack, and aborts if it is standing on a tile `$40-$7F` |
| `$8A` | 5-x | C1:B85A | 2 | 1.25 / 0.5 | depends on the light switch `$7E5AAC`. **Even: 1.25 px/f, x-ray chaser. Odd: 0.5 px/f wanderer.** `$FFFF` (no switch): 1.25 wanderer. The mode is re-read at cell centres (`+$BE` caches it) **[log]** |
| `$97` | 5-x | C1:B81B | 2 | 1.25 / 0.5 | mirror of `$8A`: odd = 1.25 x-ray chaser, even or none = 0.5 wanderer **[log]** |
| `$8E` | | C1:CBE7 | 1 | 0.5 | **passes through soft blocks** and every solid tile except `$C5`/`$C6`; blocked by `$43`, flame `$01` and other enemies. While stopped it heads for P1 straight away if P1 is in sight (walls block the sight), else generic wait and random direction |
| `$90` | 3-x | C1:C091 | 1 | 0.5 | generic walker that **shoots**. Every 64 frames, at a centre, if P1 is within 10 cells in a straight line (walls block), it fires a type `$B2` shot (1.5 px/f horizontal, 2.0 vertical) that kills on contact and disappears on solid/`$43`. hp `$FF` while firing. A blast hit turns it into a harmless **mount** (`+$BE` bit 15) and spawns a `$99` rider |
| `$91` | 5-x | C1:BA6B | 2 | 0.5 | x-ray chaser. At a centre, if P1 is within 4 cells in line (walls block), it stops (hp `$FF`) and **writes a flame tile into P1's cell if P1 is in an adjacent cell**. The chase resets after the attack **[log: chase]** |
| `$92` | 2-x | C1:BDD9 | 1 | 0.5 | generic. Every **192 frames** (`+$BC` to `$C0`), at the next centre, it lunges: 160-frame attack, hp `$FF`. On attack frames 62-84 it **flames P1's cell if P1 is 1-3 cells straight ahead, with no wall check**. **[log]** attacks at frames 198, 551, 904, 1257 (353-frame cycle) |
| `$93` | 4-x | C1:CAEF | 2 | 0.5 | generic + `SeesP1XRay` every frame, then a permanent chaser. After its 1st/2nd/3rd hit: 1.0 / 1.5 / 1.5 px/f |
| `$95` | 4-x, 5-x | C1:B347 | 1 | 0.5 | **bomb eater**. Walks onto bombs (its target test ignores `$43`). At centres it turns toward the nearest bomb within 5 cells in a straight line (solid tiles block the search). On a bomb cell it **removes the bomb without an explosion** and gives it back to P1 (`+$F8--`) **[log]** |
| `$98` | 3-x | C1:C473 | 1 | 0.5 | generic + **teleport**. `+$BC` counts frames; at `$1C0` (448) it stops, hp `$FF`, plays a vanish animation, then is **hidden** (`+4` bit 6: no contact, no occupancy) for 192 frames (`+$BA` to `$C0`). It reappears on a random free cell (list `$7FE100`, skipping cells with other enemies), then hp 1 **[log]** |
| `$9F`/`$DF` | 2-x | C1:A8CE | - | - | furnace, harmless and fixed. A bomb in the cell **below** it is turned into a `$E3` object that slides 1 cell further down (1 px/f, 16 frames) and becomes a bomb there again |
| `$DC` | 4-x | C1:9C66 | - | 0.406 | shuttle platform on a lane of solid cells. Waits 64 frames at each end. P1 can ride it (P1 `+2` bit 15: no contact while riding) |
| `$DD` | 4-x | C1:A1ED | - | 0.5 | moving block on a `$4D` track. Stamps tile `$CD` (solid) at its cell and pushes P1 standing on the track ahead of it |
| `$E0`/`$A0` | 3-x | C1:A4FE | - | - | jump pads: catch P1 on the pad centre, 36 frames, launch 8 cells right (`$E0`) or left (`$A0`) |

### Immunity and vulnerability

* `$8F` (world 3): hp `$FF` from its spawn record, so flames do nothing.
  At stage init, the cells holding tile `$68` are listed in `$7E5AD4`
  (`Check8FPanels` C1:239D). Each entry is marked done when its tile
  becomes `$6F` (a blast lights it, like the gold panel `$69` -> `$70`).
  When every entry is done, `KillAllType8F` (C1:2413) kills **every
  `$8F` at once**. In stage 3-1 there is a single `$68` at row 6,
  column 7, inside the central walled block.
* `$98`, `$90`, `$91`, `$92` and `$89` have temporary hp `$FF`, or skip
  flame checks, during their attack or teleport phases. Flames only hurt
  outside those phases.
* After a non-lethal hit, any enemy freezes (velocity 0, anim `$10`,
  `+$C0` bit 12) and is immune (`+$68` bit 0) until the hurt animation
  ends, then resumes (`EnemyHurt` C1:5110, `EnemyHurtDone` C1:4F25).

### Dormant enemies

An enemy with `+4` bit 15 set (`$CC00` / `$AC00`) is **dormant**:

* it has no update, no cell (`+$A8` = 0) and no contact;
* it sits inside a soft block.

**[log]** In 4-1 (b_w4), `$87` woke when a flame reached the soft block
at its cell. Its `+4` became `$0C20` (flashing: no contact, no flame
damage) for 128 frames, then `$0C00`. Dormant enemies sit on soft-block
cells. The bot should expect one to appear wherever such a block is
blasted, and treat it as harmless for those 128 frames. The code that
wakes them was not traced.

## 6. Prediction cheat-sheet

* Position: x/y `+$10`/`+$18` with sub-pixel parts `+$12`/`+$1A`.
  Velocity `+$14.+$16` / `+$1C.+$1E`, sign from `+0` bits 0 (−x) and
  1 (−y). Moving flag `+$C0` bit 15. Target cell `+$4C`, current cell
  `+$A8`, wait `+$4E`.
* A generic enemy moving toward `+$4C` keeps going in direction `+$4A`
  until the next cell is solid, a bomb or another enemy's cell, then
  stands still for 32 frames plus a random retry. So "cells along `+$4A`
  until the first blocker" is an exact path prediction.
* A chaser's next leg is toward P1's 8-px band along the longer axis;
  once it is level with P1 it turns.
* Deadly set at any frame: every harmful enemy's `+$A8`. Add the cell
  ahead along `+$4A` once the enemy is within 8 px of crossing into it.
  Add `$92`'s 3 cells ahead during attack frames 62-84, the cells next
  to an attacking `$91`, the blast cross of `$89` every 448 frames, and
  `$B2` shots (moving 1.5-2 px/f).
* Safe: types `$C0+`, flashing (`+4` bit 5), hidden (`+4` bit 6),
  dormant (`+4` bit 15).
