# -DSMS_ECLIPSE=ON builds Super Mario Eclipse into the port (docs/ECLIPSE.md).
#
# Eclipse, BetterSunshineEngine and BetterSunshineMoveset (and the SunshineHeaderInterface headers
# they are written against) are fetched at configure time at pinned
# revisions, never kept in this repository, fixed up mechanically
# (platform/mods/eclipse/fixup_sources.py) and built with clang into one
# library (platform/mods/eclipse/lib). Their patches register with the port's
# code-mod registry (platform/mods/modhooks.cpp) and reach the game through
# hooks in the decomp source (decomp-patches/modhook-*.patch).
#
# Their headers describe the GameCube's layout; platform/mods/eclipse/shi-layout.patch
# (tools/mods/shi_layout) re-lays their classes out as the port lays out the
# game's, on 32- and 64-bit hosts.
if(SMS_MAC_ARM64)
  # The game's own clang: the pass plugin (tools/gameptr) was built for its LLVM.
  get_filename_component(_llvm_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
  set(SMS_CLANGXX ${_llvm_bin}/clang++ CACHE FILEPATH "clang++ for the Eclipse library")
  set(SMS_CLANG ${_llvm_bin}/clang CACHE FILEPATH "clang for the Eclipse library")
endif()
find_program(SMS_CLANGXX NAMES clang++)
find_program(SMS_CLANG NAMES clang)
if(NOT SMS_CLANGXX OR NOT SMS_CLANG)
  message(FATAL_ERROR "SMS_ECLIPSE needs clang/clang++ (Eclipse's sources are written for clang)")
endif()
find_package(Git REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SMS_ECLIPSE_SRC_DIR "${CMAKE_BINARY_DIR}/eclipse-src" CACHE PATH
  "Where the Eclipse, BetterSunshineEngine, BetterSunshineMoveset and SunshineHeaderInterface sources are fetched to")
# Every build folder checks the sources out for itself, since fixup_sources.py
# edits them. What they are checked out from is downloaded once, into
# SMS_ECLIPSE_GIT_CACHE (one repository per revision), so the next build
# folder copies it from there instead of downloading about 110 MB again. SMS
# Launcher points it at one folder for all its game versions. Empty: none.
if(DEFINED ENV{SMS_ECLIPSE_GIT_CACHE})
  set(_eclipse_git_cache "$ENV{SMS_ECLIPSE_GIT_CACHE}")
else()
  set(_eclipse_git_cache "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/eclipse-git")
endif()
set(SMS_ECLIPSE_GIT_CACHE "${_eclipse_git_cache}" CACHE PATH
  "Where the Eclipse sources' downloads are kept for other build folders (empty: download for each)")

# name  url  revision  [a folder the port doesn't use, left out of the checkout]
set(_eclipse_repos
  "eclipse|https://github.com/JoshuaMKW/super-mario-eclipse|792e14fa1583910673ddae7e85048c6ddb4ba700"
  # compiler/: a 764 MB Windows clang for building BSE for the GameCube.
  "bse|https://github.com/JoshuaMKW/BetterSunshineEngine|bc6840e373d59a2601e7b5f9011732390561f6dd|compiler"
  "moveset|https://github.com/JoshuaMKW/BetterSunshineMoveset|2eb6f136cce4c0c7816808ad9dc3d1d95ef52b83"
  "shi|https://github.com/JoshuaMKW/SunshineHeaderInterface|a0d858951e7fb22dce5304aa5c50287ecb0d6862")

# Downloads `refspec` (a revision, or revision:ref) of `url` into the
# repository at `dir`, setting `result` to Git's exit code. A stalled
# download (under 1 KB/s for a minute) fails instead of waiting for ever,
# and is tried again.
function(_sms_eclipse_download dir url refspec name result)
  foreach(_attempt 1 2 3)
    if(_attempt GREATER 1)
      message(STATUS "SMS_ECLIPSE: download of ${name} stalled or failed, trying again (${_attempt} of 3)")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${dir}" -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=60
      fetch --progress --depth 1 "${url}" "${refspec}"
      RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
      break()
    endif()
  endforeach()
  set(${result} ${_rc} PARENT_SCOPE)
endfunction()

if(SMS_ECLIPSE_GIT_CACHE)
  get_filename_component(_eclipse_git_cache "${SMS_ECLIPSE_GIT_CACHE}" ABSOLUTE)
else()
  set(_eclipse_git_cache "")
endif()
foreach(r ${_eclipse_repos})
  string(REPLACE "|" ";" r "${r}")
  list(GET r 0 _name)
  list(GET r 1 _url)
  list(GET r 2 _rev)
  set(_skip "")
  list(LENGTH r _fields)
  if(_fields GREATER 3)
    list(GET r 3 _skip)
  endif()
  set(_dir "${SMS_ECLIPSE_SRC_DIR}/${_name}")
  set(_have "")
  if(EXISTS "${_dir}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" rev-parse HEAD
      OUTPUT_VARIABLE _have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  if(NOT _have STREQUAL _rev)
    message(STATUS "SMS_ECLIPSE: fetching ${_name} ${_rev}")
    file(MAKE_DIRECTORY "${_dir}")
    execute_process(COMMAND ${GIT_EXECUTABLE} init -q "${_dir}")
    if(_skip)
      execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" config core.sparseCheckout true)
      file(WRITE "${_dir}/.git/info/sparse-checkout" "/*\n!/${_skip}/\n")
    endif()
    set(_rc 1)
    if(_eclipse_git_cache)
      string(SUBSTRING "${_rev}" 0 12 _short)
      set(_kept "${_eclipse_git_cache}/${_name}-${_short}.git")
      # Fetched by a relative path: MSYS Git reads C:/... as a host name. On
      # another drive there is none, and the build folder downloads its own.
      file(RELATIVE_PATH _from "${_dir}" "${_kept}")
      if(NOT IS_ABSOLUTE "${_from}")
        execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_kept}" cat-file -e "${_rev}^{commit}"
          RESULT_VARIABLE _found OUTPUT_QUIET ERROR_QUIET)
        if(NOT _found EQUAL 0)
          file(MAKE_DIRECTORY "${_kept}")
          execute_process(COMMAND ${GIT_EXECUTABLE} init -q --bare "${_kept}")
          # A branch keeps the revision from Git's garbage collection, and
          # lets other repositories fetch it by its hash.
          _sms_eclipse_download("${_kept}" "${_url}" "+${_rev}:refs/heads/kept" "${_name}" _found)
        endif()
        if(_found EQUAL 0)
          file(TOUCH "${_kept}/last-used")
          execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" fetch -q --depth 1 "${_from}" "${_rev}"
            RESULT_VARIABLE _rc)
        endif()
      endif()
      if(NOT _rc EQUAL 0)
        message(STATUS "SMS_ECLIPSE: no kept download of ${_name} to use, downloading it for this build folder")
      endif()
    endif()
    if(NOT _rc EQUAL 0)
      _sms_eclipse_download("${_dir}" "${_url}" "${_rev}" "${_name}" _rc)
    endif()
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "SMS_ECLIPSE: could not fetch ${_url} at ${_rev}")
    endif()
    # The player's own git hooks (a global core.hooksPath or init template)
    # do not run here: a failing post-checkout hook fails the checkout
    # without a word.
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" -c advice.detachedHead=false -c core.hooksPath=/dev/null
      checkout -q --progress -f FETCH_HEAD
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "SMS_ECLIPSE: could not check out ${_name} (git: ${_rc})")
    endif()
  endif()
