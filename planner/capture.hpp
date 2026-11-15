// The world view straight from WRAM (the C++ twin of bot/worldview.lua's
// capture), and the snapshot text for a World (the same text the Lua
// encoder writes, so a capture here can be checked against it and every
// decision the agent makes can be dumped as a unit-test world).
#pragma once
#include <cstdint>
#include <string>

#include "world.hpp"

namespace sbm2 {

constexpr int kWramSize = 0x20000;

// wram: the 128 KiB of $7E0000-$7FFFFF.
World captureWorld(const uint8_t* wram, int frame);
std::string encodeWorld(const World& w);

}  // namespace sbm2
