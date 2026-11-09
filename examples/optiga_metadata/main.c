/*
 * optiga_metadata — Phase 5, Step 1: read-only metadata inventory
 *
 * Purpose: before we write anything to the chip (Step 2), find out exactly
 * where it stands. For a set of candidate data objects this reads and
 * decodes each object's TLV metadata:
 *
 *   - LcsO            the object's life cycle state (0x01 creation …
 *                     0x0F termination). Read-only here — we never write it.
 *   - Change / Read / Execute access conditions (the "locks")
 *   - max / used size
 *   - data type       (byte-string data vs. key / cert / secret / counter)
 *
 * It then prints a "safe scratch candidates" summary: objects whose
 * Change AC is open (ALW, or LcsO < X while the object is below X) AND
 * whose data type is plain byte-string data AND whose LcsO is creation.
 * Those are the slots Step 2 may write to without touching any lock or
 * life-cycle state.
 *
 * SAFETY — this example is strictly read-only. It calls no metadata-write
 * and no life-cycle API. The two irreversible hazards on this chip are:
 *
 *   1. Advancing a life cycle state (LcsG/LcsO) toward termination (0x0F)
 *      bricks the chip / slot. We only READ life cycle values.
 *   2. Writing an object's metadata to set its Change AC to NEV freezes
 *      that slot forever. The pico_optiga library exposes no metadata-write
 *      API at all, so this cannot happen through it.
 *
 * Writing a slot's *data* (Step 2) changes neither its LcsO nor its
 * metadata — it is safe and repeatable (bounded only by NVM endurance).
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

/* Metadata TLV tags (SRM, "Metadata expression"). */
#define TAG_META_START 0x20u  /* constructed TLV: start, then total length  */
#define TAG_LCSO       0xC0u  /* life cycle state of the object (1 byte)    */
#define TAG_MAX_SIZE   0xC4u  /* maximum size (1 byte)                      */
#define TAG_USED_SIZE  0xC5u  /* used size (1 byte)                         */
#define TAG_AC_CHANGE  0xD0u  /* Change access condition descriptor         */
#define TAG_AC_READ    0xD1u  /* Read access condition descriptor           */
#define TAG_AC_EXEC    0xD3u  /* Execute access condition descriptor        */
#define TAG_DATA_TYPE  0xE8u  /* data object type (1 byte)                  */

/* Life cycle states (SRM Table 2) — one direction only, irreversible. */
#define LCS_CREATION    0x01u
#define LCS_INIT        0x03u
#define LCS_OPERATIONAL 0x07u
#define LCS_TERMINATION 0x0Fu

/* Data object types we care about distinguishing (SRM). */
#define DTYPE_BYTE_STRING 0x00u  /* arbitrary data — the safe scratch kind  */
#define DTYPE_UP_COUNTER  0x01u

/* Access-condition classification. */
typedef enum {
    AC_ALW = 0,    /* always allowed          -> writable now       */
    AC_LCSO_LT,    /* allowed while LcsO < X  -> writable if below X */
    AC_NEV,        /* never                   -> locked             */
    AC_COMPLEX,    /* Conf()/Auto() combo     -> needs shielded conn */
} ac_kind_t;

typedef struct {
    ac_kind_t kind;
    uint8_t threshold;  /* the X in "LcsO < X" */
    char str[24];
} ac_t;

typedef struct {
    int      valid;      /* metadata read + parsed OK            */
    int      have_lcso;  uint8_t lcso;
    int      have_max;   uint8_t max_size;
    int      have_used;  uint8_t used_size;
    ac_t     change, read, exec;
    int      have_dtype; uint8_t dtype;
} meta_t;

/*
 * Decode one access-condition value (the bytes after tag+length) into an
 * ac_t. Known forms:
 *   0x00            -> ALW
 *   0xFF            -> NEV
 *   E1 FC XX        -> LcsO < XX
 * anything else (Conf()/Auto() combinations) -> COMPLEX, raw hex shown.
 */
static void decode_ac(const uint8_t *val, uint8_t len, ac_t *ac) {
    ac->threshold = 0;
    if ((len == 1u) && (val[0] == 0x00u)) {
        ac->kind = AC_ALW;
        snprintf(ac->str, sizeof(ac->str), "ALW");
    } else if ((len == 1u) && (val[0] == 0xFFu)) {
        ac->kind = AC_NEV;
        snprintf(ac->str, sizeof(ac->str), "NEV");
    } else if ((len == 3u) && (val[1] == 0xFCu)) {
        ac->kind = AC_LCSO_LT;
        ac->threshold = val[2];
        snprintf(ac->str, sizeof(ac->str), "LcsO<0x%02X", (unsigned)val[2]);
    } else {
        ac->kind = AC_COMPLEX;
        size_t off = 0;
        for (uint8_t i = 0; (i < len) && (off + 3u < sizeof(ac->str)); i++) {
            off += (size_t)snprintf(ac->str + off, sizeof(ac->str) - off,
                                    "%02X ", (unsigned)val[i]);
        }
        ac->str[sizeof(ac->str) - 1u] = '\0';
    }
}

