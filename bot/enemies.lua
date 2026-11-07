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
--   E.killChance(samples, list, cells, t0, t1, skip) -> expected kills, best single chance
--   E.eaterFate(g, samples, list, bc, br, tb, fuse, range, flame) -> P(eaten), P(eater killed)
return function(env)
  local r16, rd8 = env.r16, env.rd8
  local MAP_W, MAP_H, KEY_W = env.MAP_W, env.MAP_H, env.KEY_W
  local FIRST, LAST = 0x8400, 0x8F00
  local SAFE_TYPES = { [0x30] = true, [0x31] = true, [0x16] = true, [0x24] = true, [0x25] = true,
    [0x26] = true, [0x27] = true, [0x28] = true }  -- C5:D850: never kill on contact
  -- Types that use the chaser once +$D0 bit 7 is set (and $89 always).
  -- ($8C, 1-x: C1:844F sets +$D0 bits 6-7 on its first frame and then
  -- runs ChaseUpdate. Not in stages with magnets, see E.read.)
  local CHASERS = { [0x82] = true, [0x85] = true, [0x89] = true, [0x8A] = true, [0x8C] = true, [0x91] = true,
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
      if ((r16(base) & 0xF000) == 0x9000 or (r16(base) & 0xF000) == 0xA000) and (f4 & 0x8000) == 0 then
        local typ = r16(base + 0x7E) & 0xFF
        local c, r = cellOf(r16(base + 0xA8))
        if r < MAP_H and c < MAP_W then
          local e = { base = base, type = typ, c = c, r = r }
          e.gadget = (typ >= 0xC0 or (typ & 0x80) == 0) and typ ~= 0xDD
          -- (+4 bit 5, flashing after a hit or a wake-up, is harmless only
          -- for now: still harmful for planning. 4-6: P1 fled onto a
          -- flashing $93's cell, it stopped flashing, 12 deaths.)
          -- (flashing, +4 bit 5, is harmless only for now: for the chasers
          -- that come straight back at P1 it stays harmful for planning;
          -- 4-6 fled onto a flashing $93. For all types it broke 3-3, whose
          -- hit $90/$98 flash where P1 has to pass: 0/2 seeds, 18 deaths.)
          local flashHarm = typ == 0x93 or typ == 0x8A or typ == 0x97 or typ == 0x85
          local hidden = (f4 & (flashHarm and 0x40 or 0x60)) ~= 0 or (rd8(base + 0x41) & 0x80) ~= 0
            or (r16(base + 0x68) & 2) ~= 0
            or ((typ == 0x98 or typ == 0x90) and (r16(base + 0xBE) & 0x8000) ~= 0)
          -- $DD track blocks never kill by contact but crush P1 ahead of them.
          e.harmful = not e.gadget and not hidden and (not SAFE_TYPES[typ & 0x7F] or typ == 0xDD)
          -- ($91 attacking, hp $FF, still flames the cell next to it: harmful
          -- unless dying; 5-3 dodged next to one mid-attack)
          if typ == 0x91 and (r16(base + 0x68) & 2) == 0 and (f4 & 0x40) == 0 then e.harmful = true end
          local hp = rd8(base + 0x43)
          e.hp = hp & 0x7F
          -- A blast turns a $90/$98 into a harmless mount (+$BE bit 15, a $99
          -- rider spawns): no target any more (3-1, run 22: hunted the mount
          -- by the open gate until time ran out, three game overs).
          e.mount = (typ == 0x98 or typ == 0x90) and (r16(base + 0xBE) & 0x8000) ~= 0
          e.killable = not e.gadget and typ ~= 0xDD and (hp & 0x7F) ~= 0x7F and (f4 & 0x40) == 0 and not e.mount
          e.immune = (hp & 0x7F) == 0x7F
          e.chaser = CHASERS[typ] and ((rd8(base + 0xD0) & 0x80) ~= 0 or typ == 0x89)
          -- (the "magnet caught P1" in 1-3 was the $8C's own pull: see
          -- PullLine in bot.lua; it is a chaser in every stage)
          -- $8A/$97: x-ray chasers by the light switch $7E5AAC ($8A even, $97 odd).
          local light = r16(0x5AAC)
          if (typ == 0x8A or typ == 0x97) and light ~= 0xFFFF then
            e.chaser = (light & 1) == (typ == 0x97 and 1 or 0)
          end
          e.moving = (r16(base + 0xC0) & 0x8000) ~= 0 or typ == 0xB2
          local step = r16(base + 0x4A)
          if step >= 0x8000 then step = step - 0x10000 end
          if step == 0x80 then e.dr = 1 elseif step == -0x80 then e.dr = -1 else e.dr = 0 end
          e.dc = (step == 2 and 1) or (step == -2 and -1) or 0
          e.tc, e.tr = cellOf(r16(base + 0x4C))
          e.wait = r16(base + 0x4E)
          local speed = r16(base + 0x46) + r16(base + 0x48) / 0x10000
          -- $B2: $90's shot (1.5 px/f across, 2.0 up/down; its speed
          -- fields read 0), always flying, gone at the first solid cell.
          if typ == 0xB2 then
            e.shot, e.moving = true, true
            speed = e.dr ~= 0 and 2.0 or 1.5
          end
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
    if e.shot then
      -- straight on until a solid cell, then gone (no occupancy after)
      local cpf, hf = e.cpf, e.cpf / 2
      local enter = 0  -- (+$4C is 0 for a shot: step from its own cell)
      t = hf
      while t < horizon and (e.dc ~= 0 or e.dr ~= 0) and not blocked(g, c + e.dc, r + e.dr) do
        path[#path + 1] = { r * KEY_W + c, enter, t + hf }
        c, r, enter, t = c + e.dc, r + e.dr, t + hf, t + cpf
      end
      path[#path + 1] = { r * KEY_W + c, enter, t + hf }
      return path
    end
    -- $86 (docs/enemies.md, C1:664B): rests 128 frames, then dashes at
    -- most 4 cells at 2.5 px/f, toward P1 when P1 is in sight (straight
    -- line, 8 cells, walls block), else a random free way. The generic
    -- model had it at 0.5 px/f: 2-2 died to its dash three times.
    if e.type == 0x86 then
      local cpf, hf = 16 / 2.5, 8 / 2.5
      local function sight()
        for _, d in ipairs(DIRS) do
          for k = 1, 8 do
            local cc, rr = c + d[1] * k, r + d[2] * k
            if blocked(g, cc, rr) then break end
            if cc == pc and rr == pr then return d[1], d[2] end
          end
        end
      end
      local enter = 0
      local steps = 4
      if e.moving then
        if (e.tc ~= c or e.tr ~= r) then
          path[#path + 1] = { r * KEY_W + c, 0, hf }
          c, r, enter, t = e.tc, e.tr, hf, cpf
        end
        dc, dr = e.dc, e.dr
        steps = 3
      else
        t = math.max(e.wait or 0, 0)
        dc, dr = nil, nil
      end
      while t < horizon do
        if not dc then
          -- (half the samples dash a random way: P1 moving into another of
          -- its lines is seen there, and windows must cover every line)
          if math.random() < 0.5 then dc, dr = sight() end
          if not dc then
            local frees = {}
            for _, d in ipairs(DIRS) do if free(c + d[1], r + d[2]) then frees[#frees + 1] = d end end
            if #frees == 0 then break end
            local d = frees[math.random(#frees)]
            dc, dr = d[1], d[2]
          end
          steps = 4
        end
        if steps > 0 and free(c + dc, r + dr) then
          path[#path + 1] = { r * KEY_W + c, enter, t + hf }
          c, r, enter, t = c + dc, r + dr, t + hf, t + cpf
          steps = steps - 1
        else
          t = t + 128
          dc, dr = nil, nil
        end
      end
      path[#path + 1] = { r * KEY_W + c, enter, math.huge }
      return path
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
              -- $91 flames P1's cell when P1 is next to it (C1:BA6B): its
              -- neighbours are deadly too (5-3: 3 "blast" deaths by one).
              if e.type == 0x91 then
                for _, d in ipairs(DIRS) do
                  local k = iv[1] + d[2] * KEY_W + d[1]
                  local l2 = w[k]
                  if not l2 then l2 = {}; w[k] = l2 end
                  l2[#l2 + 1] = { iv[2] - margin, iv[3] + margin }
                end
              end
            end
          end
        end
      end
    end
    return w
  end

  -- cells: set of keys a blast covers during [t0, t1]. Returns the expected
  -- number of killable enemies caught and the best single-enemy chance.
  function E.killChance(samples, list, cells, t0, t1, skip)
    local total, best = 0, 0
    for i, e in ipairs(list) do
      local s = samples[i]
      if e.killable and #s > 0 and not (skip and skip[e.type]) then
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

  -- The $95 bomb eater (docs/enemies.md): at each cell centre it turns
  -- toward the nearest bomb within 5 cells in a straight line (solid tiles
  -- block the search) and swallows it on arrival, with no blast. A bomb put
  -- at (bc,br) at time tb with this fuse is eaten if an eater sees it and
  -- gets there first; if it gets there late, it walks into the blast.
  -- Samples are the eater's paths without the bomb, followed until it sees it.
  E.EATER = 0x95
  function E.eaterFate(g, samples, list, bc, br, tb, fuse, range, flame)
    local eaten, killed = 0, 0
    local te = tb + fuse
    for i, e in ipairs(list) do
      local s = samples[i]
      if e.type == E.EATER and not e.gadget and #s > 0 then
        local half = e.cpf / 2
        local ne, nk = 0, 0
        for _, path in ipairs(s) do
          for _, iv in ipairs(path) do
            local tc = math.max(iv[2] + half, 0)
            if tc > te then break end
            local c, r = iv[1] % KEY_W, iv[1] // KEY_W
            local d
            if tc >= tb and (c == bc or r == br) and (c ~= bc or r ~= br) then
              d = math.abs(c - bc) + math.abs(r - br)
              local sc, sr = (bc > c and 1) or (bc < c and -1) or 0, (br > r and 1) or (br < r and -1) or 0
              for j = 1, d - 1 do
                if d > 5 or g[r + sr * j][c + sc * j] >= 0x80 then d = nil; break end
              end
              if d and d > 5 then d = nil end
            end
            if d then
              local arrive = tc + d * e.cpf
              if arrive <= te then ne = ne + 1
              elseif d - (te + flame - tc) / e.cpf <= range then nk = nk + 1 end
              break
            end
            if iv[1] == br * KEY_W + bc then break end
          end
        end
        eaten = math.max(eaten, ne / #s)
        if e.killable then killed = math.max(killed, nk / #s) end
      end
    end
    return eaten, killed
  end

  return E
end
