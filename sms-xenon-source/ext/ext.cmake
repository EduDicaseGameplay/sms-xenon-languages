# Extensions (ext/README.md): the registry and each extension's game half are
# game code (sms_game, the game's flags and headers); their other halves
# (networking, anything that needs the host's libraries) are platform code
# (sms). An extension is a folder ext/<name>/ with game/ and, optionally,
# net/, sync/ and host/ sources; tests/ folders are left out.
target_sources(sms_game PRIVATE ${CMAKE_CURRENT_LIST_DIR}/core/port_ext.cpp)
file(GLOB SMS_EXT_GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*/game/*.cpp)
target_sources(sms_game PRIVATE ${SMS_EXT_GAME_SOURCES})
file(GLOB SMS_EXT_HOST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/*/net/*.cpp ${CMAKE_CURRENT_LIST_DIR}/*/sync/*.cpp ${CMAKE_CURRENT_LIST_DIR}/*/host/*.cpp)
target_sources(sms PRIVATE ${SMS_EXT_HOST_SOURCES})
target_include_directories(sms PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# The native mod API's header comes from the mod SDK, the sdk/ submodule
# (https://github.com/chasem-dev/sms-mod-sdk). A checkout that set up only
# decomp/ (older build scripts and launchers) gets it here.
set(_sms_mod_sdk ${CMAKE_CURRENT_LIST_DIR}/../sdk)
if(NOT EXISTS ${_sms_mod_sdk}/include/sms_mod.h)
  message(STATUS "sdk/ is not checked out: git submodule update --init sdk")
  execute_process(COMMAND git submodule update --init sdk WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    RESULT_VARIABLE _sms_mod_sdk_git)
  if(NOT EXISTS ${_sms_mod_sdk}/include/sms_mod.h)
    message(FATAL_ERROR "The mod SDK (sdk/, a git submodule) is missing and could not be fetched. "
      "Run `git submodule update --init sdk` in ${CMAKE_CURRENT_LIST_DIR}/.., or clone with --recursive.")
  endif()
endif()
target_include_directories(sms PRIVATE ${_sms_mod_sdk}/include)
target_include_directories(sms_game AFTER PRIVATE ${_sms_mod_sdk}/include)
if(WIN32)
  # multiplayer's sockets
  target_link_libraries(sms PRIVATE ws2_32)
endif()

# Both sides of the multiplayer extension must run the same game build: the
# port's commit (as the launcher names a game), and Eclipse marked.
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
  OUTPUT_VARIABLE _sms_ext_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _sms_ext_git)
if(NOT _sms_ext_git EQUAL 0 OR NOT _sms_ext_commit)
  set(_sms_ext_commit "unknown")
endif()
string(SUBSTRING "${_sms_ext_commit}" 0 12 _sms_ext_build)
# The game's version, which a multiplayer host may ask a minimum of: the
# launcher's ("2026.10.18.1", passed as SMS_GAME_VERSION when it starts the
# game, or here when it builds), else the last commit's date.
set(SMS_GAME_VERSION "" CACHE STRING "The game's version (the launcher's); empty: the last commit's date")
set(_sms_ext_version "${SMS_GAME_VERSION}")
if(NOT _sms_ext_version)
  execute_process(COMMAND git log -1 --format=%cd --date=format:%Y.%m.%d WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_VARIABLE _sms_ext_date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  set(_sms_ext_version "${_sms_ext_date}")
endif()
if(SMS_ECLIPSE)
  string(APPEND _sms_ext_build "-eclipse")
endif()
# In a generated header rather than a -D: a quoted definition does not survive
# every toolchain's compiler wrapper (32-bit Windows).
set(SMS_GAME_BUILD_STRING "${_sms_ext_build}")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/ext-generated/sms_game_build.h
     CONTENT "#define SMS_GAME_BUILD \"@SMS_GAME_BUILD_STRING@\"\n#define SMS_GAME_VERSION \"@_sms_ext_version@\"\n" @ONLY)
target_include_directories(sms PRIVATE ${CMAKE_BINARY_DIR}/ext-generated)
set_property(SOURCE ${SMS_EXT_HOST_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS SMS_HAVE_GAME_BUILD_H=1)
# for test tools that join this build (ext/multiplayer/tools/mp_shots.py)
file(WRITE ${CMAKE_BINARY_DIR}/multiplayer-build.txt "${_sms_ext_build}\n")
