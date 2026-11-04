-- Run the bot on one WRAM image for N frames (memory stays frozen) and print
-- its log: what it would plan from that exact state.
--   build/tools/lua bot/test/think.lua <file.wram> [frames=60] [KEY=VALUE ...]
-- KEY=VALUE pairs set the bot's env (e.g. SBM2_NOASSIST=1).
local ROOT = arg[0]:match("^(.*)/bot/test/[^/]*$") or "."
local E = dofile(ROOT .. "/bot/test/emu.lua")
local path = assert(arg[1], "usage: think.lua <file.wram> [frames] [KEY=VALUE ...]")
local frames = tonumber(arg[2] or "60")
local out = os.tmpname(); os.remove(out)
os.execute("mkdir -p " .. out)
local env = { SBM2_OUT = out, SBM2_NOASSIST = "1", SBM2_FRAMES = "100000000" }
for i = 3, #arg do
  local k, v = arg[i]:match("^([%w_]+)=(.*)$")
  if k then env[k] = v end
end
E.install { wram = path, rom = ROOT .. "/baserom.sfc", env = env }
local bot = E.loadBot(ROOT .. "/bot/bot.lua")
local shown = 0
for _ = 1, frames do
  E.frame()
  for i = shown + 1, #bot.log do print(bot.log[i]) end
  shown = #bot.log
end
local inp = {}
for k, v in pairs(E.input()) do if v then inp[#inp + 1] = k end end
table.sort(inp)
print(string.format("after %d frames: input {%s}", frames, table.concat(inp, ",")))
os.execute("rm -rf " .. out)
