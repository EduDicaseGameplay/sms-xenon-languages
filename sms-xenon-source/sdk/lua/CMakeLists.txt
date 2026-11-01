# The lua mod: runs Lua script mods (mod.ini `runtime = lua`). It embeds
# Lua 5.4: its sources from lua.org (checked against the release's hash), or
# a copy given with -DSMS_LUA_SOURCE_DIR=<lua-5.4.x>.
set(SMS_LUA_SOURCE_DIR "" CACHE PATH "Lua 5.4 sources (the folder with src/); empty: download")
if(NOT SMS_LUA_SOURCE_DIR)
  include(FetchContent)
  FetchContent_Declare(sms_lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(sms_lua) # no CMakeLists.txt in it: only fetched
  set(SMS_LUA_SOURCE_DIR ${sms_lua_SOURCE_DIR})
endif()
file(GLOB _lua_sources ${SMS_LUA_SOURCE_DIR}/src/*.c)
list(FILTER _lua_sources EXCLUDE REGEX "/(lua|luac)\\.c$")
if(NOT _lua_sources)
  message(FATAL_ERROR "no Lua sources in ${SMS_LUA_SOURCE_DIR}/src")
endif()

sms_add_mod(${CMAKE_CURRENT_SOURCE_DIR} lua_host.c ${_lua_sources})
set_target_properties(smsmod_lua PROPERTIES C_STANDARD 99)
target_include_directories(smsmod_lua PRIVATE ${SMS_LUA_SOURCE_DIR}/src)
if(UNIX)
  target_compile_definitions(smsmod_lua PRIVATE LUA_USE_POSIX)
  target_link_libraries(smsmod_lua PRIVATE m)
endif()
if(MSVC)
  target_compile_definitions(smsmod_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(smsmod_lua PRIVATE -O2)
  set_source_files_properties(lua_host.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
  # Lua's own sources, not ours: their warnings are not this build's to fix
  set_source_files_properties(${_lua_sources} PROPERTIES COMPILE_OPTIONS "-w")
endif()
