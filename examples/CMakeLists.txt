#
# Examples — proof-of-concept applications built on the pico_optiga module.
#
# This is a SEPARATE CMake scope from the module: the top-level CMakeLists.txt
# builds only optiga_trust_m + pico_optiga and does not reference anything in
# this directory. Keeping the examples here (and their deps as submodules
# under examples/) means embedding the module in a project never pulls in
# example-only code.
#
#   fs_audit/  read-only audit of the existing littlefs volume (Phase 7 prereq)
#   (vault/ + fido2/ arrive in Phase 7)
#
# Every examples/<name>/main.c becomes a flashable .uf2. Stdio goes over
# UART0 (CP210x USB-serial bridge on the bench), not the Pico's native USB
# port.

# ---------------------------------------------------------------------------
# Example-only dependency: pico-littlefs (examples/pico-littlefs submodule).
# Pico-side littlefs for the apps that persist to the shared volume at flash
# 0x80000 (384 x 4 KB = 1.5 MB) — the layout the sibling pico-atecc608
# project already wrote to this flash. Added only in this scope.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pico-littlefs/CMakeLists.txt")
    add_subdirectory(pico-littlefs)
else()
    message(WARNING
        "pico-littlefs not found at examples/pico-littlefs.\n"
        "Run:  git submodule update --init --recursive examples/pico-littlefs")
endif()

# ---------------------------------------------------------------------------
# Output location: keep every example's .uf2/.elf/.hex/.bin in the top-level
# build dir (build/<preset>/), not build/<preset>/examples/. The targets live
# in this subdirectory scope, so without this CMake would emit them under
# examples/ and the flash path (build/<preset>/<name>.uf2) would silently
# serve a stale file.
#
# Two vars are needed because the SDK splits the outputs:
#   * CMAKE_RUNTIME_OUTPUT_DIRECTORY  -> the .elf (and .bin/.hex/.map/dis)
#   * CMAKE_ARCHIVE_OUTPUT_DIRECTORY  -> the .uf2: pico_add_uf2_output() in
#     the SDK reads the target's ARCHIVE_OUTPUT_DIRECTORY to place the .uf2,
#     falling back to CMAKE_CURRENT_BINARY_DIR (= examples/) if unset.
# Setting both to the top-level build dir keeps the flash path stable.
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Example discovery: every <name>/main.c under examples/ becomes a .uf2.
# (The pico-littlefs submodule dir has no main.c, so it is skipped.)
# ---------------------------------------------------------------------------
# Examples that link pico-littlefs: the read-only fs_audit plus the Phase 7
# apps. A list so adding an app is a one-line change.
set(_pico_optiga_lfs_examples fs_audit vault fido2)
file(GLOB _example_dirs "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(_dir IN LISTS _example_dirs)
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/main.c")
        get_filename_component(_name "${_dir}" NAME)
        add_executable(${_name} "${_dir}/main.c")
        set(_example_deps pico_stdlib hardware_i2c)
        # The Infineon stack calls into the PAL (pico_optiga), and since Phase
        # 4 pico_optiga calls into the stack (optiga_util_*) — a circular
        # archive dependency that single-pass archive resolution cannot
        # satisfy in either order, so both go into a --start-group/--end-group
        # link group.
        if(TARGET optiga_trust_m AND TARGET pico_optiga)
            list(APPEND _example_deps
                 "-Wl,--start-group;optiga_trust_m;pico_optiga;-Wl,--end-group")
        elseif(TARGET pico_optiga)
            list(APPEND _example_deps pico_optiga)
        endif()
        # Phase 7 examples (and the read-only fs_audit) persist to littlefs.
        if(_name IN_LIST _pico_optiga_lfs_examples AND TARGET pico-littlefs)
            list(APPEND _example_deps pico-littlefs)
        endif()
        target_link_libraries(${_name} PRIVATE ${_example_deps})
        pico_enable_stdio_uart(${_name} 1)
        pico_enable_stdio_usb(${_name} 0)
        pico_add_extra_outputs(${_name})
    endif()
endforeach()
