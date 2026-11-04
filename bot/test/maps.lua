-- Text stage maps for the planner tests (bot/test/maps/*.map), and the WRAM
-- image the bot reads them from. A .map file:
--
--   # comment
--   stage 54            current stage ($1A, world<<4 | stage-1)
--   p1 2 12             P1's cell
--   hearts 1            P1 +$EF
--   fire 2              blast length (P1 +$E2 = fire - 1)
--   bombs 1             bombs that can still be placed (P1 +$F1)
--   gate 4 3            exit gate cell ($7E5D72)
--   panels 1            gold panel mode ($7E5D94 bit 0); "panels none" = $FFFF
--   panel 3 4           a panel to do (mode 0: the cell must hold $69)
--   panel 5 4 done      a toggled one (mode 1)
--   grid                then one row per line: hex tile codes, "." for $00
--   C5 C5 C5 ...
--
-- Origin is 0, so P1's pixel position is its cell centre. Everything else
-- in WRAM is 0 (no enemies, no bombs, no boss).
local M = {}

local MAP, ROW, P1 = 0x1C000, 0x80, 0x8000

function M.parse(text)
  local s = { grid = {}, panelList = {}, hearts = 0, fire = 1, bombs = 1, stage = 0x10, mode = nil }
  local inGrid = false
  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    line = line:gsub("#.*$", "")
    local words = {}
    for w in line:gmatch("%S+") do words[#words + 1] = w end
    if #words == 0 then
      -- blank lines are fine anywhere
    elseif inGrid then
      local row = {}
      for i, w in ipairs(words) do row[i - 1] = w == "." and 0 or assert(tonumber(w, 16), "bad tile " .. w) end
      s.grid[#s.grid + (s.grid[0] and 1 or 0)] = row
    else
      local k = words[1]
      local n1, n2 = tonumber(words[2]), tonumber(words[3])
      if k == "grid" then inGrid = true
      elseif k == "stage" then s.stage = tonumber(words[2], 16)
      elseif k == "p1" then s.p1 = { n1, n2 }
      elseif k == "hearts" then s.hearts = n1
      elseif k == "fire" then s.fire = n1
      elseif k == "bombs" then s.bombs = n1
      elseif k == "gate" then s.gate = { n1, n2 }
      elseif k == "panels" then s.mode = words[2] == "none" and "none" or n1
      elseif k == "panel" then s.panelList[#s.panelList + 1] = { n1, n2, words[4] == "done" }
      else error("unknown key " .. k) end
    end
  end
  assert(s.p1, "no p1 line")
  return s
end

function M.load(path)
  local fh = assert(io.open(path, "r"), "can't read " .. path)
  local text = fh:read("a"); fh:close()
  return M.parse(text)
end

-- WRAM image (table indexed from 0) for a parsed map.
function M.toWram(s)
  local w = {}
  for i = 0, 0x1FFFF do w[i] = 0 end
  local function w8(a, v) w[a] = v & 0xFF end
  local function w16(a, v) w8(a, v); w8(a + 1, v >> 8) end
  for r, row in pairs(s.grid) do
    for c, t in pairs(row) do w8(MAP + r * ROW + c * 2, t) end
  end
  local c, r = s.p1[1], s.p1[2]
  w16(P1, 0x9000)                 -- a live player object
  w16(P1 + 0x10, c * 16 + 8)      -- pixel x, y (origin 0)
  w16(P1 + 0x18, r * 16 + 8)
  w16(P1 + 0xA8, r * ROW + c * 2) -- cell index
  w8(P1 + 0xEF, s.hearts)
  w8(P1 + 0xE2, s.fire - 1)
  w8(P1 + 0xF1, s.bombs)
  w8(0x1A, s.stage)
  w8(0xBC, 1)                     -- normal game (think's in-stage test)
  w16(0x1C6C, 3)                  -- lives
  w16(0x5E10, 3)                  -- stage timer: 3 minutes
  w16(0x5D70, 0x8000)
  if s.gate then w16(0x5D72, s.gate[2] * ROW + s.gate[1] * 2) end
  -- Gold panels: 8-byte entries at $7E5B56 (+0 bit 0 toggled, +2 cell).
  local a = 0x5B56
  if s.mode == nil or s.mode == "none" then
    w16(0x5D94, 0xFFFF)
  else
    w16(0x5D94, s.mode & 1)
    for _, p in ipairs(s.panelList) do
      w16(a, p[3] and 1 or 0)
      w16(a + 2, p[2] * ROW + p[1] * 2)
      a = a + 8
    end
  end
  w16(a, 0xFFFF)
  w16(0x3C48, 0xFFFF)             -- no hidden items
  return w
end

-- A .map text from a WRAM image (bytes indexed from 0) and the bot's view.
function M.fromWram(w, title)
  local function r16(a) return w[a] | (w[a + 1] << 8) end
  local out = { "# " .. (title or "from a WRAM image") }
  local ox, oy = r16(0x5F01), r16(0x5EFB)
  local pc, pr = (r16(P1 + 0x10) + ox) // 16, (r16(P1 + 0x18) + oy) // 16
  out[#out + 1] = string.format("stage %02X", w[0x1A])
  out[#out + 1] = string.format("p1 %d %d", pc, pr)
  out[#out + 1] = string.format("hearts %d", w[P1 + 0xEF])
  out[#out + 1] = string.format("fire %d", w[P1 + 0xE2] + 1)
  out[#out + 1] = string.format("bombs %d", w[P1 + 0xF1])
  local gate = r16(0x5D72)
  if gate ~= 0 then out[#out + 1] = string.format("gate %d %d", (gate & 0x7F) // 2, gate >> 7) end
  local state = r16(0x5D94)
  if state == 0xFFFF then
    out[#out + 1] = "panels none"
  else
    out[#out + 1] = string.format("panels %d", state & 1)
    local a = 0x5B56
    while r16(a) ~= 0xFFFF and a < 0x5B56 + 8 * 32 do
      local cell = r16(a + 2)
      out[#out + 1] = string.format("panel %d %d%s", (cell & 0x7F) // 2, cell >> 7,
        ((state & 1) == 1 and (r16(a) & 1) == 1) and " done" or "")
      a = a + 8
    end
  end
  -- Columns up to the last non-zero tile; rows down to the bottom border
  -- (the first row after row 0 that is one tile all the way across).
  local maxR, maxC = 31, 0
  for r = 0, 31 do
    for c = 0, 31 do
      if w[MAP + r * ROW + c * 2] ~= 0 and c > maxC then maxC = c end
    end
  end
  for r = 1, 31 do
    local t0, same = w[MAP + r * ROW], true
    for c = 1, maxC do if w[MAP + r * ROW + c * 2] ~= t0 then same = false; break end end
    if same and t0 >= 0x80 then maxR = r; break end
  end
  out[#out + 1] = "grid"
  for r = 0, maxR do
    local t = {}
    for c = 0, maxC do
      local v = w[MAP + r * ROW + c * 2]
      t[#t + 1] = v == 0 and " ." or string.format("%02X", v)
    end
    out[#out + 1] = table.concat(t, " ")
  end
  return table.concat(out, "\n") .. "\n"
end

return M