/*
 * Parse a metadata TLV buffer into a meta_t. Layout: 0x20, total_len, then
 * a run of primitive TLVs (tag, len, value[len]). Returns 0 on success.
 */
static int parse_metadata(const uint8_t *buf, size_t len, meta_t *m) {
    memset(m, 0, sizeof(*m));
    if ((len < 2u) || (buf[0] != TAG_META_START)) {
        return -1;
    }
    uint8_t total = buf[1];
    if ((size_t)total + 2u > len) {
        return -1;
    }
    size_t off = 2u;
    size_t end = (size_t)total + 2u;
    while (off + 2u <= end) {
        uint8_t tag = buf[off];
        uint8_t vlen = buf[off + 1u];
        if (off + 2u + (size_t)vlen > end) {
            return -1;
        }
        const uint8_t *val = buf + off + 2u;
        switch (tag) {
        case TAG_LCSO:
            if (vlen >= 1u) { m->have_lcso = 1; m->lcso = val[0]; }
            break;
        case TAG_MAX_SIZE:
            if (vlen >= 1u) { m->have_max = 1; m->max_size = val[0]; }
            break;
        case TAG_USED_SIZE:
            if (vlen >= 1u) { m->have_used = 1; m->used_size = val[0]; }
            break;
        case TAG_AC_CHANGE: decode_ac(val, vlen, &m->change); break;
        case TAG_AC_READ:   decode_ac(val, vlen, &m->read);   break;
        case TAG_AC_EXEC:   decode_ac(val, vlen, &m->exec);   break;
        case TAG_DATA_TYPE:
            if (vlen >= 1u) { m->have_dtype = 1; m->dtype = val[0]; }
            break;
        default:
            break;  /* unknown tag: skip */
        }
        off += 2u + (size_t)vlen;
    }
    m->valid = 1;
    return 0;
}

static const char *lcs_name(uint8_t v) {
    switch (v) {
    case LCS_CREATION:    return "creation";
    case LCS_INIT:        return "init";
    case LCS_OPERATIONAL: return "operational";
    case LCS_TERMINATION: return "TERMINATION";
    default:              return "?";
    }
}

static const char *dtype_name(uint8_t v) {
    switch (v) {
    case DTYPE_BYTE_STRING: return "data";
    case DTYPE_UP_COUNTER:  return "counter";
    default:                return "other";
    }
}

/* Writable right now, without a shielded connection? */
static int ac_writable_now(const ac_t *ac, uint8_t lcso) {
    if (ac->kind == AC_ALW) {
        return 1;
    }
    if ((ac->kind == AC_LCSO_LT) && (lcso < ac->threshold)) {
        return 1;
    }
    return 0;
}

/*
 * Is this a safe scratch slot for Step 2?
 *   - life cycle is creation (data writes don't advance it, and it is
 *     writable via LcsO<op);
 *   - has capacity (max size > 0);
 *   - data type is generic: the 0xE8 tag is *absent* (a generic data slot)
 *     or explicitly byte-string. An explicit non-data type (key / cert /
 *     secret / counter) disqualifies.
 *   - Change AC is open now (ALW, or LcsO<X with the object below X).
 */
static int is_safe_scratch(const meta_t *m) {
    return m->valid
           && m->have_lcso && (m->lcso == LCS_CREATION)
           && m->have_max && (m->max_size > 0u)
           && (!m->have_dtype || (m->dtype == DTYPE_BYTE_STRING))
           && ac_writable_now(&m->change, m->lcso);
}

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", (unsigned)buf[i]);
    }
}

/* Candidate objects to inventory. The 0xF1Dx/0xF1Ex ranges are the
 * arbitrary-data (scratch) slots; the 0xE0Ex/0xE0Fx are commonly cited
 * open test slots; the rest are life-cycle / identity references. */
