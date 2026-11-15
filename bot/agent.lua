-- Shell for the C++ agent (planner/agent.cpp) on build/port/agentrun:
-- loads the start state, logs the pad stream (bot/inputlog.lua), and each
-- frame asks emu.agent() for the pad. Every decision is made in C++.
--
-- Env (as bot.lua): SBM2_OUT, SBM2_LOAD, SBM2_PREFIX, SBM2_FRAMES,
-- SBM2_FRAME0, SBM2_STOPEARLY (stop at a death, and at the next stage),
-- SBM2_STOPAT, SBM2_GOAL (default "exit").
-- Log lines (scenario.py greps them): "STAGE XX", "DEATH ...", "END ...".
assert(emu.agent, "bot/agent.lua needs build/port/agentrun")
local BOT_DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(BOT_DIR .. "/inputlog.lua")
local WRAM = emu.memType.snesWorkRam
local OUT = os.getenv("SBM2_OUT") or "build/agent"
local FRAMES = tonumber(os.getenv("SBM2_FRAMES") or "3600")
local FRAME0 = tonumber(os.getenv("SBM2_FRAME0") or "0")
local LOAD = os.getenv("SBM2_LOAD"); if LOAD == "" then LOAD = nil end
local GOAL = os.getenv("SBM2_GOAL") or "exit"
local STOPEARLY = os.getenv("SBM2_STOPEARLY") == "1"
-- SBM2_STOPAT=XX: stop on entering stage XX (hex); else with STOPEARLY on
-- entering the next one.
local STOPAT = tonumber(os.getenv("SBM2_STOPAT") or "", 16)

local function r16(a) return emu.read16(a, WRAM, false) end
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

local frame, loaded, stage, deadAt = FRAME0, LOAD == nil, nil, nil
local deaths = 0
if loaded then inputlog.start(nil) end

local function finish(code, why)
  log(string.format("%6d END %02X %s deaths %d", frame, emu.read(0x1A, WRAM, false), why, deaths))
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
  -- on the exit.
  local lock = r16(0x8052)
  if (lock & 0xF809) == 0x0801 and r16(0x80A8) ~= r16(0x5D72) then
    if not deadAt then
      deadAt = frame
      deaths = deaths + 1
      log(string.format("%6d DEATH at px(%d,%d)", frame, r16(0x8010), r16(0x8018)))
      writeFile(string.format("%s/wv_death_%06d.txt", OUT, frame), emu.agentWorld())
      if STOPEARLY then return finish(1, "death") end
    end
  else
    deadAt = nil
  end
  local ok, pad, text = pcall(emu.agent, frame, GOAL)
  if not ok then
    log(string.format("%6d agent error: %s", frame, tostring(pad)))
    return finish(4, "error")
  end
  if text ~= "" then
    for ln in text:gmatch("[^\n]+") do log(ln) end
  end
  if frame % 600 == 0 then flush() end
  frame = frame + 1
  if frame >= FRAMES then return finish(0, "budget") end
end, emu.eventType.endFrame)
