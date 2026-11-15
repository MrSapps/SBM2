-- World view: one frame of the game as text (docs/worldview.md).
--
-- Facts only: positions, raw timers and flags. No "harmful" or "safe"
-- guesses; those are models and belong to the planner.
--
--   local WV = dofile("bot/worldview.lua")
--   local text = WV.encode(WV.capture(rd8, frame))  -- rd8(a): WRAM byte at offset a
--
-- Offline, from a WRAM image (port/hybrid -D N:out.wram):
--   build/tools/lua bot/worldview.lua <file.wram> [frame]
local WV = {}

local MAP, UNDER = 0x1C000, 0x1D000  -- $7FC000 TileMap, $7FD000 CellItems
local ROW, MAP_W, MAP_H = 0x80, 32, 32
local P1 = 0x8000
local BOMBS, BOMB_SLOTS, BOMB_STRIDE = 0x12000, 40, 0x100

-- Tile code -> class character (docs/worldview.md, "Cell classes").
local CLASS = {}
for t = 0x00, 0xFF do CLASS[t] = "?" end
for t = 0x04, 0x3F do CLASS[t] = "." end
CLASS[0x00] = "."
for t = 0x01, 0x03 do CLASS[t] = "*" end
for t = 0x40, 0x43 do CLASS[t] = "b" end
for _, t in ipairs { 0x6F, 0x70, 0x72 } do CLASS[t] = "." end
CLASS[0x6B] = "v"   -- world 2 button: a flame on it wakes the $F5 block
CLASS[0x44] = "i"   -- item tile (stops a blast)
for t = 0xC3, 0xCF do CLASS[t] = "#" end
CLASS[0xC2] = "%"
CLASS[0xC9] = "g"   -- closed gate (the gate line says which cell)
CLASS[0x49] = "g"   -- open gate
CLASS[0xD3] = "O"
CLASS[0xE2], CLASS[0xF5] = "X", "x"
CLASS[0xD0] = "w"
for _, t in ipairs { 0xD2, 0xD5, 0xE0, 0xE1 } do CLASS[t] = "m" end
CLASS[0xD6] = "B"
CLASS[0xE7] = "F"
CLASS[0xCD] = "K"
CLASS[0x4D] = "k"
CLASS[0x4C] = "S"
CLASS[0x4B], CLASS[0x63] = "T", "t"  -- $4B stops a blast, $63 does not
CLASS[0x25], CLASS[0x65] = "c", "C"
for _, t in ipairs { 0x69, 0x73, 0x74 } do CLASS[t] = "P" end
CLASS[0x68] = "y"
CLASS[0x64], CLASS[0x6E] = "s", "s"
CLASS[0x14] = "p"
CLASS[0x0E] = "f"
CLASS[0x66] = "^"
for t = 0x17, 0x1A do CLASS[t] = "=" end
WV.CLASS = CLASS

local function stepName(v)  -- +$4A cell step
  if v >= 0x8000 then v = v - 0x10000 end
  if v == -0x80 then return "u" elseif v == 0x80 then return "d"
  elseif v == -2 then return "l" elseif v == 2 then return "r" end
  return "-"
end

