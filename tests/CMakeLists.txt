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

# FIDO2 (Phase 7.2) — the chip-agnostic protocol pieces, ported from the
# sibling pico-atecc608 project (pure C, no Pico/hardware). The sources under
# test live in examples/fido2/.
set(FIDO2_SRC "${PICO_OPTIGA_SRC}/examples/fido2")

# CBOR codec (fido_cbor.c).
add_executable(test_cbor
    test_cbor.c
    "${FIDO2_SRC}/fido_cbor.c")
target_include_directories(test_cbor PRIVATE "${FIDO2_SRC}")
target_compile_options(test_cbor PRIVATE -Wall -Wextra)

# CTAPHID transport (fido_ctaphid.c) — pure C, no TinyUSB.
add_executable(test_ctaphid
    test_ctaphid.c
    "${FIDO2_SRC}/fido_ctaphid.c")
target_include_directories(test_ctaphid PRIVATE "${FIDO2_SRC}")
target_compile_options(test_ctaphid PRIVATE -Wall -Wextra)

# The fido2 example's SHA-256 (fido_sha256.c).
add_executable(test_fido_sha256
    test_fido_sha256.c
    "${FIDO2_SRC}/fido_sha256.c")
target_include_directories(test_fido_sha256 PRIVATE "${FIDO2_SRC}")
target_compile_options(test_fido_sha256 PRIVATE -Wall -Wextra)

# DER ECDSA sig <-> raw r||s (fido_der.c) — the OPTIGA-signs-DER / CTAP2-wants-
# raw glue. Pure C.
add_executable(test_fido_der
    test_fido_der.c
    "${FIDO2_SRC}/fido_der.c")
target_include_directories(test_fido_der PRIVATE "${FIDO2_SRC}")
target_compile_options(test_fido_der PRIVATE -Wall -Wextra)

# Credential store (fido_cred.c) — the record format, its CRC-32, and the
# store ops (alloc/load/find/insert/erase, persist-first). Pure C.
add_executable(test_cred
    test_cred.c
    "${FIDO2_SRC}/fido_cred.c")
target_include_directories(test_cred PRIVATE "${FIDO2_SRC}")
target_compile_options(test_cred PRIVATE -Wall -Wextra)

# CTAP2 command dispatch (fido_ctap.c) — GET_INFO / MAKE_CREDENTIAL /
# GET_ASSERTION / RESET. Pulls in the codec + store + COSE + SHA-256.
# Pure C; a fake key backend stands in for the chip.
add_executable(test_ctap
    test_ctap.c
    "${FIDO2_SRC}/fido_ctap.c"
    "${FIDO2_SRC}/fido_cbor.c"
    "${FIDO2_SRC}/fido_cred.c"
    "${FIDO2_SRC}/fido_cose.c"
    "${FIDO2_SRC}/fido_sha256.c")
target_include_directories(test_ctap PRIVATE "${FIDO2_SRC}")
target_compile_options(test_ctap PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME status_mapping COMMAND test_optiga_status)
add_test(NAME sha256 COMMAND test_sha256)
add_test(NAME vault_gcm COMMAND test_vault_gcm)
add_test(NAME vault_kdf COMMAND test_vault_kdf)
add_test(NAME cbor COMMAND test_cbor)
add_test(NAME ctaphid COMMAND test_ctaphid)
add_test(NAME fido_sha256 COMMAND test_fido_sha256)
add_test(NAME fido_der COMMAND test_fido_der)
add_test(NAME cred COMMAND test_cred)
add_test(NAME ctap COMMAND test_ctap)
