# SmsMod.cmake: build mods for the Super Mario Sunshine PC port
# (https://github.com/chasem-dev/sms-mod-sdk, docs/MODDING.md).
#
# Get the SDK with FetchContent (its CMakeLists.txt includes this file):
#
#   include(FetchContent)
#   FetchContent_Declare(sms_mod_sdk
#     GIT_REPOSITORY https://github.com/chasem-dev/sms-mod-sdk.git
#     GIT_TAG v1.1.1)
#   FetchContent_MakeAvailable(sms_mod_sdk)
#
# or include() this file from a copy of the SDK. Then, for each mod:
#
#   sms_add_mod(<folder with mod.ini> <sources...>)
#
# builds the mod's library against the SDK's include/ (the target
# sms::mod_sdk), names its target in SMS_MOD_TARGET, and lays the mod out as
# the game loads it:
#   ${SMS_MODS_OUT}/<id>/mod.ini
#   ${SMS_MODS_OUT}/<id>/bin/<platform>/<library>.so|.dll|.dylib
#   ${SMS_MODS_OUT}/<id>/<everything else in the mod's folder but sources>
#
#   sms_add_script_mod(<folder with mod.ini>)
#
# lays out a script mod (runtime = lua in its mod.ini): its files as they are.
#
# SMS_MODS_OUT defaults to <build>/mods; point it at the game's mods/ folder
# to install. The library must match the game: a 32-bit game (the default on
# Linux, build/linux-32) needs -DSMS_MOD_ARCH=32 (or a 32-bit toolchain).
include_guard(GLOBAL)

get_filename_component(_sms_mod_sdk_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SMS_MOD_SDK_DIR "${_sms_mod_sdk_dir}" CACHE PATH "The mod SDK")
set(SMS_MODS_OUT "${CMAKE_BINARY_DIR}/mods" CACHE PATH "Where built mods are laid out")
set(SMS_MOD_ARCH "" CACHE STRING "32 or 64 to force the pointer size (Linux, gcc/clang)")

# The headers, for mods built some other way: target_link_libraries(x PRIVATE sms::mod_sdk)
if(NOT TARGET sms_mod_sdk)
  add_library(sms_mod_sdk INTERFACE)
  add_library(sms::mod_sdk ALIAS sms_mod_sdk)
  target_include_directories(sms_mod_sdk INTERFACE "${SMS_MOD_SDK_DIR}/include")
endif()

if(SMS_MOD_ARCH STREQUAL "32")
  set(_sms_mod_bits 32)
elseif(SMS_MOD_ARCH STREQUAL "64")
  set(_sms_mod_bits 64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(_sms_mod_bits 32)
else()
  set(_sms_mod_bits 64)
endif()

# The folder name the game looks in (the port's ext/modapi/host/mod_runtime.cpp
# platformDir), and the library's suffix there.
if(WIN32)
  if(_sms_mod_bits EQUAL 32)
    set(_sms_mod_platform windows-x86)
  else()
    set(_sms_mod_platform windows-x86_64)
  endif()
  set(_sms_mod_suffix .dll)
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
    set(_sms_mod_platform macos-arm64)
  else()
    set(_sms_mod_platform macos-x86_64)
  endif()
  set(_sms_mod_suffix .dylib)
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_sms_mod_platform linux-arm64)
  elseif(_sms_mod_bits EQUAL 32)
    set(_sms_mod_platform linux-x86)
  else()
    set(_sms_mod_platform linux-x86_64)
  endif()
  set(_sms_mod_suffix .so)
endif()
# a cache entry, so a project that got the SDK with FetchContent sees it too
set(SMS_MOD_PLATFORM "${_sms_mod_platform}" CACHE INTERNAL "The game's platform folder for mods (bin/<platform>)")
set(SMS_MOD_SUFFIX "${_sms_mod_suffix}" CACHE INTERNAL "A mod library's suffix")