-- Capture the frame. rd8(a) reads WRAM offset a ($7E0000 = 0).
function WV.capture(rd8, frame)
  local function rd16(a) return rd8(a) | (rd8(a + 1) << 8) end
  local function signed(v) return v >= 0x8000 and v - 0x10000 or v end
  local w = { frame = frame or 0 }
  w.stage = rd8(0x1A)
  w.time = rd16(0x5E10) * 60 + rd16(0x5E12)
  local ox, oy = signed(rd16(0x5F01)), signed(rd16(0x5EFB))
  w.origin = { ox, oy }
  local light = rd16(0x5AAC)
  w.light = light == 0xFFFF and "none" or ((light & 1) == 1 and "on" or "off")

  -- Map layers
  w.raw, w.under = {}, {}
  local maxC, maxR = 0, 0
  for r = 0, MAP_H - 1 do
    local row, und = {}, {}
    for c = 0, MAP_W - 1 do
      local t = rd8(MAP + r * ROW + c * 2)
      row[c] = t
      und[c] = rd8(UNDER + r * ROW + c * 2)
    end
    w.raw[r], w.under[r] = row, und
  end
  -- Playfield size: the bounding box of cells that differ from the border fill.
  local fill = w.raw[MAP_H - 1][MAP_W - 1]
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      if w.raw[r][c] ~= fill then
        if c > maxC then maxC = c end
        if r > maxR then maxR = r end
      end
    end
  end
  w.cols, w.rows = maxC + 1, maxR + 1

  local gate = rd16(0x5D72)
  if gate ~= 0 and gate ~= 0xFFFF then
    local gc, gr = (gate & 0x7F) // 2, gate >> 7
    w.gate = { gc, gr, w.raw[gr] and w.raw[gr][gc] == 0x49 and "open" or "closed" }
  end

  -- Player 1
  local f4 = rd16(P1 + 4)
  local d = rd16(P1 + 0x6C)
  local stats = rd8(P1 + 0xF3)
  w.player = {
    x = signed(rd16(P1 + 0x10)) + ox, y = signed(rd16(P1 + 0x18)) + oy,
    dir = (d & 8) ~= 0 and "u" or (d & 4) ~= 0 and "d" or (d & 2) ~= 0 and "l" or (d & 1) ~= 0 and "r" or "-",
    hearts = rd8(P1 + 0xEF), flash = (f4 & 0x20) ~= 0 and rd8(P1 + 0xB9) or 0,
    bombs = rd8(P1 + 0xF1), maxbombs = rd8(P1 + 0xE1), e0 = rd8(P1 + 0xE0), fire = rd8(P1 + 0xE2) + 1,
    pierce = (stats & 4) ~= 0 and 1 or 0, remote = (stats & 1) ~= 0 and 1 or 0,
    glove = rd8(P1 + 0xFE), softpass = rd8(P1 + 0xFB),
    lock = rd16(P1 + 0x52), ride = (rd16(P1 + 2) & 0x8000) ~= 0 and 1 or 0,
    f4 = f4, f60 = rd16(P1 + 0x60),
  }

  -- Bombs ($7F2000 table)
  w.bombs = {}
  for i = 0, BOMB_SLOTS - 1 do
    local base = BOMBS + i * BOMB_STRIDE
    local flags = rd8(base + 1)
    if (flags & 0x80) ~= 0 then
      local cell = rd16(base + 2)
      w.bombs[#w.bombs + 1] = {
        slot = i, c = (cell & 0x3F) // 2, r = cell >> 6,
        fuse = (flags & 0x40) ~= 0 and 0 or rd8(base + 4) * 2,
        range = rd8(base + 7) + 1, owner = (flags & 0x08) ~= 0 and "enemy" or "p1",
        remote = (rd8(base) & 1), pierce = (rd8(base) & 4) ~= 0 and 1 or 0,
        magnet = (flags & 0x10) ~= 0 and 1 or 0, flags = flags,
      }
    end
  end
  -- 2-8 boss drops (docs/enemies.md 6b; as planner/capture.cpp)
  if rd8(0x1A) == 0x27 then
    local function st(base) return rd16(base) | (rd8(base + 2) << 16) end
    local falls = {}
    for base = 0x1040, 0x17C0, 0x40 do
      if st(base) == 0xCCB189 then
        local fy = rd16(base + 0x14); if fy >= 0x8000 then fy = fy - 0x10000 end
        falls[#falls + 1] = { rd16(base + 0x11), fy }
      end
    end
    for base = 0x1040, 0x17C0, 0x40 do
      if st(base) == 0xCCB2F1 then
        local mx, my, t = rd16(base + 0x11), rd16(base + 0x14), 60
        for _, f in ipairs(falls) do
          if math.abs(f[1] - mx) <= 2 then t = math.max(0, (my - f[2]) // 2 - 3) end
        end
        local c, r = (mx + ox) // 16, (my + oy) // 16
        local dup = false
        for _, b in ipairs(w.bombs) do if b.c == c and b.r == r then dup = true end end
        if not dup then
          w.bombs[#w.bombs + 1] = { slot = -1, c = c, r = r, fuse = t, range = 10, owner = "enemy",
                                   remote = 0, pierce = 0, magnet = 0, flags = 0 }
        end
      end
    end
  end

  -- Enemies and gadgets ($8400-$8F00)
  w.enemies = {}
  for base = 0x8400, 0x8F00, 0x100 do
    local h = rd16(base) & 0xF000
    if h == 0x9000 or h == 0xA000 then
      local s0 = rd16(base)
      local function vel(a, negBit)
        local v = rd16(a) + rd16(a + 2) / 65536
        if (s0 & negBit) ~= 0 then v = -v end
        return v
      end
      local tgt = rd16(base + 0x4C)
      w.enemies[#w.enemies + 1] = {
        slot = (base >> 8) - 0x80, type = rd8(base + 0x7E),
        x = (signed(rd16(base + 0x10)) + ox) & 511, y = (signed(rd16(base + 0x18)) + oy) & 511,  -- (wrap: as the 32x32 TileMap)
        vx = vel(base + 0x14, 1), vy = vel(base + 0x1C, 2),
        step = stepName(rd16(base + 0x4A)), target = { (tgt & 0x7F) // 2, (tgt >> 7) & 0x1F },
        wait = rd16(base + 0x4E), hp = rd8(base + 0x43),
        f4 = rd16(base + 4), c0 = rd16(base + 0xC0), d0 = rd16(base + 0xD0),
        be = rd16(base + 0xBE), bc = rd16(base + 0xBC), ba = rd16(base + 0xBA),
        offscreen = h == 0xA000 and 1 or 0,
      }
    end
  end

  -- Gold panels ($5B56 list; mode $5D94 bit 0; bit 15 = all done)
  w.panels = {}
  local pstate = rd16(0x5D94)
  if pstate ~= 0xFFFF then
    local a = 0x5B56
    while rd16(a) ~= 0xFFFF and a < 0x5B56 + 8 * 32 do
      local cell = rd16(a + 2)
      local c, r = (cell & 0x7F) // 2, cell >> 7
      local done
      if (pstate & 1) == 0 then done = w.raw[r][c] ~= 0x69 else done = (rd16(a) & 1) ~= 0 end
      w.panels[#w.panels + 1] = { c, r, done and "done" or "todo", pstate & 1 }
      a = a + 8
    end
  end
  if light ~= 0xFFFF then
    local cell = rd16(0x5AAE)
    w.switch = { (cell & 0x7F) // 2, cell >> 7 }
  end

  -- Items: the $3C48 list {cell, kind} (kind bit 14: its block broke;
  -- 0 bomb up, 2 fire up, 11 heart). Visible ones are $44 tiles; hidden
  -- ones still sit in an unbroken soft block.
  w.items, w.hidden = {}, {}
  local kinds = {}
  for i = 0, 63 do
    local cell = rd16(0x3C48 + 4 * i)
    if cell >= 0x8000 then break end
    local kind = rd16(0x3C48 + 4 * i + 2)
    local c, r = (cell & 0x7F) // 2, cell >> 7
    if r < MAP_H then
      kinds[cell] = kind & 0x3FFF
      if (kind & 0x4000) == 0 and CLASS[w.raw[r][c]] == "%" then w.hidden[#w.hidden + 1] = { c, r, kind & 0x3FFF } end
    end
  end
  for r = 0, MAP_H - 1 do
    for c = 0, MAP_W - 1 do
      if w.raw[r][c] == 0x44 then
        local k = kinds[r * ROW + c * 2] or -1
        w.items[#w.items + 1] = { c, r, k == 11 and "heart" or "item", k }
      end
    end
  end

  -- Magnets ($5828 list: +2 cell, +8 dir 0/2/4/6 = up/right/down/left)
  w.magnets = {}
  local MD = { [0] = "u", [2] = "r", [4] = "d", [6] = "l" }
  for i = 0, 3 do
    local m = 0x5828 + i * 0x40
    local cell, dir = rd16(m + 2), rd16(m + 8)
    local c, r = (cell & 0x7F) // 2, cell >> 7
    if MD[dir] and r < MAP_H and CLASS[w.raw[r][c]] == "m" then
      w.magnets[#w.magnets + 1] = { c, r, MD[dir] }
    end
  end
  return w
end

local function num(v)
  if v == 0 then return "0" end
  if math.type(v) == "integer" then return tostring(v) end
  local s = string.format("%.4f", v):gsub("0+$", ""):gsub("%.$", "")
  return s
end

function WV.encode(w)
  local o = {}
  local function add(s) o[#o + 1] = s end
  add("worldview 1")
  add(string.format("stage %02X", w.stage))
  add("frame " .. w.frame)
  add("time " .. w.time)
  add(string.format("origin %d %d", w.origin[1], w.origin[2]))
  add(string.format("size %d %d", w.cols, w.rows))
  add("light " .. w.light)
  if w.gate then add(string.format("gate %d %d %s", w.gate[1], w.gate[2], w.gate[3])) end
  local p = w.player
  add(string.format("player x=%d y=%d dir=%s hearts=%d flash=%d bombs=%d maxbombs=%d e0=%d fire=%d pierce=%d remote=%d glove=%d softpass=%d lock=%04X ride=%d f4=%04X f60=%04X",
    p.x, p.y, p.dir, p.hearts, p.flash, p.bombs, p.maxbombs, p.e0, p.fire, p.pierce, p.remote, p.glove, p.softpass, p.lock, p.ride, p.f4, p.f60))
  for _, b in ipairs(w.bombs) do
    add(string.format("bomb c=%d r=%d fuse=%d range=%d owner=%s remote=%d pierce=%d magnet=%d slot=%d flags=%02X",
      b.c, b.r, b.fuse, b.range, b.owner, b.remote, b.pierce, b.magnet, b.slot, b.flags))
  end
  for _, e in ipairs(w.enemies) do
    add(string.format("enemy slot=%d type=%02X x=%d y=%d vx=%s vy=%s step=%s target=%d,%d wait=%d hp=%d f4=%04X c0=%04X d0=%04X be=%04X bc=%04X ba=%04X off=%d",
      e.slot, e.type, e.x, e.y, num(e.vx), num(e.vy), e.step, e.target[1], e.target[2], e.wait, e.hp,
      e.f4, e.c0, e.d0, e.be, e.bc, e.ba, e.offscreen))
  end
  for _, pn in ipairs(w.panels) do add(string.format("panel %d %d %s mode=%d", pn[1], pn[2], pn[3], pn[4])) end
  if w.switch then add(string.format("switch %d %d", w.switch[1], w.switch[2])) end
  for _, it in ipairs(w.items) do add(string.format("item %d %d %s kind=%d", it[1], it[2], it[3], it[4])) end
  for _, it in ipairs(w.hidden) do add(string.format("hidden %d %d kind=%d", it[1], it[2], it[3])) end
  for _, m in ipairs(w.magnets) do add(string.format("magnet %d %d dir=%s", m[1], m[2], m[3])) end
  local function layer(name, src, f)
    add(name)
    for r = 0, w.rows - 1 do
      local s = {}
      for c = 0, w.cols - 1 do s[#s + 1] = f(src[r][c]) end
      add(table.concat(s, name:sub(1, 3) == "raw" and " " or ""))
    end
  end
  layer("map", w.raw, function(t) return CLASS[t] end)
  layer("under", w.under, function(t) return CLASS[t] end)
  layer("raw", w.raw, function(t) return string.format("%02X", t) end)
  layer("rawunder", w.under, function(t) return string.format("%02X", t) end)
  return table.concat(o, "\n") .. "\n"
end

-- Offline: build/tools/lua bot/worldview.lua <file.wram> [frame]
if arg and arg[0] and arg[0]:match("worldview%.lua$") and arg[1] then
  local fh = assert(io.open(arg[1], "rb"))
  local data = fh:read("a"); fh:close()
  assert(#data == 0x20000, "not a 128 KiB WRAM image")
  local function rd8(a) return data:byte(a + 1) end
  io.write(WV.encode(WV.capture(rd8, tonumber(arg[2]) or 0)))
end

return WV
