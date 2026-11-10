#
# Native (host) unit tests for pico_optiga.
#
# The main project is cross-compiled for the RP2350, so the tests live in
# their own CMake project and build with the host compiler:
#
#   cmake -S tests -B build/tests
#   cmake --build build/tests
#   ctest --test-dir build/tests
#
# Only pure logic (no pico-sdk, no hardware) is tested here.

cmake_minimum_required(VERSION 3.22)
project(pico_optiga_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PICO_OPTIGA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_optiga_status
    test_optiga_status.c
    "${PICO_OPTIGA_SRC}/src/optiga_status.c")
target_include_directories(test_optiga_status PRIVATE
    "${PICO_OPTIGA_SRC}/include"
    "${PICO_OPTIGA_SRC}/src"
    "${PICO_OPTIGA_SRC}/third_party/optiga-trust-m/include/common")

# Portable SHA-256 (header-only) — no other sources needed.
add_executable(test_sha256
    test_sha256.c)
target_include_directories(test_sha256 PRIVATE
    "${PICO_OPTIGA_SRC}/include")

# Vault AES-256-GCM (pure C, NIST-verified) — the source under test lives in
# the vault example (it is chip-agnostic, so it compiles identically on host).
add_executable(test_vault_gcm
    test_vault_gcm.c
    "${PICO_OPTIGA_SRC}/examples/vault/vault_gcm.c")
target_include_directories(test_vault_gcm PRIVATE
    "${PICO_OPTIGA_SRC}/examples/vault")

# Vault entry-key parameterization (determinism + wrong-PIN/name/salt
# rejection). Stands in for the chip's HKDF with a host SHA-256 of the same
# three inputs; the vault GCM (above) is the vehicle.
add_executable(test_vault_kdf
    test_vault_kdf.c
    "${PICO_OPTIGA_SRC}/examples/vault/vault_gcm.c")
target_include_directories(test_vault_kdf PRIVATE
    "${PICO_OPTIGA_SRC}/examples/vault"
    "${PICO_OPTIGA_SRC}/include")

enable_testing()
add_test(NAME status_mapping COMMAND test_optiga_status)
add_test(NAME sha256 COMMAND test_sha256)
add_test(NAME vault_gcm COMMAND test_vault_gcm)
add_test(NAME vault_kdf COMMAND test_vault_kdf)
