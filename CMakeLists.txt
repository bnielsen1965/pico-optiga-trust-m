#
# pico-optiga-trust-m
#
# CMake project for a C library that talks to an Infineon OPTIGA Trust M
# (SLS32AIA) security chip from a Raspberry Pi Pico II (RP2350).
#
# Hardware:
#   - I2C0: SDA = GP20, SCL = GP21
#   - Trust M 7-bit I2C address: 0x30 (default, see datasheet Table 20/21)
#
# Layers (see PLAN.md):
#   applications  ->  pico_optiga (this project's API)
#                 ->  optiga_trust_m (Infineon host library, third_party/)
#                 ->  PAL (./pal - Pico port of the Infineon platform layer)
#                 ->  pico-sdk i2c driver  ->  RP2350 I2C0  ->  Trust M
#
cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# Raspberry Pi Pico SDK
#
# The SDK must be located and imported *before* project(): its toolchain
# selection has to be in effect before CMake detects compilers.
#
# Resolution order:
#   1. -DPICO_SDK_PATH=/path/to/pico-sdk   (explicit)
#   2. third_party/pico-sdk                (submodule / local clone)
#   3. FetchContent of a pinned release
# ---------------------------------------------------------------------------
if(NOT DEFINED PICO_SDK_PATH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico-sdk/CMakeLists.txt")
        set(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico-sdk")
    else()
        message(STATUS "Pico SDK not found locally - fetching pico-sdk 2.3.1")
        include(FetchContent)
        set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps" CACHE PATH "" FORCE)
        FetchContent_Populate(pico_sdk
                              GIT_REPOSITORY "https://github.com/raspberrypi/pico-sdk.git"
                              GIT_TAG        "2.3.1"
                              GIT_SHALLOW    TRUE)
        set(PICO_SDK_PATH "${pico_sdk_SOURCE_DIR}")
    endif()
endif()

message(STATUS "Using Pico SDK at: ${PICO_SDK_PATH}")
# pico-sdk 2.x uses external/, 1.x used externals/
if(EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    set(_pico_sdk_import "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
elseif(EXISTS "${PICO_SDK_PATH}/externals/pico_sdk_import.cmake")
    set(_pico_sdk_import "${PICO_SDK_PATH}/externals/pico_sdk_import.cmake")
else()
    message(FATAL_ERROR "pico_sdk_import.cmake not found under ${PICO_SDK_PATH}")
endif()
include("${_pico_sdk_import}")

project(pico_optiga_trust_m
        DESCRIPTION "OPTIGA Trust M (Infineon SLS32AIA) C library for the Raspberry Pi Pico II (RP2350)"
        LANGUAGES C CXX)

# Actually pull the SDK into the build (must come after project()).
pico_sdk_init()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PICO_OPTIGA_BUILD_EXAMPLES "Build the example applications" ON)
option(PICO_OPTIGA_SHIELDED_CONNECTION
       "Enable the Infineon shielded (encrypted) I2C connection. Requires Mbed TLS 4.x plus a crypto PAL (pal_crypt_*). Off by default: plain IFX I2C is sufficient for basic use."
       OFF)

# ---------------------------------------------------------------------------
# Target board: Raspberry Pi Pico II (RP2350) by default
# ---------------------------------------------------------------------------
set(PICO_BOARD "pico2" CACHE STRING "Pico SDK board name (pico2 = Raspberry Pi Pico II / RP2350)")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Infineon OPTIGA Trust M host library (third_party/optiga-trust-m)
#
# The upstream project ships no top-level CMake target, so we build the core
# sources (src/cmd, src/common, src/comms, src/crypt, src/util) into a static
# library ourselves. The PAL (platform abstraction layer) for the Pico is
# provided by this project in ./pal (see PLAN.md, Phase 2).
#
# Without PICO_OPTIGA_SHIELDED_CONNECTION the library does not require
# Mbed TLS at all.
# ---------------------------------------------------------------------------
set(OPTIGA_TRUST_M_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/optiga-trust-m")

if(EXISTS "${OPTIGA_TRUST_M_DIR}/src")
    file(GLOB_RECURSE OPTIGA_TRUST_M_SOURCES CONFIGURE_DEPENDS
         "${OPTIGA_TRUST_M_DIR}/src/*.c")

    add_library(optiga_trust_m STATIC ${OPTIGA_TRUST_M_SOURCES})
    target_include_directories(optiga_trust_m PUBLIC
        "${OPTIGA_TRUST_M_DIR}/include"
        "${OPTIGA_TRUST_M_DIR}/include/cmd"
        "${OPTIGA_TRUST_M_DIR}/include/common"
        "${OPTIGA_TRUST_M_DIR}/include/comms"
        "${OPTIGA_TRUST_M_DIR}/include/ifx_i2c"
        "${OPTIGA_TRUST_M_DIR}/include/pal")
    # The upstream sources are not warning-clean under the Pico SDK flags.
    target_compile_options(optiga_trust_m PRIVATE -w)
    if(PICO_OPTIGA_SHIELDED_CONNECTION)
        target_compile_definitions(optiga_trust_m PUBLIC OPTIGA_COMMS_SHIELDED_CONNECTION=1)
        message(WARNING
            "PICO_OPTIGA_SHIELDED_CONNECTION=ON: Mbed TLS 4.x and a crypto PAL "
            "(pal_crypt_*) must be provided. See PLAN.md, Phase 6.")
    endif()
else()
    message(WARNING
        "Infineon OPTIGA Trust M host library not found at ${OPTIGA_TRUST_M_DIR}.\n"
        "Run:  git submodule update --init third_party/optiga-trust-m")
endif()

# ---------------------------------------------------------------------------
# pico_optiga - this project's library (grows across the phases in PLAN.md)
#
# Currently a placeholder: it has no sources until Phase 2 (PAL port) and
# Phase 4 (stable public API). The target is only created once sources exist
# so the skeleton configures and builds cleanly in the meantime.
# ---------------------------------------------------------------------------
file(GLOB PICO_OPTIGA_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/pal/*.c")

if(PICO_OPTIGA_SOURCES)
    add_library(pico_optiga STATIC ${PICO_OPTIGA_SOURCES})
    target_include_directories(pico_optiga PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(_pico_optiga_deps pico_stdlib)
    if(TARGET optiga_trust_m)
        list(APPEND _pico_optiga_deps optiga_trust_m)
    endif()
    target_link_libraries(pico_optiga PUBLIC ${_pico_optiga_deps})
endif()

# ---------------------------------------------------------------------------
# Examples: every examples/<name>/main.c becomes a flashable .uf2
# ---------------------------------------------------------------------------
if(PICO_OPTIGA_BUILD_EXAMPLES)
    file(GLOB _example_dirs "${CMAKE_CURRENT_SOURCE_DIR}/examples/*")
    foreach(_dir IN LISTS _example_dirs)
        if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/main.c")
            get_filename_component(_name "${_dir}" NAME)
            add_executable(${_name} "${_dir}/main.c")
            target_link_libraries(${_name} PRIVATE pico_optiga)
            pico_enable_stdio_uart(${_name} 1)
            pico_enable_stdio_usb(${_name} 1)
            pico_add_extra_outputs(${_name})
        endif()
    endforeach()
endif()