endforeach()
# Downloads of revisions no build folder has checked out for 30 days (older
# game versions') are removed.
if(_eclipse_git_cache)
  string(TIMESTAMP _now "%s" UTC)
  file(GLOB _kept_downloads LIST_DIRECTORIES true "${_eclipse_git_cache}/*.git")
  foreach(_kept ${_kept_downloads})
    if(EXISTS "${_kept}/last-used")
      file(TIMESTAMP "${_kept}/last-used" _used "%s" UTC)
      math(EXPR _age "${_now} - ${_used}")
      if(_age GREATER 2592000)
        message(STATUS "SMS_ECLIPSE: removing ${_kept}, unused for 30 days")
        file(REMOVE_RECURSE "${_kept}")
      endif()
    endif()
  endforeach()
endif()

execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/fixup_sources.py
  "${SMS_ECLIPSE_SRC_DIR}/eclipse" "${SMS_ECLIPSE_SRC_DIR}/bse" "${SMS_ECLIPSE_SRC_DIR}/shi"
  "${SMS_ECLIPSE_SRC_DIR}/moveset"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "SMS_ECLIPSE: fixup_sources.py failed")
endif()

include(ExternalProject)
set(_eclipse_lib "${CMAKE_BINARY_DIR}/eclipse-build/libsms_eclipse.a")
# The library is its own CMake project: give it the game's objcopy (macOS has
# only llvm-objcopy, which it would not find by itself) and, on macOS, the
# game's architecture (x86_64 under Rosetta, or native arm64).
if(APPLE)
  set(_eclipse_platform_args -DCMAKE_OBJCOPY=${SMS_OBJCOPY} -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
  if(SMS_MAC_ARM64)
    # The mods reach game addresses as the game does (docs/MACOS-ARM64.md):
    # through tools/gameptr's pass, rebased above PAGEZERO.
    list(APPEND _eclipse_platform_args -DSMS_GAMEPTR_PLUGIN=${SMS_GAMEPTR_PLUGIN} -DSMS_GAMEPTR_HASH=${_gameptr_hash})
  endif()
else()
  set(_eclipse_platform_args -DCMAKE_OBJCOPY=${CMAKE_OBJCOPY})
endif()
list(APPEND _eclipse_platform_args -DPython3_EXECUTABLE=${Python3_EXECUTABLE})
ExternalProject_Add(sms_eclipse_build
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/lib
  BINARY_DIR ${CMAKE_BINARY_DIR}/eclipse-build
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER=${SMS_CLANG} -DCMAKE_CXX_COMPILER=${SMS_CLANGXX}
    -DECLIPSE_SRC=${SMS_ECLIPSE_SRC_DIR}/eclipse -DBSE_SRC=${SMS_ECLIPSE_SRC_DIR}/bse
    -DMOVESET_SRC=${SMS_ECLIPSE_SRC_DIR}/moveset
    -DSHI_SRC=${SMS_ECLIPSE_SRC_DIR}/shi -DPORT_MODS=${CMAKE_CURRENT_SOURCE_DIR}/platform/mods
    -DSMS_ARCH=${SMS_ARCH} ${_eclipse_platform_args}
  BUILD_ALWAYS ON
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${_eclipse_lib})
# The mods' plain new/delete go to the game's heaps, like the game's own
# (see the sms_game rename above). Their static constructors run when the
# modules load (sms_mod_start), not at process start (platform/mods/eclipse/lib
# moves them to their own section): Kuribo runs a module's constructors when
# it loads it, and BetterSunshineEngine's start threads and allocate from the
# game's heaps.
ExternalProject_Add_Step(sms_eclipse_build rename_new
  COMMAND ${SMS_OBJCOPY} --redefine-syms=${CMAKE_BINARY_DIR}/game_new_syms.txt ${_eclipse_lib}
  DEPENDEES build)

# Functions the mods call out of line that the decomp only has inline, and
# SDK functions the retail game never needed: built with the game's flags.
target_sources(sms_game PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/port_shims.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/sdk_extras.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/rawfn_trampolines.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/rawdata.cpp)
# The functions the mods call through SunshineHeaderInterface's raw_fn.hxx
# (tools/mods/gen_rawfn.py): C++11 for their return conversion, and members
# called as the retail code calls them whatever their access.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/rawfn_trampolines.cpp
  PROPERTIES COMPILE_OPTIONS "-std=gnu++11;-fno-access-control")

