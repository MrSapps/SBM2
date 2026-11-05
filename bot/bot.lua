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
      local t = (flags & 0x40) ~= 0 and 0 or fuse * 2
      local now, seen = r16(0x36), fuseSeen[i]  -- NmiCount as the clock
      if not seen or seen[1] ~= fuse or seen[3] ~= cell then
        fuseSeen[i] = { fuse, now, cell }
      elseif (now - seen[2]) & 0xFFFF > FROZEN and t > 0 then
        t = 100000
      end
      if remoteOn and t > 0 then t = REMOTE_T end
      -- Blast length = +7 + 1 (C0:3386).
      local b = { c, r, t, emu.read(base + 7, WRAM, false) + 1, remoteOn and t == REMOTE_T }
      list[#list + 1] = b
      byCell[r * KEY_W + c] = b
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
local function blastCells(g, c, r, p)
  local cells = { { c, r } }
  for _, dir in ipairs(DIRS) do
    for k = 1, p do
      local cc, rr = c + dir[1] * k, r + dir[2] * k
      if rr < 0 or rr >= MAP_H or cc < 0 or cc >= MAP_W then break end
      local t = g[rr][cc]
      if t >= 0x80 and t ~= GATE_OPEN then
        if BLAST_HITS[t] then cells[#cells + 1] = { cc, rr, dir } end
        if t == BARREL then
          -- BarrelExplode C1:26AA: range-1 blasts on the barrel and on the
          -- cells 2 away (not where the tile has bit 6 set).
          cells[#cells + 1] = { cc, rr }
          for _, d in ipairs(DIRS) do
            local bc, br = cc + 2 * d[1], rr + 2 * d[2]
            local centres = { { cc, rr } }
            if g[br] and g[br][bc] and (g[br][bc] & 0x40) == 0 then centres[2] = { bc, br } end
            for _, ctr in ipairs(centres) do
              for _, d2 in ipairs(DIRS) do
                local nc, nr = ctr[1] + d2[1], ctr[2] + d2[2]
                if g[nr] and g[nr][nc] and (g[nr][nc] < 0x80 or BLAST_HITS[g[nr][nc]]) then
                  cells[#cells + 1] = { nc, nr }
                end
              end
              cells[#cells + 1] = ctr
            end
          end
        end
        break
      end
      cells[#cells + 1] = { cc, rr, dir }
      if t < 0x80 and BLAST_ENDS[t & 0x3F] then break end
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
      for _, cell in ipairs(blastCells(g, b[1], b[2], b[4])) do
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
      for _, cell in ipairs(blastCells(g, b[1], b[2], b[4])) do
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
-- Blast length of a bomb P1 places at (c,r): standing on $14 gives 9
-- (world 1, docs/mechanics.md).
local function rangeAt(g, c, r) return g[r][c] == 0x14 and math.max(RANGE, 9) or RANGE end

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
  local cells = blastCells(g, c, r, rangeAt(g, c, r))
  for _, cell in ipairs(cells) do add(cell[2] * KEY_W + cell[1]) end
  for i, b in ipairs(lastBombs) do
    if lastTimes[i] > tfire then
      for _, cell in ipairs(cells) do
        if cell[1] == b[1] and cell[2] == b[2] then
          for _, bc in ipairs(blastCells(g, b[1], b[2], b[4])) do add(bc[2] * KEY_W + bc[1]) end
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
      if land then
        local lk = land[2] * KEY_W + land[1]
        local t = t0 + (n + 6) * FPT  -- ~20 frames on the pad + ~70 in the air
        if (walkable(g, land[1], land[2]) or (g[land[2]] and g[land[2]][land[1]] == SPRING))
            and not (windows and hit(windows, lk, t - FPT, t + FPT)) then
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
              if not ride.fail[k0 .. ">" .. (e[2] * KEY_W + e[1])] then
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
              if region[bk] and bt ~= 0x66 and walkable(grid, bc, br) and not (ctx.noBomb and ctx.noBomb(bk, bt)) then
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
      if yieldable and os.clock() - slice0 > ctx.slice then coroutine.yield(); slice0 = os.clock() end
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
  while #heap > 0 do
    local s = heapPop(heap)
    if s.terminal then return shorten(steps(s.state), true), exp, true end
    if s.depth > 0 then
      local enemy = false
      for k in pairs(s.region) do
        if not reach0[k] then
          local c, r = k % KEY_W, k // KEY_W
          if ctx.objGoal(c, r, s.grid) then return shorten(steps(s), false), exp end
          if ctx.enemyGoal and ctx.enemyGoal(c, r) then enemy = true end
        end
      end
      if enemy then heapPush(heap, { terminal = true, state = s, f = (s.g + ENEMY_PEN) * 1000 }) end
    end
    exp = exp + 1
    if exp > ctx.maxExp then return nil, exp end
    if yieldable and os.clock() - slice0 > ctx.slice then
      coroutine.yield()
      slice0 = os.clock()
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
  if dir == "up" or dir == "down" then
    if x < cx - STEER_SLACK then return "right" elseif x > cx + STEER_SLACK then return "left" end
  else
    if y < cy - STEER_SLACK then return "down" elseif y > cy + STEER_SLACK then return "up" end
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
local throw = { face = nil, faceFrames = 0, liftAt = nil, dodgeAt = -100 }  -- (also the dodge hold)
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
math.randomseed(SEED)
local function think()
  input = {}
  -- Locked ($52 bit 0): dying, on the exit, warping, a boss's freeze, ...
  -- Only a death is a death: C0:A675 sets $52 = $0801 (docs/enemies.md).
  local lockv = r16(P1 + P_LOCK)
  local heartsNow = emu.read(P1 + 0xEF, WRAM, false)
  if heartsNow ~= lastHearts then
    log(string.format("%6d hearts %02X -> %02X", frame, lastHearts or 0, heartsNow)); lastHearts = heartsNow
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
    -- Step off onto the far end's $36 when the platform stops beside it.
    local rc, rr = playerTile()
    for _, d in ipairs(DIRS) do
      local cc, r2 = rc + d[1], rr + d[2]
      if (cc ~= ride.from[1] or r2 ~= ride.from[2]) and r2 >= 0 and cc >= 0
          and emu.read(MAP + r2 * ROW + cc * 2, WRAM, false) == BOARD then
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
  end
  -- On a pad by pixel position: +$A8 still names the previous cell for a
  -- few frames after a landing.
  local pc0, pr0 = playerTile()
  local onPad = emu.read(MAP + pr0 * ROW + pc0 * 2, WRAM, false) == SPRING
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
      local best, bestN
      for _, d in ipairs(DIRS) do
        for _, v in ipairs({ "", "!" }) do
          local land = springLand[(pr0 * KEY_W + pc0) .. d[3] .. v]
          if land == nil then land = ride.springGuess(g0, pc0, pr0, d, v == "!") end
          if land and (land[1] ~= pc0 or land[2] ~= pr0) and not rc[land[2] * KEY_W + land[1]] then
            local _, n = bfs(g0, land[1], land[2], work)
            if next(rc) and walkable(g0, land[1], land[2]) then n = (n or 1000) - 10000 end
            if n and (not bestN or n < bestN) then best, bestN = d[3] .. v, n end
          end
        end
      end
      if best then springDir = best end
    end
    if onPad and not springDir then springDir = DIRS[math.random(4)][3] end  -- not planned: any way off
    -- An enemy at the landing (3-6's left side: the same contact death on
    -- every retry): bounce in place (nothing held; up to 25 times: the 3-6 one sits
    -- there for a ~1070-frame timer, +$4E) and decide again.
    if onPad and not springFrom and springDir and springDir ~= "stay" then
      local d = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[springDir:match("^%a+")]
      local land = springLand[(pr0 * KEY_W + pc0) .. springDir]
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
        for _, e in ipairs(enemyTiles()) do
          if math.abs(e[1] - land[1]) + math.abs(e[2] - land[2]) <= 2 then
            log(string.format("%6d pad (%d,%d): enemy near the landing (%d,%d); bouncing", frame, pc0, pr0, land[1], land[2]))
            springDir, ride.stays = "stay", ride.stays + 1
            break
          end
        end
      end
      if springDir ~= "stay" then ride.stays = 0 end
    end
    if onPad and not springFrom and springDir then
      if os.getenv("SBM2_DBG") then log(string.format("%6d dbg pad (%d,%d) jump %s (last plan %s)", frame, pc0, pr0, springDir, tostring(ride.lastPlan))) end
      springFrom = (pr0 * KEY_W + pc0) .. springDir
    end
    -- Hold it only on the pad: held in the air the flight drifts on (3-1:
    -- (9,6) Down held lands (9,12); let go after the launch, (9,9)).
    -- A "!" jump keeps it held in the air too (it flies a cell further).
    if springDir and springDir ~= "stay" and ((lockv ~= 0x1801 and lockv ~= 0x0809) or springDir:sub(-1) == "!") then
      input[springDir:match("^%a+")] = true
    end
    step = false
    return
  end
  if springFrom and (lockv & 1) == 0 then
    local c1, r1 = playerTile()
    if not springLand[springFrom] then
      log(string.format("%6d spring %s landed (%d,%d)", frame, springFrom, c1, r1))
    end
    springLand[springFrom] = { c1, r1 }
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
      log(string.format("%6d DEATH %s at px(%d,%d) enemies %s", frame, cause,
        r16(P1 + P_X), r16(P1 + P_Y), table.concat(es, " ")) .. string.format(" stats[E0..FF] %s after: %s",
        table.concat(st, " "), table.concat(recent, " | ")))
      writeFile(string.format("%s/death_%06d.png", OUT, frame), emu.takeScreenshot())
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
  if stNow ~= savedStage then
    savedStage = stNow
    softSeen, softGone, springLand, springTry, ride.fail, ride.edges, ride.tried = {}, {}, {}, nil, {}, {}, {}
    badCell, planSteps, planCo, noBombUntil = {}, nil, nil, {}
    stageTicks = ticks
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
    onNextInstruction(function()
      local data = emu.createSavestate()
      inputlog.saveState(OUT .. "/stage.mss", data)
      -- A library of stage starts (bot/bench.py collects them).
      local lib = string.format("%s/stage_%02X.mss", OUT, stNow)
      local fh = io.open(lib, "rb")
      if fh then fh:close() else inputlog.saveState(lib, data) end
    end)
  end
  -- Lives assist (logged): a game over restarts the whole world, so keep
  -- lives ($1C6C) topped up; deaths are still retried by rollback first.
  if emu.read(0x1C6C, WRAM, false) < 2 then
    inputlog.write16(0x1C6C, (r16(0x1C6C) & 0xFF00) | 3)
    local msg = string.format("frame %d stage %02X: lives assisted", frame, emu.read(0x1A, WRAM, false))
    log("ASSIST " .. msg)
    local fh = io.open(OUT .. "/assists.txt", "a"); if fh then fh:write(msg .. "\n"); fh:close() end
  end
  lastWorld = (emu.read(0x1A, WRAM, false) >> 4) - 1  -- $1A = world<<4 | stage-1
  local g = readMap()
  -- Exit gate: map index at $7E5D72; open once $7E5D94 bit 15 (all gold
  -- panels blasted, or no panels: $FFFF) is set (checked at C1:41xx).
  local gate = r16(0x5D72)
  local gc, gr = (gate & 0x7F) // 2, gate >> 7
  local gateOpen = g[gr] ~= nil and gate ~= 0 and g[gr][gc] == 0x49  -- open gate tile (C1:41A8 via $09)
  -- 5-8 shows an open gate tile at (8,3) before the boss comes, with a
  -- $C0 in it that kills on contact: no way out there, keep off it.
  if emu.read(0x1A, WRAM, false) == 0x57 and gateOpen then gateOpen = false; g[gr][gc] = 0xC9 end
  -- World 5 without the glove: 5-8 form 2 is hurt only by thrown bombs and
  -- its arena has no blocks, so one must be carried in (5-7 hides one, item
  -- kind 7, at (13,10)). While one is hidden, the gate counts as shut (a
  -- route through it would leave the stage).
  local gloveHidden = false
  if emu.read(0x1A, WRAM, false) >= 0x50 and emu.read(P1 + 0xFE, WRAM, false) == 0 then
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
  if gloveHidden and gateOpen then gateOpen = false; g[gr][gc] = 0xC9 end
  if gateOpen then g[gr][gc] = GATE_OPEN end
  RANGE = emu.read(P1 + P_FIRE, WRAM, false) + 1
  remoteOn = (emu.read(P1 + 0xF3, WRAM, false) & 1) ~= 0
  local c, r, x, y = playerTile()
  local windows = blastWindows(g)
  local danger = {}
  for k in pairs(windows) do danger[k] = true end
  -- Enemies: predicted paths give contact windows (eWin) and kill chances.
  -- enemies/walkers (cells) still drive the exit and hunt plans.
  local elist = Enemies.read(g)
  jumpObj = {}
  for _, e in ipairs(elist) do
    if e.type == 0xE0 or e.type == 0xA0 then
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
  end
  local bosses = Boss.read()
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
          b.box and " box" or "", b.sense and " sense" or "")
      end
      log(string.format("%6d bosses %s", frame, table.concat(t, " ")))
    end
  end
  local cw = combine(windows, eWin)   -- blasts + enemies, for routes
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
  local enemyNow = {}
  for k in pairs(bossNow) do enemyNow[k] = true end
  for _, e in ipairs(elist) do
    if e.harmful then
      enemyNow[e.r * KEY_W + e.c] = true
      if e.moving then enemyNow[e.tr * KEY_W + e.tc] = true end
    end
  end
  -- Cells an enemy may be in within the next 1.5 tiles' walk: never step there.
  local nearEnemy = {}
  for k, l in pairs(eWin) do
    for _, win in ipairs(l) do
      if win[1] <= FPT * 1.5 and win[2] >= 0 then nearEnemy[k] = true; break end
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
    local reach, list, head = { [r * KEY_W + c] = true }, { { c, r } }, 1
    while list[head] do
      local cc, rr = list[head][1], list[head][2]; head = head + 1
      for _, d in ipairs(DIRS) do
        local c2, r2 = cc + d[1], rr + d[2]
        local k = r2 * KEY_W + c2
        if not reach[k] and walkable(g, c2, r2) then reach[k] = true; list[#list + 1] = { c2, r2 } end
      end
    end
    local basis = {}  -- pivot bit -> {mask, set of spot keys}
    local function xorSet(a, b)
      local o = {}
      for k in pairs(a) do o[k] = true end
      for k in pairs(b) do o[k] = not o[k] or nil end
      return o
    end
    for _, cell in ipairs(list) do
      -- Only cells a bomb can be put on (not stairs, not tiles where one
      -- failed to appear, e.g. the $74 panels themselves).
      local ct = g[cell[2]][cell[1]]
      local ck = cell[2] * KEY_W + cell[1]
      if ct == 0x66 or (noBombTile[ct] or 0) >= 1 or (noBombUntil[ck] or 0) > frame then goto nextCell end
      do
      local m = 0
      for _, bc in ipairs(blastCells(g, cell[1], cell[2], rangeAt(g, cell[1], cell[2]))) do
        local i = idx[bc[2] * KEY_W + bc[1]]
        if i then m = m | (1 << i) end
      end
      local set = { [cell[2] * KEY_W + cell[1]] = true }
      for bit = np - 1, 0, -1 do
        if m & (1 << bit) ~= 0 then
          if basis[bit] then m, set = m ~ basis[bit][1], xorSet(set, basis[bit][2])
          else basis[bit] = { m, set }; break end
        end
      end
      end
      ::nextCell::
    end
    local t, sol = target, {}
    for bit = np - 1, 0, -1 do
      if t & (1 << bit) ~= 0 then
        if not basis[bit] then sol = nil; break end
        t, sol = t ~ basis[bit][1], xorSet(sol, basis[bit][2])
      end
    end
    if sol and next(sol) then panelPlan = sol end
  end
  local here = r * KEY_W + c
  -- Remote bombs: cells their blasts would cover, and whether setting them
  -- off now would hit anything worth it.
  local remoteCells, remoteWorth = {}, false
  for _, b in ipairs(lastBombs) do
    if b[5] then
      for _, cell in ipairs(blastCells(g, b[1], b[2], b[4])) do
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
    -- Not on a spring pad (the one free frame after a landing: the press
    -- did nothing and 3-6 went round its pads for good) or locked.
    -- (5-8's slam: a ball near P1 is no reason to wait, the body rises)
    if remoteWorth and not remoteCells[here] and (not nearEnemy[here] or ride.f1Home) and frame >= nextDetonate
        and g[r][c] ~= SPRING and (r16(P1 + P_LOCK) & 1) == 0 then
      input.b = true
      nextDetonate = frame + 20
      log(string.format("%6d detonate%s", frame, emu.read(0x1A, WRAM, false) == 0x57 and
        string.format(" (body %02X%04X at %d,%d hp %d)", emu.read(0x1002, WRAM, false), r16(0x1000), r16(0x1011), r16(0x1014), r16(0x1020)) or ""))
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
    if not SOFT[g[k // KEY_W][k % KEY_W]] then softGone[k] = true end
  end
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
  if not seenSigs[sig] then seenSigs[sig] = true; lastProgress, deathsAtProgress = ticks, deaths end
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
    local es = {}
    for k in pairs(panelTodo) do es[#es + 1] = string.format("panel@(%d,%d)", k % KEY_W, k // KEY_W) end
    for _, d in ipairs(dormant) do es[#es + 1] = string.format("dormant@(%d,%d)", d[1], d[2]) end
    for _, e in ipairs(elist) do
      es[#es + 1] = string.format("%02X@(%d,%d)%s%s%s", e.type, e.c, e.r, e.killable and "k" or "",
        e.harmful and "h" or "", e.gadget and "g" or "")
    end
    log(string.format("%6d stalled (%s); exploring; gate $%04X (%d,%d)%s; enemies %s", frame, sig, gate, gc, gr,
      gateOpen and " open" or "", table.concat(es, " ")))
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
  local function safeGoal(w, t0)
    return function(cc, rr, n)
      local k = rr * KEY_W + cc
      local t = (t0 or 0) + n * FPT
      return not w[k] and not hit(eWin, k, t - FPT, t + 90)
    end
  end

  allowRide = not danger[here]
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
  if f1plan then
    -- (5-8 form 1, above)
  elseif danger[here] then
    plan = "flee"
    dir = bfs(g, c, r, safeGoal(windows), nearEnemy, cw)
      or bfs(g, c, r, safeGoal(windows), enemyNow, windows)
      or bfs(g, c, r, function(cc, rr) return not windows[rr * KEY_W + cc] end, enemyNow)
  elseif nearEnemy[here] or (frame - throw.dodgeAt < 12 and hit(eWin, here, 0, 90)) then
    -- Keep at a dodge for a few frames: nearEnemy flickers as predictions
    -- shift, and turning back each other frame is how the 5-8 body corners us.
    if nearEnemy[here] then throw.dodgeAt = frame end
    plan = "dodge"
    dir = bfs(g, c, r, safeGoal(windows), nil, cw)
      or bfs(g, c, r, function(cc, rr)
        return not nearEnemy[rr * KEY_W + cc] and not danger[rr * KEY_W + cc]
      end, nil, windows)
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
    local function targetsInReach(cc, rr, t)
      if g[rr][cc] == 0x66 then return nil end  -- no bombs on stairs
      if (noBombUntil[rr * KEY_W + cc] or 0) > frame then return nil end
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
        elseif panelDone[k] then return nil end
        -- $68: lighting it ($6F) is what kills 3-1's immune $8F enemies.
        if panelTodo[k] or (SOFT[tt] and not softGone[k]) or tt == BARREL or isSwitchOff(tt) or tt == 0x68 or tt == 0x6B or tt == 0x6A or tt == 0x4B or tt == 0x63 then why = "obj" end
        -- A blast pushes an $E2 block one cell on; into a $D3 hole it fills
        -- it and opens the way (docs/mechanics.md, push blocks).
        -- While a plan is on, only its own bombs move blocks.
        if tt == 0xE2 and planSteps and pushCell ~= rr * KEY_W + cc then return nil end
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
    local function canEscape(cc, rr, t)
      local w = withBomb(g, windows, cc, rr, NEW_FUSE + t)
      -- With the remote, the end cell must also be clear of the remote
      -- bombs' blasts, or setting them off later would mean dying.
      local safe = safeGoal(w, t)
      local goal = remoteOn and function(c2, r2, n)
        return safe(c2, r2, n) and not remoteCells[r2 * KEY_W + c2]
      end or safe
      -- Rides don't count (the platform comes when it comes), unless it is
      -- here: a $DC within 2 cells of the bomb (4-4, 4-2: a lane end was
      -- the only way out of a column the blast fills).
      allowRide = false
      for _, e in ipairs(elist) do
        if e.type == 0xDC and math.abs(e.c - cc) + math.abs(e.r - rr) <= 2 then allowRide = true end
      end
      local ok = bfs(g, cc, rr, goal, nil, combine(w, eWin), t) ~= nil
      allowRide = true
      return ok
    end
    -- With the remote, one bomb at a time: a remote bomb is never danger, so
    -- a second one placed nearby can seal us in between them.
    local haveBomb = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 and not (remoteOn and next(remoteCells))
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
        end
      end
    end
    pushCell, trapCell = nil, nil
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
          ok = reachNow[st.k] and #changes > 0 and keyChanges(changes, g) == st.key
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
    if not planSteps and not planCo and settled and frame - lastObjFrame > 240 and frame >= pushNext then
      pushNext = frame + 120
      local anyWalkerIn = false
      local reachNow = regionOf(g, c, r)
      for _, e in ipairs(walkers) do if reachNow[e[2] * KEY_W + e[1]] then anyWalkerIn = true end end
      local hearts = emu.read(P1 + 0xEF, WRAM, false)
      local flashing = (r16(P1 + 4) & 0x20) ~= 0 and emu.read(P1 + 0xB9, WRAM, false) > 120
      local gc0 = {}
      for rr = 0, MAP_H - 1 do local row = {}; for cc = 0, MAP_W - 1 do row[cc] = g[rr][cc] end; gc0[rr] = row end
      local pTodo, pDone, sGone, wk = panelTodo, panelDone, softGone, walkers
      local ctx = {
        objGoal = function(cc, rr, grid)
          local k = rr * KEY_W + cc
          local t = grid[rr][cc]
          if t == GATE_OPEN or t == ITEM or pTodo[k] or t == 0x68 or t == 0x6B then return true end
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
    local why = haveBomb and targetsInReach(c, r, 0)
    if why == "enemy" and frame < ambushHold then why = nil end
    if os.getenv("SBM2_DBG") and frame % 60 == 0 then
      log(string.format("%6d dbg here (%d,%d) haveBomb=%s why=%s esc=%s left=%d remote=%s", frame, c, r, tostring(haveBomb),
        tostring(why), tostring(why and canEscape(c, r, 0)), emu.read(P1 + P_BOMBS_LEFT, WRAM, false), tostring(next(remoteCells) ~= nil)))
    end
    if why and canEscape(c, r, 0) then
      plan, bombWhy = "bomb", why
      if pushCell == here then ride.planBomb = here end  -- the planner's bomb (remote: set it off soon)
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
    elseif remoteCells[here] and (remoteWorth or not haveBomb) then
      plan = "clear"
      dir = bfs(g, c, r, function(cc, rr) return not remoteCells[rr * KEY_W + cc] and not windows[rr * KEY_W + cc] end,
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
      -- An open, unblocked gate is the way out whatever is still alive
      -- (PlayerOnExitTile C1:41A8 checks only Exit_Blocked and the panels;
      -- 2-1's furnace never stops making enemies).
      -- A heart or glove lying in reach comes before the exit.
      plan = "item"
      dir = bfs(g, c, r, function(cc, rr) return g[rr][cc] == ITEM and heartNow[rr * KEY_W + cc] == "item" end, nearEnemy, cw)
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
        for _, want in ipairs({ "obj", "enemy" }) do
          local goalK
          dir = bfs(g, c, r, function(cc, rr, n)
            if (cc == c and rr == r) or danger[rr * KEY_W + cc] then return false end
            local why = targetsInReach(cc, rr, n * FPT)
            local ok = why == want and (want == "obj" or frame >= ambushHold)
              and canEscape(cc, rr, n * FPT)
            if ok then goalK = rr * KEY_W + cc end
            return ok
          end, nearEnemy, cw)
          if dir then
            if os.getenv("SBM2_DBG") and goalK then log(string.format("%6d dbg seek %s at (%d,%d)", frame, want, goalK % KEY_W, goalK // KEY_W)) end
            -- A planned step is not a target of its own: the planner keeps
            -- going after it.
            if want == "obj" and goalK ~= pushCell then lastObjFrame = frame end
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
        dir = bfs(g, c, r, function(cc, rr)
          for _, e in ipairs(walkers) do
            if math.abs(e[1] - cc) + math.abs(e[2] - rr) <= 3 then return true end
          end
          return false
        end, nearEnemy, cw)
      end
      if not dir and plan ~= "slam" then
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
        if try and (frame - try[3] > 90 or ride.from) then ride.tried[try[1] .. try[2]] = true; ride.try, try = nil, nil end
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
                  if cc + d[1] == e.c and rr + d[2] == e.r and not ride.tried[(rr * KEY_W + cc) .. d[3]] then
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
      if frame < stallUntil and plan ~= "bomb" and plan ~= "spring" and plan ~= "trap" and plan ~= "board" then dir = nil end
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

  -- A route over a platform lane starts here: wait on the $36 for the ride.
  if dir == "ride" then
    plan, dir, step = "ride", nil, nil
    if not ride.wait or ride.wait[1] ~= here then ride.wait = { here, frame } end
    if frame - ride.wait[2] > 600 then
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
        if e.type == 0xDC and e.c == c + dc and e.r == r + dr then
          input = { [(dc == 1 and "right") or (dc == -1 and "left") or (dr == 1 and "down") or "up"] = true }
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
  if dir and not (step and plan ~= "flee" and plan ~= "dodge") then
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
    if math.abs(x - tx) <= 1 and math.abs(y - ty) <= 1 then
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
      input[steer(step.dir, x, y, step.c, step.r)] = true
      -- Once on the target tile, keep moving toward its centre.
      if c == step.tc and r == step.tr then
        input = { a = input.a, b = input.b }
        if x < tx - 1 then input.right = true elseif x > tx + 1 then input.left = true end
        if y < ty - 1 then input.down = true elseif y > ty + 1 then input.up = true end
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
    if holding then
      input.a = true
      -- A throw hits if the bomb is inside the body's +-24 box (CC:97E8)
      -- on some frame of its flight; turning first costs ~2 frames.
      local want
      for d, path in pairs(THROW_PATH) do
        local delay = (throw.face == d and throw.faceFrames >= 2) and 0 or 3
        for k, o in ipairs(path) do
          local px, py = bx, by
          if body then px, py = Boss.predict(body, k + delay) end
          if math.abs(x + o[1] - px) <= 22 and math.abs(y + o[2] - py) <= 22 then want = d; break end
        end
        if want then break end
      end
      if want then
        if throw.face == want and throw.faceFrames >= 2 then
          input.a = nil
          log(string.format("%6d throw %s at boss (%.0f,%.0f) from px(%d,%d)", frame, want, bx, by, x, y))
          throw.faceFrames = 0
        else
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
          and emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0 then
        input.a = frame % 2 == 0
        throw.liftAt = frame
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
  if frame - frozenAt[3] > 600 and timeLeft() > 1 then
    log(string.format("%6d frozen at (%d,%d) since %d; rolling back", frame, c, r, frozenAt[3]))
    rollback, rollbackBefore = true, frozenAt[3] - 100
    if lastRide and frozenAt[3] - lastRide[3] < 120 then
      badCell[lastRide[2] * KEY_W + lastRide[1]] = true
      rollbackBefore = lastRide[3] - 300
      log(string.format("%6d froze after the ride from (%d,%d): not again", frame, lastRide[1], lastRide[2]))
    end
    frozenAt = { x, y, frame }
  end
  if os.getenv("SBM2_DBG") and input.a and plan ~= "bomb" then log(string.format("%6d dbg A pressed in plan %s", frame, tostring(plan))) end
  local s = string.format("%s %s @(%d,%d) px(%d,%d)", plan == "bomb" and "bomb:" .. tostring(bombWhy) or plan, dir or "-", c, r, x, y)
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
emu.addEventCallback(function() emu.setInput(input, 0) end, emu.eventType.inputPolled)

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


local function onFrame()
  Boss.frame()
  if not loaded then
    local fh = assert(io.open(LOAD, "rb")); local data = fh:read("a"); fh:close()
    onNextInstruction(function()
      emu.loadSavestate(data)
      inputlog.start(LOAD)   -- the state's sidecar: log from power-on
    end)
    loaded = true
    return
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
      if tries <= MAX_TRIES then
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
  if frame % SNAP_EVERY == 0 and (r16(P1 + P_LOCK) & 1) == 0
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
    elseif ambush[4] < AMBUSH_TRIES and ambush[2] then
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
      onNextInstruction(function() a[2], a[6] = emu.createSavestate(), inputlog.mark() end)
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
