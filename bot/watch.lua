-- Logs the PCs that read/write a WRAM address range while bot.lua drives.
-- Env: SBM2_WATCH="start-end" (CPU addresses, hex), SBM2_WATCH_OUT file.
local lo, hi = (os.getenv("SBM2_WATCH") or ""):match("^(%x+)-(%x+)$")
lo, hi = tonumber(lo, 16), tonumber(hi, 16)
local seen, order = {}, {}
local function note(kind)
  return function(addr, value)
    local s = emu.getState()
    local key = string.format("%s %02X:%04X %06X=%04X", kind, s["cpu.k"], s["cpu.pc"], addr, value)
    if not seen[key] then seen[key] = true; order[#order + 1] = key end
  end
end
emu.addMemoryCallback(note("R"), emu.callbackType.read, lo, hi)
emu.addMemoryCallback(note("W"), emu.callbackType.write, lo, hi)
emu.addEventCallback(function()
  local fh = io.open(os.getenv("SBM2_WATCH_OUT"), "w")
  fh:write(table.concat(order, "\n"), "\n"); fh:close()
end, emu.eventType.endFrame)
