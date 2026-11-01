# The mod SDK for the Super Mario Sunshine PC port.
#
# As a dependency (FetchContent or add_subdirectory) it gives the target
# sms::mod_sdk (the headers) and sms_add_mod / sms_add_script_mod
# (cmake/SmsMod.cmake). Built on its own it builds the lua mod and the
# header checks:
#
#   cmake -S . -B build -DSMS_MOD_ARCH=32   # 32 for the default 32-bit Linux game
#   cmake --build build
cmake_minimum_required(VERSION 3.16)
# no languages of its own: a project that uses it (C, C++, or a script mod's
# NONE) needs no compiler it did not ask for
project(sms_mod_sdk VERSION 1.6.0 LANGUAGES NONE)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(_sms_mod_sdk_top ON)
  enable_language(C)
  enable_language(CXX)
else()
  set(_sms_mod_sdk_top OFF)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SmsMod.cmake)
option(SMS_MOD_SDK_BUILD_LUA "Build the lua mod" ${_sms_mod_sdk_top})
option(SMS_MOD_SDK_TESTS "Build the header checks" ${_sms_mod_sdk_top})

if(SMS_MOD_SDK_BUILD_LUA)
  add_subdirectory(lua)
endif()
if(SMS_MOD_SDK_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
