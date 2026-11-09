/*
 * fs_audit — Phase 7 prerequisite: READ-ONLY audit of the existing littlefs
 * volume.
 *
 * This Pico's flash carries a littlefs volume from the pico-atecc608
 * development (offset 0x80000, 384 x 4 KB = 1.5 MB — the pico-littlefs
 * bare-metal default). That volume holds /io_key.bin, the ATECC608
 * I/O-protection key bound to the user's provisioned ATECC608 chip. Phase 7
 * (Vault / FIDO2) will reuse this volume, so before any write we inspect it
 * READ-ONLY and report:
 *   - every entry in the root (name + size),
 *   - /io_key.bin: presence, magic/len/CRC-32 validity, and the 32-byte key,
 *   - a filesystem space summary.
 *
 * SAFETY: strictly read-only. It mounts with pico_lfs_mount_existing() (which
 * never auto-formats, unlike pico_lfs_mount()) and performs only list / read
 * operations. No flash program or erase is issued. Safe to flash and run any
 * number of times.
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "pico_littlefs.h"

/* /io_key.bin layout (shared with pico-atecc608): a fixed 44-byte
 * little-endian struct — u32 magic ('I''O''K''1'), u32 len (32), u8 key[32],
 * u32 crc32 (standard reflected CRC-32 over the first 40 bytes). */
#define IOKEY_PATH   "/io_key.bin"
#define IOKEY_MAGIC  0x494F4B31u
#define IOKEY_LEN    32u
#define IOKEY_FILESZ 44u

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint8_t  key[IOKEY_LEN];
    uint32_t crc32;
} iokey_file_t;

static uint32_t crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static int on_entry(const pico_lfs_entry_t *e, void *user) {
    (void)user;
    if (e->is_dir) {
        printf("  [dir]  /%s/\n", e->name);
    } else {
        printf("  [file] /%s  (%u bytes)\n", e->name, (unsigned)e->size);
    }
    return 0; /* keep iterating */
}

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X", (unsigned)buf[i]);
        if ((i + 1u) % 16u == 0u) {
            printf("\n    ");
        } else if (i + 1u < len) {
            printf(" ");
        }
    }
    printf("\n");
}

int main(void) {
    stdio_init_all();
    printf("== littlefs partition audit (READ-ONLY) ==\n\n");

    /* Defaults: offset 0x80000, 384 x 4 KB — the pico-atecc608 layout. */
    int rc = pico_lfs_init(NULL);
    if (0 != rc) {
        printf("lfs_init: FAIL (%d)\n", rc);
        return 1;
    }

    /* Mount an existing volume — never auto-formats. */
    rc = pico_lfs_mount_existing();
    if (0 != rc) {
        printf("mount_existing: FAIL (%d) — no valid littlefs volume at 0x80000\n", rc);
        return 1;
    }
    printf("mount: OK (existing volume, read-only)\n\n");

    printf("Root directory:\n");
    rc = pico_lfs_list("/", on_entry, NULL);
    if (0 != rc) {
        printf("  list: FAIL (%d)\n", rc);
    }
    printf("\n");

    printf("Protected I/O key %s:\n", IOKEY_PATH);
    static iokey_file_t f;
    int n = pico_lfs_read_file(IOKEY_PATH, (uint8_t *)&f, sizeof(f));
    if (n < 0) {
        printf("  NOT PRESENT (read err %d)\n", n);
    } else if (n < (int)IOKEY_FILESZ) {
        printf("  TRUNCATED (%d bytes, expected %u)\n", n, (unsigned)IOKEY_FILESZ);
    } else {
        uint32_t calc = crc32((const uint8_t *)&f, 40u);
        if (f.magic != IOKEY_MAGIC) {
            printf("  BAD MAGIC (0x%08X, expected 0x%08X)\n",
                   (unsigned)f.magic, (unsigned)IOKEY_MAGIC);
        } else if (f.len != IOKEY_LEN) {
            printf("  BAD LEN (%u, expected %u)\n", (unsigned)f.len,
                   (unsigned)IOKEY_LEN);
        } else if (calc != f.crc32) {
            printf("  CRC MISMATCH (stored 0x%08X, calc 0x%08X)\n",
                   (unsigned)f.crc32, (unsigned)calc);
        } else {
            printf("  VALID (magic + len + CRC-32 OK). 32-byte key:\n    ");
            print_hex(f.key, IOKEY_LEN);
        }
    }
    printf("\n");

    uint32_t total = 0, used = 0, freeb = 0;
    if (0 == pico_lfs_space(&total, &used, &freeb)) {
        printf("Space: total=%u KB, used=%u KB, free=%u KB\n",
               total / 1024u, used / 1024u, freeb / 1024u);
    }

    pico_lfs_unmount();
    printf("\naudit complete (read-only; nothing written)\n");
    printf("done\n");
    return 0;
}
