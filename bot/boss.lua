-- Boss model for the bot (rules: docs/enemies.md section 7).
--
-- Bosses don't use the enemy structs: they are boss-engine objects ($40
-- bytes each at $001000-$0017FF) with x/y in +$11/+$14 (screen pixels, as
-- P1's +$10/+$18). Each frame an object that can kill runs BossContactP1
-- (CC:A538) with the box at [$6D] (BoxHasP1 itself is also used for
-- harmless sight boxes, CC:9E5F). One that can be hurt by flames looks up
-- the tile under each of its sense points (x+dx, y+dy in $4A/$4C) and then
-- runs BossEatBomb (CC:A2C4) on it: BossFlameSense (CC:A266, a list of
-- points) and the 4-8 robot's centre check (CC:C4B9) both do. Both
-- routines are watched, so every boss is covered without a table of states.
--
--   local B = dofile(dir .. "/boss.lua")(env)   env: rd8, r16, origin, KEY_W, MAP_W, MAP_H
--   B.read()  -> bosses seen last frame: { x, y, vx, vy, box = {x0, x1, y0, y1}, sense = {{dx, dy}, ...} }
--   B.contactCells(list, margin, ahead) -> set of cell keys P1 must not be in
--   B.senseCells(list, ahead) -> set of cell keys a flame hurts a boss in (a bomb there is eaten)
return function(env)
  local rd8, r16 = env.rd8, env.r16
  local KEY_W, MAP_W, MAP_H = env.KEY_W, env.MAP_W, env.MAP_H
  local ROM = emu.memType.snesMemory
  local CONTACT, EAT_BOMB = 0xCCA538, 0xCCA2C4
  local SENSE_ENTRIES = { 0xCCA266, 0xCCC4B9 }  -- BossFlameSense, 4-8 centre check

  local function rl16(a)  -- 16-bit read from the CPU bus (ROM tables)
    return emu.read(a, ROM, false) | (emu.read(a + 1, ROM, false) << 8)
  end
  local function s16(v) return v >= 0x8000 and v - 0x10000 or v end
  local function ptr(dp)  -- 24-bit pointer at direct page dp (D = 0 in boss code)
    return rd8(dp) | (rd8(dp + 1) << 8) | (rd8(dp + 2) << 16)
  end

  local cur, last = {}, {}
  local track, tick = {}, 0  -- base -> ring of the last 16 positions
  local bounds = {}          -- base -> {x0, x1, y0, y1} seen so far (where it turns)
  local boxCache = {}

  local function obj(x)
    local o = cur[x]
    if not o then o = { base = x }; cur[x] = o end
    return o
  end

  emu.addMemoryCallback(function()
    local x = emu.getState()["cpu.x"]
    local p = ptr(0x6D)
    local b = boxCache[p]
    if not b then
      -- Half-open: P1 inside when x+a < p1x+$1000 <= x+b-1 (CC:A573).
      b = { s16(rl16(p)) - 0x1000, s16(rl16(p + 2)) - 0x1000, s16(rl16(p + 4)) - 0x1000, s16(rl16(p + 6)) - 0x1000 }
      boxCache[p] = b
    end
    obj(x).box = b
  end, emu.callbackType.exec, CONTACT, CONTACT)

  -- X at BossEatBomb is the object only after a flame was found (otherwise
  -- MapTileAtXY's tile index), so note the object when the check starts.
  local senseObj
  for _, a in ipairs(SENSE_ENTRIES) do
    emu.addMemoryCallback(function() senseObj = emu.getState()["cpu.x"] end, emu.callbackType.exec, a, a)
  end
  emu.addMemoryCallback(function()
    local x = senseObj
    if not x or x < 0x1000 or x >= 0x1800 then return end
    local o = obj(x)
    o.sense = o.sense or {}
    local s = o.sense
    s[#s + 1] = { s16(r16(0x4A)) - s16(r16(x + 0x11)), s16(r16(0x4C)) - s16(r16(x + 0x14)) }
    o.hurt = r16(x + 0x26) ~= 0  -- hurt timer: flames don't count until it runs out
  end, emu.callbackType.exec, EAT_BOMB, EAT_BOMB)

  local B = {}

  -- Call once per frame (endFrame): what the boss code ran this frame.
  function B.frame()
    last, cur = cur, {}
    tick = tick + 1
  end

  function B.read()
    local list = {}
    for base, o in pairs(last) do
      local x, y = s16(r16(base + 0x11)), s16(r16(base + 0x14))
      -- Speed over the last 16 frames (a blink adds $100 to x: skip those).
      local t = track[base]
      if not t or tick - t.tick > 2 then t = { n = 0 }; track[base] = t; bounds[base] = nil end
      t.tick = tick
      local vx, vy = 0, 0
      if x < 0x100 then
        t.n = t.n + 1
        t[t.n % 16] = { x, y }
        local bb = bounds[base]
        if not bb then bounds[base] = { x, x, y, y }
        else
          bb[1], bb[2] = math.min(bb[1], x), math.max(bb[2], x)
          bb[3], bb[4] = math.min(bb[3], y), math.max(bb[4], y)
        end
        -- A new object (a thrown ball) gets a speed from its first frames.
        local span = math.min(t.n - 1, 15)
        local old = span >= 3 and t[(t.n - span) % 16]
        if old and math.abs(x - old[1]) < 48 and math.abs(y - old[2]) < 48 then
          vx, vy = (x - old[1]) / span, (y - old[2]) / span
        end
      else
        x = x - 0x100
      end
      local state = r16(base) | (rd8(base + 2) << 16)
      list[#list + 1] = { base = base, state = state, x = x, y = y, vx = vx, vy = vy, box = o.box, sense = o.sense, hurt = o.hurt }
    end
    return list
  end

  local function cellOfPx(px, py)
    local ox, oy = env.origin()
    return math.floor(px + ox) >> 4, math.floor(py + oy) >> 4
  end

  -- Cells where P1 (anywhere in the cell, +-8 px of its centre) could be in
  -- a box, now or `ahead` frames on at the current speed; margin in pixels.
  function B.contactCells(list, margin, ahead)
    local set = {}
    for _, b in ipairs(list) do
      if b.box and b.x > -64 and b.x < 0x200 then
        local x0, x1 = b.x + b.box[1] - margin, b.x + b.box[2] - 1 + margin
        local y0, y1 = b.y + b.box[3] - margin, b.y + b.box[4] - 1 + margin
        local ex, ey = b.vx * ahead, b.vy * ahead
        if ex < 0 then x0 = x0 + ex else x1 = x1 + ex end
        if ey < 0 then y0 = y0 + ey else y1 = y1 + ey end
        -- P1's x within (x0, x1]: a cell's x span is [cx-8, cx+7].
        local c0, r0 = cellOfPx(x0 - 7, y0 - 7)
        local c1, r1 = cellOfPx(x1 + 8, y1 + 8)
        for r = math.max(r0, 0), math.min(r1, MAP_H - 1) do
          for c = math.max(c0, 0), math.min(c1, MAP_W - 1) do set[r * KEY_W + c] = true end
        end
      end
    end
    return set
  end

  -- Where a boss will be `t` frames on: straight on at its speed, turning
  -- back at the edges of where it has been seen to go.
  local function reflect(p, v, lo, hi)
    if hi - lo < 32 then return p end
    local span = hi - lo
    local q = (p - lo) % (2 * span)
    if q > span then q = 2 * span - q end
    return lo + q
  end
  -- A known destination (B.target[base] = {x, y}, e.g. 5-8 form 2's
  -- waypoint): straight there at the current speed, then stay.
  B.target = {}
  -- 5-8 form 1 slam (CC:CAC4): $36 counts down from $A0; while it is
  -- >= $89 the body drops 3 px a frame, below $49 it rises 1 px a frame,
  -- in between it stays (72 px down and back, x fixed).
  local SLAM, PATROL, BALL = 0xCCCAC4, 0xCCC8D6, 0xCCDAA8
  -- 5-8 balls (CC:DAA8): 1 px a frame in direction $36&3 (table CC:DC19:
  -- up, right, down, left) after a $2E hold; when $38 runs out they turn
  -- toward P1 (or at random), so after that any way; gone when $34 does.
  local BALL_V = { [0] = { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } }
  local function ballAt(b, t)  -- centre and spread at t, or nil once gone
    local base = b.base
    if t >= r16(base + 0x34) then return nil end
    local hold, run = r16(base + 0x2E), r16(base + 0x38)
    local v = BALL_V[rd8(base + 0x36) & 3]
    local go = math.min(math.max(t - hold, 0), run)
    return b.x + v[1] * go, b.y + v[2] * go, math.max(t - hold - run, 0)
  end
  local function slamDy(T, t)
    local dy = 0
    for _ = 1, t do
      if T <= 0 then break end
      if T >= 0x89 then dy = dy + 3 elseif T < 0x49 then dy = dy - 1 end
      T = T - 1
    end
    return dy
  end
  -- 5-8 form 1 patrol (CC:C8D6): while $34 >= $50 the body moves 1 px a
  -- frame the way it faces ($0F bit 7, learned from its motion); at 0 it
  -- turns round and $34 restarts at $D8.
  local faceSign = {}  -- $0F bit 7 -> +1/-1
  local function patrolDx(b, t)
    local bit = rd8(b.base + 0x0F) & 0x80
    if math.abs(b.vx) > 0.5 then faceSign[bit] = b.vx > 0 and 1 or -1; faceSign[bit ~ 0x80] = -faceSign[bit] end
    local d = faceSign[bit]
    if not d then return nil end
    local T, dx = r16(b.base + 0x34), 0
    for _ = 1, t do
      if T >= 0x50 then dx = dx + d end
      if T == 0 then d, T = -d, 0xD8 else T = T - 1 end
    end
    return dx
  end
  function B.predict(b, t)
    if b.state == SLAM then return b.x, b.y + slamDy(r16(b.base + 0x36), t) end
    -- 5-8 form 2 hurt (FinalBossForm2Hurt CC:D2E7, 63 frames): it stays put
    -- (the waypoint prediction had it fly on, and dodges walked into it)
    if b.state == 0xCCD2E7 then return b.x, b.y end
    if b.state == PATROL then
      local dx = patrolDx(b, t)
      if dx then return b.x + dx, b.y end
    end
    local tg = B.target[b.base]
    if tg then
      local dx, dy = tg[1] - b.x, tg[2] - b.y
      local d = math.sqrt(dx * dx + dy * dy)
      local go = math.min(d, math.sqrt(b.vx * b.vx + b.vy * b.vy) * t)
      if d < 1 then return b.x, b.y end
      return b.x + dx / d * go, b.y + dy / d * go
    end
    local x, y = b.x + b.vx * t, b.y + b.vy * t
    local bb = bounds[b.base]
    if bb then x, y = reflect(x, b.vx, bb[1], bb[2]), reflect(y, b.vy, bb[3], bb[4]) end
    return x, y
  end

  -- Contact windows per cell over `horizon` frames (as Enemies.windows):
  -- cell key -> { {t0, t1}, ... }, `slack` frames either side.
  function B.windows(list, horizon, margin, slack)
    local w = {}
    local STEP = 4
    for _, b in ipairs(list) do
      if b.box and b.x > -64 and b.x < 0x200 then
        for t = 0, horizon, STEP do
          local px, py, spread = B.predict(b, t)
          if b.state == BALL then px, py, spread = ballAt(b, t) end
          if not px then break end
          -- Further ahead is less sure: widen the box with time, more so
          -- for a fast boss (5-8 form 2 re-aims every 16 frames).
          local spd = math.sqrt(b.vx * b.vx + b.vy * b.vy)
          local m = margin + t // 16 + math.floor(t * math.max(0, spd - 0.8) * 0.5)
          if b.state == BALL then m = margin + (spread or 0) end
          local x0, x1 = px + b.box[1] - m, px + b.box[2] - 1 + m
          local y0, y1 = py + b.box[3] - m, py + b.box[4] - 1 + m
          local c0, r0 = cellOfPx(x0 - 7, y0 - 7)
          local c1, r1 = cellOfPx(x1 + 8, y1 + 8)
          for r = math.max(r0, 0), math.min(r1, MAP_H - 1) do
            for c = math.max(c0, 0), math.min(c1, MAP_W - 1) do
              local k = r * KEY_W + c
              local l = w[k]
              if not l then l = {}; w[k] = l end
              local last = l[#l]
              if last and last[2] >= t - STEP - slack then last[2] = t + STEP + slack
              else l[#l + 1] = { math.max(0, t - slack), t + STEP + slack } end
            end
          end
        end
      end
    end
    -- 5-8 form 1 patrol (CC:C8D6): with $3A set and $38 run out, P1 within
    -- x-16..x+15 anywhere below the body (sight box CC:D59B) starts a slam
    -- that lands 72 px down within 24 frames with the contact box (CC:CA20,
    -- y -48..+35) under it: too fast to step out of. So the column under
    -- the body, down to the slam's reach, is closed while it is armed.
    for _, b in ipairs(list) do
      if b.state == PATROL and r16(b.base + 0x3A) ~= 0 and r16(b.base + 0x38) < 16 then
        -- Cells whose centre (where P1 stands) is in reach; P1 only passes
        -- the column's edge cells off-centre, so the column is no wider.
        local ox, oy = env.origin()
        local function centred(lo, hi, o)  -- cells with centre in (lo, hi]
          return math.floor((lo + o - 8) / 16) + 1, math.floor((hi + o - 8) / 16)
        end
        local c0, c1 = centred(b.x - 16 - 3, b.x + 15 + 3, ox)
        local r0, r1 = centred(b.y, b.y + 72 + 35 + margin, oy)
        for r = math.max(r0, 0), math.min(r1, MAP_H - 1) do
          for c = math.max(c0, 0), math.min(c1, MAP_W - 1) do
            local k = r * KEY_W + c
            w[k] = w[k] or {}
            table.insert(w[k], { 0, horizon + slack })
          end
        end
      end
    end
    return w
  end

  -- Cells whose flame tile hurts a boss (MapTileAtXY of x+dx, y+dy).
  -- `ahead` frames on at the current speed.
  function B.senseCells(list, ahead)
    local set, any = {}, false
    for _, b in ipairs(list) do
      if b.sense and b.x > -64 and b.x < 0x200 then
        local bx, by = B.predict(b, ahead or 0)
        for _, d in ipairs(b.sense) do
          local c, r = cellOfPx(bx + d[1], by + d[2])
          if c >= 0 and c < MAP_W and r >= 0 and r < MAP_H then set[r * KEY_W + c] = true; any = true end
        end
      end
    end
    return set, any
  end

  -- The cell under a boss's centre (for hunting).
  function B.centres(list)
    local out = {}
    for _, b in ipairs(list) do
      if b.sense and b.x > -64 and b.x < 0x200 then
        local c, r = cellOfPx(b.x, b.y)
        if c >= 0 and c < MAP_W and r >= 0 and r < MAP_H then out[#out + 1] = { c, r } end
      end
    end
    return out
  end

  return B
end
