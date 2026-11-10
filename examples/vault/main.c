/*
 * vault — a PIN-gated password store demo built on the OPTIGA Trust M.
 *
 * The OPTIGA sibling of pico-atecc608/example/vault. Same geometry, same
 * console, same startup contract; the only real differences are the chip and
 * the KDF path (see "Division of labour" below).
 *
 * Wiring (same as the other examples):
 *   I2C0  GP20 (SDA), GP21 (SCL)  -> OPTIGA Trust M SDA/SCL (pull-ups to 3V3)
 *   UART0 GP0  (TX),  GP1 (RX)    -> USB-serial console (115200)
 *   USB   flashing
 *
 * Division of labour:
 *   chip = key master. The master is a NIST P-256 keypair generated on-chip
 *          into 0xE0F2 (KEY_AGREEMENT); the private key never leaves the chip.
 *          Each unlock is an on-chip ECDH against a fixed public peer key,
 *          landing in the chip session, then an on-chip HKDF-SHA256:
 *              session  = ECDH(0xE0F2, kPeerPub)           (on-chip, per call)
 *              entrykey = HKDF(session, salt || PIN, name)  (on-chip, 32 bytes)
 *          where salt comes from the entry (chip RNG, at add time). The
 *          32-byte entry key is the only thing exported to the host; the
 *          master private key and the ECDH session secret stay on the chip.
 *   Pico = bulk cipher: AES-256-GCM over the password (vault_gcm.c — software,
 *          host-verified against NIST GCM vectors; the OPTIGA performs no GCM).
 *
 * Why ECC and not the symmetric 0xE200 slot? The chip's derive_key (HKDF)
 * command only accepts a shared-secret input: the ECDH session, or a raw
 * secret sitting in a data object (type PRESSEC). A generated symmetric key
 * slot is neither class, so HKDF(0xE200) is refused by the chip itself (the
 * host forwards the OID verbatim; the decision is in the chip firmware). The
 * ECC master is the on-chip design the chip's KDF actually accepts. See
 * PLAN.md, Phase 7.1.
 *
 * A wrong PIN derives a wrong entry key, so the GCM auth tag fails and `get`
 * can only report "wrong PIN or corrupted entry" — the two are deliberately
 * not distinguished. The entry name is bound as GCM AAD (and as the HKDF
 * info), so ciphertexts cannot be swapped between entries or renamed.
 *
 * Entry layout: one littlefs file per entry, /v_<name>. Fixed 96-byte record:
 *   u32     magic   'VAU1'
 *   u8[16]  salt    chip RNG at add time (not secret)
 *   u8[12]  nonce   GCM IV, chip RNG at add time (not secret)
 *   u32     pt_len  password length
 *   u8[40]  ct      GCM ciphertext (first pt_len bytes valid)
 *   u8[16]  tag     GCM auth tag
 *   u32     crc32   standard reflected CRC-32 over all preceding bytes
 *
 * Startup contract: the chip must already be present AND the master keypair
 * must already be provisioned into 0xE0F2 (run `provision` once). If the
 * master is missing, add/get refuse and point at `provision`; this app does
 * NOT auto-provision on boot.
 *
 * Note on re-keying: 0xE0F2's Change AC stays open for the chip's life (the
 * lifecycle is never advanced past creation), so `provision` can be run again
 * to re-key the master — but that invalidates every existing entry (they were
 * derived from the old master). The OPTIGA has no config-zone lock (unlike the
 * ATECC608); "permanence" = the key is non-volatile and re-keying is the only
 * way to lose it.
 *
 * Demo caveats:
 *   - the console echoes what you type: the PIN and password are visible on
 *     the serial console;
 *   - the 32-byte entry key crosses I2C in the clear (this build has no
 *     shielded connection). The master private key and the ECDH session do
 *     NOT — only the derived, per-entry key is exported;
 *   - `get` prints the password to the console. That is the point of the demo.
 *
 * Commands:
 *   help                          - this list
 *   probe                         - chip presence + master slot state
 *   bootrom                       - reset into BOOTSEL mode (USB bootloader)
 *   provision                     - generate the master keypair into 0xE0F2
 *                                   (one-time; re-running re-keys and
 *                                   invalidates all existing entries)
 *   list                          - list stored entries
 *   add <name> <pin> <password>   - encrypt + store; replaces an existing entry
 *   get <name> <pin>              - decrypt + print the password
 *   dump <name>                   - show the RAW stored entry (no PIN)
 *   rm <name>                     - delete the entry
 *
 * Limits: name 1..16 chars from [A-Za-z0-9._-]; PIN 1..24 ASCII;
 * password 1..40 ASCII (no spaces — the command line is one buffer).
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico_littlefs.h"

#include "pico_optiga/optiga.h"
#include "vault_gcm.h"

// --- vault geometry ----------------------------------------------------------

// KDF master: a NIST P-256 keypair generated on-chip into 0xE0F2
// (KEY_AGREEMENT). The private key never leaves the chip. 0xE0F3 is reserved
// for FIDO2 (Phase 7.2).
#define VAULT_MASTER_OID  0xE0F2u

#define VAULT_SALT_LEN    16
#define VAULT_NONCE_LEN   12              // == VAULT_GCM_NONCE_LEN
#define VAULT_NAME_MAX    16
#define VAULT_PIN_MAX     24              // HKDF salt = salt || PIN (<= 40 B)
#define VAULT_PASS_MAX    40
#define VAULT_MAGIC       0x31554156u     // 'V''A''U''1'
#define VAULT_FILE_PREFIX "v_"
#define VAULT_PUB_CAP     128u

// Fixed peer public key for the on-chip ECDH (a valid P-256 point, in the same
// 68-byte format a keygen returns: 03 42 00 04 || X(32) || Y(32)). It is
// public, not secret — the secrecy is in the on-chip private key of 0xE0F2.
// The same point the Phase 6 crypt bench used, so the flow is bench-proven.
static const uint8_t kPeerPub[68] = {
    0x03, 0x42, 0x00, 0x04,
    0x94, 0x89, 0x2F, 0x09, 0xEA, 0x4E, 0xCA, 0xBC, 0x6A, 0x4E, 0xF2, 0x06,
    0x36, 0x26, 0xE0, 0x5D, 0xE0, 0xD5, 0xF9, 0x77, 0xEA, 0xC3, 0xB2, 0x70,
    0xAC, 0xE2, 0x19, 0x00, 0xF5, 0xDB, 0x56, 0xE7, 0x37, 0xBB, 0xBE, 0x46,
    0xE4, 0x49, 0x76, 0x38, 0x25, 0xB5, 0xF8, 0x94, 0x74, 0x9E, 0x1A, 0xB6,
    0x5A, 0xF1, 0x29, 0xD7, 0x3A, 0xB6, 0x9B, 0x80, 0xAC, 0xC5, 0xE1, 0xC3,
    0x10, 0xF2, 0x16, 0xC6};

// One fixed-size littlefs file per entry (96 bytes).
typedef struct {
    uint32_t magic;
    uint8_t  salt[VAULT_SALT_LEN];
    uint8_t  nonce[VAULT_NONCE_LEN];
    uint32_t pt_len;
    uint8_t  ct[VAULT_PASS_MAX];
    uint8_t  tag[VAULT_GCM_TAG_LEN];
    uint32_t crc32;                       // over all preceding bytes
} vault_entry_t;

// Startup-contract state, recorded in main().
static bool s_chip_present       = false;
static bool s_master_provisioned = false;

// --- helpers -------------------------------------------------------------------

static void entry_path(const char *name, char *path, size_t path_size)
{
    snprintf(path, path_size, "/%s%s", VAULT_FILE_PREFIX, name);
}

static bool valid_name(const char *name)
{
    size_t n = strlen(name);
    if ((n < 1) || (n > VAULT_NAME_MAX)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (c == '.') || (c == '_') || (c == '-'))) {
            return false;
        }
    }
    return true;
}

// Split rest (modified in place) into at most max space-separated tokens;
// returns the number found (leading/multiple spaces tolerated).
static int split_words(char *rest, int max, char **tok)
{
    int n = 0;
    while (*rest && (n < max)) {
        while (*rest == ' ') {
            rest++;
        }
        if (!*rest) {
            break;
        }
        tok[n++] = rest;
        while (*rest && (*rest != ' ')) {
            rest++;
        }
        if (*rest) {
            *rest++ = '\0';
        }
    }
    return n;
}

// Standard reflected CRC-32 (same polynomial as fs_audit / the ATECC608
// io_key file), used for the entry's trailing integrity word.
static uint32_t crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Is the master keypair present in 0xE0F2? Reads the slot metadata and looks
// for the key-algo tag (0xE0). Pure read — never modifies the chip.
static bool master_present(void)
{
    uint8_t meta[256];
    size_t n = 0;
    if (PICO_OPTIGA_OK != pico_optiga_read_metadata(VAULT_MASTER_OID, meta,
                                                    sizeof(meta), &n)) {
        return false;
    }
    for (size_t i = 0; i + 2u < n; i++) {
        if ((meta[i] == 0xE0u) && (meta[i + 1u] == 0x01u)) {
            return true;
        }
    }
    return false;
}

// The startup contract: chip present AND master provisioned. Prints what is
// missing; never auto-provisions.
static bool vault_ready(void)
{
    if (!s_chip_present) {
        printf("OPTIGA not found on the bus (see `probe`) — nothing to do.\n");
        return false;
    }
    if (!s_master_provisioned) {
        printf("master not provisioned — run `provision` first (generates the\n"
               "       P-256 keypair into 0x%04X on the chip).\n",
               (unsigned)VAULT_MASTER_OID);
        return false;
    }
    return true;
}

// entrykey = HKDF( ECDH(0xE0F2, kPeerPub) , salt || PIN , info = name ).
// The ECDH session secret stays on the chip; only the 32-byte derived key is
// exported. A wrong PIN derives a wrong key, so the GCM tag will not verify.
static bool vault_derive_key(const uint8_t salt[VAULT_SALT_LEN],
                             const char *pin, size_t pin_len,
                             const char *name,
                             uint8_t key[VAULT_GCM_KEY_LEN])
{
    uint8_t hkdf_salt[VAULT_SALT_LEN + VAULT_PIN_MAX];
    memcpy(hkdf_salt, salt, VAULT_SALT_LEN);
    memcpy(hkdf_salt + VAULT_SALT_LEN, pin, pin_len);

    int rc = pico_optiga_ecdh(VAULT_MASTER_OID, kPeerPub, sizeof(kPeerPub),
                              NULL, 0u);
    if (PICO_OPTIGA_OK != rc) {
        printf("ECDH failed: %s\n", pico_optiga_strerror(rc));
        return false;
    }
    rc = pico_optiga_hkdf(PICO_OPTIGA_KEY_SESSION, hkdf_salt,
                          VAULT_SALT_LEN + pin_len, (const uint8_t *)name,
                          strlen(name), key, VAULT_GCM_KEY_LEN);
    if (PICO_OPTIGA_OK != rc) {
        printf("HKDF failed: %s\n", pico_optiga_strerror(rc));
        return false;
    }
    return true;
}

// Read one line from the console (echoing as you type) into buf.
static void read_line(char *buf, size_t cap)
{
    size_t n = 0;
    int c;
    while ((c = stdio_getchar()) != -1) {
        if ((c == '\r') || (c == '\n')) {
            break;
        }
        if ((c == '\b') || (c == 0x7f)) {
            if (n > 0) {
                n--;
                printf("\b \b");
            }
            continue;
        }
        if ((n + 1u) < cap) {
            buf[n++] = (char)c;
            printf("%c", c);
        }
    }
    buf[n] = '\0';
    printf("\n");
}

// --- commands --------------------------------------------------------------------

static void commandHelp(char *args)
{
    (void)args;
    printf("vault: OPTIGA Trust M PIN-gated password store demo\n");
    printf("help      - display this message\n");
    printf("probe     - chip presence + master slot state\n");
    printf("bootrom   - reset into BOOTSEL mode (USB bootloader)\n");
    printf("provision - generate the master P-256 keypair into 0x%04X (one-time)\n",
           (unsigned)VAULT_MASTER_OID);
    printf("list      - list stored entries\n");
    printf("add <name> <pin> <password> - encrypt + store (replaces an entry)\n");
    printf("get <name> <pin> - decrypt + print the password\n");
    printf("dump <name> - show the raw stored entry ('?' = non-printable byte)\n");
    printf("rm <name> - delete the entry\n");
    printf("note: input is echoed, and get prints the password — that is the demo.\n");
}

static void commandProbe(char *args)
{
    (void)args;
    if (!s_chip_present) {
        printf("OPTIGA not found on the bus — check I2C0 GP20/GP21 wiring.\n");
        return;
    }
    printf("OPTIGA present (application open).\n");
    printf("master 0x%04X: %s\n", (unsigned)VAULT_MASTER_OID,
           master_present() ? "provisioned (P-256 keypair)"
                            : "EMPTY — run `provision`");
}

static void commandBootrom(char *args)
{
    (void)args;
    printf("Rebooting into BOOTSEL mode... (watch for the RP2350 drive)\n");
    // Give the console a moment to flush the message out the UART.
    sleep_ms(200);
    rom_reset_usb_boot(0, 0);
}

static void commandProvision(char *args)
{
    (void)args;
    if (!s_chip_present) {
        printf("OPTIGA not found on the bus — nothing to provision.\n");
        return;
    }
    if (s_master_provisioned) {
        printf("WARNING: 0x%04X already holds a master keypair. Re-provisioning\n"
               "         generates a NEW master and INVALIDATES every existing\n"
               "         entry (they were derived from the old master).\n",
               (unsigned)VAULT_MASTER_OID);
    }
    uint8_t pub[VAULT_PUB_CAP];
    size_t pub_len = 0;
    int rc = pico_optiga_ecc_gen_keypair(VAULT_MASTER_OID, PICO_OPTIGA_CURVE_P256,
                                         PICO_OPTIGA_USAGE_KEY_AGREEMENT, pub,
                                         VAULT_PUB_CAP, &pub_len);
    if (PICO_OPTIGA_OK != rc) {
        printf("provision: FAIL — %s\n", pico_optiga_strerror(rc));
        return;
    }
    s_master_provisioned = true;
    printf("provision: OK — P-256 master keypair generated into 0x%04X.\n",
           (unsigned)VAULT_MASTER_OID);
    printf("  public key (%u bytes, reference only — the ECDH uses kPeerPub):\n  ",
           (unsigned)pub_len);
    for (size_t i = 0; i < pub_len; i++) {
        printf("%02X", (unsigned)pub[i]);
    }
    printf("\n  the private key stays on the chip. You can now `add` entries.\n");
}

// pico_lfs_list callback: print the vault entries.
static int list_cb(const pico_lfs_entry_t *entry, void *user)
{
    int *count = (int *)user;
    if ((!entry->is_dir) &&
        (0 == strncmp(entry->name, VAULT_FILE_PREFIX, strlen(VAULT_FILE_PREFIX)))) {
        printf("  %s\n", entry->name + strlen(VAULT_FILE_PREFIX));
        (*count)++;
    }
    return 0;  // continue
}

static void commandList(char *args)
{
    (void)args;
    if (!pico_lfs_is_mounted()) {
        printf("littlefs not mounted — cannot list\n");
        return;
    }
    int count = 0;
    if (0 != pico_lfs_list("/", list_cb, &count)) {
        printf("listing failed\n");
        return;
    }
    if (0 == count) {
        printf("(no entries)\n");
    }
}

static void commandAdd(char *args)
{
    char *tok[3];
    if (3 != split_words(args, 3, tok)) {
        printf("usage: add <name> <pin> <password>\n"
               "  name 1..%d [A-Za-z0-9._-]; PIN 1..%d chars; password 1..%d chars (no spaces)\n",
               VAULT_NAME_MAX, VAULT_PIN_MAX, VAULT_PASS_MAX);
        return;
    }
    const char *name = tok[0];
    const char *pin  = tok[1];
    const char *pass = tok[2];
    if (!valid_name(name)) {
        printf("name must be 1..%d chars from [A-Za-z0-9._-]\n", VAULT_NAME_MAX);
        return;
    }
    size_t pin_len = strlen(pin);
    if ((pin_len < 1) || (pin_len > VAULT_PIN_MAX)) {
        printf("PIN must be 1..%d chars\n", VAULT_PIN_MAX);
        return;
    }
    size_t pass_len = strlen(pass);
    if ((pass_len < 1) || (pass_len > VAULT_PASS_MAX)) {
        printf("password must be 1..%d chars (no spaces)\n", VAULT_PASS_MAX);
        return;
    }
    if (!vault_ready()) {
        return;
    }

    // Fresh salt + nonce from the chip RNG (one call covers both).
    uint8_t rand[VAULT_SALT_LEN + VAULT_NONCE_LEN];
    int rc = pico_optiga_get_random(rand, sizeof(rand));
    if (PICO_OPTIGA_OK != rc) {
        printf("RNG failed: %s\n", pico_optiga_strerror(rc));
        return;
    }

    vault_entry_t e;
    memset(&e, 0, sizeof(e));
    e.magic  = VAULT_MAGIC;
    memcpy(e.salt, rand, VAULT_SALT_LEN);
    memcpy(e.nonce, rand + VAULT_SALT_LEN, VAULT_NONCE_LEN);
    e.pt_len = (uint32_t)pass_len;

    uint8_t key[VAULT_GCM_KEY_LEN];
    if (!vault_derive_key(e.salt, pin, pin_len, name, key)) {
        return;
    }

    // The name is bound as GCM AAD: the ciphertext cannot be swapped
    // between entries or renamed.
    if (!vault_gcm_encrypt(key, e.nonce, (const uint8_t *)name, strlen(name),
                           (const uint8_t *)pass, pass_len,
                           e.ct, sizeof(e.ct), e.tag)) {
        printf("GCM encrypt failed (size error) — should not happen.\n");
        return;
    }
    e.crc32 = crc32((const uint8_t *)&e, offsetof(vault_entry_t, crc32));

    char path[2 + VAULT_NAME_MAX + 1];
    entry_path(name, path, sizeof(path));
    if (0 != pico_lfs_write_file(path, (const uint8_t *)&e, sizeof(e))) {
        printf("failed to write %s\n", path);
        return;
    }
    printf("stored '%s' (%u B password, fresh salt/nonce)\n", name,
           (unsigned)pass_len);
}

static void commandGet(char *args)
{
    char *tok[2];
    if (2 != split_words(args, 2, tok)) {
        printf("usage: get <name> <pin>\n");
        return;
    }
    const char *name = tok[0];
    const char *pin  = tok[1];
    if (!valid_name(name)) {
        printf("name must be 1..%d chars from [A-Za-z0-9._-]\n", VAULT_NAME_MAX);
        return;
    }
    size_t pin_len = strlen(pin);
    if ((pin_len < 1) || (pin_len > VAULT_PIN_MAX)) {
        printf("PIN must be 1..%d chars\n", VAULT_PIN_MAX);
        return;
    }
    if (!vault_ready()) {
        return;
    }

    char path[2 + VAULT_NAME_MAX + 1];
    entry_path(name, path, sizeof(path));
    vault_entry_t e;
    int rd = pico_lfs_read_file(path, (uint8_t *)&e, sizeof(e));
    if ((int)sizeof(e) != rd) {
        printf("no entry named '%s'\n", name);
        return;
    }
    // The CRC pre-check separates storage corruption from a wrong PIN
    // (which the GCM tag check alone cannot).
    if ((VAULT_MAGIC != e.magic) || (e.pt_len > VAULT_PASS_MAX) ||
        (crc32((const uint8_t *)&e, offsetof(vault_entry_t, crc32)) != e.crc32)) {
        printf("entry '%s' is corrupt (magic/length/CRC mismatch) — `rm` it and re-add\n",
               name);
        return;
    }

    uint8_t key[VAULT_GCM_KEY_LEN];
    if (!vault_derive_key(e.salt, pin, pin_len, name, key)) {
        return;
    }

    char pass[VAULT_PASS_MAX + 1];
    if (!vault_gcm_decrypt(key, e.nonce, (const uint8_t *)name, strlen(name),
                           e.ct, e.pt_len, e.tag, (uint8_t *)pass, sizeof(pass))) {
        // Wrong PIN and entry corruption are deliberately not distinguished.
        printf("wrong PIN or corrupted entry '%s'\n", name);
        return;
    }
    pass[e.pt_len] = '\0';
    printf("%s\n", pass);
}

// One byte as a console character: printable ASCII as-is, anything else
// as '?'.
static char dump_char(uint8_t b)
{
    return ((b >= 0x20) && (b <= 0x7e)) ? (char)b : '?';
}

// READ-ONLY: print the raw stored entry as it sits in flash (no PIN, no chip
// access — a littlefs read). Demonstrates that the password is not stored in
// the clear.
static void commandDump(char *args)
{
    char *tok[1];
    if (1 != split_words(args, 1, tok)) {
        printf("usage: dump <name>\n");
        return;
    }
    const char *name = tok[0];
    if (!valid_name(name)) {
        printf("name must be 1..%d chars from [A-Za-z0-9._-]\n", VAULT_NAME_MAX);
        return;
    }
    if (!pico_lfs_is_mounted()) {
        printf("littlefs not mounted — cannot read\n");
        return;
    }
    char path[2 + VAULT_NAME_MAX + 1];
    entry_path(name, path, sizeof(path));
    uint8_t raw[sizeof(vault_entry_t)];
    if ((int)sizeof(raw) != pico_lfs_read_file(path, raw, sizeof(raw))) {
        printf("no entry named '%s'\n", name);
        return;
    }
    printf("raw entry %s (%u B) as stored in flash — non-printable bytes as '?':\n",
           path, (unsigned)sizeof(raw));
    printf("layout: [0:4] magic | [4:20] salt | [20:32] nonce | [32:36] pt_len\n"
           "        [36:76] ct (the password, GCM-encrypted) | [76:92] tag | [92:96] crc32\n");
    for (size_t off = 0; off < sizeof(raw); off += 16) {
        printf("  %02u: ", (unsigned)off);
        for (size_t i = 0; i < 16; i++) {
            printf("%c", dump_char(raw[off + i]));
        }
        printf("\n");
    }
    // Record summary: same checks `get` uses before the tag check.
    vault_entry_t *e = (vault_entry_t *)raw;
    if ((VAULT_MAGIC == e->magic) && (e->pt_len <= VAULT_PASS_MAX) &&
        (crc32(raw, offsetof(vault_entry_t, crc32)) == e->crc32)) {
        printf("record: magic OK, pt_len %u, CRC OK — the password text must NOT appear above\n",
               (unsigned)e->pt_len);
    } else {
        printf("record: CORRUPT (magic/length/CRC mismatch) — `rm` it and re-add\n");
    }
}

static void commandRm(char *args)
{
    char *tok[1];
    if (1 != split_words(args, 1, tok)) {
        printf("usage: rm <name>\n");
        return;
    }
    const char *name = tok[0];
    if (!valid_name(name)) {
        printf("name must be 1..%d chars from [A-Za-z0-9._-]\n", VAULT_NAME_MAX);
        return;
    }
    if (!pico_lfs_is_mounted()) {
        printf("littlefs not mounted — cannot remove\n");
        return;
    }
    char path[2 + VAULT_NAME_MAX + 1];
    entry_path(name, path, sizeof(path));
    if (0 != pico_lfs_remove(path)) {
        printf("no entry named '%s'\n", name);
        return;
    }
    printf("removed '%s'\n", name);
}

// Command table.
static struct {
    const char *name;
    void (*handler)(char *args);
} commands[] = {
    {"help", commandHelp},
    {"probe", commandProbe},
    {"bootrom", commandBootrom},
    {"provision", commandProvision},
    {"list", commandList},
    {"add", commandAdd},
    {"get", commandGet},
    {"dump", commandDump},
    {"rm", commandRm},
};

static void dispatch(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }
    char *space = line;
    while (*space && (*space != ' ')) {
        space++;
    }
    if (*space) {
        *space++ = '\0';
    }
    const char *cmd = line;
    for (size_t i = 0; i < (sizeof(commands) / sizeof(commands[0])); i++) {
        if (0 == strcmp(cmd, commands[i].name)) {
            commands[i].handler(space);
            return;
        }
    }
    printf("Unknown command '%s'. Try 'help'.\n", cmd);
}

int main(void)
{
    stdio_init_all();

    printf("== OPTIGA Trust M vault (PIN-gated password store) ==\n\n");

    // Startup contract: chip present + master provisioned. This app does NOT
    // auto-provision — `provision` is a deliberate, one-time action.
    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK == rc) {
        s_chip_present = true;
        s_master_provisioned = master_present();
        printf("OPTIGA on I2C0 GP20/GP21 (application open).\n");
        printf("master 0x%04X: %s\n\n", (unsigned)VAULT_MASTER_OID,
               s_master_provisioned ? "provisioned" : "EMPTY — run `provision`");
    } else {
        printf("OPTIGA init failed: %s — check I2C0 GP20/GP21 wiring.\n\n",
               pico_optiga_strerror(rc));
    }

    // Pico-side persistence (littlefs, flash @ 0x80000), shared with the
    // pico-atecc608 apps: /io_key.bin and the /v_<name> entry files survive
    // vault <-> other-app reflashes; only a full-flash erase wipes them.
    if (0 == pico_lfs_init(NULL)) {
        if (0 != pico_lfs_mount_existing()) {
            printf("littlefs: no formatted volume — formatting (one-time; can take a while)...\n");
            if (0 != pico_lfs_mount()) {
                printf("littlefs mount FAILED — entry persistence unavailable.\n");
            } else {
                printf("littlefs: fresh volume formatted.\n");
            }
        }
    } else {
        printf("littlefs init FAILED — entry persistence unavailable.\n");
    }
    printf("\n");

    printf("vault ready (demo) — type `help`. Note: input is echoed on this console.\n\n");

    char line[128];
    for (;;) {
        printf("vault> ");
        fflush(stdout);
        read_line(line, sizeof(line));
        dispatch(line);
    }
}
