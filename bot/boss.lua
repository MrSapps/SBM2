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
  -- (Objects seen in either of the last two frames: some bosses run their
  -- contact check every other frame only, BossFrameCount bit 0 in
  -- BossContactP1; the list was empty every other frame and the bot
  -- dithered under the 1-8 boss as its danger came and went.)
  local prev = {}
  function B.frame()
    prev, last, cur = last, cur, {}
    tick = tick + 1
  end

  function B.read()
    local list = {}
    local both = {}
    -- (1-8 and 3-8 only: elsewhere an object deleted last frame, a 5-8 ball,
    -- stayed listed and the whole 5-8 fight was lost)
    local stg = r16(0x1A) & 0xFF
    if stg == 0x17 or stg == 0x37 then for base, o in pairs(prev) do both[base] = o end end
    for base, o in pairs(last) do both[base] = o end
    for base, o in pairs(both) do
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
        t.vx, t.vy = vx, vy
      else
        -- (blink frame: the speed of the last real one; 0 made every
        -- other frame's prediction stand still, the danger flickered and
        -- P1 dithered under the 1-8 boss until caught)
        x = x - 0x100
        vx, vy = t.vx or 0, t.vy or 0
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
  local SLAM, PATROL, BALL, FORM2 = 0xCCCAC4, 0xCCC8D6, 0xCCDAA8, 0xCCD139
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
  -- 1-8 body (stage $17), from its code (CC:9ABA/9B54/9BDD): it moves at
  -- 1 px/f in its facing (+$0F bit 7: left) while +$34 counts down, then
  -- stands 50 frames (+$3A), turns, and moves 128 frames; a strike (9BDD,
  -- +$36 frames) freezes it and then resumes. So x swings over 64..192 on a
  -- fixed beat, whatever P1 does (strikes aside). Exact x t frames on.
  -- With p1x/vp: P1 is taken to follow the body at vp px/f from p1x, and
  -- the body strikes (CC:9BDD: stands 192 frames, then cooldown +$38 =
  -- 128) whenever P1 is 16..40 px to its side with the cooldown out
  -- (CC:9E9A). A slow P1 lags at the turns and sets strikes off: at speed
  -- 1 every shadow bomb missed by 49 px, the body frozen at x 79/177.
  function B.sunX(t, p1x, vp)
    local st = r16(0x1000) | (rd8(0x1002) << 16)
    local x = s16(r16(0x1011))
    if x >= 0x100 then x = x - 0x100 end
    local dir = (rd8(0x100F) & 0x80) ~= 0 and -1 or 1
    local move, pause, hold = r16(0x1034), 0, 0
    if st == 0xCC9B54 then move, pause = 0, r16(0x103A)
    elseif st == 0xCC9BDD then hold = r16(0x1036)
    elseif st ~= 0xCC9ABA then return nil end
    if p1x then
      local cool = r16(0x1038)
      for _ = 1, t do
        if hold > 0 then hold = hold - 1; if hold == 0 then cool = 128 end
        else
          if cool > 0 then cool = cool - 1 end
          local dx = math.abs(p1x - x)
          if cool == 0 and dx >= 16 and dx <= 40 then hold = 192
          elseif move > 0 then
            x = x + dir; move = move - 1
            if move == 0 then pause = 50 end
          elseif pause > 0 then
            pause = pause - 1
            if pause == 0 then dir, move = -dir, 128 end
          else pause = 50 end
        end
        if p1x < x - vp then p1x = p1x + vp elseif p1x > x + vp then p1x = p1x - vp else p1x = x end
      end
      return x
    end
    local left = t
    local take = math.min(left, hold); left = left - take
    while left > 0 do
      if move > 0 then
        take = math.min(left, move); x = x + dir * take; move = move - take; left = left - take
        if move == 0 and pause == 0 then pause = 50 end
      elseif pause > 0 then
        take = math.min(left, pause); pause = pause - take; left = left - take
        if pause == 0 then dir, move = -dir, 128 end
      else pause = 50 end
    end
    return x
  end

  function B.predict(b, t)
    if (b.base == 0x1000 or b.base == 0x1140 or b.base == 0x1240) and (r16(0x1A) & 0xFF) == 0x17 then
      local sx = B.sunX(t)
      if sx then return sx, b.y end
    end
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
    -- (bounds learnt from where it has been: only once they span an arena;
    -- early on they turned 1-8's body back at x 153, and P1 waited at the
    -- edge it then reached)
    local bb = bounds[b.base]
    if bb then
      if bb[2] - bb[1] >= 96 then x = reflect(x, b.vx, bb[1], bb[2]) end
      if bb[4] - bb[3] >= 48 then y = reflect(y, b.vy, bb[3], bb[4]) end
    end
    return x, y
  end

  -- Contact windows per cell over `horizon` frames (as Enemies.windows):
  -- cell key -> { {t0, t1}, ... }, `slack` frames either side.
  -- The cells a boss box at (px,py) (+ margin m) covers at time t.
  local function mark(w, b, px, py, t, m, STEP, slack)
    local x0, x1 = px + b.box[1] - m, px + b.box[2] - 1 + m
    local y0, y1 = py + b.box[3] - m, py + b.box[4] - 1 + m
    local c0, r0 = cellOfPx(x0 - 7, y0 - 7)
    local c1, r1 = cellOfPx(x1 + 8, y1 + 8)
    for r = math.max(r0, 0), math.min(r1, MAP_H - 1) do
      for c = math.max(c0, 0), math.min(c1, MAP_W - 1) do
        local k = r * KEY_W + c
        local l = w[k]
        if not l then l = {}; w[k] = l end
        -- (merge only with an overlapping window and keep the earlier start:
        -- windows from a second pass, the 1-8 hands or 5-8's branches, come
        -- after later ones from the first, and a [0, ..] one folded into a
        -- [292, ..] one left the cell "safe" until 292)
        local t0, t1 = math.max(0, t - slack), t + STEP + slack
        local last = l[#l]
        if last and last[2] >= t0 - STEP and last[1] <= t1 then
          last[1], last[2] = math.min(last[1], t0), math.max(last[2], t1)
        else l[#l + 1] = { t0, t1 } end
      end
    end
  end
  -- Cells whose centre (where P1 stands, +-tol px) is inside a box: for
  -- the exact 1-8 hand zones, where the overlap test of mark() closed the
  -- gap under the body and most of the floor ~10 px early.
  local function markCentres(w, box, px, py, t, tol, STEP, slack)
    local ox, oy = env.origin()
    local x0, x1 = px + box[1] - tol, px + box[2] - 1 + tol
    local y0, y1 = py + box[3] - tol, py + box[4] - 1 + tol
    local c0, c1 = math.ceil((x0 + ox - 8) / 16), math.floor((x1 + ox - 8) / 16)
    local r0, r1 = math.ceil((y0 + oy - 7) / 16), math.floor((y1 + oy - 7) / 16)
    for r = math.max(r0, 0), math.min(r1, MAP_H - 1) do
      for c = math.max(c0, 0), math.min(c1, MAP_W - 1) do
        local k = r * KEY_W + c
        local l = w[k]
        if not l then l = {}; w[k] = l end
        local a, z = math.max(0, t - slack), t + STEP + slack
        local last = l[#l]
        if last and last[2] >= a - STEP and last[1] <= z then last[1], last[2] = math.min(last[1], a), math.max(last[2], z)
        else l[#l + 1] = { a, z } end
      end
    end
  end
  function B.windows(list, horizon, margin, slack)
    local w = {}
    local handsDone = false
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
          mark(w, b, px, py, t, m, STEP, slack)
        end
        -- 1-8 (stage $17): two hands (CC:A203, box +-16) hang at (+-28, +36)
        -- from the body and shoot straight down to +124 at 4 px/f (state
        -- CC:9BDD), faster than P1 walks out. Their objects are stored back
        -- at the body's position each frame: only the contact check sees
        -- where they are (port probe at CC:A569). Both columns, x +-12..44
        -- and y +20..140, are contact danger along the body's path.
        -- (A hand strikes when P1 is anywhere below within x 16..40 of its
        -- side, sight boxes CC:9E9A/9EA2: entering a column is deadly. Inside
        -- one already, a slice d px below the body is reached only after
        -- (d - 36) / 4 frames: its window starts then, leaving a way out.)
        -- (anchored on the body or a hand: the hands are stored at the
        -- body's position, and in idle frames only they are listed)
        if (b.base == 0x1000 or b.base == 0x1140 or b.base == 0x1240) and not handsDone
            and (env.r16(0x1A) & 0xFF) == 0x17 then
          handsDone = true
          local hx = { { -44, -11 }, { 12, 45 } }
          local p1x, p1y = r16(0x8010) - b.x, r16(0x8018) - b.y
          for _, h in ipairs(hx) do
            -- (staging the windows when P1 is inside let it bomb in a
            -- one-cell corridor inside a column: always on)
            local inside = false
            -- (body hurt, CC:9CA7: no strikes, the hands hang at rest +36)
            local hurtNow = (r16(0x1000) | (rd8(0x1002) << 16)) == 0xCC9CA7
            for s0 = 20, hurtNow and 52 or 140, 16 do
              local t0 = inside and math.max(0, math.floor((s0 - 36) / 4) - 2) or 0
              local slice = { box = { h[1], h[2], s0, math.min(s0 + 16, 141) } }
              for t = t0 - t0 % STEP, horizon, STEP do
                local px, py = B.predict(b, t)
                -- (no margin: the box is exact, and margin + cell padding
                -- closed the next column too, so P1 dodged along its own)
                if px then markCentres(w, slice.box, px, py, t, 4, STEP, slack) end
              end
            end
          end
        end
        -- 5-8 form 2: within 24 px of its waypoint it picks a random new one
        -- of 16 (CC:D272). From when it can be there, every one of those
        -- paths is a contact window (a dodge walked into such a turn).
        local tg = B.target[b.base]
        if B.waypoints and tg and b.state == FORM2 then
          local spd = math.max(0.5, math.sqrt(b.vx * b.vx + b.vy * b.vy))
          local dx, dy = tg[1] - b.x, tg[2] - b.y
          local d = math.sqrt(dx * dx + dy * dy)
          local ta = math.max(0, (d - 24) / spd)
          local ax, ay = tg[1], tg[2]
          if d > 24 then ax, ay = tg[1] - dx / d * 24, tg[2] - dy / d * 24 else ax, ay = b.x, b.y end
          -- (every 8 frames: 16 paths at the 4-frame step made runs 4x slower)
          local t1 = math.floor(ta / STEP + 1) * STEP
          for t = t1, math.min(horizon, t1 + 48), 2 * STEP do
            local go = spd * (t - ta)
            for _, wp in ipairs(B.waypoints) do
              local ex, ey = wp[1] - ax, wp[2] - ay
              local e = math.sqrt(ex * ex + ey * ey)
              local px, py = ax, ay
              if e > 1 then local f = math.min(1, go / e); px, py = ax + ex * f, ay + ey * f end
              mark(w, b, px, py, t, margin + t // 16, 2 * STEP, slack)
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
