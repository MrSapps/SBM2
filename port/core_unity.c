/* The emulator core as one translation unit (botrun): the CPU's bus calls go
 * straight to snes.c's bus_read / bus_write / bus_idle (inlined) when the
 * machine uses them, and through the function pointers otherwise (botrun's
 * memory-callback wrappers). Same behaviour, cycle for cycle. */
#define CPU_DIRECT_BUS
#include "snes.c"
#include "cpu.c"
#include "ppu.c"
