/*
 * nord_addr.h — Bank address parsing and formatting for Nord Electro 6
 *
 * Header-only library. Handles the bank:page:prog addressing scheme
 * that maps to the Nord's physical layout (4 pages x 4 programs per bank).
 *
 * Address formats:
 *   A         Bank A, slot 0 (page 1, prog 1)
 *   A:2:3     Bank A, page 2, prog 3 → slot = (2-1)*4 + (3-1) = 6
 *   A:5       Legacy bank:slot format (deprecated, prints warning)
 *
 * Include guard: NORD_ADDR_H
 */

#ifndef NORD_ADDR_H
#define NORD_ADDR_H

#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int bank;  // 0-25 (A-Z)
    int slot;  // 0-15
} BankSpec;

/*
 * Parse a bank spec string into bank index and slot index.
 *
 * Accepted formats:
 *   "A"       → bank 0, slot 0
 *   "A:2:3"   → bank 0, slot 6  (page 2, prog 3)
 *   "A:5"     → bank 0, slot 4  (legacy format, prints deprecation warning)
 *
 * Returns true on success, false on parse error (with message to stderr).
 */
static bool parse_bank_spec(const char *s, BankSpec *spec) {
    if (!s || !s[0]) return false;

    char letter = toupper(s[0]);
    if (letter < 'A' || letter > 'Z') return false;
    spec->bank = letter - 'A';
    spec->slot = 0;  // default

    if (s[1] == ':') {
        // Check for second colon → bank:page:prog format
        const char *second_colon = strchr(s + 2, ':');
        if (second_colon) {
            // New bank:page:prog format (e.g. A:2:3)
            int page = atoi(s + 2);
            int prog = atoi(second_colon + 1);
            if (page < 1 || page > 4) {
                fprintf(stderr, "Page must be 1-4, got %d (in '%s')\n", page, s);
                return false;
            }
            if (prog < 1 || prog > 4) {
                fprintf(stderr, "Prog must be 1-4, got %d (in '%s')\n", prog, s);
                return false;
            }
            spec->slot = (page - 1) * 4 + (prog - 1);
        } else {
            // Legacy bank:slot format (e.g. A:5) — accept with deprecation warning
            int slot = atoi(s + 2);
            if (slot < 1 || slot > 16) {
                fprintf(stderr, "Slot must be 1-16, got %d (in '%s')\n", slot, s);
                return false;
            }
            spec->slot = slot - 1;
            int page = spec->slot / 4 + 1;
            int prog = spec->slot % 4 + 1;
            fprintf(stderr, "Warning: '%s' uses deprecated bank:slot format. "
                    "Use %c:%d:%d instead.\n", s, letter, page, prog);
        }
    } else if (s[1] != '\0') {
        fprintf(stderr, "Invalid bank spec: %s (use A or A:1:3)\n", s);
        return false;
    }

    return true;
}

/*
 * Format a bank index and slot index as "A:P:Q" (bank:page:prog).
 */
static void format_bank_spec(char *buf, size_t sz, int bank, int slot) {
    int page = slot / 4 + 1;
    int prog = slot % 4 + 1;
    snprintf(buf, sz, "%c:%d:%d", 'A' + bank, page, prog);
}

#endif /* NORD_ADDR_H */