# -m32 / -m64 for SMS_MOD_ARCH, on a target of the mod's (its library, or a
# static library it links).
function(sms_mod_target_arch target)
  if(SMS_MOD_ARCH STREQUAL "32" OR SMS_MOD_ARCH STREQUAL "64")
    target_compile_options(${target} PRIVATE -m${SMS_MOD_ARCH})
    target_link_options(${target} PRIVATE -m${SMS_MOD_ARCH})
  endif()
endfunction()

# A key's value in a mod.ini.
function(_sms_mod_ini file key out default)
  set(value "${default}")
  file(STRINGS "${file}" lines REGEX "^[ \t]*${key}[ \t]*=")
  if(lines)
    list(GET lines 0 line)
    string(REGEX REPLACE "^[ \t]*${key}[ \t]*=[ \t]*" "" value "${line}")
    string(STRIP "${value}" value)
  endif()
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

# The mod's folder copied to its place, but for what `skip` matches.
function(_sms_mod_copy dir out skip)
  file(GLOB_RECURSE files RELATIVE "${dir}" CONFIGURE_DEPENDS "${dir}/*")
  foreach(f ${files})
    if(NOT f MATCHES "${skip}" AND NOT f MATCHES "^(CMakeLists.txt|\\.git.*|.*/\\.git.*)$")
      configure_file("${dir}/${f}" "${out}/${f}" COPYONLY)
    endif()
  endforeach()
endfunction()

function(sms_add_mod dir)
  get_filename_component(dir "${dir}" ABSOLUTE)
  get_filename_component(folder "${dir}" NAME)
  if(NOT EXISTS "${dir}/mod.ini")
    message(FATAL_ERROR "sms_add_mod: no mod.ini in ${dir}")
  endif()
  _sms_mod_ini("${dir}/mod.ini" id id "${folder}")
  _sms_mod_ini("${dir}/mod.ini" library lib "${id}")
  set(target "smsmod_${id}")
  string(MAKE_C_IDENTIFIER "${target}" target)
  set(out "${SMS_MODS_OUT}/${id}")
  set(bin "${out}/bin/${SMS_MOD_PLATFORM}")
  add_library(${target} MODULE ${ARGN})
  target_link_libraries(${target} PRIVATE sms::mod_sdk)
  sms_mod_target_arch(${target})
  set_target_properties(${target} PROPERTIES
    PREFIX ""
    SUFFIX "${SMS_MOD_SUFFIX}"
    OUTPUT_NAME "${lib}"
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${bin}"
    RUNTIME_OUTPUT_DIRECTORY "${bin}")
  foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(${target} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${CFG} "${bin}"
      RUNTIME_OUTPUT_DIRECTORY_${CFG} "${bin}")
  endforeach()
  if(MINGW)
    # no libgcc/libstdc++ DLLs to ship beside the mod
    target_link_options(${target} PRIVATE -static-libgcc $<$<COMPILE_LANGUAGE:CXX>:-static-libstdc++>)
  elseif(MSVC)
    # the C runtime linked in, so the mod needs no redistributable of its own
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  _sms_mod_copy("${dir}" "${out}" "\\.(c|cc|cpp|cxx|h|hpp|rs|zig)$")
  message(STATUS "sms mod ${id}: ${out} (${SMS_MOD_PLATFORM})")
  set(SMS_MOD_TARGET ${target} PARENT_SCOPE)
endfunction()

# A script mod: nothing to build, its folder copied as it is (scripts included).
function(sms_add_script_mod dir)
  get_filename_component(dir "${dir}" ABSOLUTE)
  get_filename_component(folder "${dir}" NAME)
  if(NOT EXISTS "${dir}/mod.ini")
    message(FATAL_ERROR "sms_add_script_mod: no mod.ini in ${dir}")
  endif()
  _sms_mod_ini("${dir}/mod.ini" id id "${folder}")
  set(out "${SMS_MODS_OUT}/${id}")
  _sms_mod_copy("${dir}" "${out}" "^$")
  message(STATUS "sms script mod ${id}: ${out}")
endfunction()
