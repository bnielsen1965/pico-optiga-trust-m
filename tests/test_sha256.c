/*
 * test_sha256.c — native unit test for the portable SHA-256
 * (include/pico_optiga/sha256.h). Plain asserts, no framework.
 *
 * Verified against the NIST FIPS 180-4 vector set: the empty string, "abc",
 * the 56-byte "abcdbcde...nopq" string, and 1,000,000 x 'a' (the last
 * exercises the multi-block update path).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/sha256.h"

static void check(const uint8_t *data, size_t len, const char *expected_hex) {
    uint8_t digest[32];
    pico_sha256(data, len, digest);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&hex[i * 2], "%02x", (unsigned)digest[i]);
    }
    hex[64] = '\0';

    if (0 != strcmp(hex, expected_hex)) {
        fprintf(stderr, "SHA-256 MISMATCH (len=%u):\n  got      %s\n  expected %s\n",
                (unsigned)len, hex, expected_hex);
        assert(0 && "sha256 mismatch");
    }
}

int main(void) {
    /* NIST FIPS 180-4 vectors. */
    check((const uint8_t *)"", 0u,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check((const uint8_t *)"abc", 3u,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          56u,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* 1,000,000 x 'a' — a large static buffer (BSS, not the stack). */
    static uint8_t million[1000000];
    memset(million, 'a', sizeof(million));
    check(million, sizeof(million),
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    printf("test_sha256: all assertions passed\n");
    return 0;
}
