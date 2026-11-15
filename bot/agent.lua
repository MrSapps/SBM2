-- Shell for the C++ agent (planner/agent.cpp) on build/port/agentrun:
-- loads the start state, logs the pad stream (bot/inputlog.lua), and each
-- frame asks emu.agent() for the pad. Every decision is made in C++.
--
-- Env (as bot.lua): SBM2_OUT, SBM2_LOAD, SBM2_PREFIX, SBM2_FRAMES,
-- SBM2_FRAME0, SBM2_STOPEARLY (stop at a death, and at the next stage),
-- SBM2_STOPAT, SBM2_GOAL (default "powerups+exit").
-- Log lines (scenario.py greps them): "STAGE XX", "DEATH ...", "END ...".
assert(emu.agent, "bot/agent.lua needs build/port/agentrun")
local BOT_DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(BOT_DIR .. "/inputlog.lua")
local WRAM = emu.memType.snesWorkRam
local OUT = os.getenv("SBM2_OUT") or "build/agent"
local FRAMES = tonumber(os.getenv("SBM2_FRAMES") or "3600")
local FRAME0 = tonumber(os.getenv("SBM2_FRAME0") or "0")
local LOAD = os.getenv("SBM2_LOAD"); if LOAD == "" then LOAD = nil end
local GOAL = os.getenv("SBM2_GOAL") or "powerups+exit"
local STOPEARLY = os.getenv("SBM2_STOPEARLY") == "1"
local VERBOSE = (os.getenv("SBM2_AGVERBOSE") or "0") ~= "0"
-- SBM2_STOPAT=XX: stop on entering stage XX (hex); else with STOPEARLY on
-- entering the next one.
local STOPAT = tonumber(os.getenv("SBM2_STOPAT") or "", 16)

local function r16(a) return emu.read16(a, WRAM, false) end
local function s16(v) return v >= 0x8000 and v - 0x10000 or v end
local function origin() return s16(r16(0x5F01)), s16(r16(0x5EFB)) end
-- Bosses: their contact boxes and flame sense points come from exec hooks
-- on the boss code (bot/boss.lua), not WRAM; they go to the agent as
-- world-view boss lines (docs/worldview.md).
local Boss = dofile(BOT_DIR .. "/boss.lua")({ r16 = r16, rd8 = function(a) return emu.read(a, WRAM, false) end,
  origin = origin, MAP_W = 32, MAP_H = 32, KEY_W = 64 })
local function bossLines()
  local t = {}
  local ox, oy = origin()
  for _, b in ipairs(Boss.read()) do
    local s = string.format("boss base=%04X state=%06X x=%d y=%d hp=%d hurt=%d", b.base, b.state, b.x + ox, b.y + oy,
      r16(b.base + 0x20), b.hurt and 1 or 0)
    if b.box then s = s .. string.format(" box=%d,%d,%d,%d", b.box[1], b.box[2], b.box[3], b.box[4]) end
    if b.sense and #b.sense > 0 then
      local p = {}
      for i, q in ipairs(b.sense) do p[i] = q[1] .. "," .. q[2] end
      s = s .. " sense=" .. table.concat(p, ";")
    end
    t[#t + 1] = s
  end
  return table.concat(t, "\n")
end
local lines = {}
local function log(s) lines[#lines + 1] = s end
local function writeFile(path, data)
  local fh = io.open(path, "wb")
  if fh then fh:write(data); fh:close() end
end
local LOGFILE = string.format("%s/bot_%06d.log", OUT, FRAME0)
local function flush() writeFile(LOGFILE, table.concat(lines, "\n") .. "\n") end

local function onNextInstruction(fn)
  local id
  id = emu.addMemoryCallback(function()
    emu.removeMemoryCallback(id, emu.callbackType.exec, 0, 0xFFFFFF)
    fn()
  end, emu.callbackType.exec, 0, 0xFFFFFF)
end

local frame, loaded, stage, deadAt, lockAt = FRAME0, LOAD == nil, nil, nil, nil
local deaths = 0
local T0 = emu.now()
if loaded then inputlog.start(nil) end

local function finish(code, why)
  log(string.format("%6d END %02X %s deaths %d", frame, emu.read(0x1A, WRAM, false), why, deaths))
  if emu.agentStats then log(string.format("%6d STATS wall %.2f %s", frame, emu.now() - T0, emu.agentStats())) end
  flush()
  inputlog.save(OUT .. "/run.inputs")
  emu.stop(code)
end

emu.addEventCallback(function()
  if not loaded then
    local prefix = os.getenv("SBM2_PREFIX")
    if prefix and prefix ~= "" then
      inputlog.start(LOAD, prefix)
    else
      local fh = assert(io.open(LOAD, "rb")); local data = fh:read("a"); fh:close()
      onNextInstruction(function() emu.loadSavestate(data); inputlog.start(LOAD) end)
    end
    loaded = true
    return
  end
  local st = emu.read(0x1A, WRAM, false)
  local inStage = (r16(0x8000) & 0xF000) == 0x9000 and emu.read(0xBC, WRAM, false) ~= 0
  if inStage and st ~= stage then
    if st == STOPAT or (stage and STOPEARLY and not STOPAT) then
      log(string.format("%6d STAGE %02X", frame, st))
      return finish(0, "next stage")
    end
    stage = st
    log(string.format("%6d STAGE %02X hearts %02X", frame, st, emu.read(0x80EF, WRAM, false)))
  end
  -- A death: P1 locked as C0:A675 leaves it ($52 & $F809 == $0801), not
  -- on the exit, and still locked 90 frames on: a spring pad, a jump pad's
  -- throw or a carry locks it the same way but lets go sooner (the
  -- oracle's rule, port/botrun.c sim_run).
  local lock = r16(0x8052)
  if (lock & 0xF809) == 0x0801 and r16(0x80A8) ~= r16(0x5D72) then lockAt = lockAt or frame
  elseif (lock & 1) == 0 then lockAt = nil end
  if lockAt and frame - lockAt >= 90 then
    if not deadAt then
      deadAt = frame
      deaths = deaths + 1
      log(string.format("%6d DEATH at px(%d,%d) (locked since %d)", frame, r16(0x8010), r16(0x8018), lockAt))
      writeFile(string.format("%s/wv_death_%06d.txt", OUT, frame), emu.agentWorld())
      if STOPEARLY then return finish(1, "death") end
    end
  else
    deadAt = nil
  end
  Boss.frame()
  local ok, pad, text = pcall(emu.agent, frame, GOAL, bossLines())
  if not ok then
    log(string.format("%6d agent error: %s", frame, tostring(pad)))
    return finish(4, "error")
  end
  if text ~= "" then
    for ln in text:gmatch("[^\n]+") do log(ln) end
    -- (SBM2_AGVERBOSE: the world view of each decision, for repros)
    if VERBOSE then writeFile(string.format("%s/wv_%06d.txt", OUT, frame), emu.agentWorld()) end
  end
  if frame % 600 == 0 then flush() end
  frame = frame + 1
  if frame >= FRAMES then return finish(0, "budget") end
end, emu.eventType.endFrame)