static const struct {
    uint16_t oid;
    const char *name;
} kCandidates[] = {
    { 0xE0C0u, "Global LcsG"   },
    { 0xE0C2u, "Coproc UID"    },
    { 0xF1C0u, "App LcsG"      },
    { 0xF1C1u, "Security Stat" },
    { 0xE0E8u, "E0E8"          },
    { 0xE0F2u, "E0F2"          },
    { 0xE0F3u, "E0F3"          },
    { 0xE0FDu, "E0FD"          },
    { 0xF1D0u, "ArbData3 #0"   },
    { 0xF1D1u, "ArbData3 #1"   },
    { 0xF1D2u, "ArbData3 #2"   },
    { 0xF1D3u, "ArbData3 #3"   },
    { 0xF1D4u, "F1D4"          },
    { 0xF1E0u, "ArbData2 #0"   },
    { 0xF1E1u, "ArbData2 #1"   },
};

#define N_CANDIDATES (sizeof(kCandidates) / sizeof(kCandidates[0]))

int main(void) {
    stdio_init_all();

    printf("== OPTIGA Trust M metadata inventory (READ-ONLY) ==\n\n");

    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK != rc) {
        printf("init: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("init: OK\n\n");

    /* Life cycle summary (read the 1-byte values directly). */
    uint8_t g = 0, a = 0;
    size_t n = 0;
    if (PICO_OPTIGA_OK == pico_optiga_read_data_obj(0xE0C0u, &g, 1u, &n)) {
        printf("Global life cycle  (0xE0C0): 0x%02X (%s)\n",
               (unsigned)g, lcs_name(g));
    }
    if (PICO_OPTIGA_OK == pico_optiga_read_data_obj(0xF1C0u, &a, 1u, &n)) {
        printf("Application life cycle (0xF1C0): 0x%02X (%s)\n",
               (unsigned)a, lcs_name(a));
    }
    printf("\n");

    /* Metadata table. */
    printf("%-6s  %-13s %-4s %-4s %-7s  %-13s %-13s %-9s\n",
           "OID", "LcsO", "Max", "Used", "Type", "Change", "Read", "Exec");
    printf("--------------------------------------------------------------------"
           "------------------\n");

    meta_t meta[N_CANDIDATES];
    uint8_t raw[256];

    int safe_count = 0;
    for (size_t i = 0; i < N_CANDIDATES; i++) {
        uint16_t oid = kCandidates[i].oid;
        n = 0;
        rc = pico_optiga_read_metadata(oid, raw, sizeof(raw), &n);
        if (PICO_OPTIGA_OK != rc) {
            printf("0x%04X  %-13s  (metadata read failed: %s)\n",
                   (unsigned)oid, kCandidates[i].name,
                   pico_optiga_strerror(rc));
            continue;
        }
        if (0 != parse_metadata(raw, n, &meta[i])) {
            printf("0x%04X  %-13s  (metadata parse failed, %u bytes)\n",
                   (unsigned)oid, kCandidates[i].name, (unsigned)n);
            continue;
        }

        const meta_t *m = &meta[i];
        printf("0x%04X  ", (unsigned)oid);
        printf("%-13s ", m->have_lcso ? lcs_name(m->lcso) : "-");
        printf("%-4u ", m->have_max ? (unsigned)m->max_size : 0u);
        printf("%-4u ", m->have_used ? (unsigned)m->used_size : 0u);
        printf("%-7s ", m->have_dtype ? dtype_name(m->dtype) : "-");
        printf("%-13s ", m->change.str[0] ? m->change.str : "-");
        printf("%-13s ", m->read.str[0] ? m->read.str : "-");
        printf("%-9s\n", m->exec.str[0] ? m->exec.str : "-");

        if (is_safe_scratch(m)) {
            safe_count++;
        }
    }

    printf("\n");

    /* Actionable summary for Step 2. */
    printf("Safe scratch candidates (LcsO=creation, capacity>0, generic/"
           "byte-string data, Change open):\n");
    if (safe_count == 0) {
        printf("  (none found — do NOT write; inspect the table above and "
               "confirm with Infineon before proceeding)\n");
    } else {
        for (size_t i = 0; i < N_CANDIDATES; i++) {
            const meta_t *m = &meta[i];
            if (is_safe_scratch(m)) {
                printf("  0x%04X  %-13s  (max %u B, used %u B, Change %s)\n",
                       (unsigned)kCandidates[i].oid, kCandidates[i].name,
                       m->have_max ? (unsigned)m->max_size : 0u,
                       m->have_used ? (unsigned)m->used_size : 0u,
                       m->change.str);
            }
        }
    }

    printf("\nREAD-ONLY: nothing on the chip was modified.\n");
    pico_optiga_deinit();
    printf("done\n");
    return 0;
}
