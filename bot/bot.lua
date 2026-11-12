local WATCH_LUA = os.getenv("SBM2_WATCH_LUA")
if WATCH_LUA and WATCH_LUA ~= "" then dofile(WATCH_LUA) end
-- Pad log (bot/inputlog.lua) for replaying the run in port/hybrid: ckpt.inputs
-- next to ckpt.mss, run.inputs at the end; rollbacks rewind it.
local BOT_DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(BOT_DIR .. "/inputlog.lua")
-- SBM2 bot v0: single-player normal game.
--
-- Reads the tile map and player 1 from WRAM, flees bomb blasts with BFS, and
-- otherwise walks next to soft blocks and bombs them when an escape exists.
-- Logs tile codes it has not classified yet so the tables below can grow.
--
-- Normally launched by bot/drive.py, which restarts it from the last
-- checkpoint if Mesen dies. Env:
--   SBM2_OUT     output dir (bot_<frame0>.log, seg_<frame0>.cdl, ckpt.*, shots)
--   SBM2_FRAMES  stop at this frame (default 3600)
--   SBM2_FRAME0  frame number of the loaded state (default 0)
--   SBM2_CKPT    checkpoint interval in frames (default 1000)
--   SBM2_SHOTS   frames to screenshot (the tile map is logged too)
--   SBM2_LOAD    savestate to start from
--   SBM2_SAVE_AT "<frame>:<path>" also write a savestate (+ .inputs) at that
--                frame (debugging: probe the game from where the bot was)

local OUT = os.getenv("SBM2_OUT") or "build/bot"
local FRAMES = tonumber(os.getenv("SBM2_FRAMES") or "3600")
local LOAD = os.getenv("SBM2_LOAD"); if LOAD == "" then LOAD = nil end
local FRAME0 = tonumber(os.getenv("SBM2_FRAME0") or "0")
local CKPT = tonumber(os.getenv("SBM2_CKPT") or "1000")
local shots = {}
for f in (os.getenv("SBM2_SHOTS") or ""):gmatch("%d+") do shots[tonumber(f)] = true end

local WRAM = emu.memType.snesWorkRam
local function r16(a) return emu.read16(a, WRAM, false) end

-- Object coordinates are offset from the map grid by the stage's origin
-- ($7E5F01 x, $7E5EFB y; the collision lookup at C0:BFE2 adds them):
-- tile = (pos + origin) // 16, so tile centres are at c*16 + 8 - origin.
local function origin() return r16(0x5F01), r16(0x5EFB) end
local function toTile(x, y)
  local ox, oy = origin()
  return (x + ox) // 16, (y + oy) // 16
end
local function tileCentre(c, r)
  local ox, oy = origin()
  return c * 16 + 8 - ox, r * 16 + 8 - oy
end


-- RAM map (offsets into WRAM, i.e. $7E0000 = 0)
local MAP = 0x1C000          -- $7FC000: 16 x 14 cells, 2 bytes each, rows $80 apart
local MAP_W, MAP_H, ROW = 32, 32, 0x80  -- 32x32 buffer (C0:BFE2 masks the index to $0FFF)
local KEY_W = 64  -- cell key = r * KEY_W + c
local P1 = 0x8000            -- $7E8000: player 1 struct
local P_X, P_Y = 0x10, 0x18  -- pixel position (fractions at +2)
local P_LOCK = 0x52          -- bit 0 set: no control (dying/warping)
local P_BOMBS_LEFT = 0xF1    -- bombs that can still be placed (byte)
local ENEMY_FIRST, ENEMY_LAST = 0x8400, 0x8F00  -- enemy structs, same layout

-- Tiles of live enemies: +$00 is $9xxx (as for players) and +$04 bit 15
-- (set when killed) is clear.
local function enemyTiles()
  local t = {}
  for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
    if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8000) == 0 then
      local x, y = r16(base + P_X), r16(base + P_Y)
      local c, r = toTile(x, y)
      if c >= 0 and c < MAP_W and r >= 0 and r < MAP_H then t[#t + 1] = { c, r } end
    end
  end
  return t
end

-- Tile classification (grows as the log reveals more codes)
local ITEM, PANEL = 0x44, 0x69
local GATE_OPEN = 0x100  -- pseudo tile for an open exit gate
local EMPTY = { [0x00] = true, [0x66] = true, [ITEM] = true, [PANEL] = true, [0x70] = true, [0x73] = true, [0x74] = true,
  [GATE_OPEN] = true }  -- $66 stairs
local SOFT = { [0xC2] = true }
local HARD = { [0xC9] = true }
for t = 0xC3, 0xCF do HARD[t] = true end  -- walls: $C5/$C6 (world 1), $C8/$CA (world 4)
-- Hard tiles a blast still reaches (and stops at): the castle gate, which
-- has an enemy-like object parked in it.
local BLAST_STOP = { [0xC9] = true }
-- Switches: a stage clears once every switch is flipped on by a blast and
-- every enemy is dead. Seen: $D2/$D5 off (animating), $E0 on.
-- $69 is a gold floor panel ($7E5B56 list); the exit needs all of them to
-- turn into $70 ($7E5D94 bit 15, checked by C1:41xx). Test: step on them.
local SWITCH_ON = 0xE0
-- $D0-$EF are magnets etc. (C1:44F6): blasts rotate them but the exit
-- doesn't depend on them, so they are never bomb targets.
local TRY = {}  -- SBM2_TRY_TILES="D3,E2": treat these as bomb targets (experiments)
for h in (os.getenv("SBM2_TRY_TILES") or ""):gmatch("%x+") do TRY[tonumber(h, 16)] = true end
local function isSwitchOff(t) return TRY[t] == true end

-- Gold panels ($7E5B56 list, 8-byte entries: +0 bit 0 toggled, +2 map index;
-- $FFFF ends). Mode ($7E5D94 bit 0) 0: tile $69 lights to $70 when blasted.
-- Mode 1 ($73/$74 tiles): every blast that clears over a panel toggles it
-- (C0:E505 -> C1:2282); the exit needs all toggled (C1:2335).
-- Returns sets of cell keys: todo (blast these), done (don't blast again).
local function panels()
  local todo, done = {}, {}
  local state = r16(0x5D94)
  if state == 0xFFFF or (state & 0x8000) ~= 0 then return todo, done end
  local a = 0x5B56
  while r16(a) ~= 0xFFFF and a < 0x5B56 + 8 * 32 do
    local cell = r16(a + 2)
    local k = (cell >> 7) * KEY_W + (cell & 0x7F) // 2
    if (state & 1) == 0 then
      if emu.read(MAP + cell, WRAM, false) == PANEL then todo[k] = true end
    elseif (r16(a) & 1) == 0 then todo[k] = true else done[k] = true end
    a = a + 8
  end
  return todo, done
end
for t = 0xD0, 0xEF do HARD[t] = true; BLAST_STOP[t] = true end
local function isBomb(t) return t >= 0x40 and t <= 0x43 end
-- Blasts write $01 (C0:E8E0); other low codes ($07 $08 $0E $14...) are floor
-- variants whose tile handler (C1:58DE table) does nothing.
local function isFlame(t) return t >= 0x01 and t <= 0x03 end

local logLines, seenUnknown = {}, {}
local function log(s) logLines[#logLines + 1] = s end
-- SBM2_SIMPROF=1: simulateMany calls and time by calling line, logged every
-- 100 calls (to find what makes a stage slow).
if os.getenv("SBM2_SIMPROF") and emu.simulateMany then
  local orig, prof, n = emu.simulateMany, {}, 0
  emu.simulateMany = function(list, H, ...)
    local ln = debug.getinfo(2, "l").currentline
    local t0 = os.clock()
    local a, b, c2, d, e = orig(list, H, ...)
    local p = prof[ln] or { 0, 0, 0 }
    p[1], p[2], p[3] = p[1] + 1, p[2] + os.clock() - t0, p[3] + #list
    prof[ln] = p
    n = n + 1
    if n % 100 == 0 then
      local t = {}
      for k, v in pairs(prof) do t[#t + 1] = string.format("L%d x%d %.1fs n%d", k, v[1], v[2], v[3]) end
      table.sort(t)
      log("simprof " .. table.concat(t, " | "))
    end
    return a, b, c2, d, e
  end
end

local function writeFile(path, data)
  local fh = assert(io.open(path, "wb")); fh:write(data); fh:close()
end

-- Heart items (+$EF += 1, handler C0:9C5B). Items hidden in soft blocks
-- are listed at $7E3C48 ({cell, kind}, 4 bytes each, $FFFF ends; kind bit
-- 14 set once revealed, sub_C1223D). Kind 11 is the heart (art $1640 from
-- table C5:F205). A revealed item is tile $44; its kind shows as the low
-- byte of TileGfxMap ($7FA000), $40 for the heart (handler table C5:F145).
local GFX, ITEMLIST = 0x1A000, 0x3C48
local function heartCells()
  local t = {}
  for i = 0, 63 do
    local cell = r16(ITEMLIST + 4 * i)
    if cell >= 0x8000 then break end
    local kind = r16(ITEMLIST + 4 * i + 2)
    if (kind & 0x3FFF) == 11 then
      t[(cell >> 7) * KEY_W + (cell & 0x7F) // 2] = (kind & 0x4000) ~= 0 and "shown" or "hidden"
    end
  end
  local noGlove = emu.read(P1 + 0xFE, WRAM, false) == 0
  local bytes = emu.readBytes and emu.readBytes(MAP, MAP_H * ROW, WRAM)  -- port/botrun only
  if bytes then
    -- (port: find the $44 item tiles in C, at even offsets = tile codes)
    local pos = 1
    while true do
      pos = bytes:find("\68", pos, true)
      if not pos then break end
      local o = pos - 1
      if o % 2 == 0 then
        local r, c = o // ROW, (o % ROW) // 2
        if c < MAP_W then
          local gfx = emu.read(GFX + o, WRAM, false)
          if gfx == 0x40 or (gfx == 0x26 and noGlove) then t[r * KEY_W + c] = "item" end
        end
      end
      pos = pos + 1
    end
    return t
  end
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      local o = r * ROW + c * 2
      -- The glove ($26, +$FE) too while we have none: 5-8 form 2 is hurt
      -- only by thrown bombs, its arena has no blocks, and stats go back to
      -- the stage's entry stats on a death, so one must be carried in.
      if (bytes and bytes:byte(o + 1) or emu.read(MAP + o, WRAM, false)) == 0x44 then
        local gfx = emu.read(GFX + o, WRAM, false)
        if gfx == 0x40 or (gfx == 0x26 and noGlove) then t[r * KEY_W + c] = "item" end
      end
    end
  end
  return t
end

-- The grid g[r][c], plus (for this frame's scans, one pass instead of
-- several) g.flames (cell keys), g.mapBombs and g.softs ({c, r} lists).
-- (globals: the main chunk is near Lua's 200-local limit)
DeadPad = {}  -- pad key -> frame it was found not launching (floor for a while)
HasPad = false  -- the stage has spring pads ($4C)
Blind = false  -- enemies not placed yet: no bombs
PlanWork = 0  -- planner steps since its last yield
RideBlock = {}  -- platform lane ends not to ride to (an enemy there)
Breaking = {}  -- soft block key -> frame seen next to flames (breaking)
function bombAtKey(k)  -- a bomb tile ($40-$43) on cell key k
  local t = emu.read(MAP + (k // KEY_W) * ROW + (k % KEY_W) * 2, WRAM, false)
  return t >= 0x40 and t <= 0x43
end
StageStart = 0  -- frame the current stage was first seen
Riding = false  -- P1 on a $90/$98 mount (+$60 bit 15)
AvoidCell = {}  -- cells never walked into (a mount P1 would board where pads matter)
local function readMap()
  local g = {}
  local flames, mapBombs, softs = {}, {}, {}
  g.flames, g.mapBombs, g.softs = flames, mapBombs, softs
  local bytes = emu.readBytes and emu.readBytes(MAP, MAP_H * ROW, WRAM)  -- port/botrun only
  for r = 0, MAP_H - 1 do
    local row = {}
    g[r] = row
    for c = 0, MAP_W - 1 do
      local o = r * ROW + c * 2
      local t = bytes and bytes:byte(o + 1) or emu.read(MAP + o, WRAM, false)
      -- A pad found not launching (DeadPad, for a while: 3-1 run 30, (12,8),
      -- 30000 frames frozen on it under a $98 mount): plain floor.
      -- Riding a $90/$98 mount (P1 +$60 bit 15) no pad launches P1 either.
      if t == 0x4C then
        g.hasPad, HasPad = true, true
        -- (a pad under a mount to keep off: floor, so walkable says no)
        if Riding or DeadPad[r * KEY_W + c] or AvoidCell[r * KEY_W + c] then t = 0x00 end  -- (SPRING is declared below)
      end
      row[c] = t
      if t >= 0x01 and t <= 0x03 then flames[#flames + 1] = r * KEY_W + c
      elseif t >= 0x40 and t <= 0x43 then mapBombs[#mapBombs + 1] = { c, r }
      elseif SOFT[t] then softs[#softs + 1] = { c, r } end
      if not seenUnknown[t] then  -- each code is classified once
        seenUnknown[t] = true
        if not (EMPTY[t] or SOFT[t] or HARD[t] or isBomb(t) or isFlame(t)) then
          log(string.format("unknown tile %02X at (%d,%d)", t, c, r))
        end
      end
    end
  end
  return g
end

local SPRING = 0x4C  -- spring pad (world 3): launches P1 in the held direction
-- Where a jump from a pad in a direction landed: springLand[padKey .. dir]
-- = {c, r}. Until seen, assume 3 cells on for Up/Down and 5 for
-- Left/Right, again from any pad landed on (docs/mechanics.md section 5).
local springLand = {}
local springDir, springFrom  -- direction to hold on the pad; jump in flight
local springTry  -- {padKey, dir}: an untried jump we are walking to (think)
local lastAir = -100  -- last frame P1 was in the air ($52 = $1801)
local jumpUntil = -100  -- a jump-pad throw (locked, not dying) lasts until then
-- Cells that started a warp/ride after which P1 froze in place: never
-- walked onto again (walkable).
local badCell = {}
local function walkable(g, c, r)
  if r < 0 or r >= MAP_H or c < 0 or c >= MAP_W then return false end
  local t = g[r][c]
  if AvoidCell[r * KEY_W + c] then return false end
  -- $04-$3F: floor variants; some ($0B $0C $17-$1A $23) run a handler when
  -- stood on (C1:58DE table): conveyors/warps, not hazards as far as known.
  -- $40-$7F are the same floor codes with flag bits (the tile handler only
  -- looks at tile & $3F); $40-$43 are bombs.
  -- $4C spring pad: not a cell to stand on; bfs uses it as a jump edge.
  if t == SPRING then return false end
  if badCell[r * KEY_W + c] then return false end
  -- $4B/$63 traps cost a heart or a life (world 5).
  if t == 0x4B or t == 0x63 then return false end
  return EMPTY[t] == true or (t >= 0x04 and t < 0x80 and not isBomb(t))
end

local DIRS = { { 0, -1, "up" }, { 0, 1, "down" }, { -1, 0, "left" }, { 1, 0, "right" } }
local function shuffleDirs()
  for i = #DIRS, 2, -1 do
    local j = math.random(i)
    DIRS[i], DIRS[j] = DIRS[j], DIRS[i]
  end
end

-- Timing model. Fuse counts down every 2 frames; FPT is a conservative
-- walking speed at base speed (~1.2 px/frame -> ~13 frames per 16 px tile).
local P_FIRE = 0xE2           -- blast length - 1 (+E1 is the HUD value); bombs copy it to +7
local RANGE = 1
local BOMBS, BOMB_SLOTS, BOMB_STRIDE = 0x12000, 40, 0x100  -- $7F2000 bomb table
local FLAME_FRAMES = 40       -- how long a blast stays lethal (estimate)
local NEW_FUSE = 150          -- fuse of a freshly placed bomb, in frames
local FPT = 20                -- frames per tile walked (measured 17-23 at base speed)

-- Some bombs sit with a frozen fuse (seen in 1-2); after FROZEN frames
-- without a fuse change, treat them as obstacles that only a chain reaction
-- can set off.
local FROZEN, fuseSeen = 120, {}
local softSeen, softGone = {}, {}  -- world 2 regrowth (think)
softBlownAt = {}  -- frame each was first seen gone (global: the main chunk is at the local limit)
-- Remote control (P1 +$F3 bit 0): bombs never time out; each B press sets off
-- the oldest (docs/mechanics.md). They count as far-off until we press B.
local REMOTE_T, remoteOn = 100000, false
local stallSig  -- progress signature at the last stall (think)
local remoteSince  -- frame our remote bomb(s) appeared (think)
local rollbackBefore  -- a rollback must land before this frame (think)
local trappedSince  -- frame we found no way out of our remote blast (think)

-- Live bombs from the bomb table: {c, r, t (frames to explosion), power}.
local function readBombs(g)
  local list, byCell = {}, {}
  for i = 0, BOMB_SLOTS - 1 do
    local base = BOMBS + i * BOMB_STRIDE
    local flags = emu.read(base + 1, WRAM, false)
    if (flags & 0x80) == 0 then
      fuseSeen[i] = nil
    else
      local cell = r16(base + 2)
      local c, r = (cell & 0x3F) // 2, cell >> 6
      local fuse = emu.read(base + 4, WRAM, false)
      local now, seen = r16(0x36), fuseSeen[i]  -- NmiCount as the clock
      local t = (flags & 0x40) ~= 0 and 0 or fuse * 2
      if not seen or seen[1] ~= fuse or seen[3] ~= cell then
        fuseSeen[i] = { fuse, now, cell }
      elseif (now - seen[2]) & 0xFFFF > FROZEN and t > 0 then
        t = 100000
      end
      -- Enemy bombs (flags bit 3: 4-7's $A8 lays range-1 bombs) go off
      -- early when P1 comes near (one at fuse 45 of 72 killed P1 next to
      -- it): treat them as due any moment.
      -- (only within 2 cells of P1: an $A5's bomb is an ordinary one, and
      -- "due any moment" froze P1 for 130 frames short of the next cell
      -- in 1-7 until the real blast came and caught it)
      local enemyBomb = (flags & 0x08) ~= 0
      if enemyBomb and t > 8 then
        local pc, pr = toTile(r16(P1 + P_X), r16(P1 + P_Y))
        if math.abs(pc - c) + math.abs(pr - r) <= 2 then t = 8 end
      end
      if remoteOn and t > 0 and not enemyBomb then t = REMOTE_T end
      -- Blast length = +7 + 1 (C0:3386).
      local b = { c, r, t, emu.read(base + 7, WRAM, false) + 1, remoteOn and t == REMOTE_T and not enemyBomb,
        (emu.read(base, WRAM, false) & 4) ~= 0, (flags & 0x10) ~= 0 }  -- [7]: held by a magnet
      list[#list + 1] = b
      byCell[r * KEY_W + c] = b
    end
  end
  -- $B0 walking bombs (3-7's $A7 sends them; docs/enemies.md): harmless to
  -- touch, they blow up where they stop. Seen as a bomb at the target cell
  -- (+$4C) once it is about to stop there (walk + wait +$4E under 40).
  for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
    if (r16(base) & 0xF000) == 0x9000 and (r16(base + 0x7E) & 0xFF) == 0xB0 and (r16(base + 4) & 0x8000) == 0 then
      local tcell = r16(base + 0x4C)
      local tc, tr = (tcell & 0x7F) // 2, (tcell >> 7) & 0x1F
      local cx, cy = tileCentre(tc, tr)
      local dist = math.abs(r16(base + P_X) - cx) + math.abs(r16(base + P_Y) - cy)
      local w = r16(base + 0x4E)
      if w > 0x200 then w = 0 end
      if dist + w < 40 then
        local b = { tc, tr, math.max(4, dist + w), 5, false, false, false }
        list[#list + 1] = b
        byCell[tr * KEY_W + tc] = byCell[tr * KEY_W + tc] or b
      end
    end
  end
  -- Magnets (docs/mechanics.md 8, list $7E5828: +2 cell, +8 dir 0/2/4/6 =
  -- up/right/down/left): each scans 4 cells ahead to the first tile with bit
  -- 6 or 7 set and pulls a ticking bomb there (or P1 on the line) up to the
  -- cell next to it. A bomb so caught is modelled where it will end up;
  -- the scanned cells are kept in MagLine (no place to stop: 1-3,
  -- P1 waited on one and was pulled into a blast, 18 times).
  MagLine = {}
  local MD = { [0] = { 0, -1 }, [2] = { 1, 0 }, [4] = { 0, 1 }, [6] = { -1, 0 } }
  for i = 0, 3 do
    local m = 0x5828 + i * 0x40
    local cell, dir = r16(m + 2), r16(m + 8)
    local mc, mr = (cell & 0x7F) // 2, cell >> 7
    local t = g[mr] and g[mr][mc]
    local d = MD[dir]
    if d and (t == 0xD2 or t == 0xD5 or t == 0xE0 or t == 0xE1) then
      -- A bomb already held (+1 bit 4) keeps its old cell in the table
      -- while its carrier ($54) sits next to the magnet, blocking the scan.
      for k = 2, 4 do
        local hk = (mr + d[2] * k) * KEY_W + mc + d[1] * k
        local b = byCell[hk]
        if b and b[7] then
          byCell[hk] = nil
          b[1], b[2] = mc + d[1], mr + d[2]
          byCell[b[2] * KEY_W + b[1]] = b
          if b[3] < 100000 then b[3] = b[3] * 2 end
          if g[b[2]] and (g[b[2]][b[1]] or 0) & 0x3F == 0x14 then b[4] = math.max(b[4], 10) end
        end
      end
      for k = 1, 4 do
        local cc, rr = mc + d[1] * k, mr + d[2] * k
        local tt = g[rr] and g[rr][cc]
        if not tt then break end
        local b = byCell[rr * KEY_W + cc]
        if b and not b[7] and b[3] >= 6 and k > 1 then
          byCell[rr * KEY_W + cc] = nil
          b[1], b[2] = mc + d[1], mr + d[2]
          byCell[b[2] * KEY_W + b[1]] = b
          -- Held, its fuse runs at about half speed; it lands when the fuse
          -- is out and goes off at once, at length 10 on a $14 pad (C0:8CF0;
          -- 1-6: range 5 became 10 and reached P1 7 cells up).
          if b[3] < 100000 then b[3] = b[3] * 2 end
          if g[b[2]] and (g[b[2]][b[1]] or 0) & 0x3F == 0x14 then b[4] = math.max(b[4], 10) end
          break
        end
        if (tt & 0xC0) ~= 0 then break end
        MagLine[rr * KEY_W + cc] = true
      end
    end
  end
  -- 2-8 boss drops (docs/enemies.md 6): a bomb falls at 2 px/f (state
  -- CC:B189) onto the marker under it (CC:B2F1, same x) and explodes on
  -- landing with range 10. Each marker is a bomb due when the faller gets
  -- there (none seen yet: 60 frames).
  if emu.read(0x1A, WRAM, false) == 0x27 then
    local falls = {}
    for base = 0x1040, 0x17C0, 0x40 do
      local st = r16(base) | (emu.read(base + 2, WRAM, false) << 16)
      if st == 0xCCB189 then falls[#falls + 1] = { r16(base + 0x11), r16(base + 0x14) } end
    end
    for base = 0x1040, 0x17C0, 0x40 do
      local st = r16(base) | (emu.read(base + 2, WRAM, false) << 16)
      if st == 0xCCB2F1 then
        local mx, my = r16(base + 0x11), r16(base + 0x14)
        local t = 60
        for _, f in ipairs(falls) do
          if math.abs(f[1] - mx) <= 2 then
            local fy = f[2] >= 0x8000 and f[2] - 0x10000 or f[2]
            t = math.max(0, (my - fy) // 2 - 3)
          end
        end
        local c, r = toTile(mx, my)
        if not byCell[r * KEY_W + c] then
          local b = { c, r, t, 10, false, false }
          list[#list + 1] = b
          byCell[r * KEY_W + c] = b
        end
      end
    end
  end
  -- Bombs drawn on the map but missing from the table (world 2's grey cubes)
  -- never go off by themselves: obstacles a chain blast may still set off.
  local function mapBomb(c, r)
    if not byCell[r * KEY_W + c] then
      local b = { c, r, 100000, RANGE }
      list[#list + 1] = b
      byCell[r * KEY_W + c] = b
    end
  end
  if g.mapBombs then
    for _, cr in ipairs(g.mapBombs) do mapBomb(cr[1], cr[2]) end
  else
    for r = 0, MAP_H - 1 do
      for c = 0, MAP_W - 1 do
        if isBomb(g[r][c]) then mapBomb(c, r) end
      end
    end
  end
  -- Enemies that blow themselves up (docs/enemies.md), as bombs about to go
  -- off (in the list only: they are not obstacles). Range 3 before world 4,
  -- else 9 (C1:8074). $89: every 448 frames (+$BE to $1C0) where it stands.
  -- $8B (C1:836E, 3-6): rests while +$4E counts down from $500, then walks
  -- (+$C0 bit 15) to the cell at +$4C and blows up there.
  local ox, oy = origin()
  local far = emu.read(0x1A, WRAM, false) >= 0x40 and 9 or 3
  for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
    if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8000) == 0 then
      local typ = r16(base + 0x7E) & 0xFF
      local ec, er = (r16(base + P_X) + ox) // 16, (r16(base + P_Y) + oy) // 16
      if typ == 0x89 and r16(base + 0xBE) >= 0x1C0 - 120 then
        list[#list + 1] = { ec, er, math.max(1, 0x1C0 - r16(base + 0xBE)), far }
      elseif typ == 0x8B and (r16(base + 0xC0) & 0x8000) ~= 0 and r16(base + 0x4E) <= 1 then
        local tk = r16(base + 0x4C)
        local tc, tr = (tk & 0x7F) // 2, tk >> 7
        list[#list + 1] = { tc, tr, math.max(1, 32 * (math.abs(tc - ec) + math.abs(tr - er)) + 16), far }
      elseif typ == 0x8B and r16(base + 0x4E) < 0x60 then
        list[#list + 1] = { ec, er, 0x60, far }  -- about to set out: keep clear of it
      end
    end
  end
  return list, byCell
end

-- Cells a bomb at (c,r) with power p reaches (docs/mechanics.md): a blast
-- stops at solid tiles ($80+); soft blocks, the gate, push blocks ($E2) are
-- reached (destroyed / pushed) and stop it; so do items, traps and stairs
-- (floor types $04, $0B, $26).
local BARREL = 0xD6  -- world 1 barrel ($D6): blown up by a blast
local BLAST_HITS = { [0xC2] = true, [0xC9] = true, [0xE2] = true }
local BLAST_ENDS = { [0x04] = true, [0x0B] = true, [0x26] = true }
-- Pierce bombs (bomb +0 bit 2, from P1 +$F3 bit 2: the kind 5 item, 2-1
-- hides one): BlastResolve* (C0:ED42) breaks a soft block (types $02,
-- $04, $0B) and the flame goes on through it to its full range (2-2: P1
-- died two cells past a soft block the model said stopped it). pierce =
-- nil means P1's own kind (a bomb P1 would place).
local PIERCE_TYPES = { [0x02] = true, [0x04] = true, [0x0B] = true }
local barrelDepth  -- (set while expanding a barrel: its bursts don't recurse)
local blastCells
-- through: items (type $04) don't stop it. For danger: P1 picks up an item
-- on its way out, and the blast then goes on past that cell (1-6: fled
-- "behind" the item at (12,2) by walking over it, 6 seeds alike).
function blastCells(g, c, r, p, pierce, through)
  if pierce == nil then pierce = (emu.read(P1 + 0xF3, WRAM, false) & 4) ~= 0 end
  local cells = { { c, r } }
  for _, dir in ipairs(DIRS) do
    for k = 1, p do
      local cc, rr = c + dir[1] * k, r + dir[2] * k
      if rr < 0 or rr >= MAP_H or cc < 0 or cc >= MAP_W then break end
      local t = g[rr][cc]
      if pierce and PIERCE_TYPES[t & 0x3F] then
        cells[#cells + 1] = { cc, rr, dir }
      elseif t >= 0x80 and t ~= GATE_OPEN then
        if BLAST_HITS[t] then cells[#cells + 1] = { cc, rr, dir } end
        if t == BARREL and not barrelDepth then
          -- BarrelExplode C1:26AA: blasts on the barrel and on the cells 2
          -- away (not where the tile has bit 6 set), each a bomb with +0 =
          -- $8024 (pierce, bit 2) and +7 = 1: range 2 through soft blocks
          -- (C1:275F). (The model had range 1, stopped by blocks: 1-5 died
          -- 2 cells from a burst, every continue alike.)
          barrelDepth = true
          local centres = { { cc, rr } }
          for _, d in ipairs(DIRS) do
            local bc, br = cc + 2 * d[1], rr + 2 * d[2]
            if g[br] and g[br][bc] and (g[br][bc] & 0x40) == 0 then centres[#centres + 1] = { bc, br } end
          end
          for _, ctr in ipairs(centres) do
            for _, cell in ipairs(blastCells(g, ctr[1], ctr[2], 2, true)) do cells[#cells + 1] = { cell[1], cell[2] } end
          end
          barrelDepth = nil
        end
        break
      end
      cells[#cells + 1] = { cc, rr, dir }
      if t < 0x80 and BLAST_ENDS[t & 0x3F] and not (through and (t & 0x3F) == 0x04) then break end
    end
  end
  return cells
end

-- Blast windows per cell: windows[cell] = list of {t0, t1} frames from now.
-- extra = {c, r, t, power} adds a hypothetical bomb. Chain reactions: a bomb
-- caught in an earlier blast goes off at that blast's time.
local lastBombs, lastTimes  -- bombs + chained times from the latest call
local function blastWindows(g, extra)
  local bombs, byCell = readBombs(g)
  if extra then
    bombs[#bombs + 1] = extra
    byCell[extra[2] * KEY_W + extra[1]] = extra
  end
  local times = {}
  for i, b in ipairs(bombs) do times[i] = b[3] end
  local changed = true
  while changed do
    changed = false
    for i, b in ipairs(bombs) do
      for _, cell in ipairs(blastCells(g, b[1], b[2], b[4], b[6], true)) do
        local other = byCell[cell[2] * KEY_W + cell[1]]
        if other then
          for j, o in ipairs(bombs) do
            if o == other and times[j] > times[i] then times[j] = times[i]; changed = true end
          end
        end
      end
    end
  end
  local w = {}
  local function add(k, t0, t1)
    w[k] = w[k] or {}
    w[k][#w[k] + 1] = { t0, t1 }
  end
  for i, b in ipairs(bombs) do
    -- Remote and frozen bombs only go off when B is pressed or a blast
    -- reaches them (chained above); until then they are just obstacles.
    if times[i] < REMOTE_T then
      for _, cell in ipairs(blastCells(g, b[1], b[2], b[4], b[6], true)) do
        add(cell[2] * KEY_W + cell[1], times[i] - 4, times[i] + FLAME_FRAMES)
      end
    end
  end
  if g.flames then
    for _, k in ipairs(g.flames) do add(k, -FLAME_FRAMES, FLAME_FRAMES // 2) end
  else
    for r = 0, MAP_H - 1 do
      for c = 0, MAP_W - 1 do
        if isFlame(g[r][c]) then add(r * KEY_W + c, -FLAME_FRAMES, FLAME_FRAMES // 2) end
      end
    end
  end
  -- World 2 fuse lines: once a bomb has gone off in the furnace ($5D5C bit
  -- 0), every $0E cell turns to flame when $5D5E (counted down each frame)
  -- runs out, and again every $280 frames (C1:4922).
  if emu.read(0x1A, WRAM, false) >> 4 == 2 and (r16(0x5D5C) & 1) ~= 0 then
    local t = r16(0x5D5E)
    if t <= 240 then
      for r = 0, MAP_H - 1 do
        for c = 0, MAP_W - 1 do
          if g[r][c] == 0x0E then add(r * KEY_W + c, t - 8, t + FLAME_FRAMES) end
        end
      end
    end
  end
  lastBombs, lastTimes = bombs, times
  return w
end

-- windows (from blastWindows(g) this frame) plus a bomb placed at (c,r)
-- going off at tfire, or earlier if an existing blast reaches it. Bombs it
-- reaches go off with it. Returns an overlay; windows is not modified.
-- Blast length of a bomb P1 places at (c,r): a $14 pad gives +7 = 9, so
-- length 10 (BombSetRange C0:3592, world 1; docs/mechanics.md).
local function rangeAt(g, c, r) return g[r][c] == 0x14 and math.max(RANGE, 10) or RANGE end

local function withBomb(g, windows, c, r, tfire)
  for _, win in ipairs(windows[r * KEY_W + c] or {}) do
    if win[2] >= 0 and win[1] + 4 < tfire then tfire = math.max(0, win[1] + 4) end
  end
  local w = setmetatable({}, { __index = windows })
  local function add(k)
    local l = {}
    for i, win in ipairs(w[k] or {}) do l[i] = win end
    l[#l + 1] = { tfire - 4, tfire + FLAME_FRAMES }
    rawset(w, k, l)
  end
  local cells = blastCells(g, c, r, rangeAt(g, c, r), nil, true)
  for _, cell in ipairs(cells) do add(cell[2] * KEY_W + cell[1]) end
  for i, b in ipairs(lastBombs) do
    if lastTimes[i] > tfire then
      for _, cell in ipairs(cells) do
        if cell[1] == b[1] and cell[2] == b[2] then
          for _, bc in ipairs(blastCells(g, b[1], b[2], b[4], b[6], true)) do add(bc[2] * KEY_W + bc[1]) end
          break
        end
      end
    end
  end
  return w
end

local function hit(windows, k, a, b)
  local list = windows[k]
  if not list then return false end
  for _, win in ipairs(list) do
    if win[1] <= b and win[2] >= a then return true end
  end
  return false
end

-- Timed BFS from (c0,r0). Moving one tile takes FPT frames; a cell is entered
-- only if no blast window overlaps the time spent in it. t0 offsets the clock
-- (for plans that start later). Returns first step direction and length.
-- World 4 platform lanes (docs/mechanics.md section 7): a $36 cell joins
-- the $36 across a straight run of solid cells; the $DC platform shuttling
-- along it carries P1 (+2 bit 15 while riding). ride.fail["k>k2"]: we waited
-- there and no ride came.
local BOARD = 0x36
-- ride.edges[cell key] = {c, r, frames, press}: a ride seen from that cell
-- (any object that carries P1: world 4 platforms, 3-5's submarines) ended
-- at (c, r) after `frames`, boarded while holding `press` (nil = standing).
-- Routes use them as jumps (jumpObj); kept for the stage.
local ride = { fail = {}, edges = {} }
-- Pocket cells of grid g: off every loop, what is left after peeling cells
-- with one open side again and again (a corridor to a dead end, a cul-de-
-- sac room). Cached per grid table.
function ride.pockets(g)
  if ride.pocketG == g then return ride.pocketCut end
  local deg, q = {}, {}
  for rr = 0, MAP_H - 1 do for cc = 0, MAP_W - 1 do
    if walkable(g, cc, rr) then
      local n = 0
      for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then n = n + 1 end end
      deg[rr * KEY_W + cc] = n
      if n <= 1 then q[#q + 1] = rr * KEY_W + cc end
    end
  end end
  local cut, i = {}, 1
  while i <= #q do
    local k = q[i]; i = i + 1
    if not cut[k] then
      cut[k] = true
      local cc, rr = k % KEY_W, k // KEY_W
      for _, d in ipairs(DIRS) do
        local k2 = (rr + d[2]) * KEY_W + cc + d[1]
        if deg[k2] and not cut[k2] then
          deg[k2] = deg[k2] - 1
          if deg[k2] <= 1 then q[#q + 1] = k2 end
        end
      end
    end
  end
  ride.pocketG, ride.pocketCut = g, cut
  return cut
end
-- Rides come when the platform does: never count on one to escape a blast.
local allowRide = true
local pushCell, pushNext, lastObjFrame = nil, 0, 0  -- stage planner state (think)
local noBombTile, stepIdx, stepSince = {}, 0, 0
-- Experiments: plan trap crossings as if P1 had hearts, and log the map
-- once play resumes after a death.
local FAKE_HEARTS = os.getenv("SBM2_FAKE_HEARTS") == "1"
local logMapNext = false
local frozenAt, lastRide, lastHearts = { 0, 0, 0 }, nil, nil  -- P1 pixel position and since when it has not moved (think)  -- tile -> failed bomb placements on it
local planSteps, planIdx, planCo, trapCell, lastPending, settleNeed = nil, 1, nil, nil, 0, 130
local trapsOK, sacrificeUntil = false, 0  -- crossing a trap on purpose (think)
local bombWhy  -- why the last bomb was placed, for the log
local function laneEnds(g, c, r)
  local ends = {}
  for _, d in ipairs({ { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }) do
    for i = 1, 12 do
      local cc, rr = c + i * d[1], r + i * d[2]
      local t = g[rr] and g[rr][cc]
      if t == BOARD and i > 1 then ends[#ends + 1] = { cc, rr, i }; break end
      if t == nil or t < 0x80 then break end
    end
  end
  return ends
end

-- Where an untried spring jump lands (docs/mechanics.md section 5): with the
-- direction let go after the launch 3 cells on, held in the air ("!" jumps,
-- springLand key dir .. "!") 4; a landing in a wall slides on to the next
-- cell P1 can stand on (or a pad), one in water drowns. (A field of `ride`:
-- the main chunk is at Lua's 200-local limit.)
function ride.springGuess(g, c, r, dir, hold)
  for j = hold and 4 or 3, 9 do
    local cc, rr = c + j * dir[1], r + j * dir[2]
    local t = g[rr] and g[rr][cc]
    if not t or t == 0xD0 then return nil end
    -- (a soft block there: P1 lands inside it and is stuck; 3-6, (5,8)
    -- right came down in (10,8), boxed in until an $8E got through)
    if SOFT[t] then return nil end
    if walkable(g, cc, rr) or t == SPRING then return { cc, rr } end
  end
  return nil
end
local lastBfs  -- {g, goal, avoid}: the last route found, replanned on a pad
-- Jump-pad objects $E0/$A0 (world 3, docs/enemies.md): P1 on the pad's
-- cell is caught and thrown 8 cells right ($E0) or left ($A0).
-- jumpObj[cell key] = {landing c, r}; refreshed each frame (think).
local jumpObj = {}
local function bfs(g, c0, r0, goal, avoid, windows, t0)
  t0 = t0 or 0
  -- Bucket queue by steps taken: jumps and rides cost several steps.
  local buckets, dist = { [0] = { { c0, r0, nil } } }, { [r0 * KEY_W + c0] = 0 }
  local last = 0  -- the highest step count queued
  local function push(c, r, first, n)
    local k = r * KEY_W + c
    if dist[k] and dist[k] <= n then return end
    dist[k] = n
    if n > last then last = n end
    local b = buckets[n]
    if not b then b = {}; buckets[n] = b end
    b[#b + 1] = { c, r, first }
  end
  -- A pad is a node P1 can only leave by a jump, in any direction; where
  -- it lands is learnt (springLand), else assumed 5 cells on.
  local function jump(pc, pr, dir, first, n)
    for _, v in ipairs({ "", "!" }) do
      local land = springLand[(pr * KEY_W + pc) .. dir[3] .. v]
      if land == nil then land = ride.springGuess(g, pc, pr, dir, v == "!") end
      if land and g[land[2]] and SOFT[g[land[2]][land[1]]] then land = nil end
      if land then
        local lk = land[2] * KEY_W + land[1]
        local t = t0 + (n + 6) * FPT  -- ~20 frames on the pad + ~70 in the air
        -- (nor a landing in the avoid set: 3-6 fled onto the (10,10) pad
        -- and jumped up beside an $8E at (10,6))
        if (walkable(g, land[1], land[2]) or (g[land[2]] and g[land[2]][land[1]] == SPRING))
            and not (windows and hit(windows, lk, t - FPT, t + FPT)) and not (avoid and avoid[lk]) then
          push(land[1], land[2], first or (dir[3] .. v), n + 6)
        end
      end
    end
  end
  local n = 0
  while n <= last do
    local b = buckets[n]
    local i = 1
    while b and b[i] do
      local c, r, first = b[i][1], b[i][2], b[i][3]; i = i + 1
      if dist[r * KEY_W + c] == n then
        if g[r][c] == SPRING then
          for _, dir in ipairs(DIRS) do jump(c, r, dir, first, n) end
        else
          if goal(c, r, n) then lastBfs = { g, goal, avoid }; return first, n end
          local jo = jumpObj[r * KEY_W + c]
          if jo and (first or (jo.ride and allowRide)) and (walkable(g, jo[1], jo[2]) or (g[jo[2]] and g[jo[2]][jo[1]] == SPRING)) then
            if jo.ride then
              if allowRide then push(jo[1], jo[2], first or "ride", n + 8 + jo.ride[3] // FPT) end
            else push(jo[1], jo[2], first, n + 8) end
          end
          if g[r][c] == BOARD and allowRide then
            local k0 = r * KEY_W + c
            for _, e in ipairs(laneEnds(g, c, r)) do
              if not ride.fail[k0 .. ">" .. (e[2] * KEY_W + e[1])] and not RideBlock[e[2] * KEY_W + e[1]] then
                push(e[1], e[2], first or "ride", n + 3 * e[3] + 8)
              end
            end
          end
          for _, dir in ipairs(DIRS) do
            local cc, rr = c + dir[1], r + dir[2]
            local k = rr * KEY_W + cc
            local arrive = t0 + (n + 1) * FPT
            -- A pad holds P1 ~20 frames before the launch: clear for that long.
            local isPad = g[rr] and g[rr][cc] == SPRING
            if not (avoid and avoid[k]) and (walkable(g, cc, rr) or isPad)
                and not (windows and hit(windows, k, arrive - FPT, arrive + (isPad and 30 or FPT))) then
              push(cc, rr, first or dir[3], n + 1)
            elseif trapsOK and g[rr] and g[rr][cc] == 0x63 then
              push(cc, rr, first or dir[3], n + 30)
            end
          end
        end
      end
    end
    buckets[n] = nil
    n = n + 1
  end
  return nil
end

-- Stage planner: a search over map states for the cheapest chain of map
-- changes after which P1 can walk to something worth doing that it can't
-- reach now (a panel, a soft block, the open gate, an item, the $6B button,
-- a crystal; enemies only at a penalty). Actions:
--   bomb at a reachable cell: pushes the $E2 blocks its blast reaches one
--     cell on (onto $00, or into a $D3 hole, which it fills; section 1),
--     breaks soft blocks and traps ($4B and $63), presses $6B ($F5 becomes
--     an $E2);
--   trap: walk onto a $63 next to the region (a heart or a life; it is $00
--     afterwards, section 3).
-- Walking includes platform rides and spring jumps, as bfs does. A* with a
-- relaxed distance (blocks, holes, traps and soft blocks passable at a cost)
-- as the heuristic. Returns the first action {kind, k, chain} or nil.
local TRAP_COST, BOMB_COST, ENEMY_PEN, H_WEIGHT = 8, 2, 4, 8
local RELAX = { [0xE2] = 2, [0xD3] = 2, [0x63] = 1, [0x4B] = 1, [0xF5] = 3 }
local function regionOf(g, c0, r0)
  local seen, list, head = { [r0 * KEY_W + c0] = true }, { { c0, r0 } }, 1
  local function add(c, r)
    local k = r * KEY_W + c
    if not seen[k] then seen[k] = true; list[#list + 1] = { c, r } end
  end
  while list[head] do
    local c, r = list[head][1], list[head][2]; head = head + 1
    local t = g[r][c]
    if t == SPRING then
      for _, dir in ipairs(DIRS) do
        for _, v in ipairs({ "", "!" }) do
          local land = springLand[(r * KEY_W + c) .. dir[3] .. v]
          if land == nil then land = ride.springGuess(g, c, r, dir, v == "!") end
          if land and (walkable(g, land[1], land[2]) or (g[land[2]] and g[land[2]][land[1]] == SPRING)) then
            add(land[1], land[2])
          end
        end
      end
    else
      if t == BOARD then
        for _, e in ipairs(laneEnds(g, c, r)) do
          if not ride.fail[(r * KEY_W + c) .. ">" .. (e[2] * KEY_W + e[1])] then add(e[1], e[2]) end
        end
      end
      local jo = jumpObj[r * KEY_W + c]
      if jo and (walkable(g, jo[1], jo[2]) or (g[jo[2]] and g[jo[2]][jo[1]] == SPRING)) then add(jo[1], jo[2]) end
      for _, d in ipairs(DIRS) do
        local cc, rr = c + d[1], r + d[2]
        if walkable(g, cc, rr) or (g[rr] and g[rr][cc] == SPRING) then add(cc, rr) end
      end
    end
  end
  return seen
end

-- Binary heap on f.
local function heapPush(h, item)
  h[#h + 1] = item
  local i = #h
  while i > 1 do
    local p = i // 2
    if h[p].f <= h[i].f then break end
    h[p], h[i] = h[i], h[p]; i = p
  end
end
local function heapPop(h)
  local top = h[1]
  local n = #h
  h[1] = h[n]; h[n] = nil; n = n - 1
  local i = 1
  while true do
    local l, r, m = 2 * i, 2 * i + 1, i
    if l <= n and h[l].f < h[m].f then m = l end
    if r <= n and h[r].f < h[m].f then m = r end
    if m == i then break end
    h[m], h[i] = h[i], h[m]; i = m
  end
  return top
end

-- Where the tiles the planner reasons about are: a search is dropped when
-- this changes under it.
local SIG_TILES = { [0xE2] = true, [0xD3] = true, [0x63] = true, [0xC2] = true, [0x4B] = true, [0x6B] = true,
  [0xF5] = true }
local function structSig(g)
  local t = {}
  for r = 0, MAP_H - 1 do
    local row = g[r]
    for c = 0, MAP_W - 1 do
      local v = row[c]
      if SIG_TILES[v] then t[#t + 1] = (r * KEY_W + c) * 256 + v end
    end
  end
  return table.concat(t, ",")
end

-- What a bomb at (bc,br) does to the map: list of {c, r, new tile}, and the
-- set of cells its blast covers.
local function bombEffect(grid, bc, br)
  local changes, blast = {}, {}
  for _, cell in ipairs(blastCells(grid, bc, br, rangeAt(grid, bc, br))) do
    local cc, rr = cell[1], cell[2]
    blast[rr * KEY_W + cc] = true
    local t = grid[rr][cc]
    -- A blast clears traps, $63 as well as $4B (docs/mechanics.md section 3).
    if SOFT[t] or t == 0x4B or t == 0x63 then changes[#changes + 1] = { cc, rr, 0x00 }
    elseif t == 0x6B then
      changes[#changes + 1] = { cc, rr, 0x72 }
      for r = 0, MAP_H - 1 do
        for c = 0, MAP_W - 1 do if grid[r][c] == 0xF5 then changes[#changes + 1] = { c, r, 0xE2 } end end
      end
    elseif t == 0xE2 and cell[3] then
      local tc, tr = cc + cell[3][1], rr + cell[3][2]
      local tt = grid[tr] and grid[tr][tc]
      if tt == 0x00 or tt == 0xD3 then
        changes[#changes + 1] = { cc, rr, 0x00 }
        changes[#changes + 1] = { tc, tr, tt == 0xD3 and 0x00 or 0xE2 }
      end
    end
  end
  return changes, blast
end

-- Moves of blocks / the button among changes, as a string: what a planned
-- bomb must still do when it is placed (soft blocks may be gone already).
local function keyChanges(changes, grid)
  local t = {}
  for _, ch in ipairs(changes) do
    local was = grid[ch[2]][ch[1]]
    if not (ch[3] == 0x00 and (SOFT[was] or was == 0x4B or was == 0x63)) then
      t[#t + 1] = (ch[2] * KEY_W + ch[1]) * 256 + ch[3]
    end
  end
  table.sort(t)
  return table.concat(t, ",")
end

-- Runs as a coroutine when called from one: yields every SLICE seconds.
-- ctx: objGoal(c, r, grid), enemyGoal(c, r) or nil, avoid = set of cells a
-- planned blast should not touch (finished mode-1 panels: each one costs
-- AVOID_COST, a bomb to flip it back; 5-5's last push flips (10,4) with
-- fire 4), maxExp, slice.
-- Returns the list of steps {kind, k, changes, desc} (or nil), the
-- expansion count.
local function stagePlan(g, c0, r0, ctx)
  local clock0 = os.clock()
  local yieldable = coroutine.isyieldable()
  local slice0 = os.clock()
  local shorten
  local reach0 = regionOf(g, c0, r0)
  local hGoals = {}
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      local k = r * KEY_W + c
      if not reach0[k] and walkable(g, c, r) then
        if ctx.objGoal(c, r, g) then hGoals[k] = 0
        elseif ctx.enemyGoal and ctx.enemyGoal(c, r) then hGoals[k] = ENEMY_PEN end
      end
    end
  end
  if next(hGoals) == nil then return nil, 0 end
  local function hcost(grid, region)
    local dist, buckets, best, last = {}, { [0] = {} }, math.huge, 0
    for k in pairs(region) do dist[k] = 0; local b = buckets[0]; b[#b + 1] = k end
    local n = 0
    while n <= last and n < best do
      local b = buckets[n]
      local i = 1
      while b and b[i] do
        local k = b[i]; i = i + 1
        if dist[k] == n then
          local pen = hGoals[k]
          if pen and n + pen < best then best = n + pen end
          local c, r = k % KEY_W, k // KEY_W
          for _, d in ipairs(DIRS) do
            local cc, rr = c + d[1], r + d[2]
            local t = grid[rr] and grid[rr][cc]
            if t then
              local w
              if walkable(grid, cc, rr) or t == SPRING then w = 0
              elseif SOFT[t] then w = 1
              else w = RELAX[t] end
              if w then
                local nk, nn = rr * KEY_W + cc, n + w
                if not dist[nk] or dist[nk] > nn then
                  dist[nk] = nn
                  if nn > last then last = nn end
                  local bb = buckets[nn]
                  if not bb then bb = {}; buckets[nn] = bb end
                  bb[#bb + 1] = nk
                end
              end
            end
          end
        end
      end
      n = n + 1
    end
    return best == math.huge and 40 or best
  end
  -- Bomb cells in the region whose blast reaches a block, soft block, $4B
  -- or the button.
  local function bombCells(grid, region)
    local cand = {}
    for r = 0, MAP_H - 1 do
      local row = grid[r]
      for c = 0, MAP_W - 1 do
        local t = row[c]
        if t == 0xE2 or SOFT[t] or t == 0x4B or t == 0x63 or t == 0x6B then
          for _, d in ipairs(DIRS) do
            for j = 1, math.max(RANGE, 1) do
              local bc, br = c - d[1] * j, r - d[2] * j
              local bt = grid[br] and grid[br][bc]
              if bt == nil or bt >= 0x80 then break end
              local bk = br * KEY_W + bc
              if region[bk] and (bt & 0x40 == 0 or bt == ITEM) and walkable(grid, bc, br) and not (ctx.noBomb and ctx.noBomb(bk, bt)) then
                cand[bk] = true
              end
              if BLAST_ENDS[bt & 0x3F] then break end
            end
          end
        end
      end
    end
    return cand
  end
  -- Some cell outside the blast within 7 steps of the bomb.
  local function escapable(grid, bc, br, blast)
    local seen, list, head = { [br * KEY_W + bc] = 0 }, { { bc, br } }, 1
    while list[head] do
      local c, r = list[head][1], list[head][2]
      local n = seen[r * KEY_W + c]; head = head + 1
      if not blast[r * KEY_W + c] then return true end
      if n < 7 then
        for _, d in ipairs(DIRS) do
          local cc, rr = c + d[1], r + d[2]
          local k = rr * KEY_W + cc
          if seen[k] == nil and walkable(grid, cc, rr) then seen[k] = n + 1; list[#list + 1] = { cc, rr } end
        end
      end
    end
    return false
  end
  local function child(s, changes, anchor, cost, act)
    local ng, copied = {}, {}
    for r = 0, MAP_H - 1 do ng[r] = s.grid[r] end
    local diff = {}
    for k, v in pairs(s.diff) do diff[k] = v end
    for _, ch in ipairs(changes) do
      local c, r, t = ch[1], ch[2], ch[3]
      if not copied[r] then
        local row = {}
        for cc = 0, MAP_W - 1 do row[cc] = ng[r][cc] end
        ng[r], copied[r] = row, true
      end
      ng[r][c] = t
      local k = r * KEY_W + c
      if g[r][c] == t then diff[k] = nil else diff[k] = t end
    end
    local region = regionOf(ng, anchor % KEY_W, anchor // KEY_W)
    local keys, minK = {}, math.huge
    for k, v in pairs(diff) do keys[#keys + 1] = k * 256 + v end
    table.sort(keys)
    for k in pairs(region) do if k < minK then minK = k end end
    local key = table.concat(keys, ",") .. "@" .. minK
    return { grid = ng, diff = diff, region = region, key = key .. "h" .. s.hearts, g = s.g + cost,
      depth = s.depth + 1, parent = s, act = act, hearts = s.hearts }
  end
  -- Drop steps the goal is reached without (one at a time, replaying the
  -- rest on the map), cut the plan where the goal is first reached, and
  -- recompute each step's effect.
  local function replay(list, enemyOK)
    local grid, ac, ar = {}, c0, r0
    for r = 0, MAP_H - 1 do local row = {}; for c = 0, MAP_W - 1 do row[c] = g[r][c] end; grid[r] = row end
    local out = {}
    for i, st in ipairs(list) do
      local reg = regionOf(grid, ac, ar)
      local sc, sr = st.k % KEY_W, st.k // KEY_W
      local changes
      if st.kind == "bomb" then
        if not reg[st.k] then return nil end
        changes = bombEffect(grid, sc, sr)
        if #changes == 0 then return nil end
        out[i] = { kind = "bomb", k = st.k, desc = st.desc, changes = changes, key = keyChanges(changes, grid) }
      else
        if grid[sr][sc] ~= 0x63 then return nil end
        local adj = false
        for _, d in ipairs(DIRS) do if reg[(sr + d[2]) * KEY_W + sc + d[1]] then adj = true end end
        if not adj then return nil end
        changes = { { sc, sr, 0x00 } }
        out[i] = { kind = "trap", k = st.k, desc = st.desc, changes = changes }
      end
      for _, ch in ipairs(changes) do grid[ch[2]][ch[1]] = ch[3] end
      ac, ar = sc, sr
      for k in pairs(regionOf(grid, ac, ar)) do
        if not reach0[k] then
          local c, r = k % KEY_W, k // KEY_W
          if ctx.objGoal(c, r, grid) or (enemyOK and ctx.enemyGoal and ctx.enemyGoal(c, r)) then return out end
        end
      end
      -- (yields by work done, not wall time: the same plan however busy the
      -- machine, so full runs replay alike)
      PlanWork = PlanWork + 1
      if yieldable and PlanWork >= 60 then coroutine.yield(); PlanWork = 0 end
    end
    return nil
  end
  function shorten(list, enemyOK)
    local best = replay(list, enemyOK)
    if not best then return list end
    local changed = true
    while changed do
      changed = false
      local i = 1
      while i <= #best do
        local cand = {}
        for j, st in ipairs(best) do if j ~= i then cand[#cand + 1] = st end end
        local r = replay(cand, enemyOK)
        if r then best, changed = r, true else i = i + 1 end
      end
    end
    return best
  end
  local function steps(s)
    local list = {}
    while s and s.act do table.insert(list, 1, s.act); s = s.parent end
    return list
  end
  local start = { grid = g, diff = {}, region = reach0, g = 0, depth = 0, key = "", hearts = ctx.hearts,
    flashAt = ctx.flashing and (r0 * KEY_W + c0) or nil }
  start.f = 0
  local heap, best, exp = { start }, { [""] = 0 }, 0
  -- A plan whose pushes wall P1 off from where it stands now is kept only
  -- as a fallback: pushes can't be undone, and the rest of the stage may
  -- be back there (5-5, run SB s0: pushes at (11,10) (10,12) (7,10) (5,12)
  -- jammed the $E2s at (9,10) (10,10) and shut P1 in the lower left; the
  -- last panel (2,4) was beyond row 6; time-up).
  local sealed
  local function backOpen(s)
    for k in pairs(reach0) do if s.region[k] then return true end end
    return false
  end
  while #heap > 0 do
    local s = heapPop(heap)
    if s.terminal then return shorten(steps(s.state), true), exp, true end
    if s.depth > 0 then
      local enemy = false
      for k in pairs(s.region) do
        if not reach0[k] then
          local c, r = k % KEY_W, k // KEY_W
          if ctx.objGoal(c, r, s.grid) then
            if backOpen(s) then return shorten(steps(s), false), exp end
            sealed = sealed or s
            break
          end
          if ctx.enemyGoal and ctx.enemyGoal(c, r) then enemy = true end
        end
      end
      if enemy then heapPush(heap, { terminal = true, state = s, f = (s.g + ENEMY_PEN) * 1000 }) end
    end
    exp = exp + 1
    if exp > ctx.maxExp then
      if sealed then return shorten(steps(sealed), false), exp end
      return nil, exp
    end
    PlanWork = PlanWork + 1
    if yieldable and PlanWork >= 60 then
      coroutine.yield()
      PlanWork = 0
    end
    local kids = {}
    for bk in pairs(bombCells(s.grid, s.region)) do
      local bc, br = bk % KEY_W, bk // KEY_W
      local changes, blast = bombEffect(s.grid, bc, br)
      local AVOID_COST, undo = 6, 0
      for k in pairs(blast) do if ctx.avoid[k] then undo = undo + AVOID_COST end end
      if #changes > 0 and escapable(s.grid, bc, br, blast) then
        kids[#kids + 1] = child(s, changes, bk, BOMB_COST + undo, { kind = "bomb", k = bk,
          key = keyChanges(changes, s.grid), changes = changes, desc = string.format("bomb(%d,%d)", bc, br) })
      end
    end
    local traps = {}
    for k in pairs(s.region) do
      local c, r = k % KEY_W, k // KEY_W
      for _, d in ipairs(DIRS) do
        local cc, rr = c + d[1], r + d[2]
        local tk = rr * KEY_W + cc
        if s.grid[rr] and s.grid[rr][cc] == 0x63 and not traps[tk] then
          traps[tk] = true
          -- A heart pays for a trap (a death restarts the stage, traps
          -- and all); the flash that follows makes the next one nearby free.
          local free = s.flashAt and math.abs(s.flashAt % KEY_W - cc) + math.abs(s.flashAt // KEY_W - rr) <= 6
          if free or s.hearts > 0 then
            local kid = child(s, { { cc, rr, 0x00 } }, tk, TRAP_COST,
              { kind = "trap", k = tk, changes = { { cc, rr, 0x00 } }, desc = string.format("trap(%d,%d)", cc, rr) })
            kid.hearts = free and s.hearts or s.hearts - 1
            kid.flashAt = (not free) and tk or nil
            kid.key = kid.key .. "h" .. kid.hearts .. (kid.flashAt and "f" .. kid.flashAt or "")
            kids[#kids + 1] = kid
          end
        end
      end
    end
    for _, kid in ipairs(kids) do
      if not best[kid.key] or best[kid.key] > kid.g then
        best[kid.key] = kid.g
        -- Weighted A* (cost + H_WEIGHT x relaxed distance), deepest first
        -- among equals. Pure greedy on the distance dived into plateaus of
        -- pointless pushes: 5-5 with blocks at (15,12) and (17,12) found
        -- nothing in 100000 states, where this takes 323
        -- (bot/test/test_planner.lua). The plan is shortened afterwards.
        kid.f = (kid.g + H_WEIGHT * hcost(kid.grid, kid.region)) * 1000 - kid.depth
        heapPush(heap, kid)
      end
    end
  end
  if sealed then return shorten(steps(sealed), false), exp end
  return nil, exp
end

local function playerTile()
  local x, y = r16(P1 + P_X), r16(P1 + P_Y)
  local c, r = toTile(x, y)
  return c, r, x, y
end

-- Steer toward the centre of the next tile; align on the cross axis first,
-- since the engine does not slide the player around corners far.
-- Within STEER_SLACK px it does: P1 walks a row at y = r*16+7 (not +8:
-- (21,3) at y 55 in 3-5), and holding a direction nudges P1 onto that line
-- (3-5, off the sub at y 152, Right went to 150 and on to 151). With a 1 px
-- slack that nudge read as misaligned, Down undid it, and P1 shook in
-- place until the frozen check gave up on the sub for good.
local function steer(dir, x, y, c, r)
  local STEER_SLACK = 2  -- (not a chunk local: the main chunk is at Lua's 200-local limit)
  local cx, cy = tileCentre(c, r)
  -- Two-frame see-saw (2-2, by the exit in the top wall at (16,4): Left
  -- from (17,5) slid P1 to (123,85), the centring Down put it back at
  -- (125,88), every other frame until time-up): after a few A/B/A/B
  -- frames press the plain direction for a while.
  local h = ride.steerH
  if not h then h = { n = 0, raw = 0 }; ride.steerH = h end
  h.n = h.n + 1
  -- (across the way of travel only: 1-px jitter along it is normal, 1-4
  -- pressed on at (161,103)/(162,103) and a plain Right walked into a blast)
  local across = (dir == "left" or dir == "right") and y ~= (h[2] or y) or (dir == "up" or dir == "down") and x ~= (h[1] or x)
  if h[1] and across and x == h[3] and y == h[4] and h[1] == h[5] and h[2] == h[6] then
    h.alt = (h.alt or 0) + 1
    if h.alt >= 6 then
      h.raw = h.n + 40; h.alt = 0
      log(string.format("steer see-saw at px(%d,%d)/(%d,%d) going %s: plain %s for 40", x, y, h[1], h[2], dir, dir))
    end
  else
    h.alt = 0
  end
  -- A centring press that does not move P1 across at all (3-5, in the
  -- raised box after the stairs: P1 walks row 9 at y 149, Down from (13,9)
  -- over the wall only slid it back left, Right/Down alternated for 10k
  -- frames): after two such presses go plain.
  -- (map position, not screen: 1-6 scrolls, and P1's y stays put while
  -- the origin moves under it)
  local sox, soy = origin()
  if h.corrN == h.n - 1 then
    local v = (h.corr == "up" or h.corr == "down") and y + soy or x + sox
    if v == h.corrV then
      h.futile = (h.futile or 0) + 1
      if h.futile >= 2 then
        h.raw = h.n + 40; h.futile = 0
        log(string.format("steer: %s does not move P1 at px(%d,%d): plain %s for 40", h.corr, x, y, dir))
      end
    else
      h.futile = 0
    end
  end
  h.corrN = nil
  h[5], h[6], h[3], h[4], h[1], h[2] = h[3], h[4], h[1], h[2], x, y
  if h.n < h.raw then return dir end
  local function corr(d)
    h.corr, h.corrN, h.corrV = d, h.n, (d == "up" or d == "down") and y + soy or x + sox
    return d
  end
  if dir == "up" or dir == "down" then
    if x < cx - STEER_SLACK then return corr("right") elseif x > cx + STEER_SLACK then return corr("left") end
  else
    -- (a pixel more above when the cell below is stairs $66: 3-5, run 23,
    -- off the sub at (12,9), Left slid P1 to y 149, Down put it back at
    -- 152 by the docked sub, every 3 frames, until its own bomb went off)
    local below66 = emu.read(MAP + ((r + 1) << 7) + c * 2, WRAM, false) == 0x66
    if y < cy - STEER_SLACK - (below66 and 1 or 0) then return corr("down") elseif y > cy + STEER_SLACK then return corr("up") end
  end
  return dir
end

local frame, input = FRAME0, {}
local step = nil  -- committed move: from (c,r) toward (tc,tr); false while locked
local recent = {}  -- last few plan lines, dumped on death
local lastPlan = ""
local offStage = 0  -- frames since the last stage frame
local savedStage = nil
local onNextInstruction  -- defined below (savestates need an exec callback)
local lastWorld, gameOver = 0, false

-- Rollback search: in-memory savestates every SNAP_EVERY frames; a death
-- reloads an older one and replays with shuffled tie-breaks, going further
-- back the more often a snapshot has failed. After MAX_TRIES the death is
-- accepted (it still covers the death code).
local SNAP_EVERY, MAX_TRIES = 120, 12
local snaps = {}          -- { frame, data }
local triesAt = {}        -- rollbacks per snapshot frame (survives re-snapshots)
local function timeLeft() return r16(0x5E10) * 60 + r16(0x5E12) end  -- stage timer
local playing = false     -- unlocked in a stage since the last lock
local rollback = false    -- set by think() on a death
local exploreUntil = 0    -- after a rollback, "wait" becomes a random move
-- Cells where pressing A placed no bomb (stairs, gate cells, ...): no bomb
-- attempts there until noBombUntil[key].
local noBombUntil, bombTry = {}, nil  -- bombTry = { key, since }
-- Stall detection: if score, soft blocks, open panels and enemy count stay
-- the same for STALL frames, explore (novelty-seeking walk) for a while, to
-- find conveyors, warps and other ways between rooms.
local STALL, visits = 1500, {}
local progressSig, progressAt = nil, 0
local stallUntil = 0      -- explore overrides seek/hunt/item until then
local ASSIST, lastProgress = tonumber(os.getenv("SBM2_ASSIST") or "") or 6000, 0
local NOASSIST = os.getenv("SBM2_NOASSIST") == "1"  -- benchmarks: the bot on its own
if NOASSIST then ASSIST = math.huge end
local ticks = 0           -- frames run, rollbacks included (frame rewinds)
local HEART_DEATHS, deathsAtProgress, heartStage = 20, 0, nil
local seenSigs = {}
local forceStage, forceTicks = nil, 0  -- stage being cleared by assists
local stageTicks = 0  -- ticks when the current stage started
local deaths, rollbacks = 0, 0
-- Ambush search: a bomb placed only for an enemy is tried from a savestate;
-- if nobody died AMBUSH_FRAMES later, reload and retry with a random delay
-- and shuffled tie-breaks, up to AMBUSH_TRIES times.
local AMBUSH_FRAMES, AMBUSH_TRIES = 220, 10  -- (retries cost CPU, not game time: the port makes them cheap)
local ambush = nil        -- { frame, data, alive, tries }
local ambushStart = false -- think() wants to bomb for an enemy
local ambushHold = 0      -- no enemy-only bombs before this frame
local ambushes, ambushHits = 0, 0
local function liveEnemies() return #enemyTiles() end
local function score() return r16(0x5DEE) end  -- score / 100

-- Enemy prediction (bot/enemies.lua, rules in docs/enemies.md).
local Enemies = dofile(BOT_DIR .. "/enemies.lua")({ r16 = r16, rd8 = function(a) return emu.read(a, WRAM, false) end,
  MAP_W = MAP_W, MAP_H = MAP_H, KEY_W = KEY_W, tileCentre = tileCentre })
local ROLLOUTS, HORIZON = 12, 420  -- sampled paths per enemy, frames ahead
local EATER_SKIP = { [0x95] = true }  -- killChance: eaters go by eaterFate
local KILL_P = 0.3                 -- bomb for an enemy at this kill chance
local ENEMY_MARGIN = 6             -- frames of slack around predicted contact
-- Bosses (bot/boss.lua): boss-engine objects, not enemy structs.
local Boss = dofile(BOT_DIR .. "/boss.lua")({ r16 = r16, rd8 = function(a) return emu.read(a, WRAM, false) end,
  origin = origin, MAP_W = MAP_W, MAP_H = MAP_H, KEY_W = KEY_W })
-- (Kept in the Boss table: the main chunk is near Lua's 200-local limit.)
Boss.MARGIN, Boss.SLACK = 6, 12  -- pixels around a boss box; frames either side of its pass
Boss.hp = nil  -- total boss HP last frame (nil: no boss)
-- 5-8 form 2 (docs/enemies.md 7.4) is hurt only by a thrown bomb: with the
-- glove (+$FE), lift a bomb of our own and throw it when the body is ahead.
local throw = { face = nil, faceFrames = 0, liftAt = nil, dodgeAt = -100, at = -100 }  -- (also the dodge hold)
-- A thrown bomb's offset from P1, frame by frame from the release
-- (measured in 5-8; it lands after ~12 frames). Left mirrors right, up
-- mirrors down around the start 16 px above P1.
local THROW_PATH = {
  down = { { 0, -16 }, { 0, -16 }, { 0, -4 }, { 0, 6 }, { 0, 15 }, { 0, 23 }, { 0, 30 }, { 0, 36 }, { 0, 41 }, { 0, 44 }, { 0, 46 }, { 0, 48 } },
  left = { { -4, -14 }, { -4, -14 }, { -8, -19 }, { -12, -23 }, { -16, -26 }, { -20, -28 }, { -24, -31 }, { -28, -33 },
    { -32, -34 }, { -36, -35 }, { -40, -29 }, { -44, -21 }, { -48, -12 } },
}
THROW_PATH.up, THROW_PATH.right = {}, {}
for i, p in ipairs(THROW_PATH.down) do THROW_PATH.up[i] = { 0, -32 - p[2] } end
for i, p in ipairs(THROW_PATH.left) do THROW_PATH.right[i] = { -p[1], p[2] } end
-- 5-8 form 2 aim: a BFS goal for cells from which a throw, made on arrival
-- (n steps of FPT frames, plus a lift and a turn), would meet the body where
-- it is predicted then. Without this P1 only threw when the body happened to
-- pass (9 throws, 2-5 hits per 9000 frames). Cell centres: px (c*16, r*16+7).
-- A predicted form 2 position is sure while the body is still on its way
-- to the waypoint: within 24 px of it, it picks a random new one (7.4).
function Boss.sure(body, px, py)
  local tg = Boss.target[body.base]
  return not tg or math.abs(px - tg[1]) > 24 or math.abs(py - tg[2]) > 24
end
-- A thrown bomb that hits form 2 blows up where it is (in the air) about
-- when it hits, while P1 is still locked by the throw (~12 frames): only a
-- hit whose bomb cell is outside P1's blast cross is safe. Up/down throws
-- stay in P1's column; left/right ones arc a row or two up.
function Boss.throwSafe(px, py, o)
  local c, r = toTile(px, py)
  local bc, br = toTile(px + o[1], py + o[2])
  if br == r and math.abs(bc - c) <= RANGE then return false end
  if bc == c and math.abs(br - r) <= RANGE then return false end
  return true
end
function Boss.aimGoal(body, avoid)
  local cache = {}
  local function at(t)
    local p = cache[t]
    if not p then
      local px, py = Boss.predict(body, t)
      p = { px, py, Boss.sure(body, px, py) }
      cache[t] = p
    end
    return p[1], p[2], p[3]
  end
  return function(cc, rr, n)
    if avoid[rr * KEY_W + cc] then return false end
    local t0 = n * FPT + 6
    if t0 > 160 then return false end
    local cx, cy = cc * 16, rr * 16 + 7
    -- not where the body is on arrival (the throw is made standing there)
    local bx0, by0 = at(t0)
    if math.abs(cx - bx0) < 36 and math.abs(cy - by0) < 36 then return false end
    for _, path in pairs(THROW_PATH) do
      for k = 4, #path do
        local o = path[k]
        local px, py, sure = at(t0 + k)
        if not sure then break end
        if math.abs(cx + o[1] - px) <= 16 and math.abs(cy + o[2] - py) <= 16 then
          if Boss.throwSafe(cx, cy, o) then return true end
          break  -- (the first hit frame is where it blows up)
        end
      end
    end
    return false
  end
end
local function finalForm2()
  return emu.read(0x1A, WRAM, false) == 0x57 and emu.read(0x1002, WRAM, false) == 0xCC and r16(0x1000) >= 0xD0B8
end

-- a and b both map cell -> list of windows; the overlay looks up both.
local function combine(a, b)
  return setmetatable({}, { __index = function(_, k)
    local x, y = a[k], b[k]
    if not x then return y elseif not y then return x end
    local l = {}
    for i, v in ipairs(x) do l[i] = v end
    for _, v in ipairs(y) do l[#l + 1] = v end
    return l
  end })
end

local nextDetonate = 0
-- SBM2_SEED varies the random choices (bench.py -s), to tell luck from skill.
local SEED = tonumber(os.getenv("SBM2_SEED") or "0")
-- SBM2_NOREWIND=1: play as a human would: no rollbacks after deaths or
-- stalls, no ambush retries (no savestate is ever loaded).
NOREWIND = os.getenv("SBM2_NOREWIND") == "1"
-- SBM2_STOPEARLY=1 (scenarios): stop at the first death or stage change.
STOPEARLY = os.getenv("SBM2_STOPEARLY") == "1"
-- (+ the segment's start frame: after a game over drive.py continues from
-- the same stage state, and the same choices lost the same way 50 times)
math.randomseed(SEED + FRAME0 * 7919)
local function think()
  input = {}
  -- Locked ($52 bit 0): dying, on the exit, warping, a boss's freeze, ...
  -- Only a death is a death: C0:A675 sets $52 = $0801 (docs/enemies.md).
  local lockv = r16(P1 + P_LOCK)
  local heartsNow = emu.read(P1 + 0xEF, WRAM, false)
  if heartsNow ~= lastHearts then
    -- What took the heart: flame under P1, enemies within 32 px.
    local why = {}
    if isFlame(emu.read(MAP + r16(P1 + 0xA8), WRAM, false)) then why[#why + 1] = "flame" end
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000)
          and math.abs(r16(base + P_X) - r16(P1 + P_X)) < 32 and math.abs(r16(base + P_Y) - r16(P1 + P_Y)) < 32 then
        why[#why + 1] = string.format("%04X:%02X(%d,%d)", base, r16(base + 0x7E) & 0xFF, r16(base + P_X), r16(base + P_Y))
      end
    end
    log(string.format("%6d hearts %02X -> %02X at px(%d,%d) %s", frame, lastHearts or 0, heartsNow,
      r16(P1 + P_X), r16(P1 + P_Y), table.concat(why, " "))); lastHearts = heartsNow
  end
  if os.getenv("SBM2_DUMP") and frame % 600 == 50 then
    local t = {}
    for base = 0x8400, 0x9F00, 0x100 do
      if r16(base) ~= 0 then
        t[#t + 1] = string.format("[%04X %04X %04X %02X (%d,%d) hp%02X]", base, r16(base), r16(base + 4), r16(base + 0x7E) & 0xFF,
          r16(base + P_X), r16(base + P_Y), emu.read(base + 0x43, WRAM, false))
      end
    end
    log(string.format("%6d dump 5D94=%04X 5D70=%04X %s", frame, r16(0x5D94), r16(0x5D70), table.concat(t, " ")))
  end
  -- Riding a world 4 platform (+2 bit 15): hands off until it drops us.
  if (r16(P1 + 2) & 0x8000) ~= 0 and (lockv & 1) == 0 then
    if not ride.from then
      local rc, rr = playerTile()
      local press
      for _, d in ipairs(DIRS) do if input[d[3]] then press = d[3] end end
      ride.from = { rc, rr, frame, press }
      log(string.format("%6d ride from (%d,%d)", frame, rc, rr))
    end
    -- Step off onto the far end's $36 when the platform stops beside it
    -- (or an item lying there, $44 over the $36: 4-2, an item dropped on
    -- (16,9) and P1 shuttled on the platform until time ran out).
    -- A simulated ride (board search) goes on as simulated: the sim held
    -- its pads through the ride, and with "then none" it stays on and rides
    -- back. (4-2, run HZ s0: the board search's "left 60 then none" lived
    -- by riding back; the bot stepped off at (12,9) instead, where an $87
    -- came down column 12, and lost a heart.)
    local b = ride.brk
    if b and b.seq and frame - b.start <= b.H then
      local w = b.seq[frame - b.start + 1]
      if w then
        input = {}
        for nm, bit in pairs({ up = 0x0800, down = 0x0400, left = 0x0200, right = 0x0100 }) do if w & bit ~= 0 then input[nm] = true end end
        step = nil
        return
      end
    end
    local rc, rr = playerTile()
    for _, d in ipairs(DIRS) do
      local cc, r2 = rc + d[1], rr + d[2]
      local t2 = (r2 >= 0 and cc >= 0) and emu.read(MAP + r2 * ROW + cc * 2, WRAM, false)
      if (cc ~= ride.from[1] or r2 ~= ride.from[2]) and (t2 == BOARD or t2 == ITEM) then
        input[d[3]] = true
      end
    end
    step = nil
    return
  elseif ride.from then
    local rc, rr = playerTile()
    log(string.format("%6d ride from (%d,%d) ended at (%d,%d)", frame, ride.from[1], ride.from[2], rc, rr))
    if math.abs(rc - ride.from[1]) + math.abs(rr - ride.from[2]) > 1 then
      ride.edges[ride.from[2] * KEY_W + ride.from[1]] = { rc, rr, frame - ride.from[3], ride.from[4] }
    end
    lastRide = { ride.from[1], ride.from[2], frame }
    ride.from = nil
    -- A board search's sequence walks off on landing; one that goes on
    -- into a wall there ends now (4-3, runs RA/UA/VB s0: "down 60 then
    -- down" held Down on the (3,18) board, P1 frozen for the rest of its
    -- 640 frames, again and again until time-up with the gate open).
    local b = ride.brk
    if b and b.board and b.seq then
      -- (only if every press left goes into a wall: the board's own way is
      -- still held at landing, and the walk-off comes after it; 4-2_r66
      -- dropped a "then up" at (16,9) and met a $93)
      -- (a bomb or detonate press left, or a way along a lane from here,
      -- keeps it: 4-4's bomb-and-back "left 220, bomb, then right" rides
      -- back over the hard blocks)
      local g0, any = readMap(), false
      local lanes = {}
      for _, ln in ipairs(laneEnds(g0, rc, rr)) do lanes[(ln[1] - rc) // ln[3] .. "," .. (ln[2] - rr) // ln[3]] = true end
      local AB = emu.padBits({ a = true, b = true })
      for i = frame - b.start + 1, #b.seq do
        local w = b.seq[i]
        if w & AB ~= 0 then any = true; break end
        local d = (w & 0x0800 ~= 0 and { 0, -1 }) or (w & 0x0400 ~= 0 and { 0, 1 })
          or (w & 0x0200 ~= 0 and { -1, 0 }) or (w & 0x0100 ~= 0 and { 1, 0 })
        if d and (walkable(g0, rc + d[1], rr + d[2]) or lanes[d[1] .. "," .. d[2]]) then any = true; break end
      end
      if not any then
        log(string.format("%6d board search sequence ends on landing at (%d,%d)", frame, rc, rr))
        ride.brk = nil
      end
    end
  end
  -- On a pad by pixel position: +$A8 still names the previous cell for a
  -- few frames after a landing.
  local pc0, pr0 = playerTile()
  local onPad = emu.read(MAP + pr0 * ROW + pc0 * 2, WRAM, false) == SPRING
  if onPad and (lockv & 1) ~= 0 then
    if DeadPad[pr0 * KEY_W + pc0] then
      log(string.format("%6d pad (%d,%d) locked P1 after all; a pad again", frame, pc0, pr0))
      DeadPad[pr0 * KEY_W + pc0] = nil
    end
  end
  if (lockv & 0xF801) == 0x1801 then lastAir = frame end
  if os.getenv("SBM2_TRACE") and springFrom then log(string.format("%6d trace lock %04X px(%d,%d) tile (%d,%d) onPad %s", frame, lockv, r16(P1 + P_X), r16(P1 + P_Y), pc0, pr0, tostring(onPad))) end
  -- $0809: landed from a jump (on or next to a pad), not a death.
  -- Caught by an $E0/$A0 jump pad: its lock is not a death either.
  local onJump = jumpObj[pr0 * KEY_W + pc0] ~= nil and not jumpObj[pr0 * KEY_W + pc0].ride
  if onJump and (lockv & 1) ~= 0 then jumpUntil = frame + 120 end
  if (lockv & 1) ~= 0 and frame <= jumpUntil then lastAir = frame end
  if (lockv & 1) ~= 0 and (onPad or onJump or (lockv & 0xF809) == 0x0809 or (frame >= lastAir and frame - lastAir <= 6)) then
    -- On a spring pad ($0801 there is the pad's lock, not a death) or in
    -- the air ($1801): hold the planned direction through the launch.
    -- Landed on a pad: pick the jump from here toward the last plan's goal.
    if onPad and not springFrom and springTry and springTry[1] == pr0 * KEY_W + pc0 then springDir = springTry[2]
    elseif onPad and not springFrom and lastBfs then
      springDir = bfs(lastBfs[1], pc0, pr0, lastBfs[2], lastBfs[3]) or springDir
    end
    local planDir = springDir
    if onPad and not springFrom then
      -- Pick the jump by what it leads to: the landing with the shortest
      -- route (jumps included) to work - a panel to do, a soft block that
      -- has not grown back, a crystal. (lastBfs is whatever route search
      -- succeeded last, e.g. a bomb's escape check: 3-6 jumped back out of
      -- the side it had just reached.)
      local g0, todo = readMap(), panels()
      local function work(cc, rr)
        if todo[rr * KEY_W + cc] then return true end
        for _, d in ipairs(DIRS) do
          local t = g0[rr + d[2]] and g0[rr + d[2]][cc + d[1]]
          if t == 0x68 or (SOFT[t] and not softGone[(rr + d[2]) * KEY_W + cc + d[1]]) then return true end
        end
        return false
      end
      -- With a remote bomb out, not into its blast (we can't set it off
      -- from a pad, and 3-6 went round between the pad and the blast), and
      -- a landing on floor first (where it can be set off).
      local rc = {}
      for _, b in ipairs(lastBombs or {}) do
        if b[5] then for _, cell in ipairs(blastCells(g0, b[1], b[2], b[4])) do rc[cell[2] * KEY_W + cell[1]] = true end end
      end
      -- Nor into any blast due before P1 is down and away (timed bombs
      -- too: 3-1 jumped "down!" onto its own two bombs, 150 frames).
      local hotLand = {}
      for k, l in pairs(blastWindows(g0)) do
        for _, iv in ipairs(l) do if iv[1] <= 150 and iv[2] >= 0 then hotLand[k] = true end end
      end
      -- Harmful enemies (not gadgets, not hidden) by the landing: a jump
      -- there costs a lot (3-1, runs 32-34: fled its bomb onto (7,8), the
      -- pad sent P1 down into the row-12 corridor between two $8F).
      local foesNear = {}
      for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
        local typ = r16(base + 0x7E) & 0xFF
        if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8060) == 0
            and typ >= 0x80 and typ < 0xC0 and r16(base + P_Y) ~= 0 then
          local ec, er = toTile(r16(base + P_X), r16(base + P_Y))
          -- (one a bomb can't kill, like 3-1's $8F, from 3 cells; others
          -- only right by it: 3-1's last panel sits in the walled centre
          -- with a $99 rider, and every jump in was put off)
          foesNear[#foesNear + 1] = { ec, er, (emu.read(base + 0x43, WRAM, false) & 0x7F) == 0x7F and 3 or 1 }
        end
      end
      local best, bestN
      for _, d in ipairs(DIRS) do
        for _, v in ipairs({ "", "!" }) do
          local land = springLand[(pr0 * KEY_W + pc0) .. d[3] .. v]
          if land == nil then land = ride.springGuess(g0, pc0, pr0, d, v == "!") end
          if land and (land[1] ~= pc0 or land[2] ~= pr0) and not rc[land[2] * KEY_W + land[1]]
              and not hotLand[land[2] * KEY_W + land[1]]
              and ((ride.padAvoid or {})[(pr0 * KEY_W + pc0) .. d[3] .. v] or 0) <= frame then
            local _, n = bfs(g0, land[1], land[2], work)
            if next(rc) and walkable(g0, land[1], land[2]) then n = (n or 1000) - 10000 end
            -- Bouncing pad to pad without touching floor: a pad just
            -- landed on costs extra (3-6 run 30: (10,14) put off "up" for
            -- a $8B by its landing, jumped down to (10,18), whose best way
            -- was back up; 25000 frames to a time-up).
            if n then
              for _, e in ipairs(foesNear) do
                if math.abs(e[1] - land[1]) + math.abs(e[2] - land[2]) <= e[3] then n = n + 2000 end
              end
            end
            local hist = ride.padHist or {}
            if n and #hist >= 4 then
              local lk = land[2] * KEY_W + land[1]
              for i = math.max(1, #hist - 5), #hist do if hist[i] == lk then n = n + 300 end end
            end
            if n and (not bestN or n < bestN) then best, bestN = d[3] .. v, n end
          end
        end
      end
      if best then
        springDir = best
        -- The walk's route wanted another jump, and this one has been taken
        -- 3 times lately (3-6, scenario r79: the seek walked onto (5,8) to
        -- jump down toward the $8E at the bottom, the pick here sent P1
        -- "up" to (5,5) for a soft block, and round again for 10000
        -- frames): go the route's way.
        local pk = pr0 * KEY_W + pc0
        if planDir and planDir ~= best and planDir ~= "stay" then
          local n, hist = 0, (ride.jumpRecent or {})[pk .. best] or {}
          for _, f in ipairs(hist) do if frame - f < 4000 then n = n + 1 end end
          if n >= 3 then
            springDir = planDir
            log(string.format("%6d pad (%d,%d): %s taken %d times lately; the route's %s instead", frame, pc0, pr0, best, n, planDir))
          end
        end
      else
        -- every jump lands in a blast: bounce in place unless the pad is hit
        local cur = springDir and springLand[(pr0 * KEY_W + pc0) .. springDir]
        if cur and hotLand[cur[2] * KEY_W + cur[1]] then springDir = "stay" end
      end
    end
    if onPad and not springDir then springDir = DIRS[math.random(4)][3] end  -- not planned: any way off
    -- An enemy at the landing (3-6's left side: the same contact death on
    -- every retry): bounce in place (nothing held; up to 25 times: the 3-6 one sits
    -- there for a ~1070-frame timer, +$4E) and decide again.
    if onPad and not springFrom and springDir and springDir ~= "stay" then
      local d = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[springDir:match("^%a+")]
      local land = springLand[(pr0 * KEY_W + pc0) .. springDir]
      -- (a landing learnt on this pad itself is a jump that never went: a
      -- bounce taken for one. 3-6, scenario r76: "(5,8) lands on (5,8)"
      -- with an $8B by it, bounced in place 5000 frames)
      if land and land[1] == pc0 and land[2] == pr0 then
        springLand[(pr0 * KEY_W + pc0) .. springDir] = nil; land = nil
      end
      if land == nil and d then land = ride.springGuess(readMap(), pc0, pr0, d, springDir:sub(-1) == "!") end
      ride.stays = ride.stays or 0
      -- ...unless a bomb is about to blast the pad itself (fleeing our own
      -- bomb by the pad, the bounce kept P1 in it: 3-6).
      local g1, padHot = readMap(), false
      for _, b in ipairs(lastBombs or {}) do
        if not b[5] and b[3] < 120 then
          for _, cell in ipairs(blastCells(g1, b[1], b[2], b[4])) do
            if cell[1] == pc0 and cell[2] == pr0 then padHot = true end
          end
        end
      end
      if land and ride.stays < 25 and not padHot then
        -- (enemies that can kill: not the $C0 gate or other gadgets, which
        -- kept 3-6 bouncing by the open exit for 11000 frames)
        local foes = {}
        for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
          local typ = r16(base + 0x7E) & 0xFF
          if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8000) == 0
              and typ >= 0x80 and typ < 0xC0 then
            local ec, er = toTile(r16(base + P_X), r16(base + P_Y))
            foes[#foes + 1] = { ec, er }
          end
        end
        for _, e in ipairs(foes) do
          if math.abs(e[1] - land[1]) + math.abs(e[2] - land[2]) <= 2 then
            log(string.format("%6d pad (%d,%d): enemy near the landing (%d,%d); bouncing", frame, pc0, pr0, land[1], land[2]))
            local pk0 = pr0 * KEY_W + pc0
            ride.lastStay = frame
            local we = ride.walkerEta and ride.walkerEta()[pk0]
            -- Ask the simulator first (3-6, run JA s0: the last $8B kept
            -- around (5,8), the landing of the (5,13) pad, and P1 bounced
            -- ~40 times over 7000 frames waiting for it to go): a jump
            -- that lives 300 frames with room at the end is taken, and its
            -- pads are played as simulated, the flight included.
            local simSeq
            -- (not a jump that lands back on this pad: it lives by going
            -- nowhere; 3-6, scenario r76, "left" from (5,8) back onto (5,8)
            -- by an $8B, jumped every 60 frames for 3000)
            if emu.simulateMany and frame >= (ride.padSimNext or 0) and (land[1] ~= pc0 or land[2] ~= pr0) then
              ride.padSimNext = frame + 50
              local held = springDir:sub(-1) == "!"  -- (held through the flight: a cell further)
              local pj = emu.padBits({ [springDir:match("^%a+")] = true })
              local list = {}
              for _, K in ipairs(held and { 40, 60, 80 } or { 6, 14, 30 }) do
                for _, d2 in ipairs({ "none", "up", "down", "left", "right" }) do
                  local p2 = emu.padBits(d2 == "none" and {} or { [d2] = true })
                  local p0 = emu.padBits({})
                  local seq = {}
                  for f = 1, 300 do seq[f] = f <= K and pj or (f <= 90 and p0 or (f <= 130 and p2 or p0)) end
                  list[#list + 1] = seq
                end
              end
              local res, _, _, _, rch = emu.simulateMany(list, 300, false, 0)
              for i = 1, #list do
                if res[i] == -1 and (not rch or (rch[i] or 99) >= 4) then simSeq = list[i]; break end
              end
              log(string.format("%6d pad (%d,%d) %s with the enemy by (%d,%d): %s", frame, pc0, pr0, springDir,
                land[1], land[2], simSeq and "the simulator lives it; jumping" or "every simulated jump dies"))
            end
            if simSeq then
              ride.brk = { seq = simSeq, start = frame, H = #simSeq, pad = true }
              springFrom = pk0 .. springDir
            elseif we and we < 150 then
              ride.padAvoid = ride.padAvoid or {}
              ride.padAvoid[pk0 .. springDir] = frame + 300
              springDir = nil
            else
              springDir, ride.stays = "stay", ride.stays + 1
            end
            break
          end
        end
      end
      if springDir ~= "stay" then ride.stays = 0 end
    end
    if onPad and not springFrom and springDir then
      if springDir ~= "stay" then
        ride.jumpRecent = ride.jumpRecent or {}
        local jk = (pr0 * KEY_W + pc0) .. springDir
        local h = ride.jumpRecent[jk] or {}
        h[#h + 1] = frame
        if #h > 8 then table.remove(h, 1) end
        ride.jumpRecent[jk] = h
      end
      if os.getenv("SBM2_DBG") then log(string.format("%6d dbg pad (%d,%d) jump %s (last plan %s)", frame, pc0, pr0, springDir, tostring(ride.lastPlan))) end
      springFrom = (pr0 * KEY_W + pc0) .. springDir
    end
    -- Hold it only on the pad: held in the air the flight drifts on (3-1:
    -- (9,6) Down held lands (9,12); let go after the launch, (9,9)).
    -- A "!" jump keeps it held in the air too (it flies a cell further).
    if springDir and springDir ~= "stay" and ((lockv ~= 0x1801 and lockv ~= 0x0809) or springDir:sub(-1) == "!") then
      input[springDir:match("^%a+")] = true
    end
    local pb = ride.brk
    if pb and pb.pad and frame - pb.start < #pb.seq then
      local w = pb.seq[frame - pb.start + 1]
      input = {}
      for nm, bit in pairs({ up = 0x0800, down = 0x0400, left = 0x0200, right = 0x0100 }) do if w & bit ~= 0 then input[nm] = true end end
    end
    step = false
    return
  end
  if springFrom and (lockv & 1) == 0 then
    local c1, r1 = playerTile()
    if not springLand[springFrom] then
      log(string.format("%6d spring %s landed (%d,%d)", frame, springFrom, c1, r1))
      -- (a new landing learnt is progress: the stall check's random
      -- exploring had 3-6, run 27, wandering 6000 frames between tries)
      progressAt = frame
    end
    springLand[springFrom] = { c1, r1 }
    -- (a simulated pad jump ends on landing: its seq held nothing after,
    -- and on the next pad P1 bounced in place; 3-6, run OA s0, learnt
    -- (5,8) left/right/down! as landing on (5,8), the way to the last $8B)
    if ride.brk and ride.brk.pad and emu.read(MAP + r1 * ROW + c1 * 2, WRAM, false) == SPRING then ride.brk = nil end
    if emu.read(MAP + r1 * ROW + c1 * 2, WRAM, false) == SPRING then
      ride.padHist = ride.padHist or {}
      table.insert(ride.padHist, r1 * KEY_W + c1)
      if #ride.padHist > 12 then table.remove(ride.padHist, 1) end
      if #ride.padHist >= 4 and #ride.padHist % 4 == 0 then
        log(string.format("%6d pad loop? %d pad landings in a row", frame, #ride.padHist))
      end
    else
      ride.padHist = {}
    -- (per-stage memory that used to carry over: cell visit counts and
    -- failed bomb tiles from the last stage skewed exploring here, and a
    -- stage-start scenario, which starts with them empty, then played the
    -- stage differently from the full run)
    for k in pairs(visits) do visits[k] = nil end
    for k in pairs(noBombTile) do noBombTile[k] = nil end
    ride.stand, ride.standMiss, ride.wait, ride.try, ride.dodgeGoal, ride.planCell = nil, nil, nil, nil, nil, nil
    end
  elseif springFrom then
    -- Only a death the jump caused (water): an enemy at the landing cell
    -- (3-6's left side) or a blast is bad luck, not the jump's fault.
    local pcell, enemyThere = r16(P1 + 0xA8), false
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and r16(base + 0xA8) == pcell then enemyThere = true end
    end
    if enemyThere or isFlame(emu.read(MAP + pcell, WRAM, false)) then
      log(string.format("%6d spring %s: died on landing (enemy or blast); still a way", frame, springFrom))
    else
      log(string.format("%6d spring %s ended in a death; not again", frame, springFrom))
      springLand[springFrom] = false
    end
  end
  springFrom = nil
  if (lockv & 1) ~= 0 then
    if playing and (lockv & 0xF809) == 0x0801 then  -- not $1801/$0809 (spring jump)
      playing = false
      if r16(P1 + 0xA8) ~= r16(0x5D72) then
        deaths = deaths + 1
        -- A time-up can't be fixed by replaying the last few seconds.
        rollback = timeLeft() > 1 and frame >= sacrificeUntil
      end
      local es = {}
      for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
        if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) then
          es[#es + 1] = string.format("%04X:%02X(%d,%d)", base, r16(base + 0x7E) & 0xFF, r16(base + P_X), r16(base + P_Y))
        end
      end
      local st = {}
      for o = 0xE0, 0xFF do st[#st + 1] = string.format("%02X", emu.read(P1 + o, WRAM, false)) end
      -- Cause: flame under P1, or an enemy in P1's cell (contact), else other.
      local pcell = r16(P1 + 0xA8)
      local cause = isFlame(emu.read(MAP + pcell, WRAM, false)) and "blast" or "other"
      for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
        if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and r16(base + 0xA8) == pcell then
          cause = string.format("contact:%02X", r16(base + 0x7E) & 0xFF)
        end
      end
      -- A death on a gadget's cell (or to nothing seen, outside boss
      -- stages and not a time-up) makes the cell one never walked onto
      -- again this stage (3-2, run 35: explored onto the $E1 at (25,7)
      -- and died there twice).
      do
        local gad = cause:match("^contact:(%x%x)")
        -- ("other" only with no enemy close by: contact is same-cell, and a
        -- $8A a cell off killed P1 in 5-1 as "other")
        local foeBy = false
        for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
          local typ = r16(base + 0x7E) & 0xFF
          if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and typ >= 0x80 and typ < 0xC0
              and math.abs(r16(base + P_X) - r16(P1 + P_X)) < 24 and math.abs(r16(base + P_Y) - r16(P1 + P_Y)) < 24 then foeBy = true end
        end
        if r16(P1 + 0xA8) ~= r16(0x5D72)  -- (not the exit warp)
            and ((gad and tonumber(gad, 16) >= 0xC0) or (cause == "other" and timeLeft() > 1 and not foeBy))
            and (emu.read(0x1A, WRAM, false) & 0x0F) ~= 7 then
          local dk = (pcell >> 7) * KEY_W + (pcell & 0x7F) // 2
          badCell[dk] = true
          log(string.format("%6d death on (%d,%d) by %s: never walked onto again this stage", frame, dk % KEY_W, dk // KEY_W, cause))
        end
      end
      local bs = {}
      for i = 0, BOMB_SLOTS - 1 do
        local base = BOMBS + i * BOMB_STRIDE
        if (emu.read(base + 1, WRAM, false) & 0x80) ~= 0 then
          local cell = r16(base + 2)
          bs[#bs + 1] = string.format("(%d,%d)f%d/%02X", (cell & 0x3F) // 2, cell >> 6,
            emu.read(base + 4, WRAM, false), emu.read(base + 1, WRAM, false))
        end
      end
      for _, b in ipairs(lastBombs or {}) do bs[#bs + 1] = string.format("seen(%d,%d)t%d", b[1], b[2], b[3]) end
      cause = cause .. " bombs " .. table.concat(bs, ",")
      -- Boss-engine objects alive now (state, position): which part killed us.
      local bo = {}
      for base = 0x1000, 0x17C0, 0x40 do
        local st = emu.read(base + 2, WRAM, false)
        if st >= 0xC0 then
          bo[#bo + 1] = string.format("%04X@%02X%04X(%d,%d)", base, st, r16(base), r16(base + 0x11), r16(base + 0x14))
        end
      end
      if #bo > 0 then cause = cause .. " boss " .. table.concat(bo, ",") end
      -- Killed by a blast: after the rollback, don't bomb those cells for
      -- a while (the replay made the same choice: 3-6 died 16 times alike).
      if cause:find("^blast") then
        for _, b in ipairs(lastBombs or {}) do
          if b[3] < 100000 then noBombUntil[b[2] * KEY_W + b[1]] = frame + 600 end
        end
      end
      -- (on the exit cell this lock is the warp out, not a death)
      log(string.format("%6d %s %s lives %d at px(%d,%d) enemies %s", frame,
        r16(P1 + 0xA8) == r16(0x5D72) and "EXIT warp" or "DEATH", cause, emu.read(0x1C6C, WRAM, false),
        r16(P1 + P_X), r16(P1 + P_Y), table.concat(es, " ")) .. string.format(" stats[E0..FF] %s after: %s",
        table.concat(st, " "), table.concat(recent, " | ")))
      writeFile(string.format("%s/death_%06d.png", OUT, frame), emu.takeScreenshot())
      -- SBM2_STOPEARLY (scenarios): the first death settles the run.
      if STOPEARLY and r16(P1 + 0xA8) ~= r16(0x5D72) then StopNow = true end
      logMapNext = true  -- (the map then: what the blast model got wrong)
    end
    step = false
    return
  end
  if step == false then step = nil end
  if not playing and FAKE_HEARTS then logMapNext = true end
  playing = true
  -- The border is always hard wall during play; anything else means the
  -- stage is loading or a cutscene is up.
  -- In a normal-game stage: hard border tile, not battle mode ($B9 bit 15),
  -- normal flag $BC, player 1 object live. Anything else (title, attract
  -- demo, cutscenes, battle) counts as off-stage.
  local inStage = HARD[emu.read(MAP, WRAM, false)] and (r16(0xB9) & 0x8000) == 0
    and emu.read(0xBC, WRAM, false) ~= 0 and (r16(P1) & 0xF000) == 0x9000  -- $BC is 2 in 3-4
  if not inStage then
    -- Out of a stage. After a game over the CONTINUE screen waits for Start
    -- (and times out to the title); pulse Start once it has been a while.
    -- Out of a stage (intro card, cutscene, or game over -> title). After
    -- a long time out, assume game over and let drive.py continue from the
    -- world's password state (what CONTINUE + password does in the game).
    offStage = offStage + 1
    if offStage == 3000 then  -- longer than any intro or cutscene seen so far
      writeFile(OUT .. "/gameover.txt", tostring(lastWorld + 1))
      log(string.format("%6d game over in world %d", frame, lastWorld + 1))
      gameOver = true
    end
    return
  end
  offStage = 0
  -- Stage-start state (+ sidecar): after a game over, drive.py continues
  -- from it rather than the world's password state.
  local stNow = emu.read(0x1A, WRAM, false)
  -- The stage-start state is taken a frame or two after the STAGE frame
  -- (callback below); a scenario from it starts there with fresh memory.
  -- So the full run starts the stage over at that frame too: detection,
  -- reset and reseed again (same entry count), the plan and step dropped,
  -- and the .frame written now. Then the scenario replays the run exactly.
  local redetect = false
  if ride.redetect == stNow and savedStage == stNow then
    ride.redetect, savedStage, redetect = nil, nil, true
    ride.entries[stNow] = ride.entries[stNow] - 1
    step, planSteps, planIdx, planCo, trapCell, pushCell = nil, nil, 1, nil, nil, nil
    pushNext, lastObjFrame, lastPending, settleNeed = 0, 0, 0, 130
  end
  if stNow ~= savedStage then
    -- SBM2_STOPEARLY: so does the next stage.
    if STOPEARLY and savedStage then
      log(string.format("%6d STAGE %02X", frame, stNow)); StopNow = true; return
    end
    savedStage = stNow
    -- Random choices restart at each stage entry from (seed, stage, entry
    -- count): a stage-start scenario then replays a full run's stage
    -- exactly (before, the RNG state carried over from earlier stages, and
    -- 2-2_r53 passed 4/4 where the full run died every time). The entry
    -- count keeps a game-over continue from repeating the same choices.
    ride.entries = ride.entries or {}
    ride.entries[stNow] = (ride.entries[stNow] or 0) + 1
    math.randomseed(SEED * 7919 + stNow * 104729 + ride.entries[stNow] * 1299709)
    softSeen, softGone, springLand, springTry, ride.fail, ride.edges, ride.tried = {}, {}, {}, nil, {}, {}, {}
    ride.panelSol = nil
    ride.yLine, ride.cfix, ride.jumpRecent, ride.badLift = {}, nil, {}, nil
    ride.newSigAt, ride.memResetAt = frame, nil
    ride.nDC = nil
    ride.padHist = {}
    -- (per-stage memory that used to carry over: cell visit counts and
    -- failed bomb tiles from the last stage skewed exploring here, and a
    -- stage-start scenario, which starts with them empty, then played the
    -- stage differently from the full run)
    for k in pairs(visits) do visits[k] = nil end
    for k in pairs(noBombTile) do noBombTile[k] = nil end
    ride.stand, ride.standMiss, ride.wait, ride.try, ride.dodgeGoal, ride.planCell = nil, nil, nil, nil, nil, nil
    ride.gateAt, ride.skateHeld = nil, nil  -- (a stale gateAt from the last stage would cut the skate hold short)
    DeadPad, HasPad = {}, false
    softBlownAt = {}
    badCell, planSteps, planCo, noBombUntil = {}, nil, nil, {}
    stageTicks = ticks
    StageStart = frame
    log(string.format("%6d STAGE %02X hearts %02X flash %04X/%02X", frame, stNow, emu.read(P1 + 0xEF, WRAM, false),
      r16(P1 + 4), emu.read(P1 + 0xB9, WRAM, false)))
    local hs = {}
    for k, t in pairs(heartCells()) do hs[#hs + 1] = string.format("(%d,%d)%s", k % KEY_W, k // KEY_W, t) end
    log(string.format("%6d heart items: %s", frame, table.concat(hs, " ")))
    local its = {}
    for i = 0, 63 do
      local cell = r16(ITEMLIST + 4 * i)
      if cell >= 0x8000 then break end
      its[#its + 1] = string.format("(%d,%d)k%d", (cell & 0x7F) // 2, cell >> 7, r16(ITEMLIST + 4 * i + 2) & 0x3FFF)
    end
    log(string.format("%6d hidden items: %s", frame, table.concat(its, " ")))
    local ab = {}
    for i = 0xE0, 0xFF do ab[#ab + 1] = string.format("%02X", emu.read(P1 + i, WRAM, false)) end
    log(string.format("%6d P1 +E0..FF %s", frame, table.concat(ab, " ")))
    if redetect then
      -- (the frame number too: decisions keyed on frame parity/period
      -- then replay the same in a scenario, see scenario.py SBM2_FRAME0)
      local ff = io.open(string.format("%s/stage_%02X.frame", OUT, stNow), "w")
      if ff then ff:write(tostring(frame)); ff:close() end
      log(string.format("%6d stage %02X restarted at its saved state", frame, stNow))
    else
    local first = frame <= FRAME0 + 1  -- (a scenario: its state is this one)
    onNextInstruction(function()
      local data = emu.createSavestate()
      inputlog.saveState(OUT .. "/stage.mss", data)
      -- A library of stage starts (bot/bench.py collects them).
      local lib = string.format("%s/stage_%02X.mss", OUT, stNow)
      local fh = io.open(lib, "rb")
      if fh then fh:close() else
        inputlog.saveState(lib, data)
        if not first then ride.redetect = stNow end
      end
    end)
    end
  end
  -- Lives assist (logged): a game over restarts the whole world, so keep
  -- lives ($1C6C) topped up; deaths are still retried by rollback first.
  -- (an assist like the others: not with SBM2_NOASSIST, where a game over
  -- makes drive.py restart the stage)
  if not NOASSIST and emu.read(0x1C6C, WRAM, false) < 2 then
    inputlog.write16(0x1C6C, (r16(0x1C6C) & 0xFF00) | 3)
    local msg = string.format("frame %d stage %02X: lives assisted", frame, emu.read(0x1A, WRAM, false))
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  lastWorld = (emu.read(0x1A, WRAM, false) >> 4) - 1  -- $1A = world<<4 | stage-1
  -- A blasted $90/$98 is a mount: walking onto it boards it (C1:C983,
  -- within 8 px in its cell). Riding, a hit costs the mount, not a life
  -- (C1:553C, C0:A630), but no spring pad launches P1: 3-1 run 30 rode
  -- one and could not reach the walled centre's last panel (2 time-ups).
  -- In stages with pads, mounts are not walked onto.
  do
    local rd = (r16(P1 + 0x60) & 0x8000) ~= 0
    if rd ~= Riding then log(string.format("%6d riding a mount: %s", frame, tostring(rd))) end
    Riding = rd
  end
  AvoidCell = {}
  if HasPad and not Riding then
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      local t = r16(base + 0x7E) & 0xFF
      local be = r16(base + 0xBE)
      if (r16(base) & 0xF000) == 0x9000 and (t == 0x98 or t == 0x90) and (be & 0x8000) ~= 0 and (be & 0x4000) == 0 then
        local mc, mr = toTile(r16(base + P_X), r16(base + P_Y))
        AvoidCell[mr * KEY_W + mc] = true
      end
    end
  end
  -- A $DD track block crushes P1 ahead of it: track cells ($4D/$CD) within
  -- 5 cells of one are not walked onto (4-5, runs 44-45: explored back onto
  -- the row-8 track after stepping off it and was crushed). A crossing (open
  -- floor on both sides across the track) 2+ cells from the block is still
  -- passable: it moves a cell per 32 frames, P1 crosses in ~16 (4-3, 3 of
  -- 4 seeds: the whole row-14 track barred the way to the last enemy's
  -- block, time-up).
  local function trackCrossing(cc, rr)
    local function tt(c2, r2) return emu.read(MAP + r2 * ROW + c2 * 2, WRAM, false) end
    local function isTrack(t) return t == 0x4D or t == 0xCD end
    local function open(t) return (t & 0xC0) == 0 end
    if rr < 1 or cc < 1 then return false end
    if isTrack(tt(cc - 1, rr)) or isTrack(tt(cc + 1, rr)) then return open(tt(cc, rr - 1)) and open(tt(cc, rr + 1)) end
    if isTrack(tt(cc, rr - 1)) or isTrack(tt(cc, rr + 1)) then return open(tt(cc - 1, rr)) and open(tt(cc + 1, rr)) end
    return false
  end
  for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
    if (r16(base) & 0xF000) == 0x9000 and (r16(base + 0x7E) & 0xFF) == 0xDD then
      local dc, dr = toTile(r16(base + P_X), r16(base + P_Y))
      for rr = dr - 5, dr + 5 do
        for cc = dc - 5, dc + 5 do
          if math.abs(rr - dr) + math.abs(cc - dc) <= 5 and rr >= 0 and cc >= 0 then
            local t = emu.read(MAP + rr * ROW + cc * 2, WRAM, false)
            local d = math.abs(rr - dr) + math.abs(cc - dc)
            if (t == 0x4D or t == 0xCD) and (d <= 1 or not trackCrossing(cc, rr)) then AvoidCell[rr * KEY_W + cc] = true end
          end
        end
      end
    end
  end
  -- cells the lookahead shield kept P1 from stepping into (for a while:
  -- the bot plans around them instead of pushing into them again)
  if ride.laBlock then
    for k, untilF in pairs(ride.laBlock) do
      if frame < untilF then AvoidCell[k] = true else ride.laBlock[k] = nil end
    end
  end
  do
    local pc, pr = toTile(r16(P1 + P_X), r16(P1 + P_Y))
    AvoidCell[pr * KEY_W + pc] = nil  -- (never P1's own cell: it must be able to leave)
  end
  local g = readMap()
  ride.pocketG, ride.openers = nil, nil  -- (pockets afresh each frame)
  ride.crystals = 0
  for rr = 0, MAP_H - 1 do for cc = 0, MAP_W - 1 do if g[rr][cc] == 0x68 then ride.crystals = ride.crystals + 1 end end end
  -- Exit gate: map index at $7E5D72; open once $7E5D94 bit 15 (all gold
  -- panels blasted, or no panels: $FFFF) is set (checked at C1:41xx).
  local gate = r16(0x5D72)
  local gc, gr = (gate & 0x7F) // 2, gate >> 7
  local gateOpen = g[gr] ~= nil and gate ~= 0 and g[gr][gc] == 0x49  -- open gate tile (C1:41A8 via $09)
  -- 5-8 shows an open gate tile at (8,3) before the boss comes, with a
  -- $C0 in it that kills on contact: no way out there, keep off it.
  if emu.read(0x1A, WRAM, false) == 0x57 and gateOpen then gateOpen = false; g[gr][gc] = 0xC9 end
  -- 5-7 without the glove: 5-8 form 2 is hurt only by thrown bombs and its
  -- arena has no blocks, so one must be carried in (5-7 hides one, item
  -- kind 7, at (13,10)). While it is hidden, the gate counts as shut (a
  -- route through it would leave the stage).
  local gloveHidden = false
  -- (5-7 only: other world 5 stages with a glove hidden out of reach were
  -- never left, 5-5 and 5-6)
  if emu.read(0x1A, WRAM, false) == 0x56 and emu.read(P1 + 0xFE, WRAM, false) == 0 then
    for i = 0, 63 do
      local cell = r16(ITEMLIST + 4 * i)
      if cell >= 0x8000 then break end
      local kind = r16(ITEMLIST + 4 * i + 2)
      -- hidden, or revealed and still lying there (tile $44)
      if (kind & 0x3FFF) == 7 and ((kind & 0x4000) == 0 or emu.read(MAP + cell, WRAM, false) == 0x44) then
        gloveHidden = true
      end
    end
  end
  -- 5-7 also hides the remote (item kind 5 sets +$F3 bit 0): 5-8's form 1
  -- plan needs it, and a death in world 5 takes it (run 43 met 5-8
  -- without it and died there 24 times). Same hold as the glove.
  if emu.read(0x1A, WRAM, false) == 0x56 and (emu.read(P1 + 0xF3, WRAM, false) & 1) == 0 then
    for i = 0, 63 do
      local cell = r16(ITEMLIST + 4 * i)
      if cell >= 0x8000 then break end
      local kind = r16(ITEMLIST + 4 * i + 2)
      if (kind & 0x3FFF) == 5 and ((kind & 0x4000) == 0 or emu.read(MAP + cell, WRAM, false) == 0x44) then
        gloveHidden = true
      end
    end
  end
  if gloveHidden and gateOpen then gateOpen = false; g[gr][gc] = 0xC9 end
  -- At walking speed 1 (+$E0; a death takes the skate away), a skate (item
  -- kind 4) still in the stage comes before the exit, for up to 3000
  -- frames after the gate opens (run 14 left 1-6 and 1-7 with one in each,
  -- met the 1-8 boss at speed 1 and could not hit it). Revealed useful
  -- items lying about are picked up before the exit too (usefulNow).
  usefulNow, skateNow = {}, {}
  local skateHere = false
  for i = 0, 63 do
    local cell = r16(ITEMLIST + 4 * i)
    if cell >= 0x8000 then break end
    local kind = r16(ITEMLIST + 4 * i + 2)
    local kd = kind & 0x3FFF
    local lying = (kind & 0x4000) ~= 0 and emu.read(MAP + cell, WRAM, false) == 0x44
    local k = (cell >> 7) * KEY_W + (cell & 0x7F) // 2
    if lying and (kd == 4 or kd == 11 or kd == 7 or kd == 1 or kd == 0 or kd == 2
        or (kd == 5 and emu.read(0x1A, WRAM, false) == 0x56)) then usefulNow[k] = true end
    if lying and kd == 4 and emu.read(P1 + 0xE0, WRAM, false) < 2 then skateNow[k] = true end
    if kd == 4 and ((kind & 0x4000) == 0 or lying) then skateHere = true end
  end
  if gateOpen and not ride.gateAt then ride.gateAt = frame elseif not gateOpen and not gloveHidden then ride.gateAt = nil end
  if skateHere and gateOpen and emu.read(P1 + 0xE0, WRAM, false) < 2 and frame - ride.gateAt < 3000 then
    if not ride.skateHeld then log(string.format("%6d gate open, but a skate is still in the stage: getting it first", frame)) end
    ride.skateHeld = true
    gateOpen = false; g[gr][gc] = 0xC9
  elseif not skateHere or emu.read(P1 + 0xE0, WRAM, false) >= 2 then
    ride.skateHeld = nil
  end
  if gateOpen then g[gr][gc] = GATE_OPEN end
  RANGE = emu.read(P1 + P_FIRE, WRAM, false) + 1
  remoteOn = (emu.read(P1 + 0xF3, WRAM, false) & 1) ~= 0
  local c, r, x, y = playerTile()
  -- Frames per tile from P1's walking speed (speed items: 5-8 walks
  -- ~1.4 px/f, 11 frames a tile, not the base 20): route timings and goal
  -- checks are only right at the real pace. 5-8 only for now: the stage
  -- planners elsewhere are tuned around 20 (4-2_panels failed with it).
  do
    local lx, ly = throw.lastX, throw.lastY
    throw.lastX, throw.lastY = x, y
    local d = lx and (math.abs(x - lx) + math.abs(y - ly)) or 0
    if d > 0 and d <= 4 then
      throw.speed = (throw.speed or 0.8) * 0.9 + d * 0.1
      if emu.read(0x1A, WRAM, false) == 0x57 then FPT = math.max(9, math.min(22, math.floor(16 / throw.speed + 0.5))) end
    end
  end
  local windows = blastWindows(g)
  -- A thrown bomb blows up where it hits (5-8 form 2: in the body's box,
  -- 1-3 cells out) or a while after landing (3 cells, or 4 after a
  -- bounce): for 45 frames its blasts from any of those cells are danger,
  -- so P1 steps off the line at once (it died walking along it).
  if throw.land and frame - throw.at < 45 then
    local tc, tr, d = throw.land[1], throw.land[2], throw.land[3]
    for k = 1, 4 do
      for _, cell in ipairs(blastCells(g, tc + d[1] * k, tr + d[2] * k, RANGE)) do
        local key = cell[2] * KEY_W + cell[1]
        windows[key] = windows[key] or {}
        table.insert(windows[key], { 0, 45 - (frame - throw.at) + FLAME_FRAMES })
      end
    end
  end
  local danger = {}
  for k in pairs(windows) do danger[k] = true end
  -- Enemies: predicted paths give contact windows (eWin) and kill chances.
  -- enemies/walkers (cells) still drive the exit and hunt plans.
  local elist = Enemies.read(g)
  -- 5-1 tank: while the light switch still makes its $8As fast chasers
  -- (P1 can't outrun them, 2 HP each) and P1 holds 2+ hearts, they don't
  -- count as harmful: the opening (ride.sw51) goes ahead, a hit costing a
  -- heart and the flash after it. (Every opening step bombs P1's pocket
  -- open to them, and blown blocks here grow back while P1 waits: run RB
  -- s0 hid in column 2 with 8 hearts; time-up, 4 of 7 panels left.)
  do
    local hz = emu.read(P1 + 0xEF, WRAM, false)
    local sw = r16(0x5AAC)
    local tank = emu.read(0x1A, WRAM, false) == 0x50 and sw ~= 0xFFFF and (sw & 1) == 0 and hz >= 2 and hz < 0x80
    if tank ~= (ride.tank or false) then
      log(string.format("%6d 5-1 tank %s (%d hearts)", frame, tank and "on: $8A contact costs a heart" or "off", hz))
    end
    ride.tank = tank
    if tank then
      for _, e in ipairs(elist) do
        if e.type == 0x8A then e.harmful, e.chaser = false, false end
      end
    end
  end
  -- A new $DC platform (bombing a $6A spawns one: C1:4A16) may serve a
  -- lane written off as "no ride came": all lanes are tried again. (4-2,
  -- run SH3 s0: waited at (17,8) before the column-17 platform existed,
  -- wrote the lane off, then bombed (28,8)'s $6A and never went back to
  -- the open gate at (17,3); time-up.)
  do
    local nDC = 0
    for _, e in ipairs(elist) do if e.type == 0xDC then nDC = nDC + 1 end end
    if nDC > (ride.nDC or 0) and ride.nDC and next(ride.fail) then
      log(string.format("%6d a new $DC platform: written-off lanes tried again", frame))
      ride.fail = {}
    end
    ride.nDC = nDC
  end
  -- Stage start: enemies not placed yet (cell (0,0), +$A8 still 0): no
  -- plans on blind information (3-2, runs 30 and 31: bombed at frame 0,
  -- hid in a $90's row it could not see yet and was shot there).
  -- (all of them: one waiting in a pipe reads (0,0) too, and a
  -- freeze on that cost world 1 and 2 scenarios)
  -- (+$A8 0 with a y set: one waiting in a pipe reads y 0; and only in
  -- a stage's first frames: boss parts read so for good, and 1-8 and 5-8
  -- never bombed)
  if frame - StageStart > 60 then
    if Blind then log(string.format("%6d enemies: placed (or never will be)", frame)) end
    Blind = false
  else
    local placed, unplaced = 0, 0
    for _, e in ipairs(elist) do
      if not e.gadget then
        if e.c == 0 and e.r == 0 and r16(e.base + P_Y) ~= 0 then unplaced = unplaced + 1 else placed = placed + 1 end
      end
    end
    local b = unplaced > 0 and placed == 0
    if b ~= Blind then
      local t = {}
      for _, e in ipairs(elist) do t[#t + 1] = string.format("%02X(%d,%d)y%d%s", e.type, e.c, e.r, r16(e.base + P_Y), e.gadget and "g" or "") end
      log(string.format("%6d enemies %s: %s", frame, b and "not placed yet: no bombs" or "placed", table.concat(t, " ")))
    end
    Blind = b
  end
  -- $DC platforms: since when each has been still (docked at a lane end).
  -- dockedNear(cc, rr, maxParked): one within 2 cells of (cc,rr), still for
  -- 2..maxParked frames (it waits 64 at each end: time left to board).
  -- (Any $DC within 2 counted as an escape cost 4-4 thousands of deaths.)
  function ride.dockedNear(cc, rr, maxParked)
    for _, e in ipairs(elist) do
      local dk = ride.dock[e.base]
      -- (still 4+ frames: at 0.406 px/f a moving one keeps a pixel for up
      -- to 3; 4-3, run 32, bombed counting on a platform that was leaving)
      if e.type == 0xDC and dk and math.abs(e.c - cc) + math.abs(e.r - rr) <= 2
          and frame - dk[3] >= 4 and frame - dk[3] <= maxParked then return true end
    end
    return false
  end
  ride.dock = ride.dock or {}
  for _, e in ipairs(elist) do
    if e.type == 0xDC then
      local px, py = r16(e.base + P_X), r16(e.base + P_Y)
      local d = ride.dock[e.base]
      if not d or d[1] ~= px or d[2] ~= py or frame < d[3] then ride.dock[e.base] = { px, py, frame } end
    end
  end
  jumpObj = {}
  for _, e in ipairs(elist) do
    -- ($E1: 3-2's pad at (25,7), launching left like $A0; walked over as
    -- a floor cell it killed P1 in runs X2 s1 and Y2 s2, routed as a jump
    -- it carries P1 back to the left half, the last crystal's side)
    if e.type == 0xE0 or e.type == 0xA0 or e.type == 0xE1 then
      jumpObj[e.r * KEY_W + e.c] = { e.c + (e.type == 0xE0 and 8 or -8), e.r }
    end
  end
  for k, e in pairs(ride.edges) do
    if not jumpObj[k] then jumpObj[k] = { e[1], e[2], ride = e } end
  end
  local samples = Enemies.rollouts(g, elist, c, r, ROLLOUTS, HORIZON)
  local hasEater = false
  for _, e in ipairs(elist) do if e.type == Enemies.EATER then hasEater = true end end
  local eWin = Enemies.windows(samples, elist, HORIZON, ENEMY_MARGIN)
  -- Bosses: their boxes are no-go for the whole horizon; flames on their
  -- sense points (where they'll be when the bomb goes off) hurt them.
  -- 5-8 form 2 flies straight at waypoint +$28 & 15 (table CC:D272).
  Boss.target[0x1000] = nil
  if finalForm2() then
    local wp = 0xCCD272 + (r16(0x1028) & 15) * 4
    local function rom16(ad) return emu.read(ad, emu.memType.snesMemory, false) | (emu.read(ad + 1, emu.memType.snesMemory, false) << 8) end
    Boss.target[0x1000] = { rom16(wp), rom16(wp + 2) }
    if not Boss.waypoints then
      Boss.waypoints = {}
      for i = 0, 15 do Boss.waypoints[i + 1] = { rom16(0xCCD272 + i * 4), rom16(0xCCD272 + i * 4 + 2) } end
    end
  end
  local bosses = Boss.read()
  if os.getenv("SBM2_DBG") and emu.read(0x1A, WRAM, false) == 0x17 and frame % 60 == 0 then
    log(string.format("%6d dbg sunX now %s +150 %s", frame, tostring(Boss.sunX(0)), tostring(Boss.sunX(150))))
  end
  bossesNow = bosses
  if os.getenv("SBM2_DBG2") and frame % 4 == 0 then
    local t = {}
    for _, b in ipairs(bosses) do if b.base ~= 0x1000 then t[#t + 1] = string.format("%04X(%d,%d v%.2f,%.2f)", b.base, b.x, b.y, b.vx, b.vy) end end
    log(string.format("%6d dbg2 kids %s", frame, table.concat(t, " ")))
  end
  local bossNow, bossSense, bossEat = {}, {}, {}
  if #bosses > 0 then
    bossNow = Boss.contactCells(bosses, Boss.MARGIN, 0)
    for k, l in pairs(Boss.windows(bosses, HORIZON, Boss.MARGIN, Boss.SLACK)) do
      local e = eWin[k] or {}
      for _, iv in ipairs(l) do e[#e + 1] = iv end
      eWin[k] = e
    end
    for _, b in ipairs(bosses) do if b.hurt then b.sense = nil end end
    bossSense = Boss.senseCells(bosses, NEW_FUSE)
    bossEat = Boss.senseCells(bosses, 0)
    if finalForm2() then bossSense = {} end  -- flames don't hurt form 2
    -- With the remote we pick the moment: a blast within a few cells of
    -- where a boss is now is worth setting up (bosses come to P1).
    if remoteOn then
      bossSense = {}
      for k in pairs(bossEat) do
        local bc, br = k % KEY_W, k // KEY_W
        for dr = -3, 3 do
          for dc = -3 + math.abs(dr), 3 - math.abs(dr) do bossSense[(br + dr) * KEY_W + bc + dc] = true end
        end
      end
    end
    -- 5-8 form 1: bombs only into a slam (the "slam" plan lures one; see
    -- there). Patrolling, the body is out of reach from where P1 can stand.
    if emu.read(0x1A, WRAM, false) == 0x57 and not finalForm2() then
      local st = r16(0x1000) | (emu.read(0x1002, WRAM, false) << 16)
      bossSense = {}
      if st == 0xCCCAC4 then
        for t = 0, 90, 15 do for k in pairs((Boss.senseCells(bosses, t))) do bossSense[k] = true end end
      end
    end
    if frame % 60 == 0 then
      local t = {}
      for _, b in ipairs(bosses) do
        t[#t + 1] = string.format("%04X@%02X%04X(%d,%d v%.2f,%.2f hp%d%s%s)", b.base, emu.read(b.base + 2, WRAM, false),
          r16(b.base), b.x, b.y, b.vx, b.vy, r16(b.base + 0x20),
          b.box and string.format(" box%d..%d,%d..%d", b.box[1], b.box[2], b.box[3], b.box[4]) or "", b.sense and " sense" or "")
      end
      log(string.format("%6d bosses %s", frame, table.concat(t, " ")))
    end
  end
  local cw = combine(windows, eWin)   -- blasts + enemies, for routes
  -- Lane ends with a harmful enemy on or by them: no ride there (4-4, run
  -- 37: rode to (2,10), where an $89 sat, three deaths alike).
  RideBlock = {}
  -- (not after 3000 frames with nothing new: the enemies left may be the
  -- ones by the lane end, and the ride the only way to them; boarding still
  -- waits for a board search that lives. 4-5, run half45 s0: two $93 by the
  -- (9,6) lane end, P1 on the far side, stalled until time-up)
  local relax = frame - (ride.newSigAt or frame) > 3000
  ride.relax = relax
  if relax and not ride.relaxLogged then
    ride.relaxLogged = true
    log(string.format("%6d no progress for 3000 frames: lane ends by enemies are rides again (board search decides)", frame))
  elseif not relax then ride.relaxLogged = nil end
  if not relax then
  -- (and lane ends a walker or chaser reaches within 250 frames, last
  -- frame's estimate: a ride and a wait for the way back)
  do
    local we = ride.walkerEta and ride.walkerEta()
    local ce = ride.chaserEta and ride.chaserEta()
    for _, m in ipairs({ we or {}, ce or {} }) do
      for k, t in pairs(m) do if t < 250 and g[k // KEY_W] and g[k // KEY_W][k % KEY_W] == BOARD then RideBlock[k] = true end end
    end
  end
  for _, e in ipairs(elist) do
    if e.harmful and not e.gadget then
      -- (within 3 cells: a ride takes ~60 frames, and the $89 walked up
      -- from (2,12) as P1 rode in)
      for dr = -3, 3 do for dc = -3, 3 do
        if math.abs(dr) + math.abs(dc) <= 3 then RideBlock[(e.r + dr) * KEY_W + e.c + dc] = true end
      end end
    end
  end
  end
  local enemies, walkers = {}, {}
  for _, cell in ipairs(Boss.centres(bosses)) do
    enemies[#enemies + 1] = cell; walkers[#walkers + 1] = cell
  end
  local reach = {}                    -- cells a killable enemy may pass through
  for i, e in ipairs(elist) do
    if not e.gadget and e.type < 0xC0 and (e.c ~= gc or e.r ~= gr) then
      enemies[#enemies + 1] = { e.c, e.r }
      if not HARD[g[e.r][e.c]] then walkers[#walkers + 1] = { e.c, e.r } end
    end
    if e.killable then
      for _, path in ipairs(samples[i]) do
        for _, iv in ipairs(path) do reach[iv[1]] = true end
      end
    end
  end
  -- Dormant enemies (+4 bit 15: parked off-screen, e.g. on 3-6's islands)
  -- still keep the gate shut; go and find them.
  local dormant = {}
  if not gateOpen then
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      local typ = r16(base + 0x7E) & 0xFF
      local py = r16(base + P_Y)
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8000) ~= 0 and typ >= 0x80 and typ < 0xC0
          and py >= 16 and py < 0x8000 then
        local dc, dr = toTile(r16(base + P_X), r16(base + P_Y))
        if g[dr] and g[dr][dc] then dormant[#dormant + 1] = { dc, dr } end
      end
    end
    -- Only once no active enemy is left: most dormant ones come back alone.
    if #walkers == 0 then
      for _, d in ipairs(dormant) do
        if not HARD[g[d[2]][d[1]]] then walkers[#walkers + 1] = d end
      end
    end
  end
  -- Cells harmful enemies hold now (or are entering): fallback routes avoid them.
  local enemyNow, mobNow = {}, {}
  for k in pairs(bossNow) do enemyNow[k] = true end
  for _, e in ipairs(elist) do
    if e.harmful then
      enemyNow[e.r * KEY_W + e.c] = true
      mobNow[e.r * KEY_W + e.c] = true
      if e.moving then enemyNow[e.tr * KEY_W + e.tc] = true; mobNow[e.tr * KEY_W + e.tc] = true end
    end
  end
  -- Cells an enemy may be in within the next 1.5 tiles' walk: never step there.
  local nearEnemy = {}
  for k, l in pairs(eWin) do
    for _, win in ipairs(l) do
      if win[1] <= FPT * 1.5 and win[2] >= 0 then nearEnemy[k] = true; break end
    end
  end
  -- 1-8: the body's lane (rows 2-7, its box from y 32 to 120) counts as
  -- enemy-held: routes keep out of it and P1 caught in it dodges out (it
  -- went up there by dodges and seeks, and the hands got it).
  -- (Only within 80 px of the body: a bomb set in the lane ahead of it is
  -- how it gets hit, and far from it the lane is safe for a while.)
  if emu.read(0x1A, WRAM, false) == 0x17 then
    local bxs = {}
    for _, b in ipairs(Boss.read and bossesNow or {}) do bxs[#bxs + 1] = b.x end
    local ox = origin()
    for rr = 2, 7 do
      for cc = 0, MAP_W - 1 do
        local cx = cc * 16 + 8 - ox
        local near = #bxs == 0
        for _, bx in ipairs(bxs) do if math.abs(cx - bx) < 80 then near = true end end
        if near then nearEnemy[rr * KEY_W + cc] = true end
      end
    end
  end
  local panelTodo, panelDone = panels()
  local heartNow = heartCells()
  -- Mode 1 panels (worlds 3-5) flip on every blast, so which spots to bomb
  -- is a parity puzzle: find reachable spots whose blasts together flip
  -- exactly the unfinished panels (Gaussian elimination over GF(2)).
  local panelPlan
  if next(panelTodo) and (r16(0x5D94) & 1) ~= 0 then
    local idx, np, target = {}, 0, 0
    for k in pairs(panelTodo) do idx[k] = np; target = target | (1 << np); np = np + 1 end
    for k in pairs(panelDone) do idx[k] = np; np = np + 1 end
    local function xorSet(a, b)
      local o = {}
      for k in pairs(a) do o[k] = true end
      for k in pairs(b) do o[k] = not o[k] or nil end
      return o
    end
    -- (bombs down now flip their panels when they go off: the plan is for
    -- what is left after them; 4-3, a plan bomb laid at (4,18) made the plan
    -- not add up until it blew, and a new one replaced it)
    for _, b in ipairs(lastBombs) do
      for _, bc in ipairs(blastCells(g, b[1], b[2], b[4], b[6])) do
        local i = idx[bc[2] * KEY_W + bc[1]]
        if i then target = target ~ (1 << i) end
      end
    end
    -- (on foot first; rides only when walking finds no solution: with the
    -- lanes always in, world 4 plans waited on rides and 4-2..4-5 timed out)
    local basis, maskOf, nulls, sol
    for pass = 1, 2 do
      local reach, list, head = { [r * KEY_W + c] = true }, { { c, r } }, 1
      while list[head] do
        local cc, rr = list[head][1], list[head][2]; head = head + 1
        for _, d in ipairs(DIRS) do
          local c2, r2 = cc + d[1], rr + d[2]
          local k = r2 * KEY_W + c2
          if not reach[k] and walkable(g, c2, r2) then reach[k] = true; list[#list + 1] = { c2, r2 } end
        end
        -- (rides and jumps learned this stage count: 4-3, run IA s0, the last
        -- two panels were in the lower left pocket the rides reach; up top
        -- the walk-only reach had no solution, "plan: none" until time-up)
        -- (and platform lanes not ridden yet, unless one never came: from
        -- (9,25) the lane back to the pocket at (4,25) was not learned)
        local hops = {}
        if pass == 2 then
        local jo = jumpObj[rr * KEY_W + cc]
        if jo then hops[1] = jo end
        if g[rr][cc] == BOARD then
          for _, ln in ipairs(laneEnds(g, cc, rr)) do
            if not ride.fail[(rr * KEY_W + cc) .. ">" .. (ln[2] * KEY_W + ln[1])] then hops[#hops + 1] = ln end
          end
        end
        end
        for _, h in ipairs(hops) do
          if not reach[h[2] * KEY_W + h[1]] and walkable(g, h[1], h[2]) then
            reach[h[2] * KEY_W + h[1]] = true; list[#list + 1] = { h[1], h[2] }
          end
        end
      end
      basis, maskOf, nulls = {}, {}, {}
      for _, cell in ipairs(list) do
        -- Only cells a bomb can be put on (not stairs, not tiles where one
        -- failed to appear, e.g. the $74 panels themselves).
        local ct = g[cell[2]][cell[1]]
        local ck = cell[2] * KEY_W + cell[1]
        if (ct < 0x100 and ct & 0x40 ~= 0 and ct ~= ITEM) or (noBombTile[ct] or 0) >= 1 or (noBombUntil[ck] or 0) > frame then goto nextCell end
        do
        local m = 0
        for _, bc in ipairs(blastCells(g, cell[1], cell[2], rangeAt(g, cell[1], cell[2]))) do
          local i = idx[bc[2] * KEY_W + bc[1]]
          if i then m = m | (1 << i) end
        end
        maskOf[ck] = m
        local set = { [ck] = true }
        for bit = np - 1, 0, -1 do
          if m & (1 << bit) ~= 0 then
            if basis[bit] then m, set = m ~ basis[bit][1], xorSet(set, basis[bit][2])
            else basis[bit] = { m, set }; break end
          end
        end
        if m == 0 then nulls[#nulls + 1] = set end
        end
        ::nextCell::
      end
      local t
      t, sol = target, {}
      for bit = np - 1, 0, -1 do
        if t & (1 << bit) ~= 0 then
          if not basis[bit] then sol = nil; break end
          t, sol = t ~ basis[bit][1], xorSet(sol, basis[bit][2])
        end
      end
      if sol then break end
    end
    -- One plan, kept while it still adds up: a new solution every think
    -- (any of many) made each bomb part of a different plan, and the panels
    -- flipped back and forth (4-3, run IA s0: (4,24) (2,20) (4,18) (2,22)
    -- (4,18) ... in the lower left pocket, two bombs would have done it).
    -- After one of its bombs goes off the rest of the plan adds up.
    local function adds(set)
      local m = 0
      for k in pairs(set) do
        if not maskOf[k] then return false end
        m = m ~ maskOf[k]
      end
      return m == target
    end
    local old = ride.panelSol
    if not sol and old and frame - (ride.panelSolAt or 0) < 300 then
      -- (no solution for now: flames on the way in, a bomb on a plan spot;
      -- the last plan holds for a while)
      panelPlan = old
    elseif old and next(old) then
      if adds(old) then sol = old
      else
        for x in pairs(old) do
          local rest = {}
          for k in pairs(old) do if k ~= x then rest[k] = true end end
          if next(rest) and adds(rest) then sol = rest; break end
        end
      end
    end
    if sol and sol ~= old then
      -- (fewest bombs: fold in sets that flip nothing while that shrinks it)
      local function size(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
      local better = true
      while better do
        better = false
        for _, v in ipairs(nulls) do
          local t2 = xorSet(sol, v)
          if size(t2) < size(sol) then sol, better = t2, true end
        end
      end
    end
    if sol then ride.panelSolAt = frame end
    if not panelPlan and sol ~= old then
      local t = {}
      for k in pairs(sol or {}) do t[#t + 1] = string.format("(%d,%d)", k % KEY_W, k // KEY_W) end
      table.sort(t)
      local str = sol and (next(sol) and table.concat(t, " ") or "done once its bombs go off") or "none"
      if str ~= ride.panelSolStr then log(string.format("%6d panel plan: %s", frame, str)) end
      ride.panelSolStr = str
    end
    if not panelPlan then
      ride.panelSol = sol
      if sol and next(sol) then panelPlan = sol end
    end
  end
  local here = r * KEY_W + c
  -- P1's own cell field (+$A8, row*$80 + col*2) can be the next cell over
  -- before the map tile is (5-8: px 89 was tile 5 but +$A8 col 6, and the
  -- flame on col 6 killed it): danger on either is danger.
  do
    local a8 = r16(P1 + 0xA8)
    local ak = (a8 // 0x80) * KEY_W + (a8 % 0x80) // 2
    if ak ~= here and danger[ak] then danger[here] = true; ride.a8Hot = true else ride.a8Hot = nil end
  end
  -- Remote bombs: cells their blasts would cover, and whether setting them
  -- off now would hit anything worth it.
  local remoteCells, remoteWorth = {}, false
  for _, b in ipairs(lastBombs) do
    if b[5] then
      for _, cell in ipairs(blastCells(g, b[1], b[2], b[4], b[6], true)) do
        local k = cell[2] * KEY_W + cell[1]
        remoteCells[k] = true
        local t = g[cell[2]] and g[cell[2]][cell[1]]
        if (SOFT[t] and not softGone[k]) or panelTodo[k] or t == 0x68 or t == 0x6B or t == 0x6A or t == 0xE2 then remoteWorth = true end
      end
    end
  end
  if next(remoteCells) then
    for _, e in ipairs(elist) do
      if e.killable and (remoteCells[e.r * KEY_W + e.c] or (e.moving and remoteCells[e.tr * KEY_W + e.tc])) then
        remoteWorth = true
      end
    end
    -- (not 5-8's shield at (8,12): it waits for a slam, see f1plan)
    if emu.read(P1 + P_BOMBS_LEFT, WRAM, false) == 0 and not ride.f1Home then remoteWorth = true end
    -- A bomb the stage planner placed (its step's cell) is the plan: set it
    -- off as soon as we are clear (2-6: held 300 frames because the block
    -- it was for had grown back once, softGone, so the next step never came
    -- before it grew back again).
    for _, b in ipairs(lastBombs) do
      if b[5] and ride.planBomb == b[2] * KEY_W + b[1] then remoteWorth = true end
    end
    -- A boss sense point in the blast (and not on the bomb: it would eat it).
    for k in pairs(bossEat) do
      if remoteCells[k] then
        local onBomb = false
        for _, b in ipairs(lastBombs) do if b[5] and b[2] * KEY_W + b[1] == k then onBomb = true end end
        if not onBomb then remoteWorth = true end
      end
    end
    -- We hold one remote bomb at a time (think): an old one whose target
    -- got away goes off anyway, to free the bomb.
    remoteSince = remoteSince or frame
    if frame - remoteSince > 300 and not ride.f1Home then remoteWorth = true end
    -- 5-8 form 2: flames don't hurt it, and a missed throw lands as a remote
    -- bomb that blocks the next lift: set it off as soon as we are clear.
    if finalForm2() then remoteWorth = true end
    -- Not on a spring pad (the one free frame after a landing: the press
    -- did nothing and 3-6 went round its pads for good) or locked.
    -- (5-8's slam: a ball near P1 is no reason to wait, the body rises)
    -- (not while a thrown bomb flies or has just landed: it is not listed
    -- yet, and B sets it off too; 5-8 form 2 died 3 cells from one)
    -- (nor while a $8C may pull P1 into the blast: 1-3, run 20, waited
    -- a cell off the remote bomb's row on a pull line; the pull and the
    -- detonation came together)
    local pulledIn = false
    if PullLine and PullLine[here] then
      for k in pairs(remoteCells) do
        if math.abs(k % KEY_W - c) + math.abs(k // KEY_W - r) <= 2 then pulledIn = true end
      end
    end
    -- (an enemy by P1 is no reason to hold it when a killable one is in
    -- the blast: 5-6, run 36, a $8A came at P1 past the remote bomb and
    -- was let through)
    local foeIn = false
    for _, e in ipairs(elist) do
      if e.killable and (remoteCells[e.r * KEY_W + e.c] or (e.moving and remoteCells[e.tr * KEY_W + e.tc])) then foeIn = true end
    end
    -- (not while a stall breaker plan times its own B: 4-5, run half45 s0
    -- replayed, set off 5 frames after the press, its 40-frame wait unspent)
    local brkTimed = ride.brk and ride.brk.detEnd and (ride.brk.detWait or 0) > 0
    if remoteWorth and not brkTimed and not remoteCells[here] and not pulledIn and (not nearEnemy[here] or ride.f1Home or foeIn) and frame >= nextDetonate
        and frame - throw.at > 40 and (r16(P1 + 0x52) & 6) == 0
        and g[r][c] ~= SPRING and (r16(P1 + P_LOCK) & 1) == 0 then
      input.b = true
      nextDetonate = frame + 20
      log(string.format("%6d detonate%s", frame, emu.read(0x1A, WRAM, false) == 0x57 and
        string.format(" (body %02X%04X at %d,%d hp %d)", emu.read(0x1002, WRAM, false), r16(0x1000), r16(0x1011), r16(0x1014), r16(0x1020)) or ""))
      if os.getenv("SBM2_DBG") then
        local bs = {}
        for _, b in ipairs(lastBombs) do bs[#bs + 1] = string.format("(%d,%d)t%d%s", b[1], b[2], b[3], b[5] and "r" or "") end
        log(string.format("%6d dbg remote bombs %s; P1 (%d,%d) f52 %04X", frame, table.concat(bs, " "), c, r, r16(P1 + 0x52)))
      end
    end
    -- Shut in with its own remote bomb for 1200+ frames and 2+ hearts: set
    -- it off and take the hit (5-5, run AD s0: at (27,10) between the
    -- (25,10) hole and its bomb at (28,10), (28,11) grown back behind P1;
    -- waited 13000 frames with 7 hearts until time-up).
    if remoteCells[here] and frame >= nextDetonate and ride.stillAt == here
        and frame - (ride.stillSince or frame) > 1200 and emu.read(0x1A, WRAM, false) ~= 0x57 then
      local hz = emu.read(P1 + 0xEF, WRAM, false)
      if hz >= 2 and hz < 0x80 then
        input.b = true
        nextDetonate = frame + 20
        ride.heartUntil = frame + 200  -- (the simulator lets this heart go)
        log(string.format("%6d detonate: shut in with its own bomb for %d frames; a heart for it (%d held)", frame,
          frame - ride.stillSince, hz))
      end
    end
    -- Set off in the last few frames (it goes off ~8 frames after the
    -- press): its blast is danger now. (5-8 form 2: the aim plan walked P1
    -- back into a missed throw's blast after the press, 40 rollbacks.)
    if frame < nextDetonate - 4 then
      for k in pairs(remoteCells) do danger[k] = true end
    end
  else
    remoteSince = nil
  end
  visits[here] = (visits[here] or 0) + 1
  -- Soft blocks grow back in world 2 and in 5-5 at least (5-5: (25,4)
  -- broke ~50 frames after its blast and was back later; the bot bombed it
  -- five times while the last panel waited): one already blown up is not
  -- worth bombing again. Where blocks don't grow back this changes nothing.
  -- Regrown blocks that wall us off from what is left to do are the stage
  -- planner's business (it bombs the ones in the way).
  local soft, todo = 0, 0
  for _, cr in ipairs(g.softs) do softSeen[cr[2] * KEY_W + cr[1]] = true end
  soft = #g.softs
  for k in pairs(softSeen) do
    if not SOFT[g[k // KEY_W][k % KEY_W]] then
      softGone[k] = true
      softBlownAt[k] = softBlownAt[k] or frame
    else
      softBlownAt[k] = nil
    end
  end
  -- A soft block next to flames is breaking: it still reads soft for a
  -- while (5-1, run 34: (5,4) went ~15 frames after the bot planned a hide
  -- behind it, and a $8A came through). Chaser times treat it as open.
  for _, fk in ipairs(g.flames) do
    local fc, fr = fk % KEY_W, fk // KEY_W
    for _, d in ipairs(DIRS) do
      local t = g[fr + d[2]] and g[fr + d[2]][fc + d[1]]
      if t and SOFT[t] then Breaking[(fr + d[2]) * KEY_W + fc + d[1]] = frame end
    end
  end
  for k, at in pairs(Breaking) do if frame - at > 120 or frame < at then Breaking[k] = nil end end
  for _ in pairs(panelTodo) do todo = todo + 1 end
  -- Not the enemy count: spawners (the 2-1 furnace) keep adding enemies.
  local sig = string.format("%d/%d/%d", score(), soft, todo)
  -- A boss losing HP is progress too (+$20, docs/enemies.md section 7).
  Boss.hp = nil
  if #bosses > 0 then
    local hp = 0
    for _, b in ipairs(bosses) do hp = hp + r16(b.base + 0x20) end
    sig = sig .. "/hp" .. hp
    Boss.hp = hp
  end
  -- Assist (logged): a stage whose panels we can't finish (unsolved gimmick)
  -- gets the game's own "all panels lit" bit ($5D94 bit 15, as C1:2335
  -- sets it) after ASSIST frames without progress or when time is short.
  -- Progress for the assists = a state not seen before (a rollback brings
  -- back an older score, which isn't progress).
  if not seenSigs[sig] then seenSigs[sig] = true; lastProgress, deathsAtProgress = ticks, deaths; ride.newSigAt = frame end
  -- No state not seen before for 4500 frames (blocks blown and grown back
  -- are no progress): forget this stage's working memory, as at a stage
  -- entry. (2-1, run LA s0: 11000 frames of the same ambush along row 4 to
  -- a time-up, never "stalled" as its blasts kept changing the soft count;
  -- a scenario cut from the run's own pads at 49400, the same game with
  -- fresh memory, cleared it in 3000.)
  if frame - (ride.newSigAt or frame) > 4500 and frame - (ride.memResetAt or -1e9) > 4500 then
    ride.memResetAt = frame
    for k in pairs(visits) do visits[k] = nil end
    for k in pairs(noBombTile) do noBombTile[k] = nil end
    ride.stand, ride.standMiss, ride.wait, ride.try, ride.dodgeGoal, ride.planCell = nil, nil, nil, nil, nil, nil
    ride.fail, ride.tried, ride.jumpRecent, ride.padHist, ride.panelSol, ride.badLift = {}, {}, {}, {}, nil, nil
    badCell, planSteps, planCo, noBombUntil, springTry = {}, nil, nil, {}, nil
    -- (and the blocks seen to grow back, skipped as targets: run MA s0
    -- forgot the rest at 53487 and still timed out at 60216 bombing for the
    -- $8D along row 4; the cut at 49400, with them targets, blew blocks and
    -- cleared 2-1 by 52411)
    softGone = {}
    math.randomseed(SEED * 7919 + frame * 31)
    log(string.format("%6d no new progress for 4500 frames: this stage's memory forgotten", frame))
  end
  -- Heart assist (logged): after HEART_DEATHS deaths without progress, keep
  -- P1 at one heart (+$EF; C0:A5C9 spends it on a hit for brief
  -- invincibility) for the rest of the stage, rollbacks included.
  local stage = emu.read(0x1A, WRAM, false)
  if not NOASSIST and heartStage ~= stage and deaths - deathsAtProgress >= HEART_DEATHS then
    heartStage = stage
    local msg = string.format("frame %d stage %02X: heart assisted (%s)", frame, stage, sig)
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  if heartStage == stage and emu.read(P1 + 0xEF, WRAM, false) == 0 then
    inputlog.write16(P1 + 0xEF, (r16(P1 + 0xEF) & 0xFF00) | 1)
  end
  local st = r16(0x5D94)
  if st ~= 0xFFFF and (st & 0x8000) == 0 and next(panelTodo) ~= nil
      and not NOASSIST and (ticks - lastProgress > ASSIST or timeLeft() < 30)
      and (#walkers == 0 or ticks - lastProgress > 2 * ASSIST) then
    inputlog.write16(0x5D94, st | 0x8000)   -- logged: replays can't infer it
    local msg = string.format("frame %d world %d: panels assisted (%s)", frame, lastWorld + 1, sig)
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  -- Enemy assist (logged): enemies we can't wear down (high or immune $7F
  -- hit points, +$43, see EnemyHit C1:1EE2) drop to one hit point under the
  -- same conditions, so the next blast kills them. The gate object is left.
  if not NOASSIST and #enemies > 0 and (ticks - lastProgress > ASSIST or timeLeft() < 60) then
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      local hp = emu.read(base + 0x43, WRAM, false)
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8000) == 0 and hp > 1 then
        local ec, er = toTile(r16(base + P_X), r16(base + P_Y))
        if ec ~= gc or er ~= gr then
          inputlog.write16(base + 0x43, (r16(base + 0x43) & 0xFF00) | 1)
          local msg = string.format("frame %d stage %02X: enemy %04X hp %02X assisted", frame,
            emu.read(0x1A, WRAM, false), base, hp)
          log("ASSIST " .. msg)
          local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
        end
      end
    end
  end
  -- Kill assist (logged): still stuck after 2*ASSIST frames, or under 20 s
  -- left: each live enemy (not the gate, not item-holders $90/$98) gets
  -- EnemyDie's (C1:5028) struct writes, without the score.
  -- Once stuck that long the stage is marked, and both this and the panel
  -- bit are reapplied every frame, since a rollback undoes them.
  -- Score from retries counts as progress, so also force a stage that has
  -- taken 3 assist windows in all.
  if (ticks - lastProgress > 2 * ASSIST or ticks - stageTicks > 3 * ASSIST) and forceStage ~= stage
      and not NOASSIST and stage ~= 0x57 then  -- 5-8 has its own (final boss) assist
    forceStage, forceTicks = stage, ticks
    local msg = string.format("frame %d stage %02X: forced clear (%s)", frame, stage, sig)
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  -- Gate assist (logged): forced, no enemies left, gate still shut 600
  -- frames on (the game opens it from a real last kill, which the kill assist
  -- skips): write the open-gate tile $49 and clear Exit_Blocked.
  if forceStage == stage and #enemies == 0 and not gateOpen and gate ~= 0
      and ticks - forceTicks > 600 then
    inputlog.write16(MAP + gate, (r16(MAP + gate) & 0xFF00) | 0x49)
    inputlog.write16(0x5D70, 0)   -- Exit_Blocked, cleared when the gate really opens
    gateOpen, forceTicks = true, ticks - 1201
    local msg = string.format("frame %d stage %02X: gate opened (%d,%d)", frame, stage, gc, gr)
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  -- Exit assist (logged): a forced-clear stage whose open gate we still
  -- haven't reached 1200 frames later (walled off, or in another part of the
  -- map) gets the open-gate tile $49 written under P1 (its cell, +$A8), so
  -- C1:41A8 runs there.
  if forceStage == stage and gateOpen and ticks - forceTicks > 1200 and (c ~= gc or r ~= gr) then
    -- Both P1's +$A8 cell and the cell its pixel position maps to (they
    -- disagreed in 2-6), so the game's check and the bot's plan agree.
    local cell, pcell = r16(P1 + 0xA8), r * ROW + c * 2
    inputlog.write16(MAP + cell, (r16(MAP + cell) & 0xFF00) | 0x49)
    inputlog.write16(MAP + pcell, (r16(MAP + pcell) & 0xFF00) | 0x49)
    inputlog.write16(0x5D70, 0)
    inputlog.write16(0x5D72, pcell)   -- ExitGateCell: the bot's exit plan then stands on it
    forceTicks = ticks
    local msg = string.format("frame %d stage %02X: exit under player (cell %04X)", frame, stage, cell)
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  -- Final boss assist (5-8, logged): the controller at $7E1000 is not an
  -- enemy struct (docs/enemies.md section 7). After the bot's own try, take
  -- each form to its last hit the way the game does: HP $1020 = 1 and the
  -- hit flag $1024 set, while the hurt timer $1026 is idle. Form 2 (state
  -- CC:D139) only takes a hit from its hit state, so that is set up too.
  if not NOASSIST and stage == 0x57 and ticks - stageTicks > 3000 and r16(0x1026) == 0
      and r16(0x1020) > 1 and (r16(P1 + P_LOCK) & 1) == 0 then
    local state = r16(0x1000) | (emu.read(0x1002, WRAM, false) << 16)
    local form1 = state == 0xCCC8D6 or state == 0xCCCAC4 or state == 0xCCCB56
    if not form1 and state ~= 0xCCD139 then state = nil
    elseif state == 0xCCD139 then
      inputlog.write16(0x1003, 0xD139)
      inputlog.write16(0x1005, (r16(0x1005) & 0xFF00) | 0xCC)
      inputlog.write16(0x1000, 0xD2E7)
    end
    if state then
      inputlog.write16(0x1020, 1)
      inputlog.write16(0x1024, 0xFFFF)
      inputlog.write16(0x1026, 0)
      local msg = string.format("frame %d stage 57: final boss hit (state %06X)", frame, state)
      log("ASSIST " .. msg)
      local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
    end
  end
  if forceStage == stage then
    local st2 = r16(0x5D94)
    if st2 ~= 0xFFFF and (st2 & 0x8000) == 0 then inputlog.write16(0x5D94, st2 | 0x8000) end
  end
  if not NOASSIST and #enemies > 0 and (forceStage == stage or timeLeft() < 20) then
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      local t = r16(base + 0x7E)
      -- Real enemies only: not the platforms, track blocks, furnace ($C0+).
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (r16(base + 4) & 0x8000) == 0 and t ~= 0x90 and t ~= 0x98
          and (t & 0xFF) < 0xC0 then
        local ec, er = toTile(r16(base + P_X), r16(base + P_Y))
        if ec ~= gc or er ~= gr then
          local w = function(o, v) inputlog.write16(base + o, v) end
          w(0x50, 0); w(0x6A, 0x0C); w(0x68, r16(base + 0x68) & 0xFFF0)
          w(0x06, r16(base + 0x2A)); w(0x88, r16(base + 0x8A))
          w(0x04, (r16(base + 4) & 0xFFDF) | 0xA000); w(0x18, 0)
          local msg = string.format("frame %d stage %02X: enemy %04X type %02X killed (assist)", frame,
            emu.read(0x1A, WRAM, false), base, t)
          log("ASSIST " .. msg)
          local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
        end
      end
    end
  end
  if sig ~= progressSig then progressSig, progressAt, stallSig = sig, frame, nil
  elseif frame - progressAt > STALL then
    progressAt = frame
    exploreUntil, stallUntil = frame + 600, frame + 600
    -- Blocks once seen to grow back are skipped (softGone); stalled with
    -- the gate shut, they are targets again: a dormant enemy may sit in
    -- the last one (1-5 ended with one block and the gate shut).
    if not gateOpen then softGone = {} end
    -- Jumps written off (a death after one, no launch seen) are tried
    -- again: written off for good, they had 3-6's right side unreachable
    -- with its last two enemies there, until time ran out.
    if not gateOpen then
      for k, v in pairs(springLand) do
        -- (and jumps learnt to land back on their own pad, blocked that
        -- once: 3-6, run OA s0, (5,8) left "landed (5,8)" and the route to
        -- the last $8B by (3,8) was gone for good)
        local pk = tonumber(k:match("^(%d+)"))
        if v == false or (v and pk and v[2] * KEY_W + v[1] == pk) then springLand[k] = nil end
      end
    end
    local es = {}
    for k in pairs(panelTodo) do es[#es + 1] = string.format("panel@(%d,%d)", k % KEY_W, k // KEY_W) end
    for _, d in ipairs(dormant) do es[#es + 1] = string.format("dormant@(%d,%d)", d[1], d[2]) end
    for _, e in ipairs(elist) do
      es[#es + 1] = string.format("%02X@(%d,%d)%s%s%s", e.type, e.c, e.r, e.killable and "k" or "",
        e.harmful and "h" or "", e.gadget and "g" or "")
    end
    log(string.format("%6d stalled (%s); exploring; gate $%04X (%d,%d)%s; enemies %s", frame, sig, gate, gc, gr,
      gateOpen and " open" or "", table.concat(es, " ")))
    ride.brkWant, ride.lastStall = frame, frame  -- the stall breaker (think) searches
    -- Two stalls in a row with no progress between: wedged (own remote
    -- bombs, a dead end). Go back as after a death and try something else.
    if stallSig == sig and timeLeft() > 1 then
      log(string.format("%6d stuck at (%d,%d); rolling back", frame, c, r))
      rollback, stallSig = true, nil
    else
      stallSig = sig
    end
  end
  local plan, dir
  -- Walking time (frames) from the nearest chaser to each cell, a bomb at
  -- (bc,br) and the map's bombs blocking (for canEscape).
  ride.chaserEta = nil
  local chasers = {}
  -- ($93 and $85 turn into chasers the moment their x-ray sight catches P1,
  -- every frame: for escapes they are chasers already. 4-6: 12 deaths to
  -- a $93 after bombing for it and backing into a pocket.)
  for _, e in ipairs(elist) do
    if e.harmful and not e.shot and (e.chaser or e.type == 0x93 or e.type == 0x85 or e.type == 0x8C) then chasers[#chasers + 1] = e end
  end
  local function makeEta(chasers)
    return function(bc, br, bt)
      -- A bomb blocks a chaser only if it gets there before the bomb goes
      -- off (after that the way is open: 5-1 backed into a pocket that the
      -- escape bomb closed only until it blew).
      local bombT = {}
      for i, b in ipairs(lastBombs or {}) do bombT[b[2] * KEY_W + b[1]] = (lastTimes and lastTimes[i]) or b[3] end
      if bc then bombT[br * KEY_W + bc] = bt or NEW_FUSE end
      -- Soft blocks a pending blast breaks open then (5-1, run 34: hid in
      -- the (2,5) pocket from a bomb that broke (5,4) and let a $8A in).
      local softT = {}
      local function opens(c0, r0, t0, rng)
        for _, cell in ipairs(blastCells(g, c0, r0, rng)) do
          local k = cell[2] * KEY_W + cell[1]
          local tt = g[cell[2]] and g[cell[2]][cell[1]]
          if tt and SOFT[tt] and (not softT[k] or t0 < softT[k]) then softT[k] = t0 + 20 end
        end
      end
      for i, b in ipairs(lastBombs or {}) do opens(b[1], b[2], (lastTimes and lastTimes[i]) or b[3], b[4]) end
      if bc then opens(bc, br, bt or NEW_FUSE, rangeAt(g, bc, br)) end
      local eta = {}
      -- Times, not steps (a label-correcting search): a chaser held up by a
      -- bomb goes on once it has blown, after the hurt freeze if the blast
      -- only hurts it (2+ HP: 4-6's $93, 5-1's $8A came back at P1 in the
      -- pocket it had backed into); with 1 HP it dies there.
      for _, e in ipairs(chasers) do
        local start = e.r * KEY_W + e.c
        local tm, q, qi = { [start] = 0 }, { { e.c, e.r } }, 1
        local limit = 12 * e.cpf + 300
        while q[qi] do
          local cc, rr = q[qi][1], q[qi][2]; qi = qi + 1
          local k0 = rr * KEY_W + cc
          local tt = tm[k0]
          if not eta[k0] or tt < eta[k0] then eta[k0] = tt end
          if tt < limit then
            for _, dd in ipairs(DIRS) do
              local nc, nr = cc + dd[1], rr + dd[2]
              local k = nr * KEY_W + nc
              local tile = g[nr] and g[nr][nc]
              local thru = (e.type == 0x8E and tile and tile ~= 0xC5 and tile ~= 0xC6 and tile ~= 0x01) or Breaking[k] ~= nil
              if tile and (walkable(g, nc, nr) or thru or bombT[k] or isBomb(tile) or softT[k]) then
                local tn = tt + e.cpf
                if softT[k] and not thru and tn < softT[k] then tn = softT[k] + e.cpf end
                local tb = bombT[k] or (isBomb(tile) and 100000) or nil
                local ok = true
                if tb and tn < tb - 20 then
                  if (e.hp or 1) <= 1 or tb >= 100000 then ok = false else tn = tb + 100 end
                end
                if ok and (not tm[k] or tn < tm[k]) then
                  tm[k] = tn
                  q[#q + 1] = { nc, nr }
                end
              end
            end
          end
        end
      end
      return eta
    end
  end
  if #chasers > 0 then ride.chaserEta = makeEta(chasers) end
  -- Any harmful walker, as if it chased (for waits in a dead end).
  local roamers = {}
  for _, e in ipairs(elist) do
    if e.harmful and not e.shot and not e.gadget and e.type ~= 0xDD then roamers[#roamers + 1] = e end
  end
  ride.walkerEta = #roamers > 0 and makeEta(roamers) or nil
  -- A place to sit out blasts with walkers about: a junction (3+ open
  -- sides), or a cell no walker reaches until 120 frames after P1. (2-4,
  -- run 18: cleared its remote bomb's row into the dead end at (5,5) as a
  -- $82 came up the column.)
  local wEtaNow
  local function walkerSafe(cc, rr, n)
    if not ride.walkerEta then return true end
    wEtaNow = wEtaNow or ride.walkerEta()
    local e = wEtaNow[rr * KEY_W + cc]
    if not e or e > n * FPT + 120 then return true end
    if e <= n * FPT + 30 then return false end  -- (not even a junction a walker gets to first: 1-6 fled up into a $8C)
    local open = 0
    for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then open = open + 1 end end
    return open >= 3
  end
  -- A $DD track block pushes P1 along its $4D track (4-2: P1 waited in a
  -- pocket on the track and was pushed into its own blast): with one on
  -- the map, a track cell is never a place to stop.
  ride.hasDD = false
  for _, e in ipairs(elist) do if e.type == 0xDD then ride.hasDD = true end end
  stageNow17 = emu.read(0x1A, WRAM, false) == 0x17
  -- A $8C pulls P1 (C1:84B0 -> C1:85D5/861F): at a cell centre, if it sees
  -- P1 (SeesP1 C1:8AF9: same 8 px row or column band, up to 4 cells, the
  -- scan stopped by a tile with bit 7 or a bomb $43) it halts and sets P1's
  -- acceleration ($8028/$8030/$8032 = $8FFF) toward itself for up to 320
  -- frames (+$BE). Walking beats it; standing still on the line drifts P1
  -- into it (1-3 and 1-5: "waiting", P1 pulled 7-30 px into the $8C).
  PullLine = {}
  for _, e in ipairs(elist) do
    -- (from the cell it is walking to as well: 1-3, run 20, waited beside
    -- its remote bomb's row; a $8C came up the column, stopped one cell
    -- on and pulled P1 into the row as the bomb went off)
    if e.type == 0x8C and e.harmful then
      for _, at in ipairs({ { e.c, e.r }, e.moving and { e.tc, e.tr } or nil }) do
        for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
          for k = 1, 4 do
            local cc, rr = at[1] + d[1] * k, at[2] + d[2] * k
            local tt = g[rr] and g[rr][cc]
            if not tt or (tt & 0x80) ~= 0 or tt == 0x43 then break end
            PullLine[rr * KEY_W + cc] = true
          end
        end
      end
    end
  end
  -- A $90 fires a $B2 shot (2 px/f up/down, 1.5 across) every 64 frames
  -- at a centre when P1 is in line within 10 cells (walls and bombs
  -- block). No place to stop or hunt from: in a corridor there is no
  -- stepping aside. (3-3, run 19: fled to (11,23), a cell and a half
  -- below a $90, and was shot every time; hunting one head-on up a
  -- column, shot from 5 cells.)
  FireLine = {}
  for _, e in ipairs(elist) do
    -- (and a $86: rests 128 frames, then dashes up to 4 cells at 2.5 px/f,
    -- toward P1 if it sees it within 8, walls blocking; 2-2, run 20,
    -- waited in its row and was run down, twice, a game over)
    if (e.type == 0x90 or e.type == 0x86) and e.harmful then
      for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
        for k = 1, e.type == 0x86 and 8 or 10 do
          local cc, rr = e.c + d[1] * k, e.r + d[2] * k
          local tt = g[rr] and g[rr][cc]
          if not tt or (tt & 0x80) ~= 0 or tt == 0x43 then break end
          FireLine[rr * KEY_W + cc] = true
        end
      end
    end
  end
  -- Cells a $8C could pull from within ~150 frames: the 4-cell lines from
  -- every cell it can walk to by then. A dead end there is no place to sit
  -- out a blast: it can't be walked out of (1-3, the full-run path: hid at
  -- (5,14) after bombing at (6,15); a $8C came up the column and pulled
  -- P1 into the bomb's row).
  -- A $90's line of fire likewise (10 cells): 3-2, runs 30 and 31, hid
  -- in a dead-end row the $90 walked into, and was shot there.
  local PullRisk = {}
  for _, e in ipairs(elist) do
    if (e.type == 0x8C or e.type == 0x90) and e.harmful then
      local seen, q, qi = { [e.r * KEY_W + e.c] = 0 }, { { e.c, e.r } }, 1
      while q[qi] do
        local cc, rr = q[qi][1], q[qi][2]; qi = qi + 1
        local tt = seen[rr * KEY_W + cc]
        for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
          for kk = 1, e.type == 0x90 and 10 or 4 do
            local lc, lr = cc + d[1] * kk, rr + d[2] * kk
            local lt = g[lr] and g[lr][lc]
            if not lt or (lt & 0x80) ~= 0 or lt == 0x43 then break end
            PullRisk[lr * KEY_W + lc] = true
          end
          local nc, nr = cc + d[1], rr + d[2]
          local nk = nr * KEY_W + nc
          if seen[nk] == nil and tt + e.cpf <= 150 and walkable(g, nc, nr) then
            seen[nk] = tt + e.cpf; q[#q + 1] = { nc, nr }
          end
        end
      end
    end
  end
  local function safeGoal(w, t0)
    return function(cc, rr, n)
      local k = rr * KEY_W + cc
      local t = (t0 or 0) + n * FPT
      if ride.hasDD and g[rr][cc] == 0x4D then return false end
      if MagLine and MagLine[k] then return false end
      if PullLine[k] or FireLine[k] then return false end
      if PullRisk[k] then
        local open = 0
        for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then open = open + 1 end end
        if open <= 2 then return false end
      end
      -- 1-8: the body sweeps sideways at y 64 with a box down to +56 (rows
      -- 2-7): no place to stop up there; its sense grid reaches row 6, so
      -- bombs from row 8 down still hurt it (P1 was caught at (10,3)).
      if stageNow17 and rr <= 7 then return false end
      -- 2-8: drops land in P1's row or column (range 10) and give ~y/2
      -- frames of warning: 17 in row 2. No place to stop in rows 1-4
      -- (run 39 fled its own bomb into row 2 and a drop's row cross came).
      if emu.read(0x1A, WRAM, false) == 0x27 and rr <= 4 then return false end
      return not w[k] and not hit(eWin, k, t - FPT, t + 90)
    end
  end

  -- (a platform docked by us still takes us out: we bombed counting on it)
  allowRide = not danger[here] or ride.dockedNear(c, r, 50)
  -- 5-8 form 1 (docs/enemies.md 7.3): stand on our own remote bomb at
  -- (8,12). Balls turn clockwise 16 px short of a bomb (their box is +-8),
  -- so it shields P1; row 12 is below a slam's box; the body crosses x 128
  -- every sweep and slams onto P1. Then run 3 cells along row 12: the remote
  -- goes off once the blast reaches the slammed sense block (row 10).
  local f1plan
  ride.f1Home = nil
  if emu.read(0x1A, WRAM, false) == 0x57 and not finalForm2() and remoteOn then
    local st = r16(0x1000) | (emu.read(0x1002, WRAM, false) << 16)
    if st == 0xCCC8D6 or st == 0xCCCAC4 or st == 0xCCCB56 then
      local HK = 12 * KEY_W + 8
      local myCell
      for _, b in ipairs(lastBombs) do if b[5] then myCell = b[2] * KEY_W + b[1] end end
      local bx = r16(0x1011)
      if bx >= 0x100 then bx = bx - 0x100 end
      local slamHere = st == 0xCCCAC4 and math.abs(bx - 128) <= 32
      -- out of the home bomb's blast along row 12 (fire 3 with 5-7's items)
      local away = function(cc, rr) return rr == 12 and math.abs(cc - 8) > RANGE end
      -- Off a bomb only with no ball near (a slam missed is not a death);
      -- caught off it with a ball near, a new bomb right here shields too.
      local ballNear, ballSome = false, false
      ride.ballRisk = false
      -- Boss.read lists the objects whose contact code ran last frame; some
      -- frames (a slam's first) it has none: use the last list seen (8 f).
      local balls = {}
      for _, b in ipairs(bosses) do
        if b.state == 0xCCDAA8 or b.state == 0xCCA43C then balls[#balls + 1] = b end
      end
      if #balls > 0 then ride.balls, ride.ballsAt = balls, frame
      elseif ride.balls and frame - ride.ballsAt <= 8 and frame >= ride.ballsAt then balls = ride.balls end
      for _, b in ipairs(balls) do
        local d = (b.state == 0xCCDAA8 or b.state == 0xCCA43C) and math.abs(b.x - x) + math.abs(b.y - y) or 1e9
        if d < 96 then ballNear = true end
        if d < 128 then ballSome = true end
        -- leaving the bomb for a slam: ~90 frames off it, and balls home
        if d < 160 or b.y > 170 then ride.ballRisk = true end
      end
      -- calm: no ball within 128 px for 30 frames (no flicker off the bomb)
      if ballSome then ride.ballAt = frame end
      local calm = frame - (ride.ballAt or -1000) > 30
      local bombsLeft = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0
      ride.f1Home = myCell == HK
      if not (myCell == HK and slamHere) then ride.runSide = nil end
      if not (myCell == HK and slamHere) then ride.running = nil end
      -- Walk along row 12 to column tc (straight: route searches through
      -- the ball windows failed every other frame); off row 12, a route.
      local function walkTo(tc)
        if r == 12 then
          if c == tc then return nil end
          return c < tc and "right" or "left"
        end
        local row12 = function(cc, rr) return rr == 12 end
        return bfs(g, c, r, row12, nearEnemy, cw) or bfs(g, c, r, row12, nil, cw)
      end
      -- Two bombs (5-7's items): no run at all. The remote sets off the
      -- OLDEST bomb first [port probe], so keep an attack bomb at (6,11)
      -- (row 11 is never a sense cell; its blast up column 6 reaches the
      -- slammed block for body columns 4-8) older than a shield bomb at
      -- (7,12) that P1 stands on (out of the attack bomb's row and column).
      -- A slam over P1: press B, the attack bomb goes off, P1 stays shielded.
      -- Rebuild when calm and the body is away: from (6,11) drop a new
      -- attack bomb, set off the old shield, walk back to (7,12), drop one.
      local two = emu.read(P1 + 0xE0, WRAM, false) >= 2
      local AK, SK = 11 * KEY_W + 6, 12 * KEY_W + 7
      local order = ride.order or {}
      ride.order = order
      local live = {}
      for _, b in ipairs(lastBombs) do if b[5] then live[b[2] * KEY_W + b[1]] = true end end
      for i = #order, 1, -1 do if not live[order[i]] then table.remove(order, i) end end
      for k in pairs(live) do
        local seen = false
        for _, o in ipairs(order) do if o == k then seen = true end end
        if not seen then order[#order + 1] = k end
      end
      local hasA, hasS = live[AK], live[SK]
      local aOlder = hasA and hasS and order[1] == AK
      -- away, or rising from the slam that just hit (its box is then above
      -- rows 11-12)
      local bodyFar = math.abs(bx - 96) > 72 or (st == 0xCCCAC4 and r16(0x1014) < 150 and frame - (ride.attackAt or -1000) < 200)
      local function goTo(k)
        local tc, tr = k % KEY_W, k // KEY_W
        if here == k then return nil end
        return bfs(g, c, r, function(cc, rr) return cc == tc and rr == tr end, nearEnemy, cw)
          or bfs(g, c, r, function(cc, rr) return cc == tc and rr == tr end, nil, cw)
      end
      if two and (#order <= 2) then
        f1plan, step = true, nil
        ride.f1Home = true  -- no generic remote detonations: this plan picks them
        if hasA and hasS and aOlder and here == SK then
          plan, dir = "shield2", nil
          -- (column 6 is in the block for body x 56..135: columns c-2..c+2)
          if st == 0xCCCAC4 and bx >= 56 and bx <= 135 and r16(0x1014) >= 144 and frame >= nextDetonate
              and frame - (ride.attackAt or -1000) > 150 then  -- once per slam (the next press is the shield)
            input.b = true; nextDetonate = frame + 30; ride.attackAt = frame
            log(string.format("%6d detonate (5-8: attack bomb under the slam at %d,%d)", frame, bx, r16(0x1014)))
          end
        elseif hasS and not hasA and here == SK and not (calm and bodyFar) then
          plan, dir = "shield2", nil  -- wait on the shield for a calm moment
        elseif hasS and not hasA then
          -- rebuild: to (6,11) (via (6,12)), drop the new attack bomb there
          plan = "rebuild"
          if here == AK then dir = nil; input.a = frame % 2 == 0
          else dir = goTo(AK) end
        elseif hasS and hasA and not aOlder then
          -- the shield is the older: set it off from the attack bomb's cell
          plan = "rebuild"
          if here == AK and frame >= nextDetonate then
            dir = nil; input.b = true; nextDetonate = frame + 30
            log(string.format("%6d detonate (5-8: old shield)", frame))
          elseif here ~= AK then dir = nil end
        elseif hasA and not hasS then
          plan = "place2"
          if here == SK then dir = nil; input.a = frame % 2 == 0 else dir = goTo(SK) end
        elseif not hasA and not hasS then
          plan = "place2"
          if here == AK then dir = nil; input.a = frame % 2 == 0 else dir = goTo(AK) end
        else
          f1plan = nil  -- something else out (a stray bomb): the one-bomb logic below
        end
      end
      if f1plan then
        -- (two-bomb plan above)
      elseif myCell == HK and slamHere and (ride.running or not ride.ballRisk) then
        if not ride.running and os.getenv("SBM2_DBG") then
          local t = {}
          for _, b in ipairs(bosses) do t[#t + 1] = string.format("%04X:%06X(%d,%d)", b.base, b.state, b.x, b.y) end
          log(string.format("%6d dbg slamrun start risk=%s bosses %s", frame, tostring(ride.ballRisk), table.concat(t, " ")))
        end
        ride.running = true
        f1plan, plan = true, "slamrun"
        if not away(c, r) then
          local l, rt = 1e9, 1e9
          for _, b in ipairs(bosses) do
            if (b.state == 0xCCDAA8 or b.state == 0xCCA43C) then
              local d = math.abs(b.x - x) + math.abs(b.y - y)
              if b.x < x then l = math.min(l, d) else rt = math.min(rt, d) end
            end
          end
          -- keep the side unless a ball is close ahead on it
          local side = ride.runSide
          if not side or (side == "right" and rt < 64 and l > rt) or (side == "left" and l < 64 and rt > l) then
            side = (l > rt) and "left" or "right"
          end
          ride.runSide = side
          dir, step = side, nil
        elseif ballNear and frame >= nextDetonate then
          -- clear of the blast with a ball coming: set it off now (a hit if
          -- the body is down) and dig in with the bomb it gives back
          input.b = true; nextDetonate = frame + 20
          log(string.format("%6d detonate (5-8: ball near after the run)", frame))
        end
      elseif emu.read(P1 + 0xFE, WRAM, false) == 0 and SOFT[g[10][13]] and myCell and calm
          and not (myCell == HK and slamHere) and frame >= (ride.gloveFree or 0) then
        -- no glove yet and our one bomb is out: set it off from outside its
        -- blast to have it for the glove's block
        local mc, mr = myCell % KEY_W, myCell // KEY_W
        local rc = {}
        for _, cell in ipairs(blastCells(g, mc, mr, RANGE)) do rc[cell[2] * KEY_W + cell[1]] = true end
        f1plan, plan, step = true, "glove", nil
        if rc[here] then
          dir = bfs(g, c, r, function(cc, rr) return not rc[rr * KEY_W + cc] end, nearEnemy, cw)
        elseif frame >= nextDetonate then
          input.b = true; nextDetonate = frame + 20
          log(string.format("%6d detonate (5-8: bomb wanted for the glove block)", frame))
        end
      elseif emu.read(P1 + 0xFE, WRAM, false) == 0 and not myCell and (function()
          -- Form 2 is hurt only by thrown bombs: get the glove first (item
          -- kind 7, art $1626, under the soft block at (13,10)).
          for k, kind in pairs(heartNow) do
            if kind == "item" then
              local d1 = bfs(g, c, r, function(cc, rr) return rr * KEY_W + cc == k end, nearEnemy, cw)
              if d1 then dir = d1; return true end
            end
          end
          if SOFT[g[10][13]] and bombsLeft then
            if here == 10 * KEY_W + 12 or here == 10 * KEY_W + 14 then dir = nil; input.a = frame % 2 == 0; return true end
            local spot = function(cc, rr) return rr == 10 and (cc == 12 or cc == 14) end
            local d1 = bfs(g, c, r, spot, nearEnemy, cw) or bfs(g, c, r, spot, nil, cw)
            if d1 then dir = d1; return true end
          end
          return false
        end)() then
        f1plan, plan, step = true, "glove", nil
      elseif myCell and here == myCell and (not calm or myCell == HK) then
        f1plan, plan, dir, step = true, "shield", nil, nil
      elseif myCell and (myCell ~= HK or here ~= HK) and (calm or here ~= myCell) then
        -- A shield dug in elsewhere (once calm), or the home bomb with P1
        -- off it (P1 can't step back onto it; a slam came down elsewhere):
        -- walk to home, or to the row 12 cell nearest it out of that bomb's
        -- blast, and set it off.
        local mc, mr = myCell % KEY_W, myCell // KEY_W
        local rc = {}
        for _, cell in ipairs(blastCells(g, mc, mr, RANGE)) do rc[cell[2] * KEY_W + cell[1]] = true end
        -- (on P1's own side of the bomb: it can't walk through it; already
        -- clear of the blast, set it off from here)
        local tc = 8
        local sgn = c < mc and -1 or 1
        for dd = 0, 6 do
          if not rc[12 * KEY_W + 8 + sgn * dd] and g[12][8 + sgn * dd] and walkable(g, 8 + sgn * dd, 12) then tc = 8 + sgn * dd; break end
        end
        f1plan, plan, step = true, "shield", nil
        if rc[here] then dir = walkTo(tc) else dir = nil end
        if not dir and not rc[here] and frame >= nextDetonate then
          input.b = true; nextDetonate = frame + 20
          log(string.format("%6d detonate (5-8 shield at (%d,%d))", frame, mc, mr))
        end
      elseif not myCell and bombsLeft then
        f1plan, plan, step = true, "shield", nil
        if here == HK then
          dir = nil
          input.a = frame % 2 == 0
        else
          dir = walkTo(8)
          if r == 12 and dir then
            local nc = c + (dir == "right" and 1 or -1)
            -- a ball by the next cell: dig in here instead
            if nearEnemy[12 * KEY_W + nc] or danger[12 * KEY_W + nc] then
              dir = nil
              if ballNear and not danger[here] then input.a = frame % 2 == 0 end
            end
          end
          -- off row 12 with no way back: dig in, or else the usual dodge
          if not dir and r ~= 12 then
            if ballNear and not danger[here] then input.a = frame % 2 == 0 else f1plan = nil end
          end
        end
      end
    end
  end
  -- Chaser stand: a harmful chaser faster than P1 (5-1, 5-6: 1.25 px/f
  -- $8A) can't be outrun or dodged in a corridor, so make it come into a
  -- blast: drop a bomb here and step (at most 2 cells) to a cell outside
  -- the blast that it can only reach through the bomb, and wait there. It
  -- stops by the bomb and is caught when it goes off (a 2-HP one is hurt
  -- and comes again: the stand repeats). (5-6, run 36: P1 walked out of
  -- its corner to meet one and died 8 times there.)
  if ride.stand and (frame > ride.stand.untilF or (frame > ride.stand.at + 12 + (ride.stand.walk or 0) * FPT and not bombAtKey(ride.stand.bomb))) then
    -- (a stand that hurt nobody is a miss: after 3 at one bomb cell, not
    -- there again for 1500 frames. 2-3, seed 0: the same stand, bomb
    -- (4,4) and wait (3,3), every ~195 frames for 6000 frames; the $82
    -- never came in)
    local hp = 0
    for _, e in ipairs(elist) do if e.harmful and not e.gadget then hp = hp + (e.hp or 1) end end
    ride.standMiss = ride.standMiss or {}
    local bk = ride.stand.bomb
    if hp >= (ride.stand.hp0 or 0) then
      local m = ride.standMiss[bk] or { 0, 0 }
      m[1] = m[1] + 1; if m[1] >= 3 then m[2] = frame + 1500; m[1] = 0 end
      ride.standMiss[bk] = m
    else ride.standMiss[bk] = nil end
    log(string.format("%6d stand over", frame)); ride.stand = nil
  end
  if not f1plan and not ride.stand and ride.chaserEta and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0
      and not danger[here] and g[r][c] ~= SPRING and not isBomb(g[r][c]) then
    -- (also with remote bombs out: the ambush nearly always has one out,
    -- so a hurt 2-HP $82 chasing at 1.0 px/f met no stand and caught
    -- speed-1 P1; 2-3, unaided, 2 of 3 seeds)
    local fastNear = false
    for _, e in ipairs(elist) do if e.chaser and e.harmful and not e.shot and e.cpf < FPT then fastNear = true end end
    local etaNow = fastNear and ride.chaserEta()
    local eh = etaNow and etaNow[here]
    if eh and eh < 150 then
      -- Bomb cells: here, or up to 2 steps on, reached well before the
      -- chaser (5-6, run 46: from the (2,27) corner no cell worked for a
      -- bomb at the corner itself; one at (3,26) with P1 back in the corner
      -- does).
      local bq, bqi, bseen, pick = { { c, r, 0 } }, 1, { [here] = true }, nil
      while bq[bqi] and not pick do
        local bc, br, bn = bq[bqi][1], bq[bqi][2], bq[bqi][3]; bqi = bqi + 1
        local bk = br * KEY_W + bc
        local okB = bn == 0 or (not etaNow[bk] or etaNow[bk] > bn * FPT + 40)
        local sm = ride.standMiss and ride.standMiss[bk]
        if sm and frame < sm[2] then okB = false end
        if okB and (bn == 0 or (not isBomb(g[br][bc]) and g[br][bc] ~= SPRING)) then
          local blast = {}
          for _, cell in ipairs(blastCells(g, bc, br, RANGE)) do blast[cell[2] * KEY_W + cell[1]] = true end
          local etaB = ride.chaserEta(bc, br, NEW_FUSE + bn * FPT)
          local seen, q, qi = { [bk] = 0 }, { { bc, br, 0 } }, 1
          while q[qi] and not pick do
            local cc, rr, n = q[qi][1], q[qi][2], q[qi][3]; qi = qi + 1
            for _, d in ipairs(DIRS) do
              local nc, nr = cc + d[1], rr + d[2]
              local k = nr * KEY_W + nc
              if not seen[k] and n < 2 and walkable(g, nc, nr) and not nearEnemy[k] and not danger[k] then
                seen[k] = n + 1
                local en, eb = etaNow[k], etaB[k]
                local tArr = (bn + n + 1) * FPT
                if not blast[k] and (not en or en > tArr + 10) and (not eb or eb > NEW_FUSE + bn * FPT + FLAME_FRAMES)
                    and not hit(windows, k, 0, NEW_FUSE + bn * FPT + FLAME_FRAMES) then
                  pick = { nc, nr, bc, br, bn }; break
                end
                q[#q + 1] = { nc, nr, n + 1 }
              end
            end
          end
        end
        if not pick and bn < 2 then
          for _, d in ipairs(DIRS) do
            local nc, nr = bc + d[1], br + d[2]
            local k = nr * KEY_W + nc
            if not bseen[k] and walkable(g, nc, nr) and not nearEnemy[k] and not danger[k] then
              bseen[k] = true; bq[#bq + 1] = { nc, nr, bn + 1 }
            end
          end
        end
      end
      if pick then
        local hp0 = 0
        for _, e in ipairs(elist) do if e.harmful and not e.gadget then hp0 = hp0 + (e.hp or 1) end end
        ride.stand = { s = pick[2] * KEY_W + pick[1], bomb = pick[4] * KEY_W + pick[3], at = frame, walk = pick[5], hp0 = hp0,
          untilF = frame + pick[5] * FPT + (remoteOn and 400 or NEW_FUSE + FLAME_FRAMES + 10) }
        log(string.format("%6d stand: bomb at (%d,%d) for a chaser %.0f frames off, wait at (%d,%d)", frame, pick[3], pick[4], eh, pick[1], pick[2]))
      end
    end
  end
  if ride.stand and ride.stand.bomb == here and not ride.stand.placed and not bombAtKey(here) then input.a = frame % 2 == 0 end
  if ride.stand and bombAtKey(ride.stand.bomb) then ride.stand.placed = true end
  -- (with the remote the bomb waits for B: set it off once P1 is at the
  -- stand cell and a chaser is in the blast)
  -- (and never with P1 in any remote bomb's blast: B sets off the oldest
  -- one first; 2-4, run BH s0, an older bomb at (5,6) went off instead of
  -- the stand's (6,4) and its column-5 blast took a heart at (5,5))
  if ride.stand and remoteOn and here == ride.stand.s and bombAtKey(ride.stand.bomb) and frame >= nextDetonate
      and not remoteCells[here] then
    local bk = ride.stand.bomb
    local inBlast = {}
    for _, cell in ipairs(blastCells(g, bk % KEY_W, bk // KEY_W, RANGE)) do inBlast[cell[2] * KEY_W + cell[1]] = true end
    for _, e in ipairs(elist) do
      if e.harmful and not e.gadget and (inBlast[e.r * KEY_W + e.c] or (e.moving and inBlast[e.tr * KEY_W + e.tc])) then
        input.b = true; nextDetonate = frame + 20
        log(string.format("%6d detonate (stand: chaser at (%d,%d) in the blast)", frame, e.c, e.r))
        ride.stand.untilF = math.min(ride.stand.untilF, frame + FLAME_FRAMES + 10)
        break
      end
    end
  end
  if f1plan then
    -- (5-8 form 1, above)
  -- (not with an enemy about to reach P1: the dodge comes first; 2-6, run
  -- BH s0, waited at (5,8) for a chaser to walk into (6,9)'s blast; it came
  -- down column 5 instead and walked into P1)
  elseif ride.stand and not danger[ride.stand.s] and not nearEnemy[here] and (here == ride.stand.s
      or bfs(g, c, r, function(cc, rr) return cc == ride.stand.s % KEY_W and rr == ride.stand.s // KEY_W end, mobNow, cw)) then
    plan = "stand"
    local sc, sr = ride.stand.s % KEY_W, ride.stand.s // KEY_W
    if not ride.stand.placed and here ~= ride.stand.bomb then
      local bc, br = ride.stand.bomb % KEY_W, ride.stand.bomb // KEY_W
      dir = bfs(g, c, r, function(cc, rr) return cc == bc and rr == br end, mobNow, cw)
    elseif not ride.stand.placed then
      dir = nil
    elseif here ~= ride.stand.s then
      dir = bfs(g, c, r, function(cc, rr) return cc == sc and rr == sr end, mobNow, cw)
    end
  elseif danger[here] then
    -- (a stand whose cell can't be reached any more: the usual flee)
    if ride.stand and here ~= ride.stand.s then
      log(string.format("%6d stand cell unreachable: fleeing instead", frame))
      -- (a miss too: 2-3, unaided, the same stand at (13,5) and its
      -- unreachable cell alternated 100+ times until the stage timed out)
      ride.standMiss = ride.standMiss or {}
      local m = ride.standMiss[ride.stand.bomb] or { 0, 0 }
      m[1] = m[1] + 1; if m[1] >= 3 then m[2] = frame + 1500; m[1] = 0 end
      ride.standMiss[ride.stand.bomb] = m
      ride.stand = nil
    end
    plan = "flee"
    -- (first a goal chasers reach 120+ frames after P1: P1 waits there for
    -- the blasts to pass. 1-3, runs 14-16: fled to (6,19) with a $8C
    -- coming up the next column and barrel bursts due on the way out;
    -- it was pulled in, every run alike)
    if ride.chaserEta then
      local eta, sg = ride.chaserEta(), safeGoal(windows)
      dir = bfs(g, c, r, function(cc, rr, n)
        local e = eta[rr * KEY_W + cc]
        return sg(cc, rr, n) and (not e or e > n * FPT + 120)
      end, nearEnemy, cw)
    end
    -- (in a boss fight, first a junction, 3+ open sides: 2-8 waited at the
    -- arena edge in (3,4), two sides, and a drop's column and the body's
    -- glide closed both)
    if not dir and next(bossNow) then
      local sg = safeGoal(windows)
      dir = bfs(g, c, r, function(cc, rr, n)
        if not sg(cc, rr, n) then return false end
        local open = 0
        for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then open = open + 1 end end
        return open >= 3
      end, nearEnemy, cw)
    end
    if not dir and ride.walkerEta then
      local sg = safeGoal(windows)
      dir = bfs(g, c, r, function(cc, rr, n) return sg(cc, rr, n) and walkerSafe(cc, rr, n) end, nearEnemy, cw)
    end
    -- No way out on foot but a platform waiting beside P1 at a lane end:
    -- board it (4-4: flipped between "ride" and "flee" each few frames by
    -- the parked $DC at (3,10), never pressing into it, until the blast).
    if not dir then
      for _, ln in ipairs(laneEnds(g, c, r)) do
        local dc, dr = (ln[1] - c) // ln[3], (ln[2] - r) // ln[3]
        for _, e in ipairs(elist) do
          if e.type == 0xDC and e.c == c + dc and e.r == r + dr then
            plan, dir = "board", nil
            ride.boardDir = (dc == 1 and "right") or (dc == -1 and "left") or (dr == 1 and "down") or "up"
          end
        end
      end
    end
    if plan == "board" then
      -- (input set below, after the step logic)
    else
    dir = dir or bfs(g, c, r, safeGoal(windows), nearEnemy, cw)
      or bfs(g, c, r, safeGoal(windows), enemyNow, windows)
      or bfs(g, c, r, function(cc, rr) return not windows[rr * KEY_W + cc] end, enemyNow)
    end
    -- A flee flipping between two ways out (different goals winning on
    -- alternate frames) goes nowhere: after a few flips keep the last way
    -- for 20 frames while its next cell stays open and clear of enemies.
    -- (3-8, unaided run Y2 s0: bombed the boss from (2,8), then up/right
    -- by turns for 35 frames on the bomb's cell; boxed in, the body came.)
    local DIRV = { up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } }
    if plan == "flee" and dir and DIRV[dir] then
      local fh = ride.fleeH
      if fh and fh.k == here and fh.lock and frame < fh.lock[2] then
        local v = DIRV[fh.lock[1]]
        local nk = (r + v[2]) * KEY_W + c + v[1]
        if walkable(g, c + v[1], r + v[2]) and not nearEnemy[nk] then dir = fh.lock[1] end
      elseif fh and fh.k == here and frame - fh.at <= 3 then
        if dir ~= fh.d then fh.flips = fh.flips + 1 end
        fh.d, fh.at = dir, frame
        if fh.flips >= 4 then fh.lock = { dir, frame + 20 } end
      else
        ride.fleeH = { k = here, d = dir, at = frame, flips = 0 }
      end
    end
  elseif nearEnemy[here] or (frame - throw.dodgeAt < 12 and hit(eWin, here, 0, 90)) then
    -- Keep at a dodge for a few frames: nearEnemy flickers as predictions
    -- shift, and turning back each other frame is how the 5-8 body corners us.
    if nearEnemy[here] then throw.dodgeAt = frame end
    plan = "dodge"
    -- Keep the dodge goal for up to 40 frames while it stays safe and in
    -- reach: chosen afresh each frame it flipped as predictions shifted,
    -- and P1 dithered under the 1-8 boss until it was caught.
    local sg = safeGoal(windows)
    local dg = ride.dodgeGoal
    -- (never through a normal enemy's cell or the one it is entering: the
    -- windows can let P1 "swap" with a walker head-on in a corridor)
    if dg and frame < dg[3] and dg[1] * 1 == dg[1] and sg(dg[1], dg[2], 0) and not (dg[1] == c and dg[2] == r) then
      dir = bfs(g, c, r, function(cc, rr) return cc == dg[1] and rr == dg[2] end, mobNow, cw)
    end
    -- (first a goal that is no dead end, 2+ open sides, within 6 steps:
    -- 2-1, every seed, dodged down column 5 into the (5,10) dead end and an
    -- $86 dashed down after; 4-3 likewise with a $93)
    -- (and before that one off every loop: 2-2, run X2 s0, dodged an $86
    -- coming along row 5 into column 5, two sides all the way down to the
    -- push block at (5,11), and it dashed in after)
    if not dir then
      local gk, cut = nil, ride.pockets(g)
      dir = bfs(g, c, r, function(cc, rr, n)
        if n > 6 or cut[rr * KEY_W + cc] or not sg(cc, rr, n) then return false end
        gk = { cc, rr, frame + 40 }
        return true
      end, mobNow, cw)
      ride.dodgeGoal = dir and gk or nil
    end
    if not dir then
      local gk
      dir = bfs(g, c, r, function(cc, rr, n)
        if n > 6 or not sg(cc, rr, n) then return false end
        local open = 0
        for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then open = open + 1 end end
        if open >= 2 then gk = { cc, rr, frame + 40 }; return true end
        return false
      end, mobNow, cw)
      ride.dodgeGoal = dir and gk or nil
    end
    if not dir then
      local gk
      dir = bfs(g, c, r, function(cc, rr, n)
        local ok = sg(cc, rr, n)
        if ok then gk = { cc, rr, frame + 40 } end
        return ok
      end, mobNow, cw)
      ride.dodgeGoal = dir and gk or nil
    end
    if dir and os.getenv("SBM2_DBG2") then
      local d = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[dir:match("^%a+")]
      local function ws(k) local t = {} for _, iv in ipairs(eWin[k] or {}) do t[#t + 1] = string.format("%.0f-%.0f", iv[1], iv[2]) end return table.concat(t, ",") end
      local nk = (r + d[2]) * KEY_W + c + d[1]
      local nk2 = (r + 2 * d[2]) * KEY_W + c + 2 * d[1]
      log(string.format("%6d dbg2 dodge %s here[%s] next[%s] next2[%s] goal %s", frame, dir, ws(here), ws(nk), ws(nk2),
        ride.dodgeGoal and string.format("(%d,%d)", ride.dodgeGoal[1], ride.dodgeGoal[2]) or "-"))
    end
    if not dir and os.getenv("SBM2_DBG") and frame % 20 == 0 then
      local t = {}
      for dr = -4, 4 do for dc = -4, 4 do
        if math.abs(dr) + math.abs(dc) <= 4 then
          local cc, rr = c + dc, r + dr
          local k = rr * KEY_W + cc
          if walkable(g, cc, rr) then
            local fe, fb = "-", "-"
            for _, iv in ipairs(eWin[k] or {}) do if iv[2] >= 0 and (fe == "-" or iv[1] < fe) then fe = iv[1] end end
            for _, iv in ipairs(windows[k] or {}) do if iv[2] >= 0 and (fb == "-" or iv[1] < fb) then fb = iv[1] end end
            t[#t + 1] = string.format("(%d,%d)e%sb%s", cc, rr, tostring(fe), tostring(fb))
          end
        end
      end end
      log(string.format("%6d dbg dodge none at (%d,%d): %s", frame, c, r, table.concat(t, " ")))
      local bs = {}
      for _, b in ipairs(bosses) do bs[#bs + 1] = string.format("%04X:%06X(%d,%d v%.2f,%.2f)%s", b.base, b.state, b.x, b.y, b.vx, b.vy, b.box and string.format("box%d..%d,%d..%d", b.box[1], b.box[2], b.box[3], b.box[4]) or "") end
      log(string.format("%6d dbg bosses %s", frame, table.concat(bs, " ")))
    end
    -- (no fully safe goal: any cell clear of blasts and enemies now; a
    -- "least bad step" by first contact helped 5-3 a little and lost 3-8)
    -- (never through an enemy's cell: 3-2, run WS s0, in the (9,10) pocket
    -- with its way out (9,11) still burning, this walked up column 9 into
    -- the $8F coming down it; holding still, the flame ends first)
    dir = dir or bfs(g, c, r, function(cc, rr)
        return not nearEnemy[rr * KEY_W + cc] and not danger[rr * KEY_W + cc]
      end, mobNow, windows)
  elseif finalForm2() and emu.read(P1 + 0xFE, WRAM, false) > 0 and (function()
      -- Form 2 with the glove: walk to a throw spot (Boss.aimGoal).
      local body
      for _, b in ipairs(bosses) do if b.base == 0x1000 and not b.hurt and b.state ~= 0xCCD2E7 then body = b end end
      if not body then return false end
      local avoid = setmetatable({}, { __index = function(_, k) return nearEnemy[k] or danger[k] end })
      local d1, n = bfs(g, c, r, Boss.aimGoal(body, avoid), nearEnemy, cw)
      if not n then
        -- No sure shot yet: wait inside the waypoints' box (x 64..208,
        -- y 64..200, cells 4..13 x 4..12), not in the body's way, rather
        -- than in a corner it never comes near.
        d1, n = bfs(g, c, r, function(cc, rr, m)
          if avoid[rr * KEY_W + cc] or cc < 5 or cc > 12 or rr < 5 or rr > 11 then return false end
          local bx, by = Boss.predict(body, m * FPT)
          return math.abs(cc * 16 - bx) >= 48 or math.abs(rr * 16 + 7 - by) >= 48
        end, nearEnemy, cw)
        if not n then return false end
      end
      dir = d1
      return true
    end)() then
    plan = "aim"
  else
    -- Is bombing here useful (a soft block or enemy in reach)?
    -- Is bombing at (cc,rr) useful? "obj" (soft block, switch, panel),
    -- "enemy" (an enemy on or next to the blast), or nil. A blast that would
    -- re-toggle a finished panel is never useful.
    -- World 5 light switch ($64, cell $7E5AAE; docs/mechanics.md 4): bit 0
    -- of $7E5AAC odd = $8A slow (0.5 wanderer), $97 fast; even the other
    -- way round. Keep it on the side that slows most of the live ones: a
    -- wrong one is a target, a right one is never blasted.
    local swVal, swK, swWant = r16(0x5AAC), nil, nil
    if swVal ~= 0xFFFF then
      local cell = r16(0x5AAE)
      swK = (cell >> 7) * KEY_W + (cell & 0x7F) // 2
      local n8A, n97 = 0, 0
      for _, e in ipairs(elist) do
        if e.type == 0x8A then n8A = n8A + 1 elseif e.type == 0x97 then n97 = n97 + 1 end
      end
      if n8A + n97 > 0 then swWant = (n8A >= n97) and 1 or 0 end
    end
    -- Soft blocks next to an unfinished panel, while no parity plan exists.
    local walled = {}
    if not panelPlan and next(panelTodo) then
      for k in pairs(panelTodo) do
        local pc, pr = k % KEY_W, k // KEY_W
        -- (every soft block along its row and column up to a wall: 4-5's
        -- (29,10) is reached only through (28,10) and (27,10))
        for _, d in ipairs(DIRS) do
          for i = 1, 10 do
            local t2 = g[pr + d[2] * i] and g[pr + d[2] * i][pc + d[1] * i]
            if not t2 or (HARD[t2] and not SOFT[t2]) then break end
            if SOFT[t2] then walled[(pr + d[2] * i) * KEY_W + pc + d[1] * i] = true end
          end
        end
      end
    end
    local function wallBlast(cc, rr)
      if not next(walled) then return false end
      for _, cell in ipairs(blastCells(g, cc, rr, rangeAt(g, cc, rr))) do
        if walled[cell[2] * KEY_W + cell[1]] then return true end
      end
      return false
    end
    local function targetsInReach(cc, rr, t)
      -- No bomb on a tile with bit 6 set ($40-$7F: stairs $66, panels $69,
      -- lit panels $70, $72...): BombButton C0:3082 returns when the tile
      -- under P1 (+$B4) has it. (2-4, run 18: walked down a pocket to bomb
      -- from a lit panel, nothing came out, and the soft block behind P1
      -- grew back: sealed in for good.)
      local ht = g[rr][cc]
      if ht < 0x100 and ht & 0x40 ~= 0 and ht ~= ITEM then return nil end  -- (an item is picked up first)
      if (noBombUntil[rr * KEY_W + cc] or 0) > frame then return nil end
      -- (the 5-1 opening, below, goes first: no other bombs meanwhile)
      if ride.sw51 and rr * KEY_W + cc ~= ride.sw51 then return nil end
      local why, cells, near = nil, {}, false
      if pushCell == rr * KEY_W + cc then why = "obj" end
      for _, cell in ipairs(blastCells(g, cc, rr, rangeAt(g, cc, rr))) do
        local k = cell[2] * KEY_W + cell[1]
        local tt = g[cell[2]][cell[1]]
        -- The planner's own step may flip a finished panel: it priced that
        -- in, and the parity plan sets the panel right afterwards.
        if pushCell == rr * KEY_W + cc then
          -- (why is "obj" already)
        elseif panelPlan and (panelDone[k] or panelTodo[k]) then
          if not panelPlan[rr * KEY_W + cc] then return nil end
          why = "obj"
        elseif panelDone[k] then
          -- (no parity plan at all: a bomb that opens a soft block walling
          -- in an unfinished panel may flip done ones, which the solver
          -- then re-plans; 4-5, unaided, the last panel (28,10) sat inside
          -- four soft blocks and the stage timed out)
          if not wallBlast(cc, rr) then return nil end
          why = "obj"
        end
        -- $68: lighting it ($6F) is what kills 3-1's immune $8F enemies.
        if panelTodo[k] or (SOFT[tt] and not softGone[k]) or tt == BARREL or isSwitchOff(tt) or tt == 0x68 or tt == 0x6B or tt == 0x6A or tt == 0x4B or tt == 0x63 then why = "obj" end
        -- A blast pushes an $E2 block one cell on; into a $D3 hole it fills
        -- it and opens the way (docs/mechanics.md, push blocks).
        -- While a plan is on, only its own bombs move blocks.
        -- (only if it would move: 2-2, unaided run A3 s1, every bomb down
        -- column 5 opening the start pocket was refused for the $E2 at
        -- (5,11), which a blast from above only presses into the wall)
        -- (with no plan on too, a push onto floor: 5-5, run AC s0, seek and
        -- enemy bombs pushed row 10's $E2s into a pair at (10,10)/(11,10)
        -- with P1 west of them, cut off from row 4's panels; time-up. A
        -- push into a $D3 hole is still fine then: row 4's chain to 5-5's
        -- last panel is done that way when the planner runs out of states)
        if tt == 0xE2 and pushCell ~= rr * KEY_W + cc then
          local to = cell[3] and g[cell[2] + cell[3][2]] and g[cell[2] + cell[3][2]][cell[1] + cell[3][1]]
          if to == 0x00 or (to == 0xD3 and planSteps) then return nil end
        end
        if tt == 0xE2 and cell[3] then
          -- Worth pushing toward a hole one or two cells on (a push moves
          -- it one cell; a block pushed into a $D3 fills it).
          local nc, nr = cell[1] + cell[3][1], cell[2] + cell[3][2]
          local n2 = g[nr + cell[3][2]] and g[nr + cell[3][2]][nc + cell[3][1]]
          if g[nr] and (g[nr][nc] == 0xD3 or (g[nr][nc] == 0x00 and n2 == 0xD3)) then why = "obj" end
        end
        if tt == ITEM and heartNow[k] then return nil end  -- never blast a heart item
        if swWant and k == swK then
          -- (the planner's own step goes ahead: flipped, it is a target again)
          if (swVal & 1) == swWant then
            if pushCell ~= rr * KEY_W + cc then return nil end
          else why = "obj" end
        end
        cells[k] = true
        if reach[k] then near = true end
      end
      -- A bomb on a boss's sense point is eaten (BossEatBomb CC:A2C4).
      if next(bossSense) then
        if bossEat[rr * KEY_W + cc] or bossSense[rr * KEY_W + cc] then return nil end
        for k in pairs(cells) do
          if bossSense[k] then why = why or "enemy"; break end
        end
      end
      -- A bomb an eater gets to first is swallowed: no blast at all.
      local eatK = 0
      if hasEater then
        local eat
        eat, eatK = Enemies.eaterFate(g, samples, elist, cc, rr, t or 0, NEW_FUSE, rangeAt(g, cc, rr), FLAME_FRAMES)
        if eat >= 0.3 then return nil end
      end
      if not why and near then
        local t0 = (t or 0) + NEW_FUSE
        local _, best = Enemies.killChance(samples, elist, cells, t0 - 4, t0 + FLAME_FRAMES, EATER_SKIP)
        if math.max(best, eatK) >= KILL_P then why = "enemy" end
      end
      -- A chaser (4-3's $93: 2 hp, faster after a hit) follows P1 and is
      -- stopped by the bomb in its way, waiting next to it: a bomb dropped
      -- with one 4 cells or less behind catches it (the rollouts don't see
      -- the bomb block it).
      if not why and t == nil then
        for _, e in ipairs(elist) do
          if e.chaser and e.killable and math.abs(e.c - cc) + math.abs(e.r - rr) <= 4 then why = "enemy"; break end
        end
      end
      return why
    end
    -- After placing a bomb at (cc,rr) at time t, can we reach a cell no blast
    -- will ever touch, without being in any blast on the way?
    -- World 2 and 5-5: blown soft blocks grow back ~190 frames later, so
    -- an escape is searched with those blown 150+ frames ago back (the
    -- bomb's own cell excepted; a fresher gap stays open long enough to
    -- leave by: 2-4's plan bombs inside (3,8) and leaves through (4,8)). (2-2, run 20: bombed at (17,5) with the way out through
    -- (14,5), blown earlier; it grew back and P1 sat between it and the
    -- bomb.)
    -- A blown cell P1 stands on or next to does not grow back meanwhile,
    -- and only some 60 frames after P1 has moved off (2-4, run HZ s0:
    -- (4,8) stayed open 650 frames while P1 waited at (5,8), and was back
    -- 65 frames after P1 went down column 3). So one next to P1 now and to
    -- the bomb's cell stays open for that bomb (P1 walks by it to the bomb
    -- and back out by it); for bombs elsewhere it is counted as back.
    -- (there P1 waited 600 frames, refusing the panel bomb at (3,8) since
    -- (4,8) was its only way out, then walked into the column, which (4,8)
    -- then sealed, and spent both hearts on stall breaker bombs.)
    local escGrid, escGridAll, keptOpen
    local stgE = emu.read(0x1A, WRAM, false)
    if (stgE & 0xF0) == 0x20 or stgE == 0x54 then
      escGrid = {}
      for r2 = 0, MAP_H - 1 do escGrid[r2] = g[r2] end
      for k in pairs(softSeen) do
        local r2, c2 = k // KEY_W, k % KEY_W
        if not SOFT[g[r2][c2]] and g[r2][c2] < 0x40 and not (c2 == c and r2 == r)
            and frame - (softBlownAt[k] or frame) > 150 then
          if math.abs(c2 - c) + math.abs(r2 - r) <= 1 then
            keptOpen = keptOpen or {}
            keptOpen[#keptOpen + 1] = { c2, r2 }
          else
            if escGrid[r2] == g[r2] then local row = {}; for c3 = 0, MAP_W - 1 do row[c3] = g[r2][c3] end; escGrid[r2] = row end
            escGrid[r2][c2] = 0xC2
          end
        end
      end
      if keptOpen then
        escGridAll = {}
        for r2 = 0, MAP_H - 1 do escGridAll[r2] = escGrid[r2] end
        for _, kc in ipairs(keptOpen) do
          local row = {}
          for c3 = 0, MAP_W - 1 do row[c3] = escGridAll[kc[2]][c3] end
          row[kc[1]] = 0xC2
          escGridAll[kc[2]] = row
        end
      end
    end
    local function canEscape(cc, rr, t)
      local w = withBomb(g, windows, cc, rr, NEW_FUSE + t)
      -- With the remote, the end cell must also be clear of the remote
      -- bombs' blasts, or setting them off later would mean dying.
      local safe0 = safeGoal(w, t)
      -- (a bomb opening P1's pocket, ride.openers, skips the chaser and
      -- walker rules below: they kept 2-2's start-pocket opener at (5,7)
      -- from ever being laid, and the $86 came in anyway)
      local opener = false
      if ride.openers then
        for _, cell in ipairs(blastCells(g, cc, rr, rangeAt(g, cc, rr))) do
          if ride.openers[cell[2] * KEY_W + cell[1]] then opener = true; break end
        end
      end
      -- (so does one that lights the last crystal $68: every $8F dies with
      -- it, docs/mechanics.md 6. 3-2, unaided run Y2 s2: the last one at
      -- (3,10) is bombed from row 11, where the two immune $8F walk; the
      -- walker rule turned down every bomb there until time-up)
      if not opener and ride.crystals == 1 then
        for _, cell in ipairs(blastCells(g, cc, rr, rangeAt(g, cc, rr))) do
          if g[cell[2]][cell[1]] == 0x68 then opener = true; break end
        end
      end
      -- Chasers follow P1 wherever it goes (the rollouts aim them at where
      -- P1 is now): the end cell must be one P1 holds well before any
      -- chaser can walk there, the new bomb in its way (5-1: P1 bombed and
      -- backed into a column a 1.25 px/f chaser was coming up).
      local safe = safe0
      if ride.chaserEta and not opener then
        local eta = ride.chaserEta(cc, rr, NEW_FUSE + t)
        safe = function(c2, r2, n)
          if not safe0(c2, r2, n) then return false end
          local e = eta[r2 * KEY_W + c2]
          if not e then return true end
          local open = 0
          for _, d in ipairs(DIRS) do if walkable(g, c2 + d[1], r2 + d[2]) then open = open + 1 end end
          -- (a pocket or corridor: P1 still has to walk out after the blast;
          -- 5-1, run 31: ambush bomb at a pocket's mouth, the 2-HP $8A was
          -- only hurt, came on 100 frames later and caught P1 inside)
          if e > NEW_FUSE + t + FLAME_FRAMES + (open >= 3 and 0 or 200) then return true end
          -- (P1 waits there until the blast is over: a chaser coming
          -- before that is met with the blast behind P1, unless it is a
          -- junction to sidestep at. 5-1, run 34: bombed (2,7), hid on
          -- row 4 and a $8A came along it.)
          if e <= t + n * FPT + 40 then return false end
          return open >= 3
        end
      end
      -- Any walker: an end cell off a junction (two sides or fewer) that a
      -- walker reaches before the blast is over is a trap, the blast on one
      -- side and the walker on the other (2-4, run 18: bombed a $82 and
      -- waited in the row-12 corridor at (8,12); the other $82 came in).
      -- (not for a bomb that lights a gold panel still to do: the walker
      -- rule kept 1-2's panel bomb at (24,7) from ever being laid while its
      -- endless $94s roamed, and the stage timed out, unaided)
      -- (nor for one that opens P1's pocket: see ride.openers)
      local panelBomb = opener
      for _, cell in ipairs(blastCells(g, cc, rr, rangeAt(g, cc, rr))) do
        if panelTodo[cell[2] * KEY_W + cell[1]] then panelBomb = true; break end
      end
      if ride.walkerEta and not panelBomb then
        local weta, safe1 = ride.walkerEta(cc, rr, NEW_FUSE + t), safe
        safe = function(c2, r2, n)
          if not safe1(c2, r2, n) then return false end
          local e = weta[r2 * KEY_W + c2]
          if not e or e > NEW_FUSE + t + FLAME_FRAMES then return true end
          local open = 0
          for _, d in ipairs(DIRS) do if walkable(g, c2 + d[1], r2 + d[2]) then open = open + 1 end end
          return open >= 3
        end
      end
      local goal = remoteOn and function(c2, r2, n)
        return safe(c2, r2, n) and not remoteCells[r2 * KEY_W + c2]
      end or safe
      -- Rides don't count (the platform comes when it comes), unless it is
      -- here: a $DC within 2 cells of the bomb (4-4, 4-2: a lane end was
      -- the only way out of a column the blast fills).
      allowRide = ride.dockedNear(cc, rr, 20)
      -- (not through a cell an enemy holds now: 3-3, run 19, bombed with
      -- the only way out up a one-wide column a $90 was coming down, the
      -- route timed past it on its predicted walk; shot, every time)
      local eg = g
      if escGrid and not SOFT[escGrid[rr][cc]] then
        eg = escGrid
        if keptOpen then
          for _, kc in ipairs(keptOpen) do
            if math.abs(kc[1] - cc) + math.abs(kc[2] - rr) > 1 then eg = escGridAll; break end
          end
        end
      end
      local gotC
      local ok = bfs(eg, cc, rr, function(c2, r2, n) local v = goal(c2, r2, n); if v then gotC = { c2, r2, n } end; return v end, mobNow, combine(w, eWin), t) ~= nil
      if os.getenv("SBM2_DBG2") and ok and gotC then
        local e = ride.chaserEta and ride.chaserEta(cc, rr, NEW_FUSE + t)[gotC[2] * KEY_W + gotC[1]]
        log(string.format("%6d dbg2 escape from (%d,%d) t%d to (%d,%d) n%d chaserEta %s", frame, cc, rr, t, gotC[1], gotC[2], gotC[3], tostring(e)))
      end
      allowRide = true
      return ok
    end
    -- With the remote, one bomb at a time: a remote bomb is never danger, so
    -- a second one placed nearby can seal us in between them.
    local haveBomb = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and not (remoteOn and next(remoteCells))
    -- The last bomb is kept for a stand while a chaser faster than P1 can
    -- be here within 300 frames (5-6, run 41: after each death P1 spent its
    -- one bomb at the spawn and had none when the $8A came; five deaths).
    if haveBomb and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) == 1 and ride.chaserEta then
      local fast = false
      for _, e in ipairs(elist) do if e.chaser and e.harmful and not e.shot and e.cpf < FPT then fast = true end end
      local eh = fast and ride.chaserEta()[here]
      if eh and eh < 300 then haveBomb = false end
    end
    -- A cell worth reaching: the gate, an item, a panel, an enemy, a soft block.
    local function usefulAt(cc, rr, grid)
      local t = grid[rr][cc]
      if t == GATE_OPEN or t == ITEM or panelTodo[rr * KEY_W + cc] then return true end
      for _, d in ipairs(DIRS) do
        local n = grid[rr + d[2]] and grid[rr + d[2]][cc + d[1]]
        if n == 0xC9 or n == GATE_OPEN or (SOFT[n] and not softGone[(rr + d[2]) * KEY_W + cc + d[1]]) then return true end
      end
      for _, e in ipairs(walkers) do if e[1] == cc and e[2] == rr then return true end end
      return false
    end
    -- Nothing reachable to bomb for a while: the stage planner looks for a
    -- chain of pushes, bombs and trap crossings that opens up more of the
    -- map. It runs as a coroutine over several frames; its steps are then
    -- followed one by one, each checked against the map before it is taken
    -- (a step whose effect differs from the plan means a new search). Not
    -- while blasts are out or blocks still slide: the map is about to change.
    if next(danger) ~= nil or next(remoteCells) ~= nil then lastPending = frame end
    -- A pushed block slides for ~160 frames after its blast; a regrowing
    -- soft block (world 2) is back ~190 frames after it, so go at once.
    local settled = frame - lastPending > settleNeed
    local sigNow = structSig(g)
    if planCo then
      if planCo[2] ~= sigNow then
        planCo = nil
      else
        local t0 = os.clock()
        local ok, steps, exp, enemyOnly = coroutine.resume(planCo[1])
        planCo[3] = planCo[3] + os.clock() - t0
        if not ok then
          log(string.format("%6d plan error: %s", frame, tostring(steps))); planCo = nil
        elseif coroutine.status(planCo[1]) == "dead" then
          local descs = {}
          for _, st in ipairs(steps or {}) do descs[#descs + 1] = st.desc end
          log(string.format("%6d plan: %s%s [%d states, %.2fs]", frame, steps and table.concat(descs, " ") or "none",
            enemyOnly and " (enemy)" or "", exp or 0, planCo[3]))
          planSteps, planIdx, planCo, stepIdx = steps, 1, nil, 0
          if not steps then pushNext = frame + 600 end
          -- The same plan again and again with nothing left to do fewer: a
          -- loop (2-4, run BRK s0: bomb(13,8) 60 times in 22000 frames,
          -- the blast undone each time, until time ran out). Its first bomb
          -- cell is off for a while, so the search finds another way.
          if steps and steps[1] then
            local desc = table.concat(descs, " ")
            local nTodo = 0
            for _ in pairs(panelTodo) do nTodo = nTodo + 1 end
            local rp = ride.planRep
            if rp and rp.desc == desc and rp.todo == nTodo and rp.en == #elist then rp.n = rp.n + 1
            else rp = { desc = desc, todo = nTodo, en = #elist, n = 1 }; ride.planRep = rp end
            if rp.n >= 3 and steps[1].kind == "bomb" then
              log(string.format("%6d plan %s again (%d times, no progress): its first bomb cell off for 3000 frames", frame, desc, rp.n))
              noBombUntil[steps[1].k] = frame + 3000
              ride.planRep = nil
              planSteps, pushNext = nil, frame + 30
            end
          end
        end
      end
    end
    pushCell, trapCell = nil, nil
    -- (the current bomb step's cell, settled or not: walked to first, see
    -- "planstep"; it is bombed once the map has settled as before)
    ride.planCell = planSteps and planSteps[planIdx] and planSteps[planIdx].kind == "bomb" and planSteps[planIdx].k or nil
    while planSteps and settled do
      local st = planSteps[planIdx]
      if not st then planSteps = nil; lastObjFrame = frame; break end
      local done = true
      for _, ch in ipairs(st.changes) do if g[ch[2]][ch[1]] ~= ch[3] then done = false end end
      if done then
        planIdx = planIdx + 1
      else
        local reachNow = regionOf(g, c, r)
        local sc, sr = st.k % KEY_W, st.k // KEY_W
        local ok
        if st.kind == "bomb" then
          local changes = bombEffect(g, sc, sr)
          -- (reachable over a platform lane counts too: the planner routes
          -- over rides, regionOf does not; 4-2, unaided, every plan beyond
          -- the $DC lanes was dropped two frames after it was made, then
          -- time-ups)
          -- (RideBlock, enemies near a lane end, is a matter of when, not
          -- whether: the ride waits at the lane end; ignored here)
          local reach = reachNow[st.k] or st.k == here
          if not reach then
            -- (nor AvoidCell, track cells by a $DD: the block moves on; 4-2's
            -- only crossing of the column-8 track is (8,10), barred while
            -- the block passes, and the plan was dropped every time)
            local rb, ar, av = RideBlock, allowRide, AvoidCell
            RideBlock, allowRide, AvoidCell = {}, true, {}
            -- (and the block's own cell, $CD, as track: it moves off)
            local g2 = g
            for rr2 = 0, MAP_H - 1 do
              for cc2 = 0, MAP_W - 1 do
                if g[rr2][cc2] == 0xCD then
                  if g2 == g then g2 = {}; for r3 = 0, MAP_H - 1 do g2[r3] = g[r3] end end
                  if g2[rr2] == g[rr2] then local row = {}; for c3 = 0, MAP_W - 1 do row[c3] = g[rr2][c3] end; g2[rr2] = row end
                  g2[rr2][cc2] = 0x4D
                end
              end
            end
            local bcl = {}
            for k, v in pairs(badCell) do bcl[k] = v; badCell[k] = nil end
            reach = bfs(g2, c, r, function(cc, rr) return rr * KEY_W + cc == st.k end, nil, nil) ~= nil
            for k, v in pairs(bcl) do badCell[k] = v end
            if not reach and next(bcl) then log(string.format("%6d plan step reach: badCell %d cells", frame, (function() local n = 0; for _ in pairs(bcl) do n = n + 1 end; return n end)())) end
            RideBlock, allowRide, AvoidCell = rb, ar, av
          end
          ok = reach and #changes > 0 and keyChanges(changes, g) == st.key
        elseif g[sr][sc] ~= 0x63 then
          break  -- sprung, still breaking: wait for the $00
        else
          ok = (r16(P1 + 4) & 0x20) ~= 0 or emu.read(P1 + 0xEF, WRAM, false) > 0 or FAKE_HEARTS
          if ok then
            ok = false
            for _, d in ipairs(DIRS) do if reachNow[(sr + d[2]) * KEY_W + sc + d[1]] then ok = true end end
          end
        end
        if ok and planIdx == stepIdx and frame - stepSince > 1500 then
          -- Not done in 25 s (no way to bomb there, enemies in the way...).
          log(string.format("%6d plan step %s timed out; replanning", frame, st.desc))
          noBombUntil[st.k] = frame + 1800
          ok = false
        end
        if ok then
          if planIdx ~= stepIdx then stepIdx, stepSince = planIdx, frame end
          if st.kind == "bomb" then pushCell = st.k else trapCell = st.k end
          settleNeed = (st.kind == "bomb" and st.key ~= "") and 130 or 10
        else
          log(string.format("%6d plan step %s no longer fits; replanning", frame, st.desc))
          planSteps, pushNext = nil, 0
        end
        break
      end
    end
    -- (stalled, the planner runs every 1200 frames even while seek finds
    -- soft blocks to bomb, and its bomb is then the only block target:
    -- 5-5, run AF s0, one push from the last panel, P1 bombed every soft
    -- block in reach (they grow back) and the planner never ran; time-up)
    local forcedPlan = frame - (ride.lastStall or -1e9) < 3000 and frame - (ride.planForced or -1e9) > 1200
    if not planSteps and not planCo and settled and (frame - lastObjFrame > 240 or forcedPlan) and frame >= pushNext then
      ride.forcedPlan = forcedPlan and frame - lastObjFrame <= 240
      if ride.forcedPlan then ride.planForced = frame; log(string.format("%6d stalled: stage planner run now", frame)) end
      pushNext = frame + 120
      local anyWalkerIn = false
      local reachNow = regionOf(g, c, r)
      for _, e in ipairs(walkers) do if reachNow[e[2] * KEY_W + e[1]] then anyWalkerIn = true end end
      local hearts = emu.read(P1 + 0xEF, WRAM, false)
      local flashing = (r16(P1 + 4) & 0x20) ~= 0 and emu.read(P1 + 0xB9, WRAM, false) > 120
      local gc0 = {}
      for rr = 0, MAP_H - 1 do local row = {}; for cc = 0, MAP_W - 1 do row[cc] = g[rr][cc] end; gc0[rr] = row end
      local pTodo, pDone, sGone, wk = panelTodo, panelDone, softGone, walkers
      local panelsOnly = ride.forcedPlan and next(panelTodo) ~= nil
      local ctx = {
        objGoal = function(cc, rr, grid)
          local k = rr * KEY_W + cc
          local t = grid[rr][cc]
          if t == GATE_OPEN or t == ITEM or pTodo[k] or t == 0x68 or t == 0x6B then return true end
          -- (a stalled run with panels left plans for the panels only: 5-5,
          -- run AG s0, by row 4 one push from the last panel, the forced
          -- plan went for soft blocks in the lower left; time-up)
          if panelsOnly then
            for _, d in ipairs(DIRS) do if pTodo[(rr + d[2]) * KEY_W + cc + d[1]] then return true end end
            return false
          end
          for _, d in ipairs(DIRS) do
            local nk = (rr + d[2]) * KEY_W + cc + d[1]
            local n = grid[rr + d[2]] and grid[rr + d[2]][cc + d[1]]
            if n == GATE_OPEN or (SOFT[n] and not sGone[nk]) or pTodo[nk] or n == 0x6B or n == 0x68
                or n == 0x6A or n == 0x4B or n == 0x63 then return true end
          end
          return false
        end,
        enemyGoal = not anyWalkerIn and function(cc, rr)
          for _, e in ipairs(wk) do if e[1] == cc and e[2] == rr then return true end end
          return false
        end or nil,
        avoid = pDone, maxExp = 4000, slice = 0.03,
        noBomb = function(k, t) return (noBombTile[t] or 0) >= 2 or (noBombUntil[k] or 0) > frame end,
        hearts = (hearts >= 0x80 or FAKE_HEARTS) and 99 or hearts, flashing = flashing,
      }
      planCo = { coroutine.create(function() return stagePlan(gc0, c, r, ctx) end), sigNow, 0 }
    end
    -- 5-1 opening: its three $8A are 1.25 px/f x-ray chasers while the
    -- light switch at (6,7) reads even, and P1 at speed 1-3 can't outrun
    -- them (4 deaths a seed). Flipping it makes them 0.5 px/f wanderers.
    -- The way: bomb (4,4)/(5,4) open from row 4, bomb from (8,5) to push
    -- the $E2 at (8,6) into the $D3 hole at (8,7), then down column 8 and
    -- bomb from (6,8): the flame goes up column 6 onto the switch.
    ride.sw51 = nil
    if r16(0x5AAC) ~= ride.swLast then log(string.format("%6d light switch %04X -> %04X", frame, ride.swLast or 0, r16(0x5AAC))); ride.swLast = r16(0x5AAC) end
    if emu.read(0x1A, WRAM, false) == 0x50 and r16(0x5AAC) ~= 0xFFFF and (r16(0x5AAC) & 1) == 0 then
      local T
      -- The latest step P1 can reach comes first: the row-4 blocks grow
      -- back behind P1 (r89 rerun: at (8,8) with the $E2 pushed, (4,4)
      -- and (5,4) were back and the opening went back to step 1, out of
      -- reach; P1 waited there losing hearts).
      local reach = regionOf(g, c, r)
      if g[7][6] == 0x64 and reach[8 * KEY_W + 6] then T = 8 * KEY_W + 6
      elseif g[6][8] == 0xE2 and g[7][8] == 0xD3 and reach[5 * KEY_W + 8] then T = 5 * KEY_W + 8
      -- (a blasted block reads soft a while before it breaks: Breaking)
      elseif SOFT[g[4][4]] and not Breaking[4 * KEY_W + 4] then T = 4 * KEY_W + 3
      elseif SOFT[g[4][5]] and not Breaking[4 * KEY_W + 5] then T = 4 * KEY_W + 4
      elseif g[6][8] == 0xE2 and g[7][8] == 0xD3 then T = 5 * KEY_W + 8
      elseif g[7][6] == 0x64 then T = 8 * KEY_W + 6 end
      if T and (noBombUntil[T] or 0) <= frame then pushCell = T; ride.sw51 = T end
    end
    -- In a pocket (ride.pockets) with walkers about, first a soft block
    -- whose blast puts P1's cell on a loop. (2-2, runs X2 s0 and U s1:
    -- the start pocket, row 5 west of (6,5) and column 5 down to the
    -- push block at (5,11), opens at (6,7) or (6,9); P1 sought targets
    -- out along row 5, met an $86 there, backed into column 5 and the
    -- $86 dashed in after.)
    if ride.walkerEta and haveBomb then
      local cut = ride.pockets(g)
      if cut[here] then
        local op = {}
        for rr = math.max(0, r - 8), math.min(MAP_H - 1, r + 8) do
          for cc = math.max(0, c - 8), math.min(MAP_W - 1, c + 8) do
            local t, k = g[rr][cc], rr * KEY_W + cc
            if SOFT[t] and not Breaking[k] then
              local near = false
              for _, d in ipairs(DIRS) do if cut[(rr + d[2]) * KEY_W + cc + d[1]] then near = true end end
              if near then
                g[rr][cc] = 0; ride.pocketG = nil
                if not ride.pockets(g)[here] then op[k] = true end
                g[rr][cc] = t
              end
            end
          end
        end
        ride.pocketG = nil
        if os.getenv("SBM2_DBG") then local t = {}; for k in pairs(op) do t[#t + 1] = string.format("(%d,%d)", k % KEY_W, k // KEY_W) end; log(string.format("%6d dbg openers %s", frame, table.concat(t, " "))) end
        if next(op) then ride.openers = op end
      end
    end
    local why = haveBomb and targetsInReach(c, r, 0)
    if why == "enemy" and frame < ambushHold then why = nil end
    -- (not on a $8C's pull line: fleeing its blast, the pull holds P1 back
    -- and the escape timing is off; run 17's 1-6 bombed one from below)
    if why and PullLine[here] then why = nil end
    -- At speed 1, a skate lying within 4 steps is picked up before any bomb
    -- (1-6: P1 bombed a block beside the skate it was walking to; a magnet
    -- pulled the bomb onto the skate and the blast destroyed it). (For
    -- every useful item it cost 1-5 10 of 18.)
    if why and next(skateNow) then
      local _, n = bfs(g, c, r, function(cc, rr) return skateNow[rr * KEY_W + cc] end, nearEnemy, cw)
      if n and n <= 4 then why = nil end
    end
    -- A shield: in a pocket with an enemy no bomb can hurt (3-2's $8F
    -- before the last crystal) coming, a bomb here stops it (enemies turn
    -- at bombs) while P1 waits out of the blast. (3-2, unaided: the
    -- springs land P1 at (13,11), the closed east end of row 11, with an
    -- $8F walking it; it waited at (13,10) and was cornered, 3 seeds of 4.)
    -- (only hp-$7F immune ones, not a hurt one flashing, and not with the
    -- remote: 3-1's shields then sat unset and the stage timed out)
    if haveBomb and not why and not remoteOn and not danger[here] and ride.walkerEta and ride.pockets(g)[here] then
      local immune = false
      for _, e in ipairs(elist) do
        if e.harmful and e.immune and not e.gadget and math.abs(e.c - c) + math.abs(e.r - r) <= 6 then immune = true end
      end
      local eh = immune and ride.walkerEta()[here]
      -- (rarely: one at a time, 300 frames apart; 144 shields in 3-2 kept
      -- P1 fleeing its own blasts until time-up)
      if eh and eh > 30 and eh < 120 and #lastBombs == 0 and frame - (ride.shieldAt or -1000) > 300 then why = "shield" end
    end
    if os.getenv("SBM2_DBG") and frame % 60 == 0 then
      log(string.format("%6d dbg here (%d,%d) haveBomb=%s why=%s esc=%s left=%d remote=%s", frame, c, r, tostring(haveBomb),
        tostring(why), tostring(why and canEscape(c, r, 0)), emu.read(P1 + P_BOMBS_LEFT, WRAM, false), tostring(next(remoteCells) ~= nil)))
    end
    if why and canEscape(c, r, 0) then
      if os.getenv("SBM2_DBG") then
        local es = {}
        for _, e in ipairs(elist) do es[#es + 1] = string.format("%02X(%d,%d)%s%s cpf%.1f", e.type, e.c, e.r, e.chaser and "C" or "", e.harmful and "h" or "", e.cpf) end
        log(string.format("%6d dbg bomb here (%d,%d) why=%s enemies %s", frame, c, r, why, table.concat(es, " ")))
      end
      plan, bombWhy = "bomb", why
      if why == "shield" then ride.shieldAt = frame end
      if pushCell == here then
        ride.planBomb = here  -- the planner's bomb (remote: set it off soon)
        -- (a planned bomb is progress: no stall-explore right after it.
        -- 2-4, run 26: bombed (13,9) open from (13,8) for the (13,12)
        -- panel, then the stall check sent P1 exploring for 600 frames
        -- while the block grew back)
        progressAt = frame
      end
      if os.getenv("SBM2_DBG") and why == "obj" and lastPlan and not lastPlan:find("^bomb") then
        local t = {}
        for _, cell in ipairs(blastCells(g, c, r, rangeAt(g, c, r))) do
          local k = cell[2] * KEY_W + cell[1]
          t[#t + 1] = string.format("(%d,%d)%X%s%s", cell[1], cell[2], g[cell[2]][cell[1]], panelTodo[k] and "T" or "", panelDone[k] and "D" or "")
        end
        log(string.format("%6d dbg bomb cells %s plan=%s push=%s", frame, table.concat(t, " "), tostring(panelPlan ~= nil), tostring(pushCell)))
      end
      if why == "obj" and pushCell ~= here then lastObjFrame = frame end
      if not bombTry or bombTry[1] ~= here then bombTry = { here, frame }
      elseif frame - bombTry[2] > 20 then
        noBombUntil[here] = frame + 600
        -- Twice on the same kind of tile (lit panels $70): never plan bombs there.
        if emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and g[r][c] ~= 0x00 then
          noBombTile[g[r][c]] = (noBombTile[g[r][c]] or 0) + 1
        end
        local bs = {}
        for _, b in ipairs(lastBombs) do bs[#bs + 1] = string.format("(%d,%d)t%d%s", b[1], b[2], b[3], b[5] and "r" or "") end
        log(string.format("%6d no bomb appears at (%d,%d); avoiding it; left %d bombs %s", frame, c, r,
          emu.read(P1 + P_BOMBS_LEFT, WRAM, false), table.concat(bs, " ")))
        bombTry = nil
      end
      if why == "enemy" and not ambush then ambushStart = true end
      -- Bomb placement is edge-triggered: press on alternate frames.
      input.a = frame % 2 == 0
    elseif why == "obj" and not danger[here] and not nearEnemy[here]
        and (not ride.hold or ride.hold[1] ~= here or frame - ride.hold[2] < 90)
        and (canEscape(c, r, 15) or canEscape(c, r, 30) or canEscape(c, r, 60)) then
      -- The bomb here is out a little later (an older blast still in the
      -- way): stand by. (2-2, every seed: the seek, timed for arrival,
      -- sent P1 to (5,9) for the start-pocket opener; there it was too
      -- early, the seek sent it on to (5,7), and back, until an $86 came.)
      if not ride.hold or ride.hold[1] ~= here then ride.hold = { here, frame } end
      plan, dir = "hold", nil
    elseif remoteCells[here] and (remoteWorth or not haveBomb)
        and not (finalForm2() and (r16(P1 + 0x52) & 6) ~= 0) then
      -- (holding a bomb, B sets nothing off: the remote one waits; walking
      -- out of its blast and back for 500 frames was 5-8 form 2's stall)
      plan = "clear"
      dir = bfs(g, c, r, function(cc, rr, n) return not remoteCells[rr * KEY_W + cc] and not windows[rr * KEY_W + cc]
          and walkerSafe(cc, rr, n) end, nearEnemy, cw)
        or bfs(g, c, r, function(cc, rr) return not remoteCells[rr * KEY_W + cc] and not windows[rr * KEY_W + cc] end,
        nearEnemy, cw)
        or bfs(g, c, r, function(cc, rr) return not remoteCells[rr * KEY_W + cc] end, nil, windows)
      -- Sealed in by our own remote bomb: setting it off is a death, so go
      -- back as after one rather than stand here until the stall check.
      if dir then trappedSince = nil
      elseif not trappedSince then trappedSince = frame
      elseif frame - trappedSince > 120 and timeLeft() > 1 then
        log(string.format("%6d trapped by own remote bomb at (%d,%d); rolling back", frame, c, r))
        rollback, trappedSince, rollbackBefore = true, nil, (remoteSince or frame) - 10
      end
    else
      -- Soft blocks grow back (world 2, 5-5): a cell behind one blown open
      -- is a trap unless, with every blown block back, its area still has a
      -- cell where a bomb (not on a bit-6 tile) breaks a soft block and P1
      -- can step out of the blast. (2-4, run 18: hunted a $82 down the
      -- right-hand column; (13,9) grew back and the three cells left were
      -- all in reach of any bomb at range 9.)
      local sealCache, gridR = {}, nil
      local stg = emu.read(0x1A, WRAM, false)
      local regrow = (stg & 0xF0) == 0x20 or stg == 0x54
      local function sealOK(cc, rr)
        if not regrow then return true end
        if not gridR then
          gridR = {}
          for r2 = 0, MAP_H - 1 do gridR[r2] = g[r2] end
          for k in pairs(softSeen) do
            local r2, c2 = k // KEY_W, k % KEY_W
            if not SOFT[gridR[r2][c2]] and gridR[r2][c2] < 0x40 then
              if gridR[r2] == g[r2] then local row = {}; for c3 = 0, MAP_W - 1 do row[c3] = g[r2][c3] end; gridR[r2] = row end
              gridR[r2][c2] = 0xC2
            end
          end
        end
        if not walkable(gridR, cc, rr) then return true end  -- a blown cell itself: judged by where it leads
        local reg = regionOf(gridR, cc, rr)
        if reg[r * KEY_W + c] then return true end
        local minK = math.huge
        for k in pairs(reg) do if k < minK then minK = k end end
        if sealCache[minK] ~= nil then return sealCache[minK] end
        local ok = false
        for k in pairs(reg) do
          local bc, br = k % KEY_W, k // KEY_W
          local t = gridR[br][bc]
          if t < 0x40 then
            local opens, blast = false, {}
            for _, cell in ipairs(blastCells(gridR, bc, br, rangeAt(gridR, bc, br))) do
              blast[cell[2] * KEY_W + cell[1]] = true
              if SOFT[gridR[cell[2]][cell[1]]] then opens = true end
            end
            if opens then
              for k2 in pairs(reg) do if not blast[k2] then ok = true; break end end
            end
          end
          if ok then break end
        end
        sealCache[minK] = ok
        return ok
      end
      -- An open, unblocked gate is the way out whatever is still alive
      -- (PlayerOnExitTile C1:41A8 checks only Exit_Blocked and the panels;
      -- 2-1's furnace never stops making enemies).
      -- A heart or glove lying in reach comes before the exit.
      plan = "item"
      dir = bfs(g, c, r, function(cc, rr)
        local k = rr * KEY_W + cc
        return g[rr][cc] == ITEM and (heartNow[k] == "item" or usefulNow[k])
      end, nearEnemy, cw)
      if not dir and gateOpen and r16(0x5D70) < 0x8000 and not gloveHidden then
        plan = "exit"
        dir = bfs(g, c, r, function(cc, rr) return g[rr][cc] == GATE_OPEN end, nearEnemy, cw)
      end
      if not dir then
        plan = "item"
        dir = bfs(g, c, r, function(cc, rr) return g[rr][cc] == ITEM end, nearEnemy, cw)
      end
      if not dir and haveBomb then
        -- Objectives (blocks, panels) first; enemy-only spots after.
        plan = "seek"
        -- Soft blocks hiding a useful item (list $7E3C48; kind bit 14 set =
        -- already out): speed (4: +$E0, the skate; run 7 met the 1-8 boss
        -- at walking speed 1 and could not keep in the gap), heart (11),
        -- glove (7), remote (1), bomb+ (0), fire+ (2). Opened first.
        local itemBlk = {}
        for i = 0, 63 do
          local cell = r16(ITEMLIST + 4 * i)
          if cell >= 0x8000 then break end
          local kind = r16(ITEMLIST + 4 * i + 2)
          local kd = kind & 0x3FFF
          if (kind & 0x4000) == 0 and (kd == 4 or kd == 11 or kd == 7 or kd == 1 or kd == 0 or kd == 2) then
            local ic, ir = (cell & 0x7F) // 2, cell >> 7
            if g[ir] and SOFT[g[ir][ic]] then itemBlk[ir * KEY_W + ic] = true end
          end
        end
        -- The stage planner's bomb step goes first: as one "obj" among the
        -- rest it waited behind every nearer block and enemy until it timed
        -- out (1-2, unaided: the panel bomb at (24,7), planned twice, never
        -- laid; a time-up in 2 of 3 seeds).
        -- (off unless SBM2_PLANSTEP=1: from 03289bd (11-09) to here the
        -- switch check below sat inside this block and reset dir, so every
        -- scenario since passed without it; on, 2-1_r80b dithered at (7,4)/
        -- (8,4) and timed out)
        if ride.PLANSTEP == nil then ride.PLANSTEP = os.getenv("SBM2_PLANSTEP") == "1" end
        if ride.PLANSTEP and not ride.sw51 and ride.planCell and ride.planCell ~= here and planSteps then
          local T = ride.planCell
          dir = bfs(g, c, r, function(cc, rr, n)
            return rr * KEY_W + cc == T and canEscape(cc, rr, n * FPT)
          end, nearEnemy, cw)
          if dir then plan = "planstep" end
          -- (no way now, but one past a $DD's cell / the track rule: follow
          -- that one while its next cell is open, else wait for the block to
          -- move on; 4-2, unaided, P1 left the column-8 crossing every time)
          if not dir and canEscape(T % KEY_W, T // KEY_W, 0) then
            local rb, ar, av = RideBlock, allowRide, AvoidCell
            RideBlock, allowRide, AvoidCell = {}, true, {}
            local g2 = g
            for rr2 = 0, MAP_H - 1 do
              for cc2 = 0, MAP_W - 1 do
                if g[rr2][cc2] == 0xCD then
                  if g2 == g then g2 = {}; for r3 = 0, MAP_H - 1 do g2[r3] = g[r3] end end
                  if g2[rr2] == g[rr2] then local row = {}; for c3 = 0, MAP_W - 1 do row[c3] = g[rr2][c3] end; g2[rr2] = row end
                  g2[rr2][cc2] = 0x4D
                end
              end
            end
            -- (enemies are not walls here: the step itself waits for a cell
            -- an enemy is entering; with them as walls a roaming $93 made
            -- the long ride back the only route, and P1 shuttled)
            local d2 = bfs(g2, c, r, function(cc, rr) return rr * KEY_W + cc == T end, nil, nil)
            RideBlock, allowRide, AvoidCell = rb, ar, av
            if d2 then
              local dv = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[d2:match("^%a+") or ""]
              if d2 == "ride" or (dv and walkable(g, c + dv[1], r + dv[2]) and not danger[(r + dv[2]) * KEY_W + c + dv[1]]) then dir = d2
              else dir = nil end
              plan = "planstep"
            end
          end
        end
        -- (5-1's light switch steps; this check had slipped inside the one
        -- above, where ride.sw51 is always nil: never walked to, 5-1 timed
        -- out with the $8As fast chasers, runs RB s0 and r74 reruns)
        if not dir and ride.sw51 and ride.sw51 ~= here then
          local T = ride.sw51
          dir = bfs(g, c, r, function(cc, rr, n)
            return rr * KEY_W + cc == T and canEscape(cc, rr, n * FPT)
          end, nearEnemy, cw)
          if dir then plan = "switch51" end
        end
        if plan == "planstep" and dir then wants = {} end
        local wants = next(itemBlk) and { "item", "obj", "enemy" } or { "obj", "enemy" }
        if ride.openers then table.insert(wants, 1, "open") end
        if dir then wants = {} end
        for _, want in ipairs(wants) do
          local goalK
          dir = bfs(g, c, r, function(cc, rr, n)
            if (cc == c and rr == r) or danger[rr * KEY_W + cc] or (want == "enemy" and not sealOK(cc, rr)) then return false end
            if want == "obj" and ride.forcedPlan and planSteps and pushCell and rr * KEY_W + cc ~= pushCell then return false end
            local why = targetsInReach(cc, rr, n * FPT)
            if want == "item" or want == "open" then
              if why ~= "obj" then return false end
              local set = want == "item" and itemBlk or ride.openers
              local hits = false
              for _, cell in ipairs(blastCells(g, cc, rr, rangeAt(g, cc, rr))) do
                if set[cell[2] * KEY_W + cell[1]] then hits = true; break end
              end
              if not hits then return false end
              why = want
            end
            local ok = why == want and (want ~= "enemy" or frame >= ambushHold)
              and canEscape(cc, rr, n * FPT)
            if ok then goalK = rr * KEY_W + cc end
            return ok
          end, nearEnemy, cw)
          if dir then
            if os.getenv("SBM2_DBG") and goalK then log(string.format("%6d dbg seek %s at (%d,%d)", frame, want, goalK % KEY_W, goalK // KEY_W)) end
            -- A planned step is not a target of its own: the planner keeps
            -- going after it.
            if (want == "obj" or want == "open") and goalK ~= pushCell then lastObjFrame = frame end
            break
          end
          -- The planned trap comes before bombing for enemies.
          if want == "obj" and trapCell then break end
        end
      end
      -- The planner's next step is a $63 trap: walk next to it, then onto it.
      if not dir and trapCell then
        local tc, tr = trapCell % KEY_W, trapCell // KEY_W
        if g[tr][tc] ~= 0x63 then
          trapCell, pushNext = nil, frame + 30
        elseif math.abs(tc - c) + math.abs(tr - r) == 1 then
          if not danger[trapCell] and not nearEnemy[trapCell] then
            plan, dir = "trap", (tc > c and "right") or (tc < c and "left") or (tr > r and "down") or "up"
          end
        else
          dir = bfs(g, c, r, function(cc, rr) return math.abs(tc - cc) + math.abs(tr - rr) == 1 end, nearEnemy, cw)
          if dir then plan = "trap" end
        end
      end
      if not dir and trapCell and plan ~= "trap" and haveBomb then
        plan = "seek"
        dir = bfs(g, c, r, function(cc, rr, n)
          if (cc == c and rr == r) or danger[rr * KEY_W + cc] then return false end
          return targetsInReach(cc, rr, n * FPT) == "enemy" and frame >= ambushHold and canEscape(cc, rr, n * FPT)
        end, nearEnemy, cw)
      end
      if not dir and gateOpen and #walkers == 0 and not gloveHidden then
        plan = "exit"
        dir = bfs(g, c, r, function(cc, rr) return g[rr][cc] == GATE_OPEN end, nil, cw)
          or bfs(g, c, r, function(cc, rr) return g[rr][cc] == GATE_OPEN end, nil, windows)
      end
      if not dir and haveBomb and emu.read(0x1A, WRAM, false) == 0x57 and not finalForm2()
          and (r16(0x1000) | (emu.read(0x1002, WRAM, false) << 16)) == 0xCCC8D6 then
        -- 5-8 form 1 patrolling: lure a slam (docs/enemies.md 7.3). P1 under
        -- the body sets it off; it lands at y 152 with its contact box
        -- ending at y 187, short of row 12 (P1 at y 199), and its sense
        -- block then reaches row 10. So wait in row 12 where the body will
        -- pass, in a column whose row 11 is open (even, not 6 or 10): the
        -- bomb dropped there when the slam starts (targetsInReach) blasts
        -- up into the block, and the remote sets it off.
        -- The body has to pass over P1 (x within -16..+15); it sweeps x
        -- 59..196, so (8,12) (x 128) is passed both ways every sweep.
        local want = 8
        plan = "slam"
        if c ~= want or r ~= 12 then
          dir = bfs(g, c, r, function(cc, rr) return cc == want and rr == 12 end, nearEnemy, cw)
        end
      end
      if not dir and plan ~= "slam" and #walkers > 0 and haveBomb then
        -- Nothing to bomb from here: close in on the nearest enemy.
        plan = "hunt"
        -- (first a junction, 3+ open sides: 2-4, run 21, waited for a $82 in
        -- the row-4 corridor, then backed into the (3,3) pocket ahead of it)
        -- (with a chaser faster than P1 about, only junctions: 5-1, runs
        -- 31-34, waited for a 1.25 px/f $8A at a pocket's mouth and was run
        -- down in the pocket, again and again)
        local fast = false
        for _, e in ipairs(elist) do if e.chaser and e.harmful and e.cpf < FPT then fast = true end end
        -- (not the $8F while a crystal is unlit: no bomb hurts them, and
        -- 3-2, unaided, followed them through pads and springs into the
        -- (13,11)/(17,11) pockets at the ends of row 11)
        local immune8F = {}
        -- (only one in a pocket: in 3-1 closing in on them pays)
        if (ride.crystals or 0) > 0 then
          local cut = ride.pockets(g)
          for _, e in ipairs(elist) do
            if e.type == 0x8F and cut[e.r * KEY_W + e.c] then immune8F[e.r * KEY_W + e.c] = true end
          end
        end
        for pass = 1, fast and 1 or 2 do
          local hn
          dir, hn = bfs(g, c, r, function(cc, rr)
            if not sealOK(cc, rr) or FireLine[rr * KEY_W + cc] then return false end
            if pass == 1 then
              local open = 0
              for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then open = open + 1 end end
              if open < 3 then return false end
            end
            for _, e in ipairs(walkers) do
              if math.abs(e[1] - cc) + math.abs(e[2] - rr) <= 3 and not immune8F[e[2] * KEY_W + e[1]] then return true end
            end
            return false
          end, nearEnemy, cw)
          if dir or hn then
            -- (already on a hunt goal: wait for the enemy here rather than
            -- go pad-exploring; 3-6 flipped hunt/spring at (10,3)/(10,4))
            if not dir and hn then plan = "wait"; ride.huntHere = frame end
            break
          end
        end
      end
      -- (not with a remote bomb out: set it off first, standing still. 3-6,
      -- run 27: the press came as P1 landed off a pad and did nothing; with
      -- no bomb free it went pad-exploring, back to the same landing, for
      -- 20000 frames)
      if not dir and plan ~= "slam" and next(remoteCells) and not remoteCells[here] and g[r][c] ~= SPRING then
        plan = "wait"
      elseif not dir and plan ~= "slam" and ride.huntHere ~= frame then
        -- Nothing left on this side: try a spring jump not made yet. Where
        -- it lands (or that it drowns: rollback) is learnt, so the planner
        -- can then route over the spring network (3-1, 3-6).
        springTry = nil
        dir = bfs(g, c, r, function(cc, rr)
          for _, d in ipairs(DIRS) do
            local pc, pr = cc + d[1], rr + d[2]
            if g[pr] and g[pr][pc] == SPRING then
              local pk = pr * KEY_W + pc
              for _, d2 in ipairs(DIRS) do
                for _, v in ipairs({ "", "!" }) do
                  -- (not one the model expects to drown: a death costs a rollback)
                  if springLand[pk .. d2[3] .. v] == nil and ride.springGuess(g, pc, pr, d2, v == "!") then
                    springTry = { pk, d2[3] .. v, pc, pr }; return true
                  end
                end
              end
            end
          end
          return false
        end, nearEnemy, cw)
        if springTry then
          plan = "spring"
          if not dir then  -- next to the pad: step onto it
            local dc, dr = springTry[3] - c, springTry[4] - r
            dir = (dc == 1 and "right") or (dc == -1 and "left") or (dr == 1 and "down") or "up"
          end
        end
      end
      if not dir and plan ~= "slam" then
        -- Still nothing: push into a gadget object parked on a walkable
        -- cell next to the region (3-5's subs dock at stairs and take P1
        -- across the water; a ride seen becomes a route, ride.edges). Each
        -- cell and direction once per stage, for up to 90 frames.
        ride.tried = ride.tried or {}
        local try = ride.try
        -- (a tried cell may be tried again 1500 frames later: 3-5's subs come
        -- and go, and unaided seed 1 spent its few tries while they were away,
        -- then never boarded and the island's panels timed the stage out)
        if try and (frame - try[3] > 90 or ride.from) then ride.tried[try[1] .. try[2]] = frame; ride.try, try = nil, nil end
        if try and try[1] == here then
          plan, dir, step = "board", nil, nil
          input[try[2]] = true
        else
          local want
          dir = bfs(g, c, r, function(cc, rr)
            for _, e in ipairs(elist) do
              -- Known rides only: 3-5's subs $DA/$DB, world 4's $DC platform
              -- (pushing into $E1 or world 5's $C0 kills: 3-2, 2-8).
              if (e.type == 0xDA or e.type == 0xDB or e.type == 0xDC) and walkable(g, e.c, e.r) then
                for _, d in ipairs(DIRS) do
                  local tk = ride.tried[(rr * KEY_W + cc) .. d[3]]
                  if cc + d[1] == e.c and rr + d[2] == e.r and not (tk and (tk == true or frame - tk < 1500)) then
                    want = { rr * KEY_W + cc, d[3] }; return true
                  end
                end
              end
            end
            return false
          end, nearEnemy, cw)
          if want then
            plan = "board"
            if want[1] == here then ride.try = { want[1], want[2], frame }; input[want[2]] = true; dir, step = nil, nil end
          end
        end
      end
      -- (a hunt or seek, every other 1500 frames, once lane ends by
      -- enemies are rides again, is no local wandering: 4-5's $93s were across the (9,6) ride
      -- and exploring held P1 on the far side)
      if frame < stallUntil and plan ~= "bomb" and plan ~= "spring" and plan ~= "trap" and plan ~= "board"
          and not (ride.relax and (plan == "hunt" or plan == "seek") and (dir == "ride" or (frame // 1500) % 2 == 0)) then dir = nil end
      if not dir and frame < exploreUntil and plan ~= "board" and plan ~= "slam" then
        -- After a death or a long stall: walk toward the least visited
        -- neighbour (random tie-break) and sometimes drop a bomb.
        plan = "explore"
        local best, opts = math.huge, {}
        for _, d in ipairs(DIRS) do
          local k = (r + d[2]) * KEY_W + c + d[1]
          if walkable(g, c + d[1], r + d[2]) and not danger[k] and not nearEnemy[k] then
            local v = visits[k] or 0
            if v < best then best, opts = v, { d[3] } elseif v == best then opts[#opts + 1] = d[3] end
          end
        end
        if #opts > 0 then dir = opts[math.random(#opts)] end
        -- (only with a way on and out: dropped in a dead end it was a death,
        -- 3-6's left side)
        if dir and haveBomb and not remoteOn and math.random() < 0.1 and canEscape(c, r, 0) then input.a = true end
      end
      if not dir and plan ~= "board" and plan ~= "slam" then plan = "wait" end
    end
  end

  -- 1-8: the only safe place under the body is the gap between its hands
  -- (+-11 px of its x, below the lane), and it slides with the body at
  -- 0.3-0.7 px/f: route steps of a cell can't follow it. Shadow it in the
  -- open rows (8, 10, 12), steering x onto the body's every frame.
  -- Attack from there: set a bomb where P1 stands when the body, by its
  -- path (it turns at the arena edges), will be 14-36 px from that column
  -- when it blows (its sense grid is +-32: a hit), then leave the bomb's
  -- row at the next other even column (2 rows down, or up from row 12) and
  -- keep shadowing 8 px off the body on the side away from the column.
  if emu.read(0x1A, WRAM, false) == 0x17 then
    local body
    for _, b in ipairs(bosses) do if b.base == 0x1000 or b.base == 0x1140 then body = body or b end end
    local sb = ride.sb
    if sb and frame - sb.at > NEW_FUSE + 60 then ride.sb, sb = nil, nil end
    -- (the pad is read every other frame: a one-frame A press was lost half
    -- the time, and P1 then left "its bomb" for a whole fuse; press 2
    -- frames, and forget a bomb that never appeared)
    if sb and frame - sb.at <= 1 then input.a = true end
    if sb and frame - sb.at >= 8 then
      local seen, boom = false, false
      for _, b in ipairs(lastBombs or {}) do
        if b[1] == sb.c and b[2] == sb.r then seen = true; boom = b[3] == 0 end
      end
      if not seen and not sb.seen then ride.sb, sb = nil, nil
      else
        -- (how long our bombs take here, placed to blast: 150 frames in one
        -- 1-8 start, 205 in another; the timing uses the last one)
        if boom and not sb.timed then sb.timed = true; ride.fuse18 = frame - sb.at end
        sb.seen = true
      end
    end
    local ox = origin()
    local function colX(cc) return cc * 16 + 8 - ox end
    -- P1's walking speed, px/f, from its moves along the rows here
    if ride.lastX and ride.lastXf == frame - 1 and ride.shadowDir and math.abs(x - ride.lastX) > 0 and math.abs(x - ride.lastX) <= 3 then
      ride.vp = (ride.vp or 1) * 0.9 + math.abs(x - ride.lastX) * 0.1
    end
    ride.lastX, ride.lastXf = x, frame
    if body and r >= 8 and math.abs(x - body.x) <= 18 and (sb or (plan ~= "flee" and plan ~= "bomb")) then
      local tx = (Boss.predict(body, 4) or body.x)
      -- (with a bomb out: hold the even column 2 over, on the body's side;
      -- the body drifts toward P1 and so over the bomb's sense reach,
      -- 32 px off its column, with P1 outside the column)
      if sb then tx = tx + ((body.x >= colX(sb.c)) and 8 or -8) end
      -- (still in the bomb's row: make for the nearest even column off the
      -- bomb's, on the body's side, to go down or up there; following a
      -- body that had stopped kept P1 on an odd column in the row)
      if sb and r == sb.r then
        local side = (body.x >= colX(sb.c)) and 1 or -1
        local ec = sb.c + 2 * side
        if math.abs(c - sb.c) >= 2 and c % 2 == 0 then ec = c end
        -- (only inside the gap: 32 px off the body is a hand's zone)
        -- (or anywhere while it is hurt, CC:9CA7: the hands hang at rest)
        local hurt = (r16(0x1000) | (emu.read(0x1002, WRAM, false) << 16)) == 0xCC9CA7
        if hurt or math.abs(colX(ec) - body.x) <= 10 then tx = colX(ec) end
      end
      if not sb and r % 2 == 0 and r - RANGE <= 6 and c % 2 == 0 and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and math.abs(x - colX(c)) <= 3 then
        -- (strikes P1 sets off by lagging count: Boss.sunX with P1's speed)
        -- (with the fuse measured, ~215 frames in both 1-8 starts seen: the
        -- body inside its +-32 sense reach of the column; until then the
        -- old 150-frame / 14-36 px rule, tuned to hit at that real fuse)
        local bx = Boss.sunX(ride.fuse18 or NEW_FUSE, x, ride.vp or 1)
        local lo, hi = 14, 36
        if ride.fuse18 then lo, hi = 0, 28 end
        if bx and math.abs(bx - colX(c)) >= lo and math.abs(bx - colX(c)) <= hi then
          ride.sb = { c = c, r = r, at = frame }
          sb = ride.sb
          input.a = true
          log(string.format("%6d shadow bomb at (%d,%d), body at x %.0f, expect x %.0f at %d (plain %s; P1 %.2f px/f, cool %d, face %02X, move %d)", frame, c, r, body.x, bx, ride.fuse18 or NEW_FUSE, tostring(Boss.sunX(ride.fuse18 or NEW_FUSE)), ride.vp or 1, r16(0x1038), emu.read(0x100F, WRAM, false), r16(0x1034)))
        end
      end
      local nd, vd
      if sb and r == sb.r and c ~= sb.c and c % 2 == 0 and math.abs(x - colX(c)) <= 3 then
        vd = (r < 12) and "down" or "up"  -- leave the bomb's row here
      end
      -- No bomb out and too low for P1's blast to reach the body's sense
      -- rows (row - RANGE <= 6): climb at an even column to the lowest row
      -- that does (fullNR13: left a bomb's row down to 12 at RANGE 5 and
      -- shadowed there 240000 frames without another bomb).
      local rt = 6 + RANGE
      rt = rt - rt % 2
      local climb = not sb and rt >= 8 and r > rt
      if climb and r % 2 == 0 and c % 2 == 0 and math.abs(x - colX(c)) <= 3 then vd = "up" end
      if not vd then
        if x < tx - 3 then nd = "right" elseif x > tx + 3 then nd = "left" end
      end
      if r % 2 == 0 or vd or sb or climb then
        plan, dir, step = "shadow", nil, nil
        ride.shadowDir, ride.shadowV = nd, vd
      end
    end
  end
  -- Boss fights: don't wait in a cell with under 3 open sides; go to a safe
  -- junction (2-8: waited at (3,4) on the arena edge; a drop's column and
  -- the body's glide closed both its sides).
  if next(bossNow) and plan == "wait" and not stageNow17 then
    local function open(cc, rr)
      local n = 0
      for _, d in ipairs(DIRS) do if walkable(g, cc + d[1], rr + d[2]) then n = n + 1 end end
      return n
    end
    if open(c, r) < 3 then
      local sg = safeGoal(windows)
      local d2, n = bfs(g, c, r, function(cc, rr, m) return sg(cc, rr, m) and open(cc, rr) >= 3 end, nearEnemy, cw)
      if d2 and n <= 3 then plan, dir, step = "junction", d2, nil end
    end
  end
  -- Waiting where a chaser can walk within 150 frames: move to where P1's
  -- lead on it is 30 frames longer and no blast comes for the walk + 90
  -- frames (the flee logic moves P1 again before a later one). (2-4, run
  -- 18: waited in the pocket at (3,3) for 100 frames while a $82 came up,
  -- then dodged into it.)
  -- (in a dead end, one open side, any walker counts: 2-4, run 18, sat in
  -- the corner at (3,3) for 100 frames until a $82 wandered in)
  local deadEnd = 0
  for _, d in ipairs(DIRS) do if walkable(g, c + d[1], r + d[2]) then deadEnd = deadEnd + 1 end end
  local etaF = deadEnd <= 1 and ride.walkerEta or nil
  if etaF and plan == "wait" then
    local eta = etaF()
    local eh = eta[here]
    if eh and eh < 150 then
      local d2 = bfs(g, c, r, function(cc, rr, n)
        local k = rr * KEY_W + cc
        local e = eta[k]
        return walkable(g, cc, rr) and not hit(windows, k, 0, n * FPT + 90) and not PullLine[k]
          and not (MagLine and MagLine[k]) and not hit(eWin, k, n * FPT - FPT, n * FPT + 60)
          and (not e or (e - n * FPT > 16 and e > eh + 16))
      end, nearEnemy, cw)
      if d2 then plan, dir, step = "evade", d2, nil end
    end
  end
  -- Nor in a $90's line of fire (see FireLine).
  if FireLine[here] and (not dir or plan == "wait") and plan ~= "bomb" then
    local d2 = bfs(g, c, r, safeGoal(windows), nearEnemy, cw)
    if d2 then plan, dir, step = "offfire", d2, nil end
  end
  -- Never stand on a $8C's pull line (see PullLine): walk off it.
  if PullLine[here] and (not dir or plan == "wait") and plan ~= "bomb" then
    local d2 = bfs(g, c, r, safeGoal(windows), nearEnemy, cw)
      or bfs(g, c, r, function(cc, rr)
        local k = rr * KEY_W + cc
        return not PullLine[k] and not remoteCells[k] and not windows[k]
      end, nearEnemy, cw)
    if d2 then plan, dir, step = "offpull", d2, nil end
  end
  -- Off a $DD's track when not going anywhere (4-2: waiting or exploring
  -- on the track, the block pushed P1 into its own blast, twice).
  if ride.hasDD and (g[r][c] == 0x4D or g[r][c] == 0xCD)
      and (not dir or plan == "wait" or plan == "explore") then
    local d2 = bfs(g, c, r, safeGoal(windows), nearEnemy, cw)
    if d2 then plan, dir, step = "offtrack", d2, nil end
  end
  -- (a ride wait sticks while nothing threatens here: 4-3, unaided, P1
  -- left the (3,18) lane end every few frames for nearer targets and the
  -- planned bomb beyond the lane timed out with the stage)
  if dir ~= "ride" and ride.wait and ride.wait[1] == here and frame - ride.wait[2] < (ride.waitMax or 600)
      and not danger[here] and not nearEnemy[here] and plan ~= "flee" and plan ~= "dodge" and plan ~= "bomb" then
    dir = "ride"
  end
  -- A ride simulated: board (held 60/120/200 frames), then each way or
  -- none on landing; the first that lives (heart losses count) and ends
  -- nearer the far end than here. Returns that pad sequence and its name.
  local function boardSim(bc, br, ln, bd)
    local list, desc = {}, {}
    local pb = emu.padBits({ [bd] = true })
    -- (640 frames: 4-5's row-6 ride takes ~380 after a pause)
    for _, T in ipairs({ 60, 120, 240, 480 }) do
      -- (walking on off the far end first: "then none" rides back and
      -- forth, which lives but gets nowhere; 4-2 took 8000+ frames)
      -- (and not "none" at all: still aboard when the check ends, the
      -- ride went on; 4-4, run QA s0, "left 60 then none" lived 640
      -- frames, then the platform brought P1 to (2,10) onto an $89)
      local d2s = { bd }
      for _, nm in ipairs({ "up", "down", "left", "right" }) do if nm ~= bd then d2s[#d2s + 1] = nm end end
      for _, d2 in ipairs(d2s) do
        local p2 = emu.padBits(d2 == "none" and {} or { [d2] = true })
        local seq = {}
        for f = 1, 640 do seq[f] = f <= T and pb or p2 end
        list[#list + 1] = seq; desc[#desc + 1] = string.format("%s %d then %s", bd, T, d2)
      end
    end
    -- Bomb-and-back: ride over, bomb on landing, ride back on the same
    -- platform (4-4: the $89 lives in the column-2 pocket, reached only by
    -- the (5,10)->(2,10) ride, and sits by the landing; runs XB/YB s0-s1
    -- were "blocked by an enemy" there until time-up). Taken when it lives
    -- and leaves fewer enemies than standing still.
    local back = ({ up = "down", down = "up", left = "right", right = "left" })[bd]
    local pa, pk = emu.padBits({ a = true }), emu.padBits({ [back] = true })
    local nBomb0 = #list
    local hb = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0
    -- (with the remote the bomb is set off from the ride back, 90 frames on)
    local pkb = remoteOn and emu.padBits({ [back] = true, b = true }) or pk
    if hb then
      for _, T in ipairs({ 140, 180, 220, 260, 320 }) do
        local seq = {}
        for f = 1, 640 do seq[f] = (f <= T and pb) or (f <= T + 3 and pa) or ((f > T + 90 and f <= T + 93) and pkb) or pk end
        list[#list + 1] = seq; desc[#desc + 1] = string.format("%s %d, bomb, then %s", bd, T, back)
      end
      local still = {}
      for f = 1, 640 do still[f] = 0 end
      list[#list + 1] = still; desc[#desc + 1] = "still"
    end
    local res, _, live, ps, rch = emu.simulateMany(list, 640, false, 0)
    if hb then
      local base = live[#list]
      for i = nBomb0 + 1, #list - 1 do
        if res[i] == -1 and base and live[i] and live[i] < base then return list[i], desc[i] .. " (kills)" end
      end
      for i = #list, nBomb0 + 1, -1 do list[i], desc[i], res[i] = nil, nil, nil end
    end
    local ex, ey = ln[1] * 16 + 8, ln[2] * 16 + 8
    local hx, hy = bc * 16 + 8, br * 16 + 8
    -- (one that leaves by the open gate first: the sim ends it as -2, not
    -- -1; 4-2, run PA s0, "ride to (17,4); board search: none" with the
    -- gate (17,3) open above that lane end, until the memory reset)
    for i = 1, #list do if res[i] == -2 then return list[i], desc[i] .. " (exit)" end end
    for i = 1, #list do
      local fx, fy = tostring(ps[i]):match(":(%-?%d+),(%-?%d+)")
      fx, fy = tonumber(fx), tonumber(fy)
      -- (and not walled in at the end: 4-2, run HZ s0 replayed, "left 60
      -- then left" lived, but an $87's soft blocks shut P1 in at (11,10),
      -- a one-cell pocket, until a stall breaker bomb took a heart)
      if res[i] == -1 and fx and math.abs(fx - ex) + math.abs(fy - ey) < math.abs(fx - hx) + math.abs(fy - hy)
          and (not rch or (rch[i] or 99) >= 4) then return list[i], desc[i] end
    end
    return nil, "none"
  end
  -- A step into a waiting $DC from its lane end is a ride too (4-4, run HY
  -- s0: "seek left" from (5,10) walked onto the platform; it carried P1 to
  -- (2,10) as an $89 came there): not unless the simulator finds one that
  -- lives, or a blast is coming.
  if g[r][c] == BOARD and emu.simulateMany and not ride.brk and not danger[here] and not hit(windows, here, 0, 600) then
    for _, ln in ipairs(laneEnds(g, c, r)) do
      local dc, dr = (ln[1] - c) // ln[3], (ln[2] - r) // ln[3]
      local bd = (dc == 1 and "right") or (dc == -1 and "left") or (dr == 1 and "down") or "up"
      if input[bd] or dir == bd or (step and step.tc == c + dc and step.tr == r + dr) then
        for _, e in ipairs(elist) do
          if e.type == 0xDC and e.c == c + dc and e.r == r + dr then
            if frame >= (ride.boardSimNext or 0) then
              ride.boardSimNext = frame + 120
              local seq, why = boardSim(c, r, ln, bd)
              log(string.format("%6d step onto the ride to (%d,%d); board search: %s", frame, ln[1], ln[2], why))
              if seq then ride.brk = { seq = seq, start = frame, H = 640, board = true } end
            end
            if not ride.brk then
              input[bd] = nil
              if dir == bd then dir = nil end
              if step and step.tc == c + dc and step.tr == r + dr then step = nil end
            end
          end
        end
      end
    end
  end
  -- Stepping from the floor onto a lane end where the $DC waits is a ride
  -- too (4-4, scenario r45b: "seek left" from (6,10) onto (5,10), the
  -- platform carried P1 to (2,10) into an $89): simulated first as well.
  -- (not with the gate open: the warp out reads as no life in the sim, and
  -- 4-2_panels never rode (17,8) up to the open gate at (17,3))
  if g[r][c] ~= BOARD and emu.simulateMany and not ride.brk and not danger[here] and not hit(windows, here, 0, 600)
      and not gateOpen then
    for _, d in ipairs(DIRS) do
      local nc, nr, bd = c + d[1], r + d[2], d[3]
      if g[nr] and g[nr][nc] == BOARD and (input[bd] or dir == bd or (step and step.tc == nc and step.tr == nr)) then
        local dc
        for _, e in ipairs(elist) do if e.type == 0xDC and math.abs(e.c - nc) + math.abs(e.r - nr) <= 1 then dc = e end end  -- (at or coming in: a tile off by its x)
        if dc then
          local ln
          for _, l in ipairs(laneEnds(g, nc, nr)) do ln = ln or l end
          if ln and frame >= (ride.boardSimNext or 0) then
            ride.boardSimNext = frame + 60
            local seq, why = boardSim(c, r, ln, bd)
            log(string.format("%6d step onto the waiting ride at (%d,%d) to (%d,%d); board search: %s", frame, nc, nr, ln[1], ln[2], why))
            if seq then ride.brk = { seq = seq, start = frame, H = 640, board = true } end
          end
          if ln and not ride.brk then
            input[bd] = nil
            if dir == bd then dir = nil end
            if step and step.tc == nc and step.tr == nr then step = nil end
          end
        end
      end
    end
  end
  -- A route over a platform lane starts here: wait on the $36 for the ride.
  if dir == "ride" then
    plan, dir, step = "ride", nil, nil
    if not ride.wait or ride.wait[1] ~= here then ride.wait = { here, frame } end
    -- (a $DC on the lane comes back on its own, if slowly: 4-5, run HY s0,
    -- the row-6 platform went home to (19,6) between rides and took longer
    -- than 600 frames to come back for P1 at (9,6); written off, time-up)
    local waitMax = 600
    for _, ln in ipairs(laneEnds(g, c, r)) do
      for _, e in ipairs(elist) do
        if e.type == 0xDC and ((ln[2] == r and e.r == r and (e.c - c) * (ln[1] - e.c) >= 0)
            or (ln[1] == c and e.c == c and (e.r - r) * (ln[2] - e.r) >= 0)) then waitMax = 1500 end
      end
    end
    ride.waitMax = waitMax
    if frame - ride.wait[2] > waitMax then
      for _, e in ipairs(laneEnds(g, c, r)) do ride.fail[here .. ">" .. (e[2] * KEY_W + e[1])] = true end
      ride.edges[here] = nil
      log(string.format("%6d no ride came at (%d,%d); not again", frame, c, r))
      ride.wait = nil
    end
    local tx, ty = tileCentre(c, r)
    if x < tx - 1 then input.right = true elseif x > tx + 1 then input.left = true end
    if y < ty - 1 then input.down = true elseif y > ty + 1 then input.up = true end
    -- Board: step onto the platform while it waits at our end of the lane.
    for _, ln in ipairs(laneEnds(g, c, r)) do
      local dc, dr = (ln[1] - c) // ln[3], (ln[2] - r) // ln[3]
      for _, e in ipairs(elist) do
        local bd = (dc == 1 and "right") or (dc == -1 and "left") or (dr == 1 and "down") or "up"
        -- (with a blast coming here, board whatever waits at the far end:
        -- 4-2, run BH s0, fled its own bomb at (12,8) to the (12,9) lane
        -- end, the $DC came, RideBlock held it back, and the blast came)
        if e.type == 0xDC and e.c == c + dc and e.r == r + dr
            and (danger[here] or hit(windows, here, 0, 600) or (not emu.simulateMany and not RideBlock[ln[2] * KEY_W + ln[1]])) then
          input = { [bd] = true }
        -- (every other ride is simulated first, enemy at the far end or not:
        -- 4-4, run HY s0, rode (5,10) to (2,10) with the far end clear at
        -- boarding; an $89 walked onto it during the ride and took a heart)
        elseif e.type == 0xDC and e.c == c + dc and e.r == r + dr and emu.simulateMany and not ride.brk
            and frame >= (ride.boardSimNext or 0) then
          -- An enemy by the far end (RideBlock) is a matter of when: the
          -- simulator tries the ride (board held 60/120/200 frames) and a
          -- walk off on landing; one that lives and lands is followed.
          -- (4-2, run WS s0: a $93 roams the small middle area by (16,9)
          -- the whole stage; P1 waited beside the parked $DC at (13,9),
          -- "no ride came", and never crossed until time-up.)
          -- (again in 30 frames while it waits beside P1: on arrival it
          -- pauses before going back, past the sim's horizon)
          ride.boardSimNext = frame + 30
          local seq, why = boardSim(c, r, ln, bd)
          log(string.format("%6d ride to (%d,%d)%s; board search: %s", frame, ln[1], ln[2],
            RideBlock[ln[2] * KEY_W + ln[1]] and " blocked by an enemy" or "", why))
          if seq then ride.brk = { seq = seq, start = frame, H = 640, board = true } end
        end
      end
    end
    -- A learned ride boarded by pushing into the object (3-5's subs).
    local jo = jumpObj[here]
    if jo and jo.ride and jo.ride[4] then input = { [jo.ride[4]] = true } end
  else
    ride.wait = nil
  end
  -- Commit to the next tile until its centre is reached, so the plan can't
  -- flip back and forth on a tile boundary.
  if dir then dir = dir:match("^%a+") end  -- a jump's "!" (hold in the air) is for springDir
  ride.lastPlan = plan
  -- (a clear too: bombing mid-step toward (5,4), 2-4's P1 kept the step
  -- and the bomb then stood between it and the way out)
  if dir and not (step and plan ~= "flee" and plan ~= "dodge" and plan ~= "clear") then
    local d = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[dir]
    step = { c = c, r = r, tc = c + d[1], tr = r + d[2], dir = dir }
    if plan == "trap" and g[step.tr] and (g[step.tr][step.tc] == 0x63 or g[step.tr][step.tc] == 0x4B) then
      log(string.format("%6d crossing the trap at (%d,%d) on purpose (hearts %d)", frame, step.tc, step.tr,
        emu.read(P1 + 0xEF, WRAM, false)))
    end
    springDir = g[step.tr] and g[step.tr][step.tc] == SPRING and dir or nil
    if springDir and springTry and springTry[1] == step.tr * KEY_W + step.tc then springDir = springTry[2] end
  end
  if step then
    local tx, ty = tileCentre(step.tc, step.tr)
    -- (a row P1 cannot be centred on: 3-5, the raised box after the stairs
    -- is walked at y 149, 3 px up; Down from (13,9) over the wall only slid
    -- P1 left, Right put it back, for 10k frames. Two up/down presses that
    -- left y as it was make that y the row's line.)
    local yk = step.tr * KEY_W + step.tc
    ride.yLine = ride.yLine or {}
    local cf = ride.cfix
    local _, myOy = origin()
    local my = y + myOy  -- (map y: 1-6 scrolls under a P1 whose y stays put)
    if cf and cf[1] == yk and frame - cf[3] <= 3 then
      if cf[2] == my and not cf[5] then
        cf[5] = true; cf[4] = cf[4] + 1
        if cf[4] >= 2 and ride.yLine[yk] ~= my then
          ride.yLine[yk] = my
          log(string.format("%6d (%d,%d) is walked at y %d (centre %d): taken as centred", frame, step.tc, step.tr, y, ty))
        end
      elseif cf[2] ~= my then
        cf[4] = 0
      end
    end
    if math.abs(x - tx) <= 1 and (math.abs(y - ty) <= 2 or ride.yLine[yk] == my) then
      step = nil
    elseif plan ~= "flee" and (danger[step.tr * KEY_W + step.tc]
        or (plan ~= "dodge" and nearEnemy[step.tr * KEY_W + step.tc])) then
      -- The committed tile became dangerous; replan next frame. A dodge
      -- goes through cells near an enemy on purpose (around a boss, all
      -- of them can be).
      step = nil
      -- 5-8 shield plan stopped by a ball near the next cell: dig in here.
      -- (one remote bomb at a time: two sealed P1 in between them)
      if plan == "shield" and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and not next(remoteCells) then
        input.a = frame % 2 == 0
      end
    else
      -- (P1 frozen 8 frames on a step: press the way itself. 2-8, unaided
      -- seed 2: a step right from (2,4) at y 69 kept pressing Down to line
      -- up, into its own bomb at (2,5), for 140 frames until it went off)
      local sd = steer(step.dir, x, y, step.c, step.r)
      local fz = ride.stepFreeze
      if not fz or fz[1] ~= x or fz[2] ~= y or fz[3] ~= step.tc or fz[4] ~= step.tr then ride.stepFreeze = { x, y, step.tc, step.tr, frame }
      elseif frame - fz[5] >= 8 and sd ~= step.dir then sd = step.dir end
      input[sd] = true
      -- Once on the target tile, keep moving toward its centre.
      if c == step.tc and r == step.tr then
        -- (one axis at a time, y to within 2: P1 rests a row at +7, and
        -- with a wall below, Down+Left held against it moved P1 neither
        -- way; 2-2 sat 6800 frames under the open gate, 2 px off centre)
        -- (the axis of the step's own move first: 3-1, run 26, stepping down
        -- into (12,4) at y 64, Left for x slid P1 back up into (12,3) by
        -- the wall, Down again, every frame for 600 frames)
        input = { a = input.a, b = input.b }
        local vert = step.dir == "up" or step.dir == "down"
        local yOk = ride.yLine[yk] == my
        if vert and not yOk and y < ty - 2 then input.down = true elseif vert and not yOk and y > ty + 2 then input.up = true
        elseif x < tx - 1 then input.right = true elseif x > tx + 1 then input.left = true
        elseif not yOk and y < ty - 2 then input.down = true elseif not yOk and y > ty + 2 then input.up = true end
        if input.down or input.up then ride.cfix = { yk, my, frame, cf and cf[1] == yk and cf[2] == my and frame - cf[3] <= 3 and cf[4] or 0 } end
      end
    end
  end
  -- Form 2 throws (thrown bomb object inside the body's x/y +-24, CC:97E8;
  -- it flies ~52 px in ~14 frames from 16 px ahead of P1).
  if finalForm2() and emu.read(P1 + 0xFE, WRAM, false) > 0 then
    local f52 = r16(P1 + 0x52)
    local holding = (f52 & 0x6) ~= 0
    local body
    for _, b in ipairs(bosses) do if b.base == 0x1000 then body = b end end
    local bx, by = r16(0x1011), r16(0x1014)
    local DV = { up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } }
    for d in pairs(DV) do if input[d] then throw.face = d end end
    -- (hurt, CC:D2E7: a hit then does nothing; hold the bomb for later)
    if body and (body.hurt or body.state == 0xCCD2E7) then body = nil end
    if holding and not body then
      input.a = true
    elseif holding then
      input.a = true
      -- A throw hits if the bomb is inside the body's +-24 box (CC:97E8)
      -- on some frame of its flight; turning first costs ~2 frames.
      local want
      for d, path in pairs(THROW_PATH) do
        local delay = (throw.face == d and throw.faceFrames >= 2) and 0 or 3
        for k, o in ipairs(path) do
          local px, py = bx, by
          if body then
            px, py = Boss.predict(body, k + delay)
            if not Boss.sure(body, px, py) then break end
          end
          if math.abs(x + o[1] - px) <= 22 and math.abs(y + o[2] - py) <= 22 then
            if Boss.throwSafe(x, y, o) then want = d end
            break
          end
        end
        if want then break end
      end
      if want then
        -- (a retried ambush waits a random while: another moment, another try)
        -- (not right after a detonate press: the thrown bomb then went off
        -- on landing, 3 cells from P1)
        if throw.face == want and throw.faceFrames >= 2 and frame >= ambushHold and frame >= nextDetonate and not input.b then
          input.a = nil
          log(string.format("%6d throw %s at boss (%.0f,%.0f) from px(%d,%d)", frame, want, bx, by, x, y))
          throw.at = frame
          local DV2 = { up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } }
          throw.land = { c, r, DV2[want] }
          throw.faceFrames = 0
          -- An ambush: if no hit follows, reload and throw again (retries
          -- cost CPU, not game time).
          if not ambush then ambushStart = true end
        elseif plan ~= "dodge" and plan ~= "flee" then
          -- (turning is a step toward the body: not while dodging it, which
          -- this override turned into walking into it)
          input = { a = true, [want] = true }
          if throw.face == want then throw.faceFrames = throw.faceFrames + 1 else throw.face, throw.faceFrames = want, 1 end
        end
      end
      throw.liftAt = nil
    else
      local dist = math.abs(bx - x) + math.abs(by - y)
      local onBomb = isBomb(g[r][c])
      if onBomb and throw.liftAt and frame - throw.liftAt < 60 then
        -- Our bomb is here: release, then press again to lift it (edge).
        input = { a = frame % 2 == 0 }
      elseif not onBomb and dist >= 40 and dist <= 140 and not danger[here] and not nearEnemy[here]
          and not next(remoteCells) and frame - throw.at > 40
          and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 then
        input.a = frame % 2 == 0
        throw.liftAt = frame
      end
    end
  end
  -- (Opt-in, SBM2_THROW=1: on the glove stages it lifted often but rarely
  -- found a >= 40% landing, and held bombs cost bombing time: 3-5/7 glove
  -- stages cleared against 6/7 without, one seed each.)
  -- Throws at enemies (with the glove, not 5-8): a thrown bomb that lands
  -- on an enemy's cell hurts it at once (C1:1F95 -> EnemyHit; at 1 HP it
  -- dies: docs/mechanics.md 10b), 3 cells ahead, no fuse to wait out.
  -- Holding: throw where an enemy is likely on the landing cell when the
  -- bomb comes down (~12 frames, +3 to turn). Not holding: place a bomb
  -- and lift it when a throw would likely hit after the lift (~30 frames).
  if not finalForm2() and emu.read(0x1A, WRAM, false) ~= 0x57 and emu.read(P1 + 0xFE, WRAM, false) > 0
      and #elist > 0 and os.getenv("SBM2_THROW") == "1" then
    local holding = (r16(P1 + 0x52) & 0x6) ~= 0
    local DV = { up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } }
    local function chance(d, tl)
      local v = DV[d]
      local lc, lr = c + v[1] * 3, r + v[2] * 3
      if not walkable(g, lc, lr) then return 0 end
      local _, best = Enemies.killChance(samples, elist, { [lr * KEY_W + lc] = true }, tl - 4, tl + 6)
      return best
    end
    local function bestDir(base)
      local best, bd = 0, nil
      for d in pairs(DV) do
        local delay = (throw.face == d and throw.faceFrames >= 2) and 0 or 3
        local p = chance(d, base + delay)
        if p > best then best, bd = p, d end
      end
      return bd, best
    end
    for d in pairs(DV) do if input[d] then throw.face = d end end
    if holding then
      throw.eHeld = throw.eHeld or frame
      input.a = true
      local bd, best = bestDir(12)
      if bd and best >= 0.4 then
        if throw.face == bd and throw.faceFrames >= 2 then
          input.a = nil
          log(string.format("%6d throw %s at an enemy (chance %.2f) from (%d,%d)", frame, bd, best, c, r))
          throw.at, throw.eHeld, throw.faceFrames = frame, nil, 0
        elseif plan ~= "dodge" and plan ~= "flee" then
          input = { a = true, [bd] = true }
          if throw.face == bd then throw.faceFrames = throw.faceFrames + 1 else throw.face, throw.faceFrames = bd, 1 end
        end
      elseif frame - throw.eHeld > 60 and not danger[here] then
        -- nothing came within reach: put it down ahead (it lands 3 out)
        input.a = nil
        log(string.format("%6d throw %s away (no target for 60 frames)", frame, tostring(throw.face)))
        throw.at, throw.eHeld = frame, nil
      end
    else
      throw.eHeld = nil
      local onBomb = isBomb(g[r][c])
      if onBomb and throw.eLift and frame - throw.eLift < 40 then
        input = { a = frame % 2 == 0 }  -- release, press again: lift (edge)
      elseif not onBomb and not danger[here] and not nearEnemy[here] and frame - throw.at > 40
          and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and plan ~= "flee" and plan ~= "dodge" then
        local bd, best = bestDir(30)
        if bd and best >= 0.5 then
          input.a = frame % 2 == 0
          throw.eLift = frame
          log(string.format("%6d lift for a throw %s (chance %.2f)", frame, bd, best))
        end
      end
    end
  end
  -- On a cell boundary with the game's cell (+$A8) in a blast and P1's own
  -- tile safe: the flee goal is "here", so nothing moved P1; walk it to the
  -- centre of its tile (1-3: 18 deaths at y 152/153 between rows 18/19).
  if ride.a8Hot and not windows[here] then
    local cx, cy = tileCentre(c, r)
    cy = cy - 1  -- (rows are walked at r*16+7)
    input.up, input.down, input.left, input.right = nil, nil, nil, nil
    if y > cy + 1 then input.up = true elseif y < cy - 1 then input.down = true end
    if x > cx + 1 then input.left = true elseif x < cx - 1 then input.right = true end
  end
  if plan == "shadow" then
    input = { a = input.a, b = input.b }
    if ride.shadowV then
      input[ride.shadowV] = true
    else
      if ride.shadowDir then input[ride.shadowDir] = true end
      local _, cy = tileCentre(c, r)
      cy = cy - 1  -- (rows are walked at r*16+7)
      if r % 2 == 1 then
        input.down = (ride.sb and ride.sb.r < r) or nil
        input.up = not input.down or nil
      elseif y > cy + 1 then input.up = true elseif y < cy - 1 then input.down = true end
    end
  end
  -- Last guard: never step into fire. A pressed direction whose next
  -- position (2 px on) is in a cell burning or blasting within 6 frames,
  -- when P1's own cell is not, is dropped (5-8: steering for "aim left"
  -- walked P1 right into a column of flames).
  -- Nor into a new cell that blasts while P1 crosses it (FPT + 4) and
  -- sooner than P1's own cell (1-7: fled from an enemy bomb counted
  -- as due in 8 frames, really 134, into its own bomb's row 7 frames
  -- before it went off).
  do
    local function first(k)
      local f = 1e9
      for _, iv in ipairs(windows[k] or {}) do if iv[2] >= 0 and iv[1] < f then f = iv[1] end end
      return f
    end
    local fh = first(here)
    -- (not in the final fight: there it kept P1 from its throw spots)
    local cross = emu.read(0x1A, WRAM, false) ~= 0x57
    for d, v in pairs({ up = { 0, -2 }, down = { 0, 2 }, left = { -2, 0 }, right = { 2, 0 } }) do
      if input[d] then
        local nc, nr = toTile(x + v[1], y + v[2])
        local nk = nr * KEY_W + nc
        local fn = first(nk)
        if (fh > 6 and fn <= 6) or (cross and nk ~= here and fn <= FPT + 4 and fn < fh) then input[d] = nil end
      end
    end
  end
  if plan ~= "bomb" then bombTry = nil end
  -- Pressing a direction but not moving for 600 frames (held by an object,
  -- a warp gone wrong): go back to before it.
  local pressing = input.up or input.down or input.left or input.right
  -- (5-8: the cutscene after form 1 holds P1 invulnerable, hearts $FF, for
  -- ~1550 frames; rolling back from it undid the kill)
  local held = emu.read(0x1A, WRAM, false) == 0x57 and emu.read(P1 + 0xEF, WRAM, false) == 0xFF
  if not pressing or held or math.abs(x - frozenAt[1]) + math.abs(y - frozenAt[2]) > 3 or frame < frozenAt[3] then
    frozenAt = { x, y, frame }
  end
  -- On a spring pad pressing one way for 60 frames without a launch: that
  -- jump is no way (3-1, run 22: the exit route jumped right off (4,6),
  -- the pad never went, 600 frames, and again after each "rollback").
  -- (not while bouncing on purpose, "stay": 3-6, run 24, marked the pad
  -- "no way" while it waited out an $8B by the landing, then fled by it)
  for k, at in pairs(DeadPad) do if frame - at > 900 or frame < at then DeadPad[k] = nil end end
  if g[r][c] == SPRING and frame - frozenAt[3] > 180 and springDir ~= "stay" and frame - (ride.lastStay or -1000) > 300 then
    for _, d in ipairs(DIRS) do
      if input[d[3]] then
        local key = (r * KEY_W + c) .. d[3]
        -- (never one already made: a landing learnt means it launches;
        -- 3-6, run 27, banned (10,10) right and down after waits there and
        -- was shut in the upper right)
        -- Any pad that holds P1 this long without a launch is floor for
        -- a while (3-1 run 30: a $98 mount sat on (12,8), whose jumps were
        -- all learnt, and P1 stood frozen on it 30000 frames).
        if not DeadPad[r * KEY_W + c] then
          local ms = ""
          for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
            local t = r16(base + 0x7E) & 0xFF
            if (r16(base) & 0xF000) == 0x9000 and (t == 0x98 or t == 0x90) then
              ms = ms .. string.format(" %02X@px(%d,%d) BE=%04X", t, r16(base + P_X), r16(base + P_Y), r16(base + 0xBE))
            end
          end
          log(string.format("%6d pad (%d,%d): no launch; plain floor for 900 frames; P1 px(%d,%d) +52=%04X C6=%02X;%s", frame, c, r,
            r16(P1 + P_X), r16(P1 + P_Y), r16(P1 + 0x52), emu.read(P1 + 0xC6, WRAM, false), ms))
          DeadPad[r * KEY_W + c] = frame
          step = nil  -- (its jump was the committed step: Left held into the wall)
        end
        if springLand[key] == nil then
          log(string.format("%6d pad (%d,%d) %s: no launch in 180 frames; not a way", frame, c, r, d[3]))
          springLand[key], springLand[key .. "!"] = false, false
          frozenAt = { x, y, frame }
        end
      end
    end
  end
  if ride.badLift then
    for k, t in pairs(ride.badLift) do
      if frame >= t or frame < t - 1500 then badCell[k] = nil; ride.badLift[k] = nil end
    end
  end
  if frame - frozenAt[3] > 600 and timeLeft() > 1 then
    log(string.format("%6d frozen at (%d,%d) since %d; rolling back", frame, c, r, frozenAt[3]))
    rollback, rollbackBefore = true, frozenAt[3] - 100
    if lastRide and frozenAt[3] - lastRide[3] < 120 then
      local bk = lastRide[2] * KEY_W + lastRide[1]
      badCell[bk] = true
      rollbackBefore = lastRide[3] - 300
      -- (no rewinds: P1 plays on from past the ride, and that ride may be
      -- the only way back; a ban for good shut 4-3's exit route, run half4
      -- s0, (3,12) banned at 23986, gate open at 32271, exit only after the
      -- memory reset at 35271; so the ban lifts after 1500 frames)
      if NOREWIND then ride.badLift = ride.badLift or {}; ride.badLift[bk] = frame + 1500 end
      log(string.format("%6d froze after the ride from (%d,%d): not again%s", frame, lastRide[1], lastRide[2],
        NOREWIND and " for 1500 frames" or ""))
    end
    frozenAt = { x, y, frame }
  end
  if os.getenv("SBM2_DBG") and input.a and plan ~= "bomb" then log(string.format("%6d dbg A pressed in plan %s", frame, tostring(plan))) end
  if plan == "board" and ride.boardDir then input = { [ride.boardDir] = true } end
  -- Lookahead (port only; SBM2_LOOKAHEAD=0 turns it off): a safety shield on emu.simulate, scratch
  -- copies of the machine run ahead (the real game is never rolled back).
  -- Near a threat, every 8 frames (or when the bot's move changes) the
  -- bot's move held K frames and then some continuation (one direction for
  -- A frames, then another) must keep P1 alive for the horizon (long enough
  -- for a fresh fuse and its flames). If none does, a bomb press is dropped
  -- if that is enough; else the last verified sequence is followed while it
  -- still lives, or a new one is searched for (the longest-living).
  if ride.LA == nil then ride.LA = os.getenv("SBM2_LOOKAHEAD") ~= "0" and emu.simulateMany ~= nil end
  -- The simulator counts a heart lost as a death (port/botrun.c), except
  -- while a trap is crossed on purpose (plan "trap": a heart is spent).
  if emu.simHearts then emu.simHearts(plan ~= "trap" and not (ride.brk and ride.brk.heart) and frame >= (ride.heartUntil or 0) and not ride.tank) end
  -- (P1's cell and since when: a long stay is a wedge, see below)
  if ride.stillAt ~= r * KEY_W + c then ride.stillAt, ride.stillSince = r * KEY_W + c, frame end
  local wedged = frame - ride.stillSince > 300
  -- Exit search: the gate open, P1 wedged or stalled: the simulator tries
  -- holding one direction (20 or 60 frames) then another, for one that
  -- leaves by the gate. (4-2, run SH3 s0: all panels done, the gate at
  -- (17,3) open, reached only by the column-17 platform; waited at the
  -- lane end (17,8) beside it, "no ride came", explored until time-up.)
  if gateOpen and (wedged or frame - (ride.lastStall or -1e9) < 600) and not ride.brk and emu.simulateMany
      and frame >= (ride.exitNext or 0) and next(danger) == nil then
    ride.exitNext = frame + 300
    local NM = { "up", "down", "left", "right" }
    local list, desc = {}, {}
    for _, A in ipairs({ 20, 60 }) do
      for _, d1 in ipairs(NM) do
        for _, d2 in ipairs({ "up", "down", "left", "right", "none" }) do
          if d1 ~= d2 then
            local p1, p2 = emu.padBits({ [d1] = true }), emu.padBits(d2 == "none" and {} or { [d2] = true })
            local seq = {}
            for f = 1, 400 do seq[f] = f <= A and p1 or p2 end
            list[#list + 1] = seq; desc[#desc + 1] = string.format("%s %d then %s", d1, A, d2)
          end
        end
      end
    end
    local res = emu.simulateMany(list, 400, false, 0)
    local hit
    for i = 1, #list do if res[i] == -2 then hit = i; break end end
    log(string.format("%6d exit search at (%d,%d): %s", frame, c, r, hit and desc[hit] or "none"))
    if hit then ride.brk = { seq = list[hit], start = frame, H = 400 } end
  end
  -- Stall breaker: stalled (nothing changed for a while), the simulator
  -- tries "walk to a cell near, bomb, walk to cover" for the cells whose
  -- blast reaches an enemy or a soft block, and the bot carries out the one
  -- that lives and kills the most enemies (else breaks the most blocks).
  -- (3-3, run SH3 s0: the last $99 in a corridor behind a parked mount;
  -- no plan found, explored until time ran out.)
  local function brkPad(wp, st, key)
    local ox, oy = origin()
    local x0, y0 = (r16(P1 + P_X) + ox) & 0xFFFF, (r16(P1 + P_Y) + oy) & 0xFFFF
    local n = #wp // 2
    while st[key] < n and math.abs(wp[2 * st[key] + 1] - x0) <= 1 and math.abs(wp[2 * st[key] + 2] - y0) <= 1 do st[key] = st[key] + 1 end
    if st[key] >= n then return {} end
    local dx, dy = wp[2 * st[key] + 1] - x0, wp[2 * st[key] + 2] - y0
    if math.abs(dx) > math.abs(dy) then return { [dx > 0 and "right" or "left"] = true } end
    return { [dy > 0 and "down" or "up"] = true }
  end
  if ride.brkWant and not ride.brk and emu.simulateMany and frame - ride.brkWant < 600
      and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and next(danger) == nil
      and not (remoteOn and next(remoteCells)) then
    ride.brkWant = nil
    local prev, order, q, qi = { [r * KEY_W + c] = -1 }, {}, { { c, r, 0 } }, 1
    while qi <= #q do
      local cc, rr, dd = q[qi][1], q[qi][2], q[qi][3]; qi = qi + 1
      order[#order + 1] = { cc, rr, dd }
      if dd < 60 then
        for _, d in ipairs(DIRS) do
          local nc, nr = cc + d[1], rr + d[2]
          local k = nr * KEY_W + nc
          if prev[k] == nil and walkable(g, nc, nr) then prev[k] = rr * KEY_W + cc; q[#q + 1] = { nc, nr, dd + 1 } end
        end
      end
    end
    local near = {}
    for _, e in ipairs(elist) do
      if e.killable or e.harmful then
        for dr = -2, 2 do for dc = -2, 2 do near[(e.r + dr) * KEY_W + e.c + dc] = true end end
      end
    end
    local function wpOf(k)
      local cells = {}
      while k ~= -1 do table.insert(cells, 1, k); k = prev[k] end
      return cells
    end
    local cand = {}
    for _, o in ipairs(order) do
      local val = 0
      local bc = blastCells(g, o[1], o[2], rangeAt(g, o[1], o[2]))
      for _, cell in ipairs(bc) do
        local t = g[cell[2]] and g[cell[2]][cell[1]]
        if near[cell[2] * KEY_W + cell[1]] then val = val + 10 elseif t and SOFT[t] then val = val + 1 end
        if panelTodo[cell[2] * KEY_W + cell[1]] then val = val + 50 end
      end
      if val > 0 then cand[#cand + 1] = { o[1], o[2], o[3], val, bc } end
    end
    table.sort(cand, function(u, v) if u[4] ~= v[4] then return u[4] > v[4] end return u[3] < v[3] end)
    local A2 = emu.padBits({ a = true })
    local list, meta, maxLen = { { emu.padBits({}) } }, { false }, 0
    -- With hearts in hand and no cover anywhere (2-4, run HX s0: sealed by
    -- regrown blocks in the 5-cell column pocket at (3,8)-(3,12) with the
    -- last panel at its end; every bomb's blast covered the whole pocket),
    -- a try may spend a heart: bomb, then stand at the far end; simulated
    -- with heart losses allowed.
    local hearts = emu.read(P1 + 0xEF, WRAM, false)
    local hlist, hmeta = { { emu.padBits({}) } }, { false }
    for i = 1, math.min(#cand, 24) do
      local cd = cand[i]
      local blast = {}
      for _, cell in ipairs(cd[5]) do blast[cell[2] * KEY_W + cell[1]] = true end
      -- cover: the nearest cells off the blast, from the bomb's cell
      local pv, q2, qj, got = { [cd[2] * KEY_W + cd[1]] = -1 }, { { cd[1], cd[2], 0 } }, 1, {}
      local far, farD = cd[2] * KEY_W + cd[1], -1
      while qj <= #q2 and #got < 2 do
        local cc, rr, dd = q2[qj][1], q2[qj][2], q2[qj][3]; qj = qj + 1
        if dd > farD then far, farD = rr * KEY_W + cc, dd end
        if not blast[rr * KEY_W + cc] then got[#got + 1] = rr * KEY_W + cc
        elseif dd < 8 then
          for _, d in ipairs(DIRS) do
            local nc, nr = cc + d[1], rr + d[2]
            local k = nr * KEY_W + nc
            if pv[k] == nil and walkable(g, nc, nr) then pv[k] = rr * KEY_W + cc; q2[#q2 + 1] = { nc, nr, dd + 1 } end
          end
        end
      end
      local path = {}
      for _, k in ipairs(wpOf(cd[2] * KEY_W + cd[1])) do
        path[#path + 1] = (k % KEY_W) * 16 + 8; path[#path + 1] = (k // KEY_W) * 16 + 8
      end
      local hgot = (#got == 0 and hearts > 0) and { far } or {}
      for _, gk in ipairs(hgot) do
        local cells, k = {}, gk
        while k ~= -1 do table.insert(cells, 1, k); k = pv[k] end
        local path2 = {}
        for j = 2, #cells do
          path2[#path2 + 1] = (cells[j] % KEY_W) * 16 + 8; path2[#path2 + 1] = (cells[j] // KEY_W) * 16 + 8
        end
        local pan = false
        for _, cell in ipairs(cd[5]) do if panelTodo[cell[2] * KEY_W + cell[1]] then pan = true end end
        hlist[#hlist + 1] = { path = path, post = { A2, A2 }, path2 = path2, detEnd = remoteOn or nil }
        hmeta[#hmeta + 1] = { cd[1], cd[2], gk % KEY_W, gk // KEY_W, pan }
        maxLen = math.max(maxLen, cd[3] + #cells)
      end
      for _, gk in ipairs(got) do
        local cells, k = {}, gk
        while k ~= -1 do table.insert(cells, 1, k); k = pv[k] end
        local path2 = {}
        for j = 2, #cells do
          path2[#path2 + 1] = (cells[j] % KEY_W) * 16 + 8; path2[#path2 + 1] = (cells[j] // KEY_W) * 16 + 8
        end
        -- (a remote bomb goes off only on B: pressed once in cover, or
        -- after a wait there for a chaser to walk into the blast: 4-5, run
        -- half45 s0, two $93 (2 HP) never hit by a B pressed on arrival)
        for _, wt in ipairs(remoteOn and ride.chaserEta and { 0, 40, 90, 160 } or { 0 }) do
          list[#list + 1] = { path = path, post = { A2, A2 }, path2 = path2, detEnd = remoteOn or nil, detWait = wt }
          meta[#meta + 1] = { cd[1], cd[2], gk % KEY_W, gk // KEY_W, wt }
        end
        maxLen = math.max(maxLen, cd[3] + #cells)
      end
    end
    if #list == 1 and #hlist > 1 then
      local H = math.min(1100, 16 * maxLen + 260)
      if emu.simHearts then emu.simHearts(false) end
      local res, fl, en = emu.simulateMany(hlist, H, false, 0)
      if emu.simHearts then emu.simHearts(true) end
      local best, bestS
      for i = 2, #hlist do
        if res[i] and res[i] < 0 then
          local sc = (en[1] - en[i]) * 1000 + (fl[i] - fl[1]) + (hmeta[i][5] and 500 or 0)
          if sc > 0 and (not bestS or sc > bestS) then best, bestS = i, sc end
        end
      end
      log(string.format("%6d stall breaker: no cover; %d tries spending a heart (%d held): %s", frame, #hlist - 1, hearts,
        best and string.format("bomb at (%d,%d), stand at (%d,%d)%s", hmeta[best][1], hmeta[best][2], hmeta[best][3], hmeta[best][4],
          hmeta[best][5] and ", a panel in the blast" or "") or "none"))
      if best then
        local e = hlist[best]
        ride.brk = { path = e.path, post = e.post, path2 = e.path2, detEnd = e.detEnd, ph = 1, at = 0, at2 = 0, start = frame, H = H, heart = true }
        -- (the hit is the plan until its bomb has gone off: with the
        -- breaker's walk over, the lookahead saw the coming hit as a death
        -- and lifted the bomb away with the glove; 4-2, run HZ s0 replayed,
        -- every 1500 frames for 10000)
        ride.heartUntil = frame + H
      end
    elseif #list == 1 then
      log(string.format("%6d stall breaker: no tries (%d cells reached, %d with a target)", frame, #order, #cand))
    else
      local H = math.min(1260, 16 * maxLen + 260 + ((remoteOn and ride.chaserEta) and 160 or 0))
      local t0 = os.clock()
      local res, fl, en, _, _, hpS = emu.simulateMany(list, H, false, 0)
      local best, bestS
      for i = 2, #list do
        if res[i] and res[i] < 0 then
          -- (a hit that leaves a 2-HP enemy alive counts too)
          local sc = (en[1] - en[i]) * 1000 + (hpS and (hpS[1] - hpS[i]) * 400 or 0) + (fl[i] - fl[1])
          if sc > 0 and (not bestS or sc > bestS) then best, bestS = i, sc end
        end
      end
      log(string.format("%6d stall breaker: %d tries, H=%d, %.1fs: %s", frame, #list - 1, H, os.clock() - t0,
        best and string.format("bomb at (%d,%d), cover (%d,%d), %d kills, %d HP, %+d floor%s", meta[best][1], meta[best][2],
          meta[best][3], meta[best][4], en[1] - en[best], hpS and hpS[1] - hpS[best] or 0, fl[best] - fl[1],
          (meta[best][5] or 0) > 0 and string.format(", set off after %d", meta[best][5]) or "") or "none"))
      if best then
        local e = list[best]
        ride.brk = { path = e.path, post = e.post, path2 = e.path2, detEnd = e.detEnd, detWait = e.detWait, ph = 1, at = 0, at2 = 0, start = frame, H = H }
        -- (an older lookahead commitment would take over and drop it)
        if ride.la then ride.la.over, ride.la.commitUntil, ride.la.cand = nil, nil, nil end
      end
    end
  end
  -- Wedged long (600 frames on a cell, nothing about to go off): the
  -- simulator tries walking to far cells and to cells by panels left,
  -- over track cells too (the bot keeps off tracks near a $DD block),
  -- and the bot takes one that lives and gets there. (4-2, runs SH3 s0
  -- and 4-2_r54: P1 stood 1300+ frames in the left area the column-4
  -- track and its $DD block fence in, until time ran out.)
  if wedged and frame - ride.stillSince > 600 and not ride.brk and emu.simulateMany and next(danger) == nil
      and frame >= (ride.goNext or 0) then
    ride.goNext = frame + 300
    local function relaxed(cc, rr)
      local t = g[rr] and g[rr][cc]
      return t ~= nil and (walkable(g, cc, rr) or t == 0x4D)
    end
    local prev, order, q, qi = { [r * KEY_W + c] = -1 }, {}, { { c, r, 0 } }, 1
    while qi <= #q do
      local cc, rr, dd = q[qi][1], q[qi][2], q[qi][3]; qi = qi + 1
      order[#order + 1] = { cc, rr, dd }
      if dd < 40 then
        for _, d in ipairs(DIRS) do
          local nc, nr = cc + d[1], rr + d[2]
          local k = nr * KEY_W + nc
          if prev[k] == nil and relaxed(nc, nr) then prev[k] = rr * KEY_W + cc; q[#q + 1] = { nc, nr, dd + 1 } end
        end
      end
    end
    local pick, seen = {}, {}
    for _, o in ipairs(order) do
      local k = o[2] * KEY_W + o[1]
      for _, d in ipairs(DIRS) do
        if panelTodo[(o[2] + d[2]) * KEY_W + o[1] + d[1]] and not seen[k] then pick[#pick + 1] = o; seen[k] = true end
      end
    end
    for i = #order, 1, -1 do
      if #pick >= 16 then break end
      local o = order[i]
      if not seen[o[2] * KEY_W + o[1]] and o[3] >= 4 then pick[#pick + 1] = o; seen[o[2] * KEY_W + o[1]] = true end
    end
    local list, maxLen = {}, 0
    for i = 1, math.min(#pick, 16) do
      local cells, k = {}, pick[i][2] * KEY_W + pick[i][1]
      while k ~= -1 do table.insert(cells, 1, k); k = prev[k] end
      local wp = {}
      for _, kk in ipairs(cells) do wp[#wp + 1] = (kk % KEY_W) * 16 + 8; wp[#wp + 1] = (kk // KEY_W) * 16 + 8 end
      list[#list + 1] = { path = wp, pre = {} }
      maxLen = math.max(maxLen, #cells)
    end
    if #list > 0 then
      local H = math.min(1000, 20 * maxLen + 120)
      local res, _, _, ps = emu.simulateMany(list, H, false, 0)
      local best
      for i = 1, #list do
        local wp = list[i].path
        local fx, fy = tostring(ps[i]):match(":(%-?%d+),(%-?%d+)")
        local got = fx and math.abs(tonumber(fx) - wp[#wp - 1]) <= 4 and math.abs(tonumber(fy) - wp[#wp]) <= 4
        if res[i] == -1 and got then best = i; break end
      end
      log(string.format("%6d wedged %d frames at (%d,%d); walk search (%d tries, H=%d): %s", frame, frame - ride.stillSince, c, r, #list, H,
        best and string.format("to (%d,%d)", pick[best][1], pick[best][2]) or "none"))
      if best then ride.brk = { path = list[best].path, post = {}, path2 = {}, ph = 1, at = 0, at2 = 0, start = frame, H = H, walk = true } end
    end
  end
  if ride.brk then
    local b, inp = ride.brk, nil
    -- A wedged walk is blind to enemies that stray from the sim (3-2, run
    -- WS s0: it led down column 21 into an $8F): one within 24 px drops
    -- it, and the usual dodge takes over. (Not a breaker's bomb walk: its
    -- target is an enemy; 1-2, run BP s0, dropped for the $94 it was for.)
    local foe
    for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
      local typ = r16(base + 0x7E) & 0xFF
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and typ >= 0x80 and typ < 0xC0
          and typ ~= 0x90 and typ ~= 0x98
          and math.abs(r16(base + P_X) - r16(P1 + P_X)) < 24 and math.abs(r16(base + P_Y) - r16(P1 + P_Y)) < 24 then
        foe = string.format("%02X", typ)
      end
    end
    if foe and b.walk then
      log(string.format("%6d stall breaker dropped: a $%s within 24 px", frame, foe)); ride.brk = nil
    elseif frame - b.start > b.H or (ride.la and ride.la.over and (ride.la.start or 0) > b.start) then
      log(string.format("%6d stall breaker dropped%s", frame, frame - b.start > b.H and " (time)" or " (lookahead)")); ride.brk = nil
    elseif b.seq then
      local w = b.seq[frame - b.start + 1]
      if not w then ride.brk = nil else
        inp = {}
        for nm, bit in pairs({ up = 0x0800, down = 0x0400, left = 0x0200, right = 0x0100 }) do if w & bit ~= 0 then inp[nm] = true end end
      end
      if inp then input = inp; plan, dir, step = "exit", next(inp) or "none", nil end
    else
      if b.ph == 1 then inp = brkPad(b.path, b, "at"); if not next(inp) then b.ph, b.p0 = 2, frame end end
      if b.ph == 2 then if frame - b.p0 < #b.post then inp = { a = true } else b.ph = 3 end end
      if b.ph == 3 then
        inp = brkPad(b.path2, b, "at2")
        if not next(inp) then if b.detEnd then b.ph, b.p0 = 4, frame + (b.detWait or 0) else ride.brk = nil end end
      end
      if b.ph == 4 then
        if frame < b.p0 then inp = {}
        elseif frame - b.p0 < 4 then inp = { b = true } else
          inp, ride.brk = {}, nil
          local hs = {}
          for _, e in ipairs(elist) do if e.killable then hs[#hs + 1] = string.format("%02X@(%d,%d)hp%d", e.type, e.c, e.r, e.hp or 0) end end
          log(string.format("%6d stall breaker done; %s", frame, table.concat(hs, " ")))
        end
      end
      if inp then input = inp; plan, dir, step = "breaker", next(inp) or "a", nil end
    end
  end
  if ride.LA and not Riding and (plan ~= "board" or wedged) then
    local K, A = 8, 20
    local NAMES = { "none", "up", "down", "left", "right" }
    local function pad(nm, bomb)
      local p = (nm == "none") and {} or { [nm] = true }
      if bomb then p.a = true end
      return emu.padBits(p)
    end
    -- seq: `pre` (pad words) then d1 for A frames then d2; length H
    local function seqOf(pre, d1, d2, H)
      local seq = {}
      for i = 1, #pre do seq[i] = pre[i] end
      local p1, p2 = pad(d1), pad(d2)
      for f = #pre + 1, H do seq[f] = (f - #pre <= A) and p1 or p2 end
      return seq
    end
    local function conts(pre, first, H)
      local order = { first }
      for _, nm in ipairs(NAMES) do if nm ~= first then order[#order + 1] = nm end end
      local list = {}
      for _, d1 in ipairs(order) do
        for _, d2 in ipairs(order) do list[#list + 1] = seqOf(pre, d1, d2, H) end
      end
      return list
    end
    -- A live one that ends with room to move first: a dodge into a
    -- one-cell pocket lives the horizon out, then an $87 shuts it with a
    -- soft block (4-2, run HZ s0 replayed: fled the $87 in column 12 from
    -- the (12,9) lane end into (11,10), and sat there sealed in for 10000
    -- frames). With the room count (botrun's 5th result) all are run.
    local function firstLive(list, H)
      local best, bestT, tight = nil, -2, nil
      local from, calls = 1, 0
      while from <= #list and calls < 3 do
        calls = calls + 1
        local sub = list
        if from > 1 then sub = {}; for i = from, #list do sub[#sub + 1] = list[i] end end
        local res, _, _, _, rch = emu.simulateMany(sub, H, true, ride.laDet or 0)
        local done = 0
        for i = 1, #sub do
          local t = res[i]
          if t == nil then break end
          done = i
          if t < 0 then
            if not rch or (rch[i] or 99) >= 4 then return sub[i], -1 end
            tight = tight or sub[i]
          elseif t > bestT then best, bestT = sub[i], t end
        end
        -- (only tight ones lived so far: the rest are tried too)
        if done == 0 then break end
        from = from + done
      end
      if tight then return tight, -1 end
      return best, bestT
    end
    -- closed-loop path candidates: { path = {x1, y1, ...}, pre = pads };
    -- the simulator steers along them (botrun.c path_pad)
    local function pathCands(pre, maxN)
      local prev, order, q, qi = { [r * KEY_W + c] = -1 }, {}, { { c, r, 0 } }, 1
      while qi <= #q do
        local cc, rr, dd = q[qi][1], q[qi][2], q[qi][3]; qi = qi + 1
        order[#order + 1] = { cc, rr, dd }
        if dd < 10 then
          for _, d in ipairs(DIRS) do
            local nc, nr = cc + d[1], rr + d[2]
            local k = nr * KEY_W + nc
            if prev[k] == nil and walkable(g, nc, nr) then prev[k] = rr * KEY_W + cc; q[#q + 1] = { nc, nr, dd + 1 } end
          end
        end
      end
      table.sort(order, function(u, v)
        local du, dv = danger[u[2] * KEY_W + u[1]] and 1 or 0, danger[v[2] * KEY_W + v[1]] and 1 or 0
        if du ~= dv then return du < dv end
        return u[3] < v[3]
      end)
      local list = {}
      for i = 1, math.min(#order, maxN) do
        local cells, k = {}, order[i][2] * KEY_W + order[i][1]
        while k ~= -1 do table.insert(cells, 1, k); k = prev[k] end
        local wp = {}
        for _, kk in ipairs(cells) do
          wp[#wp + 1] = (kk % KEY_W) * 16 + 8; wp[#wp + 1] = (kk // KEY_W) * 16 + 8  -- world pixels
        end
        list[#list + 1] = { path = wp, pre = pre }
      end
      return list
    end
    local function pathPad(wp, st)
      local ox, oy = origin()
      local x0, y0 = (r16(P1 + P_X) + ox) & 0xFFFF, (r16(P1 + P_Y) + oy) & 0xFFFF
      local n = #wp // 2
      while st.at < n and math.abs(wp[2 * st.at + 1] - x0) <= 1 and math.abs(wp[2 * st.at + 2] - y0) <= 1 do st.at = st.at + 1 end
      if st.at >= n then return {} end
      local dx, dy = wp[2 * st.at + 1] - x0, wp[2 * st.at + 2] - y0
      if math.abs(dx) > math.abs(dy) then return { [dx > 0 and "right" or "left"] = true } end
      return { [dy > 0 and "down" or "up"] = true }
    end
    local function cat(a1, a2) for _, v in ipairs(a2) do a1[#a1 + 1] = v end; return a1 end
    local la = ride.la or {}
    ride.la = la
    if ride.LADBG == nil then ride.LADBG = os.getenv("SBM2_LADEBUG") == "1" end
    if ride.LADBG then
      ride.simq = ride.simq or {}
      local sq = ride.simq[frame]
      if sq then log(string.format("%6d simtest predicted px(%d,%d) frame %d; real px(%d,%d) frame %d", frame, sq[1], sq[2], sq[3], x, y, emu.frame and emu.frame() or -1)) end
      local s2 = ride.simq2 and ride.simq2[frame]
      if s2 then
        local ox, oy = origin()
        log(string.format("%6d simtest2 follow predicted %s; real %d,%d; still following %s", frame, s2[1],
          (x + ox) & 0xFFFF, (y + oy) & 0xFFFF, tostring(la.cand == s2[2] and la.over)))
        ride.simq2[frame] = nil
      end
      if frame % 50 == 0 then
        local p0 = emu.padBits(input)
        local dd, sx, sy, sf = emu.simulate({ p0, p0, p0, p0, p0 }, 5)
        ride.simq[frame + 5] = { sx, sy, sf }
      end
    end
    if la.veto and frame < la.veto and input.a then input.a = nil end
    local myDir = (input.up and "up") or (input.down and "down") or (input.left and "left") or (input.right and "right") or "none"
    local hb = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0
    local press = input.a and hb or false
    local myPad = emu.padBits(input)
    if Blind then myPad = myPad & ~0x0080 end
    local key = string.format("%s%s/%04X", myDir, press and "+a" or "", myPad)
    -- (boss stages: $1A = $x7, 1-8 .. 5-8; Boss.read lists objects anywhere)
    local bossStage = (emu.read(0x1A, WRAM, false) & 0x0F) == 7
    local threat = press or bossStage
    if not threat then
      for dr = -3, 3 do
        for dc = -3 + math.abs(dr), 3 - math.abs(dr) do
          if danger[(r + dr) * KEY_W + c + dc] then threat = true end
        end
      end
    end
    if not threat then
      for _, e in ipairs(elist) do
        if math.abs(e.c - c) + math.abs(e.r - r) <= 6 then threat = true; break end
      end
    end
    local bombNear = press
    if not bombNear then
      for k in pairs(danger) do
        local kr, kc = k // KEY_W, k % KEY_W
        if math.abs(kr - r) + math.abs(kc - c) <= 6 then bombNear = true; break end
      end
    end
    local H = (bombNear or bossStage) and 175 or 150
    -- Own remote bombs out: every sequence ends with setting them off (B),
    -- so "lives" means P1 can still leave their blast (world 2's soft
    -- blocks grow back: 2-4, run SH2 s0 stood by a remote bomb at (3,5)
    -- until it was sealed in; never set off, the sim saw no death).
    -- (not for the bot's own press about to lay one: 5-1, run r74 rerun,
    -- laying at (2,5) then "died" at +154, P1 fled into the (2,12) corner
    -- and never killed the $8As)
    ride.laDet = (remoteOn and next(remoteCells) ~= nil) and H - 30 or 0
    if ride.laDet > 0 then threat = true end
    -- (a move checked in the last 8 frames is not checked again when the
    -- bot flips back to it: 3-5, P1 dithered left/right at (13,9) and every
    -- frame ran the full check, 0.6 s each; the scenario took over an hour)
    la.seenKey = la.seenKey or {}
    local due = frame >= (la.nextF or 0) or (key ~= la.key and frame >= (la.seenKey[key] or 0))
    -- the rest of the followed candidate, from now
    local function rest()
      local cd = la.cand
      if not cd then return nil end
      local t = frame - la.start
      if cd.path then
        local pre = {}
        for i = t + 1, #cd.pre do pre[#pre + 1] = cd.pre[i] end
        local wp = {}
        for i = 2 * la.st.at + 1, #cd.path do wp[#wp + 1] = cd.path[i] end
        if #wp == 0 then return nil end
        return { path = wp, pre = pre }
      end
      if t >= #cd - 30 then return nil end
      local q = {}
      for i = t + 1, #cd do q[#q + 1] = cd[i] end
      return q
    end
    local function follow(cd, why)
      la.cand, la.start, la.st, la.over = cd, frame, { at = 0 }, true
      if ride.LADBG then
        local _, _, _, ps = emu.simulateMany({ cd }, 30, false, ride.laDet or 0)
        ride.simq2 = ride.simq2 or {}
        ride.simq2[frame + 30] = { tostring(ps[1]), cd }
      end
      if why then
        log(string.format("%6d lookahead: %s", frame, why))
        -- kept to for a while (the bot's next move, standing say, may
        -- check out while its plan still knows no other way)
        la.commitUntil = frame + 40
        -- the bot's way was overridden: it plans around that cell for a while
        for _, d in ipairs(DIRS) do
          if d[3] == myDir then
            ride.laBlock = ride.laBlock or {}
            ride.laBlock[(r + d[2]) * KEY_W + c + d[1]] = frame + 90
          end
        end
      end
    end
    local committed = false
    if la.over and la.cand and frame < (la.commitUntil or 0) then
      committed = true
      if frame >= (la.nextF or 0) then
        la.nextF = frame + 8
        local old = rest()
        local rr = old and emu.simulateMany({ old }, H, false, ride.laDet or 0)[1]
        if ride.LADBG then log(string.format("%6d dbg recheck rest %s: %s", frame, old and (old.path and "path" or ("seq " .. #old)) or "nil", tostring(rr))) end
        -- (one that dies only near the end of the check is still the way:
        -- 4-4, run QA s0 replayed, a "bomb out" held down by the wall, its
        -- rest dying at +173 of 175; dropped, P1 stood and the blast took a
        -- heart at +100)
        if old and (rr < 0 or rr >= H - 30) then follow(old)
        else committed, la.commitUntil, due = false, nil, true end
      end
    end
    -- Wedged (300 frames on one cell, no bomb about): search for a bomb
    -- now plus a way out (on foot or a ride) that lives and leaves more
    -- floor than standing would (4-2: an $87 walled P1 in at a lane end
    -- with soft blocks; the bot waited there for a ride until time ran out).
    if wedged and hb and not committed and not bombNear and frame >= (ride.brkNext or 0) then
      ride.brkNext = frame + 120
      local HB = 240
      local bp = { pad("none", true), pad("none", true) }
      local list = { { pad("none") } }
      cat(list, conts(bp, "none", HB)); cat(list, pathCands(bp, 16))
      local res, fl, _, psW = emu.simulateMany(list, HB, false, remoteOn and HB - 30 or 0)
      local best, bestF = nil, fl[1]
      local ox, oy = origin()
      local hx, hy = (r16(P1 + P_X) + ox) & 0xFFFF, (r16(P1 + P_Y) + oy) & 0xFFFF
      for i = 2, #list do
        -- (and P1 off the bombed cell at the end: 4-4, run QA s0, a bomb
        -- laid in the (2,12) dead end "lived" held down against the wall
        -- only in the sim's exact timing; three hearts went there)
        local fx, fy = tostring(psW[i]):match(":(%-?%d+),(%-?%d+)")
        local moved = fx and (math.abs(tonumber(fx) - hx) >= 12 or math.abs(tonumber(fy) - hy) >= 12)
        if res[i] and res[i] == -1 and fl[i] > bestF and moved then best, bestF = list[i], fl[i] end
      end
      if os.getenv("SBM2_DBG") and ride.logMap and not ride.mapDumped then ride.mapDumped = true; ride.logMap() end
      log(string.format("%6d wedged at (%d,%d) %d frames; bomb search: %s", frame, c, r, frame - ride.stillSince,
        best and string.format("+%d floor", bestF - fl[1]) or "none"))
      if best and ride.LADBG then
        local r0, _, _, psb = emu.simulateMany({ best }, HB, false, 0)
        local stay = { pad("none", true), pad("none", true) }
        for f = 3, HB do stay[f] = pad("none") end
        local rs, _, _, pss = emu.simulateMany({ stay }, HB, false, 0)
        local mid = {}
        for f = 1, 120 do mid[f] = best[f] or best[#best] end
        local _, _, _, psm = emu.simulateMany({ mid }, 120, false, 0)
        log(string.format("%6d dbg bomb out: best ends %s (at 120: %s); stand on the bomb: %d ends %s; seq[1..3,25,60] %04X %04X %04X %04X %04X", frame,
          tostring(psb[1]), tostring(psm[1]), rs[1], tostring(pss[1]), best[1] or 0, best[2] or 0, best[3] or 0, best[25] or 0, best[60] or 0))
        local r1 = emu.simulateMany({ best }, HB, false, HB - 30)
        local d = best.path and string.format("path %d wps pre %d", #best.path // 2, #best.pre) or string.format("seq %d", #best)
        log(string.format("%6d dbg bomb out: %s remoteOn %s; lives det0 %d det%d %d; bombs left %d", frame, d, tostring(remoteOn),
          r0[1], HB - 30, r1[1], emu.read(P1 + P_BOMBS_LEFT, WRAM, false)))
      end
      if best then
        follow(best, "wedged; bomb out")
        la.commitUntil = frame + HB
        committed = true
      end
    end
    -- A stall breaker plan in progress was simulated whole: the rest of it
    -- is checked instead, and kept while it lives (4-5, run half45 s0: a
    -- plan to hit a $93 with a remote bomb set off 40 frames after cover
    -- was dropped 7 frames in for the lookahead's own dodge).
    local b = ride.brk
    if threat and due and not committed and b and b.path and not b.walk and frame - b.start < b.H then
      local ox, oy = origin()
      local x0, y0 = (r16(P1 + P_X) + ox) & 0xFFFF, (r16(P1 + P_Y) + oy) & 0xFFFF
      local here2 = { x0, y0 }
      local function from(wp, at)
        local t = {}
        for i = 2 * at + 1, #wp do t[#t + 1] = wp[i] end
        return #t > 0 and t or here2
      end
      local e
      if b.ph == 1 then
        e = { path = from(b.path, b.at), post = b.post, path2 = #b.path2 > 0 and b.path2 or here2, detEnd = b.detEnd, detWait = b.detWait }
      elseif b.ph == 2 then
        local po = {}
        for i = frame - b.p0 + 1, #b.post do po[#po + 1] = b.post[i] end
        e = { path = here2, post = po, path2 = #b.path2 > 0 and b.path2 or here2, detEnd = b.detEnd, detWait = b.detWait }
      elseif b.ph == 3 then
        e = { path = here2, post = {}, path2 = from(b.path2, b.at2), detEnd = b.detEnd, detWait = b.detWait }
      elseif b.ph == 4 then
        e = { path = here2, post = {}, path2 = here2, detEnd = true, detWait = math.max(0, b.p0 - frame) }
      end
      if e then
        local hl = b.heart
        if hl and emu.simHearts then emu.simHearts(false) end
        local res = emu.simulateMany({ e }, H, false, 0)
        if hl and emu.simHearts then emu.simHearts(true) end
        if res[1] and res[1] < 0 then
          committed = true
          la.nextF = frame + 8
        elseif not b.warned then
          b.warned = true
          log(string.format("%6d stall breaker plan dies at +%d now (phase %d)", frame, res[1] or -9, b.ph))
        end
      end
    end
    if threat and due and not committed then
      la.lastF, la.nextF, la.key = frame, frame + 8, key
      if frame % 64 == 0 then la.seenKey = {} end
      la.seenKey[key] = frame + 8
      local pre = {}
      for i = 1, K do pre[i] = myPad end
      -- the bot's move, then any continuation
      local list = conts(pre, myDir, H)
      local cd, t = firstLive(list, H)
      if ride.LADBG and t >= 0 then
        local d1, sx, sy, sf, lk, a8, ex = emu.simulate(pre, t + 1)
        log(string.format("%6d la probe dies %d px(%d,%d) lock %04X a8 %04X exit %04X", frame, d1, sx, sy, lk, a8, ex))
      end
      if ride.LADBG then log(string.format("%6d la check %s H=%d t=%d px(%d,%d) over=%s cand=%s", frame, key, H, t, x, y, tostring(la.over), la.cand and (la.cand.path and "path" or "seq") or "-")) end
      local viaPath = false
      if t >= 0 then
        local c2, t2 = firstLive(pathCands(press and { pad(myDir, true), pad(myDir, true) } or {}, 24), H)
        if t2 < 0 then
          follow(c2, string.format("%s dies at +%d; path%s", key, t, press and " (bomb kept)" or ""))
          cd, t, viaPath = c2, t2, true
        end
      end
      if t < 0 then
        -- the bot keeps control; this is the fallback while it lives
        if not viaPath then la.cand, la.start, la.st, la.over = cd, frame, { at = 0 }, nil end
      else
        local done = false
        if press then
          local c3, t3 = firstLive(cat(conts({}, myDir, H), pathCands({}, 24)), H)
          if t3 < 0 then
            follow(c3, string.format("bomb vetoed (dies at +%d)", t))
            la.veto = frame + 30
            input.a = nil
            done = true
          end
        end
        if not done then
          local old = rest()
          if old then
            local res = emu.simulateMany({ old }, H, false, ride.laDet or 0)
            if res[1] >= 0 then old = nil end
          end
          if old then
            follow(old, string.format("%s dies at +%d; fallback", key, t))
          else
            local c4, t4 = firstLive(cat(pathCands({}, 24), conts({}, myDir, H)), H)
            if t4 >= 0 and hb then
              -- a bomb only when that verifiably lives
              -- (with remote control the new bomb is set off near the end
              -- of the check: 3-2, run NA s0, a bomb laid at (13,11) "lived"
              -- with P1 in the dead end (13,10) above it, walled in for good)
              local det0 = ride.laDet
              if remoteOn and (ride.laDet or 0) == 0 then ride.laDet = 145 end
              local c5, t5 = firstLive(pathCands({ pad("none", true), pad("none", true) }, 16), 175)
              ride.laDet = det0
              if t5 < 0 then c4, t4 = c5, t5 end
            end
            -- (nothing verified to live: the bot's own plan may know a way
            -- the candidates miss; overridden only when its move dies soon)
            if c4 and (t4 < 0 or (t < 40 and t4 > t + 20)) then
              follow(c4, string.format("%s dies at +%d; override %s(%s)", key, t, c4.path and "path " or "",
                t4 < 0 and "lives" or "+" .. t4))
            else
              la.over = nil
            end
          end
        end
      end
    elseif not threat then
      la.over, la.cand = nil, nil
    end
    -- (a rewind (ambush retry) takes frame back past the candidate's start)
    if la.over and la.cand and frame < la.start then la.over, la.cand = nil, nil end
    if la.over and la.cand then
      local cd, t = la.cand, frame - la.start
      local inp
      if cd.path then
        if t < #cd.pre then
          local w = cd.pre[t + 1]
          inp = {}
          if w & 0x0080 ~= 0 then inp.a = true end
          for nm, bit in pairs({ up = 0x0800, down = 0x0400, left = 0x0200, right = 0x0100 }) do if w & bit ~= 0 then inp[nm] = true end end
        else
          inp = pathPad(cd.path, la.st)
        end
      elseif t < #cd then
        local w = cd[t + 1]
        inp = {}
        if w & 0x0080 ~= 0 then inp.a = true end
        for nm, bit in pairs({ up = 0x0800, down = 0x0400, left = 0x0200, right = 0x0100 }) do if w & bit ~= 0 then inp[nm] = true end end
      end
      if inp and ride.LADBG then
        local w = cd.path and -1 or (cd[t + 1] or -2)
        log(string.format("%6d dbg follow t=%d pad %04X seqw %04X px(%d,%d)", frame, t, emu.padBits(inp), w & 0xFFFF, x, y))
      end
      if inp then
        input = inp
        plan, dir, step = "lookahead", next(input) or "none", nil
      else
        la.over = nil
      end
    end
  end
  local s = string.format("%s %s @(%d,%d) px(%d,%d)", plan == "bomb" and "bomb:" .. tostring(bombWhy) or plan, dir or "-", c, r, x, y)
  if ride.origLog then
    local ox, oy = origin(); local k = {}
    for b in pairs(input or {}) do k[#k + 1] = b end
    s = s .. string.format(" org(%d,%d) in %s", ox, oy, table.concat(k, "+"))
  end
  if s ~= lastPlan then
    log(string.format("%6d %s", frame, s)); lastPlan = s
    recent[#recent + 1] = string.format("%d %s", frame, s)
    if #recent > 6 then table.remove(recent, 1) end
  end
end

local function dumpCdl(path)
  if emu.getCdlString then return writeFile(path, emu.getCdlString()) end  -- port/botrun
  local cdl = emu.getCdlData(emu.memType.snesPrgRom)
  local t = {}
  for i = 0, #cdl do t[#t + 1] = string.char(cdl[i] or 0) end
  writeFile(path, table.concat(t))
end

function onNextInstruction(fn)  -- forward-declared above
  local id
  id = emu.addMemoryCallback(function()
    emu.removeMemoryCallback(id, emu.callbackType.exec, 0, 0xFFFFFF)
    fn()
  end, emu.callbackType.exec, 0, 0xFFFFFF)
end

local loaded = LOAD == nil
if loaded then inputlog.start(nil) end
emu.addEventCallback(function()
  -- B (remote detonation) is held 2 frames: the game reads the pad only on
  -- frames it is not lagging, and a 1-frame press every 20 frames (even)
  -- can fall on the skipped frame every time (3-1, run BQ s0: 30k frames
  -- of presses at (15,10), the remote bomb at (14,8) never went off and
  -- every plan waited on it).
  -- (on a copy: a think that does not run leaves `input` as it was)
  local i = {}
  for k, v in pairs(input) do i[k] = v end
  if input.b then BHeld = frame elseif BHeld == frame - 1 then i.b = true end
  -- (no bomb while the stage's enemies are not placed yet: see Blind)
  if Blind then i.a = nil end
  emu.setInput(i, 0)
end, emu.eventType.inputPolled)

local LOGFILE = string.format("%s/bot_%06d.log", OUT, FRAME0)
local function flushLog() writeFile(LOGFILE, table.concat(logLines, "\n") .. "\n") end

local function logMap()
  local rows = {}
  for r = 0, MAP_H - 1 do
    local t = {}
    for c = 0, MAP_W - 1 do t[#t + 1] = string.format("%02X", emu.read(MAP + r * ROW + c * 2, WRAM, false)) end
    rows[#rows + 1] = string.format("  map %2d: %s", r, table.concat(t, " "))
  end
  log(string.format("%6d map", frame) .. "\n" .. table.concat(rows, "\n"))
end

ride.logMap = logMap

-- Savestate + this segment's CDL; the driver resumes from here on a crash.
local function checkpoint()
  onNextInstruction(function()
    writeFile(OUT .. "/ckpt.mss.tmp", emu.createSavestate())
    local logged = inputlog.save(OUT .. "/ckpt.inputs.tmp")
    dumpCdl(string.format("%s/seg_%06d.cdl", OUT, FRAME0))
    flushLog()
    if logged then os.rename(OUT .. "/ckpt.inputs.tmp", OUT .. "/ckpt.inputs")
    else os.remove(OUT .. "/ckpt.inputs") end   -- start state had no sidecar
    os.rename(OUT .. "/ckpt.mss.tmp", OUT .. "/ckpt.mss")
    writeFile(OUT .. "/ckpt.txt", tostring(frame))
  end)
end


ride.flushEvery = tonumber(os.getenv("SBM2_FLUSHEVERY") or "")  -- frames; to watch a slow run
ride.origLog = os.getenv("SBM2_ORIGLOG")  -- log the stage origin and pressed buttons with each plan line
local function onFrame()
  if StopNow then flushLog(); emu.stop(0); return end
  if ride.flushEvery and frame % ride.flushEvery == 0 then flushLog() end
  Boss.frame()
  -- The ending (stage $60): the game is beaten. Let it play a while, save
  -- the run and stop for good (no rollbacks: its locks are not deaths).
  if loaded and emu.read(0x1A, WRAM, false) == 0x60 then
    if not ride.endAt then
      ride.endAt = frame
      log(string.format("%6d THE END (stage $60): game beaten", frame))
      flushLog()
    end
    input = {}
    frame, ticks = frame + 1, ticks + 1
    if frame - ride.endAt >= 600 then
      log(string.format("%6d END 60 ending", frame))
      flushLog()
      dumpCdl(string.format("%s/seg_%06d.cdl", OUT, FRAME0))
      writeFile(OUT .. "/ckpt.txt", tostring(math.max(frame, FRAMES)))  -- drive.py: done
      writeFile(OUT .. "/ending.txt", tostring(ride.endAt))
      inputlog.save(OUT .. "/run.inputs")
      emu.stop(0)
    end
    return
  end
  if not loaded then
    -- botrun has loaded the state and played SBM2_PREFIX (scenario.py's
    -- pads) after it; loading it again would throw the pads away (every
    -- scenario with pads ran from its stage start until this check).
    local prefix = os.getenv("SBM2_PREFIX")
    if prefix and prefix ~= "" then
      inputlog.start(LOAD, prefix)
      loaded = true
      return
    end
    local fh = assert(io.open(LOAD, "rb")); local data = fh:read("a"); fh:close()
    onNextInstruction(function()
      emu.loadSavestate(data)
      inputlog.start(LOAD)   -- the state's sidecar: log from power-on
    end)
    loaded = true
    return
  end
  if rollback and NOREWIND then
    -- (no rewinds: a death is played on from the respawn, a stall too)
    rollback, rollbackBefore = false, nil
  end
  if rollback then
    rollback = false
    ambush = nil
    -- Newest snapshot at least 60 frames before the death, then further
    -- back by one per 3 failures there.
    local i = #snaps
    while i > 1 and snaps[i][1] > math.min(frame - 60, rollbackBefore or frame) do i = i - 1 end
    rollbackBefore = nil
    if i >= 1 then
      i = math.max(1, i - (triesAt[snaps[i][1]] or 0) // 3)
      local snap = snaps[i]
      local tries = (triesAt[snap[1]] or 0) + 1
      -- (5-8: a death there costs the fight, and retries only cost CPU)
      if tries <= (emu.read(0x1A, WRAM, false) == 0x57 and 40 or MAX_TRIES) then
        triesAt[snap[1]] = tries
        for k = #snaps, i + 1, -1 do snaps[k] = nil end
        rollbacks = rollbacks + 1
        log(string.format("%6d rollback to %d (try %d, deaths %d)", frame, snap[1], tries, deaths))
        math.randomseed(rollbacks * 7919 + snap[1] + SEED)
        exploreUntil = snap[1] + 60 * (tries + 1)
        shuffleDirs()
        onNextInstruction(function()
          emu.loadSavestate(snap[2])
          inputlog.truncate(snap[3])
        end)
        frame, step, playing, fuseSeen = snap[1], nil, false, {}
        remoteSince, trappedSince = nil, nil
        return
      end
    end
  end
  -- SBM2_POKE="<frame>:<hex addr>=<hex value>[,...]": 16-bit WRAM writes
  -- once at that frame. Tests only (e.g. 5-8 straight to form 2); not an
  -- assist a run may use.
  local pokeF, pokes = (os.getenv("SBM2_POKE") or ""):match("^(%d+):(.+)$")
  if pokeF and frame >= tonumber(pokeF) and not ride.poked then
    ride.poked = true
    for a, v in pokes:gmatch("(%x+)=(%x+)") do
      emu.write16(tonumber(a, 16), tonumber(v, 16), WRAM)
      log(string.format("%6d test poke %s=%s", frame, a, v))
    end
  end
  local saveF, savePath = (os.getenv("SBM2_SAVE_AT") or ""):match("^(%d+):(.+)$")
  if saveF and frame >= tonumber(saveF) and not ride.savedAt then  -- once (a rollback may skip the frame)
    ride.savedAt = frame
    onNextInstruction(function() inputlog.saveState(savePath, emu.createSavestate()) end)
  end
  if not NOREWIND and frame % SNAP_EVERY == 0 and (r16(P1 + P_LOCK) & 1) == 0
      and (#snaps == 0 or snaps[#snaps][1] < frame) then
    local f = frame
    onNextInstruction(function()
      snaps[#snaps + 1] = { f, emu.createSavestate(), inputlog.mark() }
      if #snaps > 40 then table.remove(snaps, 1) end
    end)
  end
  if ambush and frame >= ambush[1] + AMBUSH_FRAMES then
    if liveEnemies() < ambush[3] or score() > ambush[5] or (Boss.hp and ambush[7] and Boss.hp < ambush[7]) then
      ambushHits = ambushHits + 1
      log(string.format("%6d ambush hit (try %d; %d/%d)", frame, ambush[4], ambushHits, ambushes))
      ambush = nil
    elseif ambush[4] < AMBUSH_TRIES and ambush[2] and not NOREWIND then
      ambush[4] = ambush[4] + 1
      local snap = ambush
      for k = #snaps, 1, -1 do if snaps[k][1] > snap[1] then snaps[k] = nil end end
      shuffleDirs()
      onNextInstruction(function()
        emu.loadSavestate(snap[2])
        inputlog.truncate(snap[6])
      end)
      frame, step, playing, fuseSeen = snap[1], nil, false, {}
      ambushHold = frame + math.random(8, 60)
      return
    else
      log(string.format("%6d ambush gave up", frame))
      ambush, ambushHold = nil, frame + 30
    end
  end
  local t0 = os.clock()
  local ok, err = pcall(think)
  if ambushStart then
    ambushStart = false
    if not ambush then
      ambushes = ambushes + 1
      local a = { frame, nil, liveEnemies(), 0, score(), nil, Boss.hp }
      ambush = a
      if not NOREWIND then onNextInstruction(function() a[2], a[6] = emu.createSavestate(), inputlog.mark() end) end
    end
  end
  if not ok then log("error: " .. tostring(err)) end
  local dt = os.clock() - t0
  if dt > 0.05 then log(string.format("%6d slow think %.3fs", frame, dt)) end
  if logMapNext then logMapNext = false; logMap() end
  if shots[frame] then
    writeFile(string.format("%s/f%06d.png", OUT, frame), emu.takeScreenshot())
    logMap()
  end
  frame, ticks = frame + 1, ticks + 1
  if gameOver then
    -- Stop here; drive.py restarts from the world's start state.
    flushLog()
    dumpCdl(string.format("%s/seg_%06d.cdl", OUT, FRAME0))
    writeFile(OUT .. "/ckpt.txt", tostring(frame))
    inputlog.save(OUT .. "/run.inputs")
    emu.stop(6)
  elseif frame >= FRAMES then
    -- For bot/bench.py: stage, score/soft blocks/panels left, live enemies.
    log(string.format("%6d END %02X %s %d", frame, emu.read(0x1A, WRAM, false), progressSig or "-", liveEnemies()))
    flushLog()
    dumpCdl(string.format("%s/seg_%06d.cdl", OUT, FRAME0))
    writeFile(OUT .. "/ckpt.txt", tostring(frame))
    inputlog.save(OUT .. "/run.inputs")
    emu.stop(0)
  elseif (frame - FRAME0) % CKPT == 0 then
    checkpoint()
  end
end

emu.addEventCallback(function()
  local ok, err = pcall(onFrame)
  if not ok then
    log(string.format("%6d fatal: %s", frame, tostring(err)))
    flushLog()
    inputlog.save(OUT .. "/run.inputs")
    emu.stop(4)
  end
end, emu.eventType.endFrame)

-- bot/test (unit tests on WRAM images, outside Mesen): the planner's parts.
-- No locals here: the main chunk is near Lua's 200-local limit.
if BOT_TEST then
  BOT_TEST.bot = {
    readMap = readMap, walkable = walkable, bfs = bfs, regionOf = regionOf, stagePlan = stagePlan,
    blastCells = blastCells, blastWindows = blastWindows, bombEffect = bombEffect, panels = panels,
    heartCells = heartCells, playerTile = playerTile, structSig = structSig, steer = steer,
    log = logLines, frame = function() return frame end,
    KEY_W = KEY_W, MAP_W = MAP_W, MAP_H = MAP_H, MAP = MAP, ROW = ROW, P1 = P1,
  }
end