add_dependencies(sms sms_eclipse_build)
# After each link, check that the game and the mods agree on the widths and
# signedness of the integer and float results and arguments where they call
# each other: by name, through game virtual functions, and at the patch
# targets (tools/mods/abi_check.py, from the binary's debug info: about half
# a minute). A failed check is redone on the next build.
option(SMS_ECLIPSE_ABI_CHECK "Check the mods' result and argument types against the game's after linking" ON)
if(SMS_ECLIPSE_ABI_CHECK)
  if(APPLE)
    # Mach-O executables keep their DWARF in the object files; gather it first.
    set(_abi_binary ${CMAKE_BINARY_DIR}/sms.dSYM)
    set(_abi_dsym COMMAND dsymutil $<TARGET_FILE:sms> -o ${_abi_binary})
  else()
    set(_abi_binary $<TARGET_FILE:sms>)
    set(_abi_dsym)
  endif()
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/sms_abi_check.stamp
    ${_abi_dsym}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mods/abi_check.py ${_abi_binary} ${_eclipse_lib}
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/sms_abi_check.stamp
    DEPENDS sms ${CMAKE_CURRENT_SOURCE_DIR}/tools/mods/abi_check.py
    COMMENT "Checking the mods' result and argument types against the game's" VERBATIM)
  add_custom_target(sms_abi_check ALL DEPENDS ${CMAKE_BINARY_DIR}/sms_abi_check.stamp)
endif()
target_compile_definitions(sms PRIVATE SMS_ECLIPSE=1)
# --defsym=name=target: name is another name for target (ld64: -alias).
function(sms_eclipse_alias name target)
  if(APPLE)
    target_link_options(sms PRIVATE "LINKER:-alias,_${target},_${name}")
  else()
    target_link_options(sms PRIVATE "-Wl,--defsym=${_object_prefix}${name}=${_object_prefix}${target}")
  endif()
endfunction()
if(APPLE)
  sms_link_paths(sms -force_load "${_eclipse_lib}")
else()
  target_link_libraries(sms PRIVATE -Wl,--whole-archive ${_eclipse_lib} -Wl,--no-whole-archive)
endif()
if(WIN32)
  # The mods' headers define some game functions inline (TMario's parameter
  # constructors...). On ELF and Mach-O those copies are weak and the game's
  # own definition wins; on PE a COMDAT copy and a plain definition collide.
  # The game's archive comes first on the link line, so its definition wins.
  target_link_options(sms PRIVATE -Wl,--allow-multiple-definition)
