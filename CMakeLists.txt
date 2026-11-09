# SPDX-License-Identifier: MIT
# ctp_hft - multi process CTP (SFIT CTP Mini) high frequency trading framework
#
# Build with MSVC (full version, links the CTP SDK):
#     cmake -B build -G "Visual Studio 17 2022" -A x64
#     cmake --build build --config Release
#
# Build with MinGW (simulation only, no CTP SDK needed):
#     cmake -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-mingw
cmake_minimum_required(VERSION 3.15)
project(ctp_hft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(HFT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(CTP_SDK_ROOT "${HFT_ROOT}/../win64" CACHE PATH "Root of the win64 CTP Mini SDK")
set(CTP_STD_SDK_ROOT "${HFT_ROOT}/sdk/stdctp" CACHE PATH "Root of the standard CTP 6.x SDK")

# Two live flavours exist (see src/common/ctp_compat.h):
#   mini  - the win64 SFIT CTP Mini SDK, for CTPII Mini brokers
#   std   - the standard CTP 6.x SDK (sdk/stdctp), for SimNow / ordinary
#           brokers; adds the settlement-confirmation step and Init() shim.
set(HFT_CTP_FLAVOUR "mini" CACHE STRING "CTP SDK flavour: mini or std")

# Live CTP support now works for both toolchains: the SDK DLLs are resolved
# at run time (src/common/ctp_loader.*) instead of linking the MSVC import
# libraries, so MinGW builds can trade live as well. In sim mode the DLLs are
# never touched - enabling this costs nothing when they are absent.
option(HFT_ENABLE_CTP "Compile the live CTP gateway (runtime DLL loading)" ON)

find_package(Threads REQUIRED)

# MinGW links its own runtime dynamically by default, which makes every .exe
# depend on libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll - none of
# which exist on a stock Windows machine (=> "cannot start because
# libgcc_s_seh-1.dll is missing"). Statically pulling them in yields a self
# contained binary that only needs KERNEL32 plus the system UCRT.
if(MINGW)
    option(HFT_STATIC_RUNTIME "Statically link the MinGW runtime" ON)
else()
    option(HFT_STATIC_RUNTIME "Statically link the MinGW runtime" OFF)
endif()

set(HFT_COMMON_SOURCES
    src/common/shm.cpp
    src/common/ctp_loader.cpp
    src/common/config.cpp
    src/common/logger.cpp
    src/common/process_app.cpp
    src/sim/sim_market.cpp
    src/sim/sim_matcher.cpp
)

set(HFT_INCLUDE_DIRS "${HFT_ROOT}/include" "${HFT_ROOT}/third_party/fmtlog")
add_compile_definitions(FMT_HEADER_ONLY=1)

# SFIT ships the SDK headers GBK encoded; our own sources are ASCII. The
# translation units below (kept pure ASCII on purpose) pull those headers in,
# so under GCC they must be compiled with -finput-charset=GBK. MSVC reads the
# local code page by default, which matches GBK on Chinese systems already.
set(HFT_GBK_TUS
    src/md/ctp_md_gateway.cpp
    src/md/md_main.cpp
    src/trader/ctp_trader_gateway.cpp
    src/trader/trader_main.cpp
    src/query/ctp_query_gateway.cpp
    src/query/query_main.cpp)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT MSVC)
    set_source_files_properties(${HFT_GBK_TUS} PROPERTIES
        COMPILE_OPTIONS "-finput-charset=GBK")
endif()

# ---- process targets --------------------------------------------------------
# Each process is a standalone executable; they talk only through shared memory.
function(add_hft_process target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${HFT_INCLUDE_DIRS} "${HFT_ROOT}/src")
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /O2 /wd4819)
    else()
        target_compile_options(${target} PRIVATE -O2 -Wall)
    endif()
    if(HFT_STATIC_RUNTIME)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
endfunction()

add_hft_process(md_proc
    src/md/md_main.cpp
    src/md/ctp_md_gateway.cpp
    ${HFT_COMMON_SOURCES})

add_hft_process(trader_proc
    src/trader/trader_main.cpp
    src/trader/ctp_trader_gateway.cpp
    ${HFT_COMMON_SOURCES})

add_hft_process(query_proc
    src/query/query_main.cpp
    src/query/ctp_query_gateway.cpp
    ${HFT_COMMON_SOURCES})

add_hft_process(strategy_proc
    src/strategy/strategy_main.cpp
    src/strategy/ma_tick_strategy.cpp
    ${HFT_COMMON_SOURCES})

# Read-only observer of the shared bus (optional, very handy in production).
add_hft_process(monitor
    src/tools/monitor_main.cpp
    ${HFT_COMMON_SOURCES})

