/*
 * nord_names.h — Program name uniqueness logic (header-only)
 *
 * Pure functions for checking and enforcing unique program names.
 * No USB or hardware dependencies — testable standalone.
 */

#ifndef NORD_NAMES_H
#define NORD_NAMES_H

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#define NORD_NAME_MAX 255
#define NORD_NAME_MAX_LEN 16

/* Allowed characters: A-Z, a-z, 0-9, space, hyphen */
static inline bool nord_name_valid_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == ' ' || c == '-';
}

/*
 * Validate a program name for Nord firmware compatibility.
 * Returns -1 if valid, or the byte index of the first problem:
 *   - If index == strlen(name), the name is too long (> NORD_NAME_MAX_LEN).
 *   - Otherwise, name[index] is an invalid character.
 */
static inline int nord_name_validate(const char *name) {
    if (!name || !name[0]) return 0;
    int len = (int)strlen(name);
    for (int i = 0; i < len; i++) {
        if (!nord_name_valid_char(name[i]))
            return i;
    }
    if (len > NORD_NAME_MAX_LEN) return len;
    return -1;
}

/*
 * Check if `name` conflicts with any entry in `names`.
 * Returns the index of the first conflict, or -1 if unique.
 * `skip_index` is excluded from comparison (-1 to skip nothing).
 */
static inline int nord_name_conflicts(const char *name,
                                       const char *names[], int count,
                                       int skip_index) {
    if (!name || !name[0]) return -1;

    for (int i = 0; i < count; i++) {
        if (i == skip_index) continue;
        if (names[i] && strcmp(name, names[i]) == 0)
            return i;
    }
    return -1;
}

/*
 * Generate a unique name by appending " 2", " 3", etc.
 * Writes the result into `out` (up to `outSize` bytes including NUL).
 * Returns the suffix number used (0 = no change needed).
 *
 * If the base name + suffix would exceed outSize, the base is truncated
 * to make room for the suffix.
 */
static inline int nord_make_unique(const char *base,
                                    const char *names[], int count,
                                    int skip_index,
                                    char *out, int outSize) {
    if (outSize <= 0) return -1;

    /* Try the base name first */
    snprintf(out, outSize, "%s", base);
    if (nord_name_conflicts(out, names, count, skip_index) < 0)
        return 0;

    /* Append " 2", " 3", ... until unique */
    for (int suffix = 2; suffix <= 999; suffix++) {
        char sfx[8];
        snprintf(sfx, sizeof(sfx), " %d", suffix);
        int sfxLen = (int)strlen(sfx);

        int baseLen = (int)strlen(base);
        int maxBase = outSize - 1 - sfxLen;
        if (maxBase < 0) maxBase = 0;
        if (baseLen > maxBase) baseLen = maxBase;

        snprintf(out, outSize, "%.*s%s", baseLen, base, sfx);

        if (nord_name_conflicts(out, names, count, skip_index) < 0)
            return suffix;
    }

    /* Exhausted — shouldn't happen with 26 banks */
    snprintf(out, outSize, "%s", base);
    return -1;
}

#endif /* NORD_NAMES_H */
