-- Pad log for scripts that know nothing about bot/inputlog.lua, e.g.
--   SBM2_WATCH_LUA=$PWD/bot/inputlog_wrap.lua ... ./Mesen --testrunner rom bot/bot.lua
-- (bot.lua runs SBM2_WATCH_LUA first). Wraps emu.createSavestate /
-- emu.loadSavestate so in-memory rollbacks rewind the log, starts from
-- SBM2_LOAD's sidecar when that state is loaded, and writes
-- <SBM2_OUT>/run.inputs when the script stops. Checkpoint files written by
-- the script get no sidecar this way (wire inputlog.saveState for that).
local DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(DIR .. "/inputlog.lua")
local LOAD = os.getenv("SBM2_LOAD")
if LOAD == "" then LOAD = nil end
local OUT = os.getenv("SBM2_OUT") or "."

local create, load, stop = emu.createSavestate, emu.loadSavestate, emu.stop
local marks = {}

if not LOAD then inputlog.start(nil) end

emu.createSavestate = function()
  local data = create()
  marks[data] = inputlog.mark()
  return data
end

emu.loadSavestate = function(data)
  local r = load(data)
  local m = marks[data]
  if m then inputlog.truncate(m) else inputlog.start(LOAD) end
  return r
end

emu.stop = function(code)
  inputlog.save(OUT .. "/run.inputs")
  return stop(code)
end