# Cold-start utility: zeroes every shared-memory section of the bus. The
# start_*.bat scripts run it before launching the four processes so stale
# queue contents from an earlier run are never replayed.
add_hft_process(shm_reset
    src/tools/shm_reset_main.cpp
    src/common/shm.cpp)

# ---- MinGW runtime safety net -----------------------------------------------
# With -static the binaries have no external dependencies left. Some MinGW
# distributions however cannot fully statically link libwinpthread (no static
# lib shipped), in which case the executables would again require
# libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll at start-up.
# Copying the DLLs next to the binaries makes them resolvable either way.
if(MINGW)
    get_filename_component(HFT_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${HFT_COMPILER_DIR}/${_dll}")
            file(COPY "${HFT_COMPILER_DIR}/${_dll}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
            message(STATUS "ctp_hft: safety-net DLL copied: ${_dll}")
        endif()
    endforeach()
endif()

# ---- CTP SDK wiring ---------------------------------------------------------
# No import library is linked: the gateways resolve the SDK factories at run
# time (see src/common/ctp_loader.h), which is what makes this work with both
# MSVC and MinGW. All that is needed here are headers, the macro and the DLLs
# copied next to the executables (sim mode never loads them).
if(HFT_ENABLE_CTP)
    if(HFT_CTP_FLAVOUR STREQUAL "std")
        set(HFT_CTP_SDK_INC "${CTP_STD_SDK_ROOT}/include")
        set(HFT_CTP_TRADER_DLL "${CTP_STD_SDK_ROOT}/dll/thosttraderapi_se.dll")
        set(HFT_CTP_MD_DLL "${CTP_STD_SDK_ROOT}/dll/thostmduserapi_se.dll")
        set(HFT_CTP_EXTRA_DEFS HFT_CTP_STD=1)
        if(NOT EXISTS "${HFT_CTP_SDK_INC}/ThostFtdcMdApi.h")
            message(FATAL_ERROR "standard CTP SDK not found at ${CTP_STD_SDK_ROOT}")
        endif()
    else()
        set(HFT_CTP_SDK_INC "${CTP_SDK_ROOT}/mdapi")
        set(HFT_CTP_TRADER_DLL "${CTP_SDK_ROOT}/traderapi/thosttraderapi.dll")
        set(HFT_CTP_MD_DLL "${CTP_SDK_ROOT}/mdapi/thostmduserapi.dll")
        set(HFT_CTP_EXTRA_DEFS "")
        if(NOT EXISTS "${CTP_SDK_ROOT}/mdapi/ThostFtdcMdApi.h")
            message(FATAL_ERROR "CTP Mini SDK not found at ${CTP_SDK_ROOT}")
        endif()
    endif()

    foreach(_t md_proc trader_proc query_proc)
        target_compile_definitions(${_t} PRIVATE HFT_ENABLE_CTP=1 ${HFT_CTP_EXTRA_DEFS})
    endforeach()
    target_include_directories(md_proc PRIVATE "${HFT_CTP_SDK_INC}")
    target_include_directories(trader_proc PRIVATE "${HFT_CTP_SDK_INC}")
    target_include_directories(query_proc PRIVATE "${HFT_CTP_SDK_INC}")

    # The strategy process never talks to CTP, but it shares the config with the
    # others, so it only needs the macro for consistency.
    target_compile_definitions(strategy_proc PRIVATE HFT_ENABLE_CTP=1)

    # Copy the runtime DLLs next to the executables so live mode finds them.
    if(EXISTS "${HFT_CTP_MD_DLL}")
        add_custom_command(TARGET md_proc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HFT_CTP_MD_DLL}" $<TARGET_FILE_DIR:md_proc>)
    endif()
    if(EXISTS "${HFT_CTP_TRADER_DLL}")
        add_custom_command(TARGET trader_proc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HFT_CTP_TRADER_DLL}" $<TARGET_FILE_DIR:trader_proc>)
    endif()

    # Remove the OPPOSITE flavour's DLLs: a std-compiled executable must never
    # load a mini DLL (or vice versa) - the vtable layouts differ. Stale files
    # only exist when the flavour was switched without wiping the build tree.
    if(HFT_CTP_FLAVOUR STREQUAL "std")
        set(HFT_CTP_WRONG_DLLS thosttraderapi.dll thostmduserapi.dll)
    else()
        set(HFT_CTP_WRONG_DLLS thosttraderapi_se.dll thostmduserapi_se.dll)
    endif()
    foreach(_wrong ${HFT_CTP_WRONG_DLLS})
        add_custom_command(TARGET md_proc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:md_proc>/${_wrong}")
        add_custom_command(TARGET trader_proc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:trader_proc>/${_wrong}")
    endforeach()
endif()

message(STATUS "ctp_hft: CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER_ID} HFT_ENABLE_CTP=${HFT_ENABLE_CTP} HFT_CTP_FLAVOUR=${HFT_CTP_FLAVOUR}")
