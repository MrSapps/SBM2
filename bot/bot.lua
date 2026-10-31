local WATCH_LUA = os.getenv("SBM2_WATCH_LUA")
if WATCH_LUA and WATCH_LUA ~= "" then dofile(WATCH_LUA) end
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
    if (r16(base) & 0xF000) == 0x9000 and (r16(base + 4) & 0x8000) == 0 then
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
local function isSwitchOff(t) return false end

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

local function readMap()
  local g = {}
  for r = 0, MAP_H - 1 do
    g[r] = {}
    for c = 0, MAP_W - 1 do
      local t = emu.read(MAP + r * ROW + c * 2, WRAM, false)
      g[r][c] = t
      if not (EMPTY[t] or SOFT[t] or HARD[t] or isBomb(t) or isFlame(t)) and not seenUnknown[t] then
        seenUnknown[t] = true
        log(string.format("unknown tile %02X at (%d,%d)", t, c, r))
      end
    end
  end
  return g
end

local function walkable(g, c, r)
  if r < 0 or r >= MAP_H or c < 0 or c >= MAP_W then return false end
  local t = g[r][c]
  -- $04-$3F: floor variants; some ($0B $0C $17-$1A $23) run a handler when
  -- stood on (C1:58DE table): conveyors/warps, not hazards as far as known.
  -- $40-$7F are the same floor codes with flag bits (the tile handler only
  -- looks at tile & $3F); $40-$43 are bombs.
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
      -- Blast length = +7 + 1 (C0:3386).
      local b = { c, r, t, emu.read(base + 7, WRAM, false) + 1 }
      list[#list + 1] = b
      byCell[r * KEY_W + c] = b
    end
  end
  -- Bombs drawn on the map but missing from the table: assume they're close.
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      if isBomb(g[r][c]) and not byCell[r * KEY_W + c] then
        local b = { c, r, 30, RANGE }
        list[#list + 1] = b
        byCell[r * KEY_W + c] = b
      end
    end
  end
  return list, byCell
end

-- Cells a bomb at (c,r) with power p reaches.
local function blastCells(g, c, r, p)
  local cells = { { c, r } }
  for _, dir in ipairs(DIRS) do
    for k = 1, p do
      local cc, rr = c + dir[1] * k, r + dir[2] * k
      if rr < 0 or rr >= MAP_H or cc < 0 or cc >= MAP_W then break end
      local t = g[rr][cc]
      if HARD[t] and not BLAST_STOP[t] then break end
      cells[#cells + 1] = { cc, rr }
      if SOFT[t] or BLAST_STOP[t] then break end
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
    for _, cell in ipairs(blastCells(g, b[1], b[2], b[4])) do
      add(cell[2] * KEY_W + cell[1], times[i] - 4, times[i] + FLAME_FRAMES)
    end
  end
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      if isFlame(g[r][c]) then add(r * KEY_W + c, -FLAME_FRAMES, FLAME_FRAMES // 2) end
    end
  end
  lastBombs, lastTimes = bombs, times
  return w
end

-- windows (from blastWindows(g) this frame) plus a bomb placed at (c,r)
-- going off at tfire, or earlier if an existing blast reaches it. Bombs it
-- reaches go off with it. Returns an overlay; windows is not modified.
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
  local cells = blastCells(g, c, r, RANGE)
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
local function bfs(g, c0, r0, goal, avoid, windows, t0)
  t0 = t0 or 0
  local q, head = { { c0, r0, nil, 0 } }, 1
  local seen = { [r0 * KEY_W + c0] = true }
  while q[head] do
    local c, r, first, n = table.unpack(q[head]); head = head + 1
    if goal(c, r, n) then return first, n end
    for _, dir in ipairs(DIRS) do
      local cc, rr = c + dir[1], r + dir[2]
      local k = rr * KEY_W + cc
      local arrive = t0 + (n + 1) * FPT
      if not seen[k] and walkable(g, cc, rr) and not (avoid and avoid[k])
          and not (windows and hit(windows, k, arrive - FPT, arrive + FPT)) then
        seen[k] = true
        q[#q + 1] = { cc, rr, first or dir[3], n + 1 }
      end
    end
  end
  return nil
end

local function playerTile()
  local x, y = r16(P1 + P_X), r16(P1 + P_Y)
  local c, r = toTile(x, y)
  return c, r, x, y
end

-- Steer toward the centre of the next tile; align on the cross axis first,
-- since the engine does not slide the player around corners.
local function steer(dir, x, y, c, r)
  local cx, cy = tileCentre(c, r)
  if dir == "up" or dir == "down" then
    if x < cx - 1 then return "right" elseif x > cx + 1 then return "left" end
  else
    if y < cy - 1 then return "down" elseif y > cy + 1 then return "up" end
  end
  return dir
end

local frame, input = FRAME0, {}
local step = nil  -- committed move: from (c,r) toward (tc,tr); false while locked
local recent = {}  -- last few plan lines, dumped on death
local lastPlan = ""

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
local deaths, rollbacks = 0, 0
-- Ambush search: a bomb placed only for an enemy is tried from a savestate;
-- if nobody died AMBUSH_FRAMES later, reload and retry with a random delay
-- and shuffled tie-breaks, up to AMBUSH_TRIES times.
local AMBUSH_FRAMES, AMBUSH_TRIES = 220, 5
local ambush = nil        -- { frame, data, alive, tries }
local ambushStart = false -- think() wants to bomb for an enemy
local ambushHold = 0      -- no enemy-only bombs before this frame
local ambushes, ambushHits = 0, 0
local function liveEnemies() return #enemyTiles() end
local function score() return r16(0x5DEE) end  -- score / 100

local function think()
  input = {}
  if (r16(P1 + P_LOCK) & 1) ~= 0 then
    if playing then
      playing = false
      if r16(P1 + 0xA8) ~= r16(0x5D72) then
        deaths = deaths + 1
        -- A time-up can't be fixed by replaying the last few seconds.
        rollback = timeLeft() > 1
      end
    end
    if step ~= false then
      local es = {}
      for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
        if (r16(base) & 0xF000) == 0x9000 then
          es[#es + 1] = string.format("%04X(%d,%d)", base, r16(base + P_X), r16(base + P_Y))
        end
      end
      local st = {}
      for o = 0xE0, 0xFF do st[#st + 1] = string.format("%02X", emu.read(P1 + o, WRAM, false)) end
      log(string.format("%6d DEATH/lock at px(%d,%d) enemies %s stats[E0..FF] %s after: %s", frame,
        r16(P1 + P_X), r16(P1 + P_Y), table.concat(es, " "), table.concat(st, " "), table.concat(recent, " | ")))
      writeFile(string.format("%s/death_%06d.png", OUT, frame), emu.takeScreenshot())
    end
    step = false
    return
  end
  if step == false then step = nil end
  playing = true
  -- The border is always hard wall during play; anything else means the
  -- stage is loading or a cutscene is up.
  if not HARD[emu.read(MAP, WRAM, false)] then return end
  local g = readMap()
  -- Exit gate: map index at $7E5D72; open once $7E5D94 bit 15 (all gold
  -- panels blasted, or no panels: $FFFF) is set (checked at C1:41xx).
  local gate = r16(0x5D72)
  local gc, gr = (gate & 0x7F) // 2, gate >> 7
  local gateOpen = g[gr] ~= nil and gate ~= 0 and g[gr][gc] == 0x49  -- open gate tile (C1:41A8 via $09)
  if gateOpen then g[gr][gc] = GATE_OPEN end
  RANGE = emu.read(P1 + P_FIRE, WRAM, false) + 1
  local c, r, x, y = playerTile()
  local windows = blastWindows(g)
  local danger = {}
  for k in pairs(windows) do danger[k] = true end
  -- All live enemies are bomb targets; ones parked on hard tiles (the castle
  -- gate) can't be walked up to, so hunting only considers the rest.
  local enemies, walkers = {}, {}
  for _, e in ipairs(enemyTiles()) do
    -- The object parked in the gate cell is the gate itself (C1:2ABF).
    if e[1] ~= gc or e[2] ~= gr then
      enemies[#enemies + 1] = e
      if not HARD[g[e[2]][e[1]]] then walkers[#walkers + 1] = e end
    end
  end
  -- Enemy tiles and their neighbours: never path through them.
  local nearEnemy = {}
  for _, e in ipairs(enemies) do
    nearEnemy[e[2] * KEY_W + e[1]] = true
    for _, d in ipairs(DIRS) do nearEnemy[(e[2] + d[2]) * KEY_W + e[1] + d[1]] = true end
  end
  local panelTodo, panelDone = panels()
  local here = r * KEY_W + c
  visits[here] = (visits[here] or 0) + 1
  local soft, todo = 0, 0
  for rr = 0, MAP_H - 1 do for cc = 0, MAP_W - 1 do if SOFT[g[rr][cc]] then soft = soft + 1 end end end
  for _ in pairs(panelTodo) do todo = todo + 1 end
  local sig = string.format("%d/%d/%d/%d", score(), soft, todo, #enemies)
  if sig ~= progressSig then progressSig, progressAt = sig, frame
  elseif frame - progressAt > STALL then
    progressAt = frame
    exploreUntil, stallUntil = frame + 600, frame + 600
    log(string.format("%6d stalled (%s); exploring", frame, sig))
  end
  local plan, dir
  local function safeGoal(w)
    return function(cc, rr) return not w[rr * KEY_W + cc] end
  end

  if danger[here] then
    plan = "flee"
    dir = bfs(g, c, r, safeGoal(windows), nearEnemy, windows)
      or bfs(g, c, r, safeGoal(windows), nil, windows)
      or bfs(g, c, r, safeGoal(windows))
  elseif nearEnemy[here] then
    plan = "dodge"
    dir = bfs(g, c, r, function(cc, rr)
      return not nearEnemy[rr * KEY_W + cc] and not danger[rr * KEY_W + cc]
    end, nil, windows)
  else
    -- Is bombing here useful (a soft block or enemy in reach)?
    -- Is bombing at (cc,rr) useful? "obj" (soft block, switch, panel),
    -- "enemy" (an enemy on or next to the blast), or nil. A blast that would
    -- re-toggle a finished panel is never useful.
    local function targetsInReach(cc, rr)
      if g[rr][cc] == 0x66 then return nil end  -- no bombs on stairs
      if (noBombUntil[rr * KEY_W + cc] or 0) > frame then return nil end
      local why = nil
      for _, cell in ipairs(blastCells(g, cc, rr, RANGE)) do
        local k = cell[2] * KEY_W + cell[1]
        if panelDone[k] then return nil end
        local t = g[cell[2]][cell[1]]
        if panelTodo[k] or SOFT[t] or isSwitchOff(t) then why = "obj" end
        -- Enemies wander, so one tile off the blast is close enough to try.
        if not why then
          for _, e in ipairs(walkers) do
            if math.abs(e[1] - cell[1]) + math.abs(e[2] - cell[2]) <= 1 then why = "enemy" end
          end
        end
      end
      return why
    end
    -- After placing a bomb at (cc,rr) at time t, can we reach a cell no blast
    -- will ever touch, without being in any blast on the way?
    local function canEscape(cc, rr, t)
      local w = withBomb(g, windows, cc, rr, NEW_FUSE + t)
      return bfs(g, cc, rr, safeGoal(w), nearEnemy, w, t) ~= nil
    end
    local haveBomb = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0
    local why = haveBomb and targetsInReach(c, r)
    if why == "enemy" and frame < ambushHold then why = nil end
    if why and canEscape(c, r, 0) then
      plan = "bomb"
      if not bombTry or bombTry[1] ~= here then bombTry = { here, frame }
      elseif frame - bombTry[2] > 20 then
        noBombUntil[here] = frame + 600
        log(string.format("%6d no bomb appears at (%d,%d); avoiding it", frame, c, r))
        bombTry = nil
      end
      if why == "enemy" and not ambush then ambushStart = true end
      -- Bomb placement is edge-triggered: press on alternate frames.
      input.a = frame % 2 == 0
    else
      plan = "item"
      dir = bfs(g, c, r, function(cc, rr) return g[rr][cc] == ITEM end, nearEnemy, windows)
      if not dir and haveBomb then
        plan = "seek"
        dir = bfs(g, c, r, function(cc, rr, n)
          return (cc ~= c or rr ~= r) and not danger[rr * KEY_W + cc]
            and targetsInReach(cc, rr) and canEscape(cc, rr, n * FPT)
        end, nearEnemy, windows)
      end
      if not dir and gateOpen and #walkers == 0 then
        plan = "exit"
        dir = bfs(g, c, r, function(cc, rr) return g[rr][cc] == GATE_OPEN end, nil, windows)
      end
      if not dir and #walkers > 0 and haveBomb then
        -- Nothing to bomb from here: close in on the nearest enemy.
        plan = "hunt"
        dir = bfs(g, c, r, function(cc, rr)
          for _, e in ipairs(walkers) do
            if math.abs(e[1] - cc) + math.abs(e[2] - rr) <= 3 then return true end
          end
          return false
        end, nearEnemy, windows)
      end
      if frame < stallUntil and plan ~= "bomb" then dir = nil end
      if not dir and frame < exploreUntil then
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
        if haveBomb and math.random() < 0.1 then input.a = true end
      end
      if not dir then plan = "wait" end
    end
  end

  -- Commit to the next tile until its centre is reached, so the plan can't
  -- flip back and forth on a tile boundary.
  if dir and not (step and plan ~= "flee" and plan ~= "dodge") then
    local d = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[dir]
    step = { c = c, r = r, tc = c + d[1], tr = r + d[2], dir = dir }
  end
  if step then
    local tx, ty = tileCentre(step.tc, step.tr)
    if math.abs(x - tx) <= 1 and math.abs(y - ty) <= 1 then
      step = nil
    elseif plan ~= "flee" and (danger[step.tr * KEY_W + step.tc] or nearEnemy[step.tr * KEY_W + step.tc]) then
      step = nil  -- the committed tile became dangerous; replan next frame
    else
      input[steer(step.dir, x, y, step.c, step.r)] = true
      -- Once on the target tile, keep moving toward its centre.
      if c == step.tc and r == step.tr then
        input = { a = input.a }
        if x < tx - 1 then input.right = true elseif x > tx + 1 then input.left = true end
        if y < ty - 1 then input.down = true elseif y > ty + 1 then input.up = true end
      end
    end
  end
  if plan ~= "bomb" then bombTry = nil end
  local s = string.format("%s %s @(%d,%d) px(%d,%d)", plan, dir or "-", c, r, x, y)
  if s ~= lastPlan then
    log(string.format("%6d %s", frame, s)); lastPlan = s
    recent[#recent + 1] = string.format("%d %s", frame, s)
    if #recent > 6 then table.remove(recent, 1) end
  end
end

local function dumpCdl(path)
  local cdl = emu.getCdlData(emu.memType.snesPrgRom)
  local t = {}
  for i = 0, #cdl do t[#t + 1] = string.char(cdl[i] or 0) end
  writeFile(path, table.concat(t))
end

local function onNextInstruction(fn)
  local id
  id = emu.addMemoryCallback(function()
    emu.removeMemoryCallback(id, emu.callbackType.exec, 0, 0xFFFFFF)
    fn()
  end, emu.callbackType.exec, 0, 0xFFFFFF)
end

local loaded = LOAD == nil
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
    dumpCdl(string.format("%s/seg_%06d.cdl", OUT, FRAME0))
    flushLog()
    os.rename(OUT .. "/ckpt.mss.tmp", OUT .. "/ckpt.mss")
    writeFile(OUT .. "/ckpt.txt", tostring(frame))
  end)
end


local function onFrame()
  if not loaded then
    local fh = assert(io.open(LOAD, "rb")); local data = fh:read("a"); fh:close()
    onNextInstruction(function() emu.loadSavestate(data) end)
    loaded = true
    return
  end
  if rollback then
    rollback = false
    ambush = nil
    -- Newest snapshot at least 60 frames before the death, then further
    -- back by one per 3 failures there.
    local i = #snaps
    while i > 1 and snaps[i][1] > frame - 60 do i = i - 1 end
    if i >= 1 then
      i = math.max(1, i - (triesAt[snaps[i][1]] or 0) // 3)
      local snap = snaps[i]
      local tries = (triesAt[snap[1]] or 0) + 1
      if tries <= MAX_TRIES then
        triesAt[snap[1]] = tries
        for k = #snaps, i + 1, -1 do snaps[k] = nil end
        rollbacks = rollbacks + 1
        log(string.format("%6d rollback to %d (try %d, deaths %d)", frame, snap[1], tries, deaths))
        math.randomseed(rollbacks * 7919 + snap[1])
        exploreUntil = snap[1] + 60 * (tries + 1)
        shuffleDirs()
        onNextInstruction(function() emu.loadSavestate(snap[2]) end)
        frame, step, playing, fuseSeen = snap[1], nil, false, {}
        return
      end
    end
  end
  if frame % SNAP_EVERY == 0 and (r16(P1 + P_LOCK) & 1) == 0
      and (#snaps == 0 or snaps[#snaps][1] < frame) then
    local f = frame
    onNextInstruction(function()
      snaps[#snaps + 1] = { f, emu.createSavestate() }
      if #snaps > 40 then table.remove(snaps, 1) end
    end)
  end
  if ambush and frame >= ambush[1] + AMBUSH_FRAMES then
    if liveEnemies() < ambush[3] or score() > ambush[5] then
      ambushHits = ambushHits + 1
      log(string.format("%6d ambush hit (try %d; %d/%d)", frame, ambush[4], ambushHits, ambushes))
      ambush = nil
    elseif ambush[4] < AMBUSH_TRIES and ambush[2] then
      ambush[4] = ambush[4] + 1
      local snap = ambush
      for k = #snaps, 1, -1 do if snaps[k][1] > snap[1] then snaps[k] = nil end end
      shuffleDirs()
      onNextInstruction(function() emu.loadSavestate(snap[2]) end)
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
      local a = { frame, nil, liveEnemies(), 0, score() }
      ambush = a
      onNextInstruction(function() a[2] = emu.createSavestate() end)
    end
  end
  if not ok then log("error: " .. tostring(err)) end
  local dt = os.clock() - t0
  if dt > 0.05 then log(string.format("%6d slow think %.3fs", frame, dt)) end
  if shots[frame] then
    writeFile(string.format("%s/f%06d.png", OUT, frame), emu.takeScreenshot())
    logMap()
  end
  frame = frame + 1
  if frame >= FRAMES then
    flushLog()
    dumpCdl(string.format("%s/seg_%06d.cdl", OUT, FRAME0))
    writeFile(OUT .. "/ckpt.txt", tostring(frame))
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
    emu.stop(4)
  end
end, emu.eventType.endFrame)
