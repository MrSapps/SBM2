# World view (snapshot format v1)

A text snapshot of one frame of the normal game. It holds everything a planner may
look at, and nothing it may not. It has two jobs:

* **Planner input.** The new planner (`planner/`, C++) reads only this. It never reads
  RAM. So any decision can be replayed and tested from a text file, offline, in
  milliseconds.
* **Repro.** A death or a bad decision is pinned by the snapshot just before it. Every
  snapshot is plain text, has no ROM-derived bytes in it, and can be committed as a
  test.

Rules:

* **Facts, not predictions.** Write what RAM says: positions, raw timers, flags. Don't
  write "harmful" or "safe": that is a model, and models live in the planner where
  tests can pin them. (The old bot bakes guesses into what it reads, for example "a
  flashing `$83` is harmless", and that is where many of its bugs hide.)
* **Semantic first, raw kept.** Each cell has a semantic class (one character), so the
  planner never needs stage numbers or tile codes. The raw tile code is kept beside it
  so the classification can be checked or redone.
* **Map pixels.** All positions are map pixels: object x/y plus the stage origin
  (`$5F01`/`$5EFB`). Cell = pixel // 16. The origin is in the header for repro only.

Producers: `bot/worldview.lua` (live in the bot, or offline from a WRAM image:
`build/tools/lua bot/worldview.lua <file.wram>`). Consumer: `planner/world.cpp`.

## Layout

Line-oriented. `#` starts a comment. Header lines are `key value...`. Object lines are
`kind key=value ...`. Unknown keys are ignored, so the format can grow.

```
worldview 1
stage 23                  # $1A: world<<4 | stage-1 (hex)
frame 12345               # bot frame (logic frames since run start)
time 9000                 # stage time left, frames
origin 16 8               # $5F01 $5EFB (repro only)
size 17 14                # cols rows of the playfield (cells outside are '#')
light off                 # light switch $5AAC: on | off | none
gate 9 4 open             # exit gate cell, open | closed
player x=213 y=96 dir=r hearts=1 flash=0 bombs=3 maxbombs=5 fire=9 pierce=0 remote=0 glove=0 softpass=0 lock=0000 ride=0
bomb x=11 y=10 fuse=12 range=10 owner=p1 remote=0 pierce=0 magnet=0 slot=3
enemy slot=5 type=93 x=214 y=96 vx=0.5 vy=0 step=r target=14,6 wait=0 hp=2 f4=0000 c0=8000 d0=0000 be=0000 bc=0000 ba=0000
panel 3 12 todo           # todo | done; mode 0 ($69) or 1 (toggle)
switch 5 5                # light switch cell
item 4 5 heart            # visible item: heart | item
magnet 7 3 dir=r          # magnet cell and pull direction
map
#################
#..%%.P.b*..#...#
...
under                     # CellItems: what a blast leaves behind (same alphabet)
...
raw                       # TileMap codes, hex, 2 chars a cell
...
rawunder                  # CellItems codes, hex
...
```

## Cell classes

| char | class | tiles (raw) | notes |
|---|---|---|---|
| `.` | floor | `$00`, floor variants `$04`-`$3F` not listed below, `$6B`, `$6F`, `$70`, `$72` | |
| `i` | item | `$44` | walkable; stops a blast (picking it up clears it) |
| `#` | wall | `$C3`-`$CF` except below | stops blasts |
| `%` | soft block | `$C2` | blast breaks it; may hide an item or a dormant enemy |
| `b` | bomb | `$40`-`$43` | details in `bomb` lines |
| `*` | flame | `$01`-`$03` | |
| `O` | hole | `$D3` | solid; a push block fills it |
| `X` | push block | `$E2`, `$F5` | |
| `w` | water | `$D0` | |
| `m` | magnet | `$D2` `$D5` `$E0` `$E1` | details in `magnet` lines |
| `B` | barrel | `$D6` | blast sets off 5 small blasts |
| `F` | furnace | `$E7` | |
| `K` | track block | `$CD` | |
| `k` | track | `$4D` | walkable; a `$DD` block moves along it |
| `S` | spring pad | `$4C` | |
| `T` | trap | `$4B` | costs a heart or a life; stops a blast |
| `t` | trap | `$63` | as `T`, but a blast goes on past it |
| `c` | crumble floor | `$25` | becomes `C` once left |
| `C` | cracked floor | `$65` | becomes a hole once left |
| `P` | gold panel | `$69`, `$73`, `$74` | todo/done in `panel` lines |
| `y` | crystal | `$68` | |
| `s` | light switch | `$64`, `$6E` | |
| `p` | power spot | `$14` | bombs placed here get range 10 |
| `f` | fuse line | `$0E` | walkable |
| `^` | stairs | `$66` | walkable, stops blasts |
| `g` | gate | the `gate` cell | open or closed in the header |
| `=` | conveyor | types `$17`-`$1A` | |
| `?` | unknown | anything else | always a bug in the classifier: log it |

Solid in the game is tile bit 7 (`$80`-`$FF`). The planner's walkability comes from
the class, and from the `raw` layer only when a test needs it.

## Players

`dir` is the facing direction `u d l r`. `flash` = frames of hurt-flash left (`+$B9`
while `+4` bit 5 is set, else 0). `lock` = `+$52` in hex. `ride` = `+2` bit 15
(launched or riding).

## Enemies

Every live object in `$8400`-`$8F00`. The fields are raw, so models can be written and
tested against them: `f4` = `+4` (bit 15 dormant, 6 hidden, 5 flashing), `c0` = `+$C0`
(bit 15 moving, 12 hurt), `d0` = `+$D0` (chaser state), `be`/`bc`/`ba` = type timers,
`wait` = `+$4E`, `target` = `+$4C` as a cell, `step` = `+$4A` as `u d l r -`, `vx`/`vy`
signed px/frame. The `type` is the raw `+$7E` byte. A per-type behaviour table
(`docs/enemies.md` section 5) belongs to the planner's models, not here.

## Versioning

Bump `worldview N` only when a line changes meaning. New keys and new line kinds are
compatible.
