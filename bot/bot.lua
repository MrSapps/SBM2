-- SBM2 bot v0: single-player normal game.
--
-- Reads the tile map and player 1 from WRAM, flees bomb blasts with BFS, and
-- otherwise walks next to soft blocks and bombs them when an escape exists.
-- Logs tile codes it has not classified yet so the tables below can grow.
--
-- Env (same harness conventions as harness.lua):
--   SBM2_OUT     output dir; writes bot.log, run.cdl and shots
--   SBM2_FRAMES  frames to run (default 3600)
--   SBM2_SHOTS   frames to screenshot
--   SBM2_LOAD    savestate to start from

local OUT = os.getenv("SBM2_OUT") or "build/bot"
local FRAMES = tonumber(os.getenv("SBM2_FRAMES") or "3600")
local LOAD = os.getenv("SBM2_LOAD"); if LOAD == "" then LOAD = nil end
local shots = {}
for f in (os.getenv("SBM2_SHOTS") or ""):gmatch("%d+") do shots[tonumber(f)] = true end

local WRAM = emu.memType.snesWorkRam
local function r16(a) return emu.read16(a, WRAM, false) end

-- RAM map (offsets into WRAM, i.e. $7E0000 = 0)
local MAP = 0x1C000          -- $7FC000: 16 x 14 cells, 2 bytes each, rows $80 apart
local MAP_W, MAP_H, ROW = 16, 14, 0x80
local P1 = 0x8000            -- $7E8000: player 1 struct
local P_X, P_Y = 0x10, 0x18  -- pixel position (fractions at +2)
local P_LOCK = 0x52          -- bit 0 set: no control (dying/warping)
local P_BOMBS_LEFT = 0xF1    -- bombs that can still be placed (byte)
local ENEMY_FIRST, ENEMY_LAST = 0x8400, 0x8F00  -- enemy structs, same layout

