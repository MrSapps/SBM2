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

  emu.addMemoryCallback(function()
    local x = emu.getState()["cpu.x"]
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
      if not t or tick - t.tick > 2 then t = { n = 0 }; track[base] = t end
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
        local old = t.n >= 16 and t[(t.n + 1) % 16]
        if old and math.abs(x - old[1]) < 48 and math.abs(y - old[2]) < 48 then
          vx, vy = (x - old[1]) / 15, (y - old[2]) / 15
        end
      else
        x = x - 0x100
      end
      list[#list + 1] = { base = base, x = x, y = y, vx = vx, vy = vy, box = o.box, sense = o.sense, hurt = o.hurt }
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
  function B.predict(b, t)
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
          local px, py = B.predict(b, t)
          -- Further ahead is less sure: widen the box with time.
          local m = margin + t // 16
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
