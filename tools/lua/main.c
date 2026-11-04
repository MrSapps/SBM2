/* Minimal standalone Lua 5.4 front-end over Mesen's bundled Lua sources
   (tools/mesen2-src/Lua), for running the bot's unit tests outside Mesen.
   Usage: lua script.lua [args...]  (args in the global `arg`, arg[0] = script) */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

extern int SANDBOX_ALLOW_LOADFILE;  /* Mesen's sandbox switch (lauxlib.c) */

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s script.lua [args...]\n", argv[0]); return 2; }
    SANDBOX_ALLOW_LOADFILE = 1;
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_createtable(L, argc, 0);
    for (int i = 1; i < argc; i++) { lua_pushstring(L, argv[i]); lua_rawseti(L, -2, i - 1); }
    lua_setglobal(L, "arg");
    int st = luaL_loadfile(L, argv[1]);
    if (st == LUA_OK) {
        lua_getglobal(L, "debug"); lua_getfield(L, -1, "traceback"); lua_remove(L, -2);
        lua_insert(L, -2);
        st = lua_pcall(L, 0, 0, -2);
    }
    if (st != LUA_OK) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); lua_close(L); return 1; }
    lua_close(L);
    return 0;
}
