-- Loaded through SBM2_WATCH_LUA alongside bot.lua: whenever a live enemy's
-- cell holds a flame tile, log the frame and its whole $100-byte struct to
-- SBM2_WATCH_OUT, plus the same struct for the next few frames.
local WRAM = emu.memType.snesWorkRam
local function r16(a) return emu.read16(a, WRAM, false) end
local out = assert(io.open(os.getenv("SBM2_WATCH_OUT"), "w"))
local n, follow = 0, {}

local function dump(tag, base)
  local t = {}
  for i = 0, 0xFF do t[#t + 1] = string.format("%02X", emu.read(base + i, WRAM, false)) end
  out:write(string.format("%d %s %04X %s\n", n, tag, base, table.concat(t)))
end

emu.addEventCallback(function()
  n = n + 1
  local ox, oy = r16(0x5F01), r16(0x5EFB)
  for base = 0x8400, 0x8F00, 0x100 do
    if (r16(base) & 0xF000) == 0x9000 then
      local c, r = (r16(base + 0x10) + ox) // 16, (r16(base + 0x18) + oy) // 16
      local tile = emu.read(0x1C000 + r * 0x80 + c * 2, WRAM, false)
      if tile >= 1 and tile <= 3 then
        dump("FLAME", base); follow[base] = 8
      elseif follow[base] then
        dump("after", base)
        follow[base] = follow[base] > 1 and follow[base] - 1 or nil
      end
    end
  end
  out:flush()
end, emu.eventType.endFrame)
