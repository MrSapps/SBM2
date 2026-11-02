-- Enemy model for the bot (rules: docs/enemies.md).
--
-- Contact kills P1 when an enemy's cell (+$A8) equals P1's; the cell flips
-- half-way between centres. Generic walkers go straight until the next cell
-- is solid ($80+), a bomb or another enemy, stop at the centre, wait 32
-- frames (+$4E counts down), then take a random free direction. Chasers
-- (+$D0 bit 7) re-aim at P1 at each centre. Paths are sampled (rollouts) so
-- both danger windows and kill chances come from the same prediction.
--
--   local E = dofile(dir .. "/enemies.lua")(env)   env: r16, rd8, MAP_W, MAP_H, KEY_W, tileCentre
--   E.read(g)                       -> enemies (see fields below)
--   E.rollouts(g, list, pc, pr, n, horizon) -> samples (per enemy: n paths of {k, t0, t1})
--   E.windows(samples, list, tmax, margin)   -> windows[k] = { {t0, t1}, ... } (contact)
--   E.killChance(samples, list, cells, t0, t1) -> expected kills, best single chance
return function(env)
  local r16, rd8 = env.r16, env.rd8
  local MAP_W, MAP_H, KEY_W = env.MAP_W, env.MAP_H, env.KEY_W
  local FIRST, LAST = 0x8400, 0x8F00
  local SAFE_TYPES = { [0x30] = true, [0x31] = true, [0x16] = true, [0x24] = true, [0x25] = true,
    [0x26] = true, [0x27] = true, [0x28] = true }  -- C5:D850: never kill on contact
  -- Types that use the chaser once +$D0 bit 7 is set (and $89 always).
  local CHASERS = { [0x82] = true, [0x85] = true, [0x89] = true, [0x8A] = true, [0x91] = true,
    [0x93] = true, [0x97] = true }
  local DIRS = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }  -- $0400 & 3: up, down, left, right

  local function cellOf(idx)
    local c = (idx & 0x7F) // 2
    if c >= 32 then c = c - 32 end
    return c, (idx >> 7) & 0x1F
  end

  local E = {}

  -- Live enemy objects. Fields: base, type, c, r (cell, +$A8), tc, tr (target
  -- cell, +$4C), dc, dr (step, +$4A), moving, wait (+$4E), cpf (frames per
  -- cell), remain (frames to the target centre), harmful (kills on contact),
  -- killable (a blast can kill it), chaser, gadget (type $C0+).
  function E.read(g)
    local list = {}
    for base = FIRST, LAST, 0x100 do
      local f4 = r16(base + 4)
      if (r16(base) & 0xF000) == 0x9000 and (f4 & 0x8000) == 0 then
        local typ = r16(base + 0x7E) & 0xFF
        local c, r = cellOf(r16(base + 0xA8))
        if r < MAP_H and c < MAP_W then
          local e = { base = base, type = typ, c = c, r = r }
          e.gadget = (typ >= 0xC0 or (typ & 0x80) == 0) and typ ~= 0xDD
          local hidden = (f4 & 0x60) ~= 0 or (rd8(base + 0x41) & 0x80) ~= 0
            or (r16(base + 0x68) & 2) ~= 0
            or ((typ == 0x98 or typ == 0x90) and (r16(base + 0xBE) & 0x8000) ~= 0)
          -- $DD track blocks never kill by contact but crush P1 ahead of them.
          e.harmful = not e.gadget and not hidden and (not SAFE_TYPES[typ & 0x7F] or typ == 0xDD)
          local hp = rd8(base + 0x43)
          e.killable = not e.gadget and typ ~= 0xDD and (hp & 0x7F) ~= 0x7F and (f4 & 0x40) == 0
          e.immune = (hp & 0x7F) == 0x7F
          e.chaser = CHASERS[typ] and ((rd8(base + 0xD0) & 0x80) ~= 0 or typ == 0x89)
          -- $8A/$97: x-ray chasers by the light switch $7E5AAC ($8A even, $97 odd).
          local light = r16(0x5AAC)
          if (typ == 0x8A or typ == 0x97) and light ~= 0xFFFF then
            e.chaser = (light & 1) == (typ == 0x97 and 1 or 0)
          end
          e.moving = (r16(base + 0xC0) & 0x8000) ~= 0
          local step = r16(base + 0x4A)
          if step >= 0x8000 then step = step - 0x10000 end
          if step == 0x80 then e.dr = 1 elseif step == -0x80 then e.dr = -1 else e.dr = 0 end
          e.dc = (step == 2 and 1) or (step == -2 and -1) or 0
          e.tc, e.tr = cellOf(r16(base + 0x4C))
          e.wait = r16(base + 0x4E)
          local speed = r16(base + 0x46) + r16(base + 0x48) / 0x10000
          if speed <= 0.05 then speed = 0.5 end
          e.cpf = 16 / speed
          if e.moving then
            local cx, cy = env.tileCentre(e.tc, e.tr)
            local d = math.abs(r16(base + 0x10) - cx) + math.abs(r16(base + 0x18) - cy)
            if d > 0x8000 then d = 0 end
            e.remain = math.min(d / speed, 2 * e.cpf)
          else
            e.remain = 0
          end
          list[#list + 1] = e
        end
      end
    end
    return list
  end

  local function blocked(g, c, r)
    if r < 0 or r >= MAP_H or c < 0 or c >= MAP_W then return true end
    local t = g[r][c]
    return t >= 0x80 or (t >= 0x40 and t <= 0x43)
  end

  -- One sampled path: list of {k, t0, t1} occupancy intervals (frames from now).
  local function rollout(g, e, pc, pr, horizon, occupied)
    local path = {}
    local c, r, t = e.c, e.r, 0
    local dc, dr = e.dc, e.dr
    local half = e.cpf / 2
    local function free(cc, rr)
      -- $DD track blocks only move along the $4D track (docs/mechanics.md section 7).
      if e.type == 0xDD then return g[rr] ~= nil and g[rr][cc] == 0x4D end
      return not blocked(g, cc, rr) and not occupied[rr * KEY_W + cc]
    end
    local function chooseDir()
      if e.chaser then
        local ddc, ddr = pc - c, pr - r
        local opts = {}
        if math.abs(ddc) >= math.abs(ddr) then
          opts = { { ddc > 0 and 1 or -1, 0 }, { 0, ddr >= 0 and 1 or -1 }, { 0, ddr >= 0 and -1 or 1 } }
        else
          opts = { { 0, ddr > 0 and 1 or -1 }, { ddc >= 0 and 1 or -1, 0 }, { ddc >= 0 and -1 or 1, 0 } }
        end
        for _, d in ipairs(opts) do
          if (d[1] ~= 0 or d[2] ~= 0) and free(c + d[1], r + d[2]) then return d[1], d[2] end
        end
      end
      local frees = {}
      for _, d in ipairs(DIRS) do if free(c + d[1], r + d[2]) then frees[#frees + 1] = d end end
      if #frees == 0 then return nil end
      local d = frees[math.random(#frees)]
      return d[1], d[2]
    end
    -- In motion toward the target cell: occupy the current cell until the
    -- half-way point, the target from then on.
    local enter = 0
    if e.moving and (e.tc ~= c or e.tr ~= r) then
      local flip = math.max(0, e.remain - half)
      path[#path + 1] = { r * KEY_W + c, 0, flip }
      c, r, enter, t = e.tc, e.tr, flip, e.remain
    elseif e.moving then
      t = e.remain
    else
      t = e.wait
      dc, dr = nil, nil
    end
    while t < horizon do
      if dc and (dc ~= 0 or dr ~= 0) and not e.chaser and free(c + dc, r + dr) then
        path[#path + 1] = { r * KEY_W + c, enter, t + half }
        c, r, enter, t = c + dc, r + dr, t + half, t + e.cpf
      else
        if dc then t = t + 32 end  -- stopped: +$4E = $20
        local ndc, ndr = chooseDir()
        if not ndc then break end
        if e.chaser then
          if not free(c + ndc, r + ndr) then break end
          path[#path + 1] = { r * KEY_W + c, enter, t + half }
          c, r, enter, t = c + ndc, r + ndr, t + half, t + e.cpf
          dc, dr = ndc, ndr
        else
          dc, dr = ndc, ndr
        end
      end
    end
    path[#path + 1] = { r * KEY_W + c, enter, math.huge }
    return path
  end

  function E.rollouts(g, list, pc, pr, n, horizon)
    local occupied = {}
    for _, e in ipairs(list) do
      if not e.gadget then
        occupied[e.r * KEY_W + e.c] = true
        if e.moving then occupied[e.tr * KEY_W + e.tc] = true end
      end
    end
    local samples = {}
    for i, e in ipairs(list) do
      local own = {}
      for k in pairs(occupied) do own[k] = true end
      own[e.r * KEY_W + e.c] = nil
      own[e.tr * KEY_W + e.tc] = nil
      local s = {}
      if not e.gadget then
        for j = 1, n do s[j] = rollout(g, e, pc, pr, horizon, own) end
      end
      samples[i] = s
    end
    return samples
  end

  -- Contact danger: every cell a harmful enemy may be in before tmax.
  function E.windows(samples, list, tmax, margin)
    local w = {}
    for i, e in ipairs(list) do
      if e.harmful then
        for _, path in ipairs(samples[i]) do
          for _, iv in ipairs(path) do
            if iv[2] <= tmax then
              local l = w[iv[1]]
              if not l then l = {}; w[iv[1]] = l end
              l[#l + 1] = { iv[2] - margin, iv[3] + margin }
            end
          end
        end
      end
    end
    return w
  end

  -- cells: set of keys a blast covers during [t0, t1]. Returns the expected
  -- number of killable enemies caught and the best single-enemy chance.
  function E.killChance(samples, list, cells, t0, t1)
    local total, best = 0, 0
    for i, e in ipairs(list) do
      local s = samples[i]
      if e.killable and #s > 0 then
        local hits = 0
        for _, path in ipairs(s) do
          for _, iv in ipairs(path) do
            if cells[iv[1]] and iv[2] <= t1 and iv[3] >= t0 then hits = hits + 1; break end
          end
        end
        local p = hits / #s
        total = total + p
        if p > best then best = p end
      end
    end
    return total, best
  end

  return E
end
