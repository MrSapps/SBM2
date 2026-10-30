-- Runs the game for FRAMES frames with no input, then writes Mesen's
-- code/data log for PRG-ROM (one flag byte per ROM byte) to OUT.
local FRAMES = tonumber(os.getenv("SBM2_FRAMES") or "1800")
local OUT = os.getenv("SBM2_CDL_OUT") or "build/coverage.cdl"

local frames = 0
emu.addEventCallback(function()
  frames = frames + 1
  if frames < FRAMES then return end
  local cdl = emu.getCdlData(emu.memType.snesPrgRom)
  local f = assert(io.open(OUT, "wb"))
  local chunk = {}
  for i = 0, #cdl do
    chunk[#chunk + 1] = string.char(cdl[i] or 0)
    if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
  end
  f:write(table.concat(chunk))
  f:close()
  emu.stop(0)
end, emu.eventType.endFrame)
