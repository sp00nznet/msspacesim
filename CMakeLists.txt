# Microsoft Space Simulator (1994), statically recompiled.
#
# One executable, spacesim, built only if its lifted C exists -- run
# `py -3 tools/lift.py` first (Setup.cmd does). The lifted C is generated from
# your own copy of the game and is never committed; see README.
#
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(spacesim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# Static CRT: the exe runs on a machine with no VC++ redistributable (netlab
# test VMs have none; repo rules, section 10).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(SPACESIM_TRACE "record lifted calls and enable SPACESIM_WATCH (slow)" OFF)

# pcrecomp supplies the CPU model (runtime/recomp16/cpu.h) the lifted C uses.
set(PCRECOMP_HOME "$ENV{PCRECOMP_HOME}" CACHE PATH "pcrecomp checkout")
if(NOT PCRECOMP_HOME)
    foreach(cand work/pcrecomp ../pcrecomp ../pcrecomp-main)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${cand}/runtime/recomp16/cpu.h")
            get_filename_component(PCRECOMP_HOME "${CMAKE_SOURCE_DIR}/${cand}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${PCRECOMP_HOME}/runtime/recomp16/cpu.h")
    message(FATAL_ERROR "pcrecomp not found: set PCRECOMP_HOME to a checkout of https://github.com/sp00nznet/pcrecomp")
endif()

if(MSVC)
    add_compile_options(/W1 /wd4996 /wd4244 /wd4146 /wd4102 /wd4101 /wd4267 /wd4018 /wd4305 /bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_link_options(/STACK:16777216)   # lifted call chains are deep
endif()

add_library(ymfm STATIC
    third_party/ymfm/ymfm_opl.cpp third_party/ymfm/ymfm_adpcm.cpp third_party/ymfm/ymfm_pcm.cpp)
target_include_directories(ymfm PUBLIC third_party/ymfm)

# The DOS machine, built once and linked into the game and each install tool.
add_library(dosmachine STATIC src/main.c src/machine.c src/dos.c src/ems.c src/drv.c src/vga.c
            src/audio.c src/x87.c src/host.c src/present_d3d.c src/state.c src/nocompanion.c src/opl.cpp)
target_include_directories(dosmachine PUBLIC src "${PCRECOMP_HOME}/runtime")
target_compile_definitions(dosmachine PRIVATE SPACESIM_RUNTIME)
target_link_libraries(dosmachine PUBLIC ymfm user32 gdi32 winmm d3d11 dxguid)
if(SPACESIM_TRACE)
    target_compile_definitions(dosmachine PUBLIC RECOMP_TRACE SPACESIM_DEBUG)
    target_compile_options(dosmachine PUBLIC /Zi /Oy-)
    target_link_options(dosmachine PUBLIC /DEBUG)
endif()

# The game, and the programs its installer ran to build the star and planet
# files (tools/lift.py GAMES; tools/setup.py runs them). Each is built only if
# its lifted C exists.
set(PROGRAMS spacesim makedata makehmo)
foreach(n RANGE 1 19)
    if(n LESS 10)
        list(APPEND PROGRAMS s0${n}data)
    else()
        list(APPEND PROGRAMS s${n}data)
    endif()
endforeach()
foreach(prog ${PROGRAMS})
    set(gen "${CMAKE_SOURCE_DIR}/work/${prog}/gen")
    if(EXISTS "${gen}/recomp_dispatch.c")
        file(GLOB lifted CONFIGURE_DEPENDS "${gen}/*.c")
        add_executable(${prog} ${lifted})
        target_include_directories(${prog} PRIVATE "${gen}")
        target_link_libraries(${prog} PRIVATE dosmachine)
    else()
        message(STATUS "${prog}: no lifted code in work/${prog}/gen -- run tools/lift.py ${prog}")
    endif()
endforeach()

# SS1.EXE, lifted as spacesim's companion (tools/lift.py COMPANIONS): its own
# library, so its recomp_all.h is the one its files see.
set(ss1gen "${CMAKE_SOURCE_DIR}/work/ss1/gen")
if(TARGET spacesim AND EXISTS "${ss1gen}/recomp_dispatch.c")
    file(GLOB ss1lifted CONFIGURE_DEPENDS "${ss1gen}/*.c")
    add_library(ss1 STATIC ${ss1lifted})
    target_include_directories(ss1 PRIVATE "${ss1gen}")
    target_link_libraries(ss1 PRIVATE dosmachine)
    target_sources(spacesim PRIVATE src/companion_ss1.c)
    target_link_libraries(spacesim PRIVATE ss1)
endif()

# The hardware models on their own, no game data needed: what CI runs.
add_executable(selftest tests/selftest.c src/vga.c src/x87.c src/audio.c src/opl.cpp)
target_include_directories(selftest PRIVATE src "${PCRECOMP_HOME}/runtime")
target_link_libraries(selftest PRIVATE ymfm)
target_compile_definitions(selftest PRIVATE SPACESIM_SELFTEST)