-- Tiles of live enemies; a slot is live when +$00 is $9xxx (as for players).
local function enemyTiles()
  local t = {}
  for base = ENEMY_FIRST, ENEMY_LAST, 0x100 do
    if (r16(base) & 0xF000) == 0x9000 then
      local x, y = r16(base + P_X), r16(base + P_Y)
      t[#t + 1] = { (x + 8) // 16, y // 16 }
    end
  end
  return t
end

-- Tile classification (grows as the log reveals more codes)
local EMPTY = { [0x00] = true }
local SOFT = { [0xC2] = true }
local HARD = { [0xC5] = true, [0xC6] = true }
local function isBomb(t) return t >= 0x40 and t <= 0x4F end
local function isFlame(t) return t >= 0x01 and t <= 0x3F end

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
  return EMPTY[t] == true
end

local DIRS = { { 0, -1, "up" }, { 0, 1, "down" }, { -1, 0, "left" }, { 1, 0, "right" } }

-- Cells reached by blasts. Per-bomb power isn't known yet, so use the
-- player's fire stat (guess: +$E1; confirm with a fire power-up).
local P_FIRE = 0xE1
local RANGE = 1
local function dangerMap(g, extraBomb)
  local d = {}
  local function mark(c, r) d[r * 16 + c] = true end
  local function blast(c, r)
    mark(c, r)
    for _, dir in ipairs(DIRS) do
      for k = 1, RANGE do
        local cc, rr = c + dir[1] * k, r + dir[2] * k
        if rr < 0 or rr >= MAP_H or cc < 0 or cc >= MAP_W then break end
        local t = g[rr][cc]
        if HARD[t] then break end
        mark(cc, rr)
        if SOFT[t] then break end
      end
    end
  end
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      if isBomb(g[r][c]) then blast(c, r) end
      if isFlame(g[r][c]) then mark(c, r) end
    end
  end
  if extraBomb then blast(extraBomb[1], extraBomb[2]) end
  return d
end

-- BFS from (c,r); goal(c,r) -> bool. Returns first step direction and path length.
local function bfs(g, c0, r0, goal, avoid)
  local q, head = { { c0, r0, nil, 0 } }, 1
  local seen = { [r0 * 16 + c0] = true }
  while q[head] do
    local c, r, first, n = table.unpack(q[head]); head = head + 1
    if goal(c, r) then return first, n end
    for _, dir in ipairs(DIRS) do
      local cc, rr = c + dir[1], r + dir[2]
      local k = rr * 16 + cc
      if not seen[k] and walkable(g, cc, rr) and not (avoid and avoid[k]) then
        seen[k] = true
        q[#q + 1] = { cc, rr, first or dir[3], n + 1 }
      end
    end
  end
  return nil
end

local function playerTile()
  local x, y = r16(P1 + P_X), r16(P1 + P_Y)
  -- Tile (c,r) is centred on x = c*16, y = r*16 + 8 (from the start position).
  return (x + 8) // 16, y // 16, x, y
end

-- Steer toward the centre of the next tile; align on the cross axis first,
-- since the engine does not slide the player around corners.
local function steer(dir, x, y, c, r)
  local cx, cy = c * 16, r * 16 + 8
  if dir == "up" or dir == "down" then
    if x < cx - 1 then return "right" elseif x > cx + 1 then return "left" end
  else
    if y < cy - 1 then return "down" elseif y > cy + 1 then return "up" end
  end
  return dir
end

local frame, input = 0, {}
local step = nil  -- committed move: from (c,r) toward (tc,tr)
local lastPlan = ""

local function think()
  input = {}
  if (r16(P1 + P_LOCK) & 1) ~= 0 then step = nil; return end
  -- The border is always hard wall during play; anything else means the
  -- stage is loading or a cutscene is up.
  if not HARD[emu.read(MAP, WRAM, false)] then return end
  local g = readMap()
  RANGE = math.max(1, emu.read(P1 + P_FIRE, WRAM, false))
  local c, r, x, y = playerTile()
  local danger = dangerMap(g)
  local enemies = enemyTiles()
  -- Enemy tiles and their neighbours: never path through them.
  local nearEnemy = {}
  for _, e in ipairs(enemies) do
    nearEnemy[e[2] * 16 + e[1]] = true
    for _, d in ipairs(DIRS) do nearEnemy[(e[2] + d[2]) * 16 + e[1] + d[1]] = true end
  end
  local avoid = {}
  for k in pairs(danger) do avoid[k] = true end
  for k in pairs(nearEnemy) do avoid[k] = true end
  local here = r * 16 + c
  local plan, dir

  if danger[here] then
    plan = "flee"
    dir = bfs(g, c, r, function(cc, rr) return not danger[rr * 16 + cc] end, nearEnemy)
      or bfs(g, c, r, function(cc, rr) return not danger[rr * 16 + cc] end)
  elseif nearEnemy[here] then
    plan = "dodge"
    dir = bfs(g, c, r, function(cc, rr) return not avoid[rr * 16 + cc] end)
  else
    -- Is bombing here useful (a soft block in reach) and survivable?
    local function bricksInReach(cc, rr)
      for _, d in ipairs(DIRS) do
        for k = 1, RANGE do
          local tc, tr = cc + d[1] * k, rr + d[2] * k
          if tr < 0 or tr >= MAP_H or tc < 0 or tc >= MAP_W then break end
          local t = g[tr][tc]
          if SOFT[t] then return true end
          for _, e in ipairs(enemies) do
            if e[1] == tc and e[2] == tr then return true end
          end
          if HARD[t] or not walkable(g, tc, tr) then break end
        end
      end
      return false
    end
    local function canEscape(cc, rr)
      local after = dangerMap(g, { cc, rr })
      return bfs(g, cc, rr, function(ec, er) return not after[er * 16 + ec] end) ~= nil
    end
    local haveBomb = emu.read(P1 + P_BOMBS_LEFT, WRAM, false) > 0
    if haveBomb and bricksInReach(c, r) and canEscape(c, r) then
      plan = "bomb"
      -- Bomb placement is edge-triggered: press on alternate frames.
      input.a = frame % 2 == 0
    else
      plan = "seek"
      dir = bfs(g, c, r, function(cc, rr)
        return (cc ~= c or rr ~= r or haveBomb) and bricksInReach(cc, rr) and canEscape(cc, rr)
      end, avoid)
    end
  end

  -- Commit to the next tile until its centre is reached, so the plan can't
  -- flip back and forth on a tile boundary.
  if dir and not (step and plan == "seek") then
    local d = ({ up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } })[dir]
    step = { c = c, r = r, tc = c + d[1], tr = r + d[2], dir = dir }
  end
  if step then
    local tx, ty = step.tc * 16, step.tr * 16 + 8
    if math.abs(x - tx) <= 1 and math.abs(y - ty) <= 1 then
      step = nil
    elseif plan == "seek" and avoid[step.tr * 16 + step.tc] then
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
  local s = string.format("%s %s @(%d,%d) px(%d,%d)", plan, dir or "-", c, r, x, y)
  if s ~= lastPlan then log(string.format("%6d %s", frame, s)); lastPlan = s end
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

local function onFrame()
  if not loaded then
    local fh = assert(io.open(LOAD, "rb")); local data = fh:read("a"); fh:close()
    onNextInstruction(function() emu.loadSavestate(data) end)
    loaded = true
    return
  end
  local t0 = os.clock()
  local ok, err = pcall(think)
  if not ok then log("error: " .. tostring(err)) end
  local dt = os.clock() - t0
  if dt > 0.05 then log(string.format("%6d slow think %.3fs", frame, dt)) end
  if frame % 300 == 0 then writeFile(OUT .. "/bot.log", table.concat(logLines, "\n") .. "\n") end
  if shots[frame] then
    writeFile(string.format("%s/f%06d.png", OUT, frame), emu.takeScreenshot())
  end
  frame = frame + 1
  if frame >= FRAMES then
    writeFile(OUT .. "/bot.log", table.concat(logLines, "\n") .. "\n")
    dumpCdl(OUT .. "/run.cdl")
    emu.stop(0)
  end
end

emu.addEventCallback(function()
  local ok, err = pcall(onFrame)
  if not ok then
    log(string.format("%6d fatal: %s", frame, tostring(err)))
    writeFile(OUT .. "/bot.log", table.concat(logLines, "\n") .. "\n")
    emu.stop(4)
  end
end, emu.eventType.endFrame)