endif()
set_property(TARGET sms APPEND PROPERTY LINK_DEPENDS ${_eclipse_lib})
# Constructors and destructors the mods call that the game has only in their
# other variant (complete- or base-object): GCC on ELF aliases them itself.
if(APPLE OR WIN32)
  set(_structors ${CMAKE_BINARY_DIR}/eclipse_structor_aliases.ld)
  add_custom_command(OUTPUT ${_structors}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mods/structor_aliases.py ${CMAKE_NM}
      $<TARGET_FILE:sms_game> ${_eclipse_lib} ${_structors} $<IF:$<BOOL:${APPLE}>,apple,gnu>
    DEPENDS sms_game sms_eclipse_build ${CMAKE_CURRENT_SOURCE_DIR}/tools/mods/structor_aliases.py VERBATIM)
  add_custom_target(sms_eclipse_structors DEPENDS ${_structors})
  add_dependencies(sms sms_eclipse_structors)
  set_property(TARGET sms APPEND PROPERTY LINK_DEPENDS ${_structors})
  if(APPLE)
    sms_link_paths(sms -alias_list "${_structors}")
  else()
    target_link_options(sms PRIVATE ${_structors})
  endif()
endif()
# Names the mods use for things the decomp spells otherwise: retail globals
# under their map names, and functions whose u32 is unsigned int there and
# unsigned long here (the same type on the 32-bit port).
sms_eclipse_alias(gStageBGM _ZN10MSMainProc11MSStageInfo8stageBgmE)
sms_eclipse_alias(gAudioVolume _ZN5MSBgm12smMainVolumeE)
sms_eclipse_alias(waterColor gModelWaterManagerWaterColor)
# TMarDirector::fireStartDemoCamera's callback argument (and the callback's
# first parameter) is uintptr_t in the decomp and u32 in the mods' headers;
# the mods pass their own callbacks and 0 through it, and the game only hands
# the argument back to the callback.
if(SMS_ARCH STREQUAL "32")
  sms_eclipse_alias(_ZN7JKRHeap5allocEjiPS_ _ZN7JKRHeap5allocEmiPS_)
  sms_eclipse_alias(_ZN13JKRMemArchiveC1EPvj15JKRMemBreakFlag _ZN13JKRMemArchiveC1EPvm15JKRMemBreakFlag)
  sms_eclipse_alias(_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEElfbPFlmmEmPN6JDrama6TActorENS9_6TFlagTItEE _ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEElfbPFljmEjPN6JDrama6TActorENS9_6TFlagTItEE)
else()
  # On LP64 hosts it is the other way round: the game's u32 is unsigned int,
  # and these declarations (size_t, unsigned long) say unsigned long.
  sms_eclipse_alias(_ZN7JKRHeap5allocEmiPS_ _ZN7JKRHeap5allocEjiPS_)
  sms_eclipse_alias(_ZN13JKRMemArchiveC1EPvm15JKRMemBreakFlag _ZN13JKRMemArchiveC1EPvj15JKRMemBreakFlag)
  sms_eclipse_alias(_ZN6JStage6TActor11JSGSetShapeEm _ZN6JStage6TActor11JSGSetShapeEj)
  sms_eclipse_alias(_ZN6JStage6TActor15JSGSetAnimationEm _ZN6JStage6TActor15JSGSetAnimationEj)
  sms_eclipse_alias(_ZN6JStage7TSystem16JSGGetSystemDataEm _ZN6JStage7TSystem16JSGGetSystemDataEj)
  sms_eclipse_alias(_ZN6JStage7TSystem16JSGSetSystemDataEmm _ZN6JStage7TSystem16JSGSetSystemDataEjj)
  if(WIN32)
    # LLP64: size_t and the game's uintptr_t are unsigned long long.
    sms_eclipse_alias(_ZN7JKRHeap5allocEyiPS_ _ZN7JKRHeap5allocEjiPS_)
    sms_eclipse_alias(_ZN13JKRMemArchiveC1EPvy15JKRMemBreakFlag _ZN13JKRMemArchiveC1EPvj15JKRMemBreakFlag)
    sms_eclipse_alias(_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEEifbPFijjEjPN6JDrama6TActorENS9_6TFlagTItEE _ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEEifbPFiyjEyPN6JDrama6TActorENS9_6TFlagTItEE)
  else()
    sms_eclipse_alias(_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEEifbPFijjEjPN6JDrama6TActorENS9_6TFlagTItEE _ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEEifbPFimjEmPN6JDrama6TActorENS9_6TFlagTItEE)
  endif()
endif()
message(STATUS "SMS port: Super Mario Eclipse built in (sources in ${SMS_ECLIPSE_SRC_DIR})")
