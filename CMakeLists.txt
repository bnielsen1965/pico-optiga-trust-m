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
# Target board: Raspberry Pi Pico II (RP2350) by default.
# Must be set before the SDK is imported (the SDK reads it at init time).
# ---------------------------------------------------------------------------
set(PICO_BOARD "pico2" CACHE STRING "Pico SDK board name (pico2 = Raspberry Pi Pico II / RP2350)")

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

option(PICO_OPTIGA_PAL_TRACE
       "Enable the PAL bench trace on the serial console: [I2C] per transaction (register, data, driver result) and [EVT] for long one-shots. Off by default (too noisy in normal operation); turn on when debugging the IFX protocol."
       OFF)

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
    # The vendor's default config (optiga_lib_config_m_v3.h) enables the
    # shielded connection, which requires a crypto PAL (pal_crypt_*,
    # Mbed TLS based). Select the Pico config instead (plain IFX I2C,
    # warm reset): include/optiga_lib_config_pico.h, picked up via
    # OPTIGA_LIB_EXTERNAL. Our include/ dir is added to the search path
    # so the header resolves.
    if(PICO_OPTIGA_SHIELDED_CONNECTION)
        message(WARNING
            "PICO_OPTIGA_SHIELDED_CONNECTION=ON: the vendor v3 config is "
            "used; Mbed TLS 4.x and a crypto PAL (pal_crypt_*) must be "
            "provided. See PLAN.md, Phase 6.")
    else()
        target_include_directories(optiga_trust_m PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_definitions(optiga_trust_m PUBLIC
            OPTIGA_LIB_EXTERNAL="optiga_lib_config_pico.h")
    endif()
else()
    message(WARNING
        "Infineon OPTIGA Trust M host library not found at ${OPTIGA_TRUST_M_DIR}.\n"
        "Run:  git submodule update --init third_party/optiga-trust-m")
endif()

# ---------------------------------------------------------------------------
# pico_optiga - this project's library (grows across the phases in PLAN.md)
#
# src/ holds the public API (Phase 4), pal/ the Pico port of the Infineon
# PAL (Phase 2). Both go into the same archive: the stack calls into the
# PAL, and the API calls into the stack, so consumers link the pair in a
# --start-group/--end-group link group (see the examples section).
# ---------------------------------------------------------------------------
file(GLOB PICO_OPTIGA_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/pal/*.c")

if(PICO_OPTIGA_SOURCES)
    add_library(pico_optiga STATIC ${PICO_OPTIGA_SOURCES})
    target_include_directories(pico_optiga PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    # The PAL implements the platform surface of the Infineon host
    # library: it consumes the library's headers but links *below* it
    # (the stack calls into the PAL). The include directories are added
    # directly instead of via a link dependency, because
    # target_link_libraries(pico_optiga ... optiga_trust_m) would put
    # libpico_optiga.a *before* liboptiga_trust_m.a on the final link
    # line and break single-pass archive resolution (the stack's
    # pal_* references would go unresolved).
    if(TARGET optiga_trust_m)
        target_include_directories(pico_optiga SYSTEM PUBLIC
            "${OPTIGA_TRUST_M_DIR}/include"
            "${OPTIGA_TRUST_M_DIR}/include/cmd"
            "${OPTIGA_TRUST_M_DIR}/include/common"
            "${OPTIGA_TRUST_M_DIR}/include/comms"
            "${OPTIGA_TRUST_M_DIR}/include/ifx_i2c"
            "${OPTIGA_TRUST_M_DIR}/include/pal")
    endif()
    # hardware_i2c: the PAL (pal/pal_i2c.c) drives the SDK I2C driver.
    set(_pico_optiga_deps pico_stdlib hardware_i2c)
    target_link_libraries(pico_optiga PUBLIC ${_pico_optiga_deps})
    if(PICO_OPTIGA_PAL_TRACE)
        target_compile_definitions(pico_optiga PUBLIC PICO_OPTIGA_PAL_TRACE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples: every examples/<name>/main.c becomes a flashable .uf2
#
# Stdio goes over UART0 (CP210x USB-serial bridge on the bench), not the
# Pico's native USB port.
# ---------------------------------------------------------------------------
if(PICO_OPTIGA_BUILD_EXAMPLES)
    file(GLOB _example_dirs "${CMAKE_CURRENT_SOURCE_DIR}/examples/*")
    foreach(_dir IN LISTS _example_dirs)
        if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/main.c")
            get_filename_component(_name "${_dir}" NAME)
            add_executable(${_name} "${_dir}/main.c")
            set(_example_deps pico_stdlib hardware_i2c)
            # The Infineon stack calls into the PAL (pico_optiga), and
            # since Phase 4 pico_optiga calls into the stack
            # (optiga_util_*) — a circular archive dependency that
            # single-pass archive resolution cannot satisfy in either
            # order, so both go into a --start-group/--end-group link
            # group.
            if(TARGET optiga_trust_m AND TARGET pico_optiga)
                list(APPEND _example_deps
                     "-Wl,--start-group;optiga_trust_m;pico_optiga;-Wl,--end-group")
            elseif(TARGET pico_optiga)
                list(APPEND _example_deps pico_optiga)
            endif()
            target_link_libraries(${_name} PRIVATE ${_example_deps})
            pico_enable_stdio_uart(${_name} 1)
            pico_enable_stdio_usb(${_name} 0)
            pico_add_extra_outputs(${_name})
        endif()
    endforeach()
endif()
