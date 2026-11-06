-- Probe: do thrown bombs hurt normal enemies? (C1:1F95 on landing calls
-- EnemyHit C1:1EE2 / EnemyStun C1:1E6F for an object on the landing cell.)
-- P1 gets the glove (+$FE) and throws a bomb every 64 frames, alternating
-- left/right (and up/down every other pair), and stays put otherwise.
-- Every landing check, EnemyHit and EnemyStun call is logged with the
-- enemy's type and +$43 before and 8 frames after.
-- Run: SBM2_LOAD=build/lib/stage_10.mss SBM2_FRAMES=N SBM2_OUT=dir build/port/botrun baserom.sfc bot/throwprobe.lua
local WRAM = emu.memType.snesWorkRam
local function r16(a) return emu.read16(a, WRAM, false) end
local function r8(a) return emu.read(a, WRAM, false) end
local P1 = 0x8000
local out = assert(io.open((os.getenv("SBM2_OUT") or ".") .. "/throwprobe.txt", "w"))
local frames = tonumber(os.getenv("SBM2_FRAMES") or "3000")
local n, input, watch = 0, {}, {}
local DIRS = { "right", "left", "down", "up" }

local function log(s) out:write(s .. "\n"); out:flush() end
local function enemyAt82(tag)
  local base = r16(0x82)
  local hp = r8(base + 0x43)
  log(string.format("%5d %s enemy %04X type %02X hp %02X cell %04X P1 (%d,%d)", n, tag, base, r16(base + 0x7E) & 0xFF,
    hp, r16(base + 0xA8), r16(P1 + 0x10), r16(P1 + 0x18)))
  watch[base] = n + 1
end
emu.addMemoryCallback(function() enemyAt82("HIT ") end, emu.callbackType.exec, 0xC11EE2)
emu.addMemoryCallback(function() enemyAt82("STUN") end, emu.callbackType.exec, 0xC11E6F)
emu.addMemoryCallback(function()
  local base = r16(0x6A)
  log(string.format("%5d EnemyDie %04X type %02X", n, base, r16(base + 0x7E) & 0xFF))
end, emu.callbackType.exec, 0xC15028)
emu.addMemoryCallback(function() log(string.format("%5d landing check (thrown bomb down)", n)) end, emu.callbackType.exec, 0xC11F95)

emu.addEventCallback(function() emu.setInput(input, 0) end, emu.eventType.inputPolled)
emu.addEventCallback(function()
  n = n + 1
  emu.write(P1 + 0xFE, 1, WRAM)       -- the glove
  emu.write(P1 + 0xEF, 0x80, WRAM)    -- endless hearts: keep the probe alive
  for base, due in pairs(watch) do
    if n == due then
      local live = false
      for i = 8, 0x7E, 2 do
        local p = r16(0x2620 + i)
        if p == 0xFFFF then break end
        if p == base then live = true end
      end
      log(string.format("%5d after enemy %04X type %02X hp %02X +68 %04X +04 %04X in live list: %s", n, base, r16(base + 0x7E) & 0xFF,
        r8(base + 0x43), r16(base + 0x68), r16(base + 4), tostring(live)))
      watch[base] = (n - 0 < 2000) and n + 30 or nil
      if live == false then watch[base] = nil end
    end
  end
  -- 64-frame cycle: face (1 frame), place (A), release, lift (A held), throw (release)
  local k, t = n // 64, n % 64
  local d = DIRS[(k % 4) + 1]
  input = {}
  if n > 120 then
    if t == 0 then input[d] = true
    elseif t == 4 then input.a = true
    elseif t >= 10 and t < 34 then input.a = true
    end
  end
  if n >= frames then out:close(); emu.stop(0) end
end, emu.eventType.endFrame)
