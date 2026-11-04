-- Print the stage map of a WRAM image: tile codes, P1 (@), live enemies (e),
-- bombs (b), the gate (G), unfinished panels (marked *).
--   build/tools/lua bot/test/map.lua <file.wram>
local ROOT = arg[0]:match("^(.*)/bot/test/[^/]*$") or "."
local E = dofile(ROOT .. "/bot/test/emu.lua")
local out = os.tmpname(); os.remove(out)
E.install { wram = assert(arg[1]), rom = ROOT .. "/baserom.sfc", env = { SBM2_OUT = out } }
local bot = E.loadBot(ROOT .. "/bot/bot.lua")
local g = bot.readMap()
local function r16(a) return E.peek(a) | (E.peek(a + 1) << 8) end
local KEY_W = bot.KEY_W
local mark = {}
local pc, pr = bot.playerTile()
mark[pr * KEY_W + pc] = "@"
local ox, oy = r16(0x5F01), r16(0x5EFB)
local es = {}
for base = 0x8400, 0x8F00, 0x100 do
  if (r16(base) & 0xF000) == 0x9000 then
    local c, r = (r16(base + 0x10) + ox) // 16, (r16(base + 0x18) + oy) // 16
    local dormant = (r16(base + 4) & 0x8000) ~= 0
    mark[r * KEY_W + c] = mark[r * KEY_W + c] or (dormant and "d" or "e")
    es[#es + 1] = string.format("%04X type %02X (%d,%d)%s", base, r16(base + 0x7E) & 0xFF, c, r, dormant and " dormant" or "")
  end
end
local gate = r16(0x5D72)
mark[(gate >> 7) * KEY_W + (gate & 0x7F) // 2] = "G"
local todo = bot.panels()
local maxC, maxR = 0, 0
for r = 0, bot.MAP_H - 1 do
  for c = 0, bot.MAP_W - 1 do
    if g[r][c] ~= 0 and not (r == 0 and c == 0) then
      if c > maxC and g[r][c] ~= g[0][0] then maxC = c end
      if r > maxR and g[r][c] ~= g[0][0] then maxR = r end
    end
  end
end
local hdr = { "    " }
for c = 0, maxC + 1 do hdr[#hdr + 1] = string.format("%3d", c) end
print(table.concat(hdr))
for r = 0, maxR + 1 do
  local t = { string.format("%3d ", r) }
  for c = 0, maxC + 1 do
    local k = r * KEY_W + c
    t[#t + 1] = string.format("%s%02X", mark[k] or (todo[k] and "*") or " ", g[r][c])
  end
  print(table.concat(t))
end
print(string.format("stage %02X  P1 (%d,%d) px(%d,%d)  gate (%d,%d) 5D94=%04X 5D70=%04X  bombs left %d fire %d",
  E.peek(0x1A), pc, pr, r16(0x8010), r16(0x8018), (gate & 0x7F) // 2, gate >> 7, r16(0x5D94), r16(0x5D70),
  E.peek(0x80F1), E.peek(0x80E2) + 1))
for _, e in ipairs(es) do print("  " .. e) end
for _, l in ipairs(bot.log) do if l:match("unknown tile") then print("  " .. l) end end
