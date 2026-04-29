/*
 * test_name_uniqueness.c — Unit tests for nord_names.h
 *
 * Standalone test binary — no USB or hardware dependencies.
 *
 * Build:
 *   cd nord-electro-6-skill && make tests/test_name_uniqueness
 *
 * Run:
 *   cd nord-electro-6-skill && make test
 */

#include "nord_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass_count = 0;
static int fail_count = 0;

#define PASS(msg) do { printf("  PASS  %s\n", msg); pass_count++; } while(0)
#define FAIL(msg) do { printf("  FAIL  %s\n", msg); fail_count++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)
#define ASSERT_EQ_INT(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("  FAIL  %s (expected %d, got %d)\n", msg, (b), (a)); \
        fail_count++; return; \
    } \
} while(0)
#define ASSERT_EQ_STR(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL  %s (expected \"%s\", got \"%s\")\n", msg, (b), (a)); \
        fail_count++; return; \
    } \
} while(0)

static void header(const char *title) {
    printf("\n");
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n  %s\n", title);
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n");
}

/* ─── nord_name_conflicts tests ─────────────────────── */

static void test_conflicts_no_match(void) {
    const char *names[] = {"Piano", "Organ", "Strings"};
    int idx = nord_name_conflicts("Brass", names, 3, -1);
    ASSERT_EQ_INT(idx, -1, "no conflict returns -1");
    PASS("no conflict returns -1");
}

static void test_conflicts_exact_match(void) {
    const char *names[] = {"Piano", "Organ", "Strings"};
    int idx = nord_name_conflicts("Organ", names, 3, -1);
    ASSERT_EQ_INT(idx, 1, "exact match returns index");
    PASS("exact match returns correct index");
}

static void test_conflicts_first_match(void) {
    const char *names[] = {"Piano", "Organ", "Piano"};
    int idx = nord_name_conflicts("Piano", names, 3, -1);
    ASSERT_EQ_INT(idx, 0, "first match returned");
    PASS("returns first matching index");
}

static void test_conflicts_skip_index(void) {
    const char *names[] = {"Piano", "Organ", "Strings"};
    int idx = nord_name_conflicts("Piano", names, 3, 0);
    ASSERT_EQ_INT(idx, -1, "skipped index 0");
    PASS("skip_index excludes self from comparison");
}

static void test_conflicts_skip_only_skips_one(void) {
    const char *names[] = {"Piano", "Organ", "Piano"};
    int idx = nord_name_conflicts("Piano", names, 3, 0);
    ASSERT_EQ_INT(idx, 2, "skip_index 0 still finds index 2");
    PASS("skip_index only excludes the specified index");
}

static void test_conflicts_empty_list(void) {
    int idx = nord_name_conflicts("Piano", NULL, 0, -1);
    ASSERT_EQ_INT(idx, -1, "empty list returns -1");
    PASS("empty name list returns -1");
}

static void test_conflicts_empty_name(void) {
    const char *names[] = {"Piano"};
    int idx = nord_name_conflicts("", names, 1, -1);
    ASSERT_EQ_INT(idx, -1, "empty name returns -1");
    PASS("empty name returns -1 (no conflict)");
}

static void test_conflicts_null_name(void) {
    const char *names[] = {"Piano"};
    int idx = nord_name_conflicts(NULL, names, 1, -1);
    ASSERT_EQ_INT(idx, -1, "null name returns -1");
    PASS("null name returns -1");
}

static void test_conflicts_case_sensitive(void) {
    const char *names[] = {"Piano", "PIANO", "piano"};
    int idx = nord_name_conflicts("Piano", names, 3, -1);
    ASSERT_EQ_INT(idx, 0, "case-sensitive match");

    idx = nord_name_conflicts("PIANO", names, 3, -1);
    ASSERT_EQ_INT(idx, 1, "case-sensitive match uppercase");
    PASS("name comparison is case-sensitive");
}

/* ─── nord_name_validate tests ─────────────────────── */

static void test_validate_simple_name(void) {
    ASSERT_EQ_INT(nord_name_validate("Piano"), -1, "simple name is valid");
    PASS("simple alphanumeric name is valid");
}

static void test_validate_with_spaces(void) {
    ASSERT_EQ_INT(nord_name_validate("Grand Piano"), -1, "spaces allowed");
    PASS("name with spaces is valid");
}

static void test_validate_with_hyphens(void) {
    ASSERT_EQ_INT(nord_name_validate("--Defaults--"), -1, "hyphens allowed");
    PASS("name with hyphens is valid");
}

static void test_validate_with_digits(void) {
    ASSERT_EQ_INT(nord_name_validate("Organ2 Low3"), -1, "digits allowed");
    PASS("name with digits is valid");
}

static void test_validate_max_length(void) {
    /* Exactly 16 chars — should be valid */
    ASSERT_EQ_INT(nord_name_validate("BeatItOnDownTheL"), -1, "16 chars valid");
    PASS("name at max length (16) is valid");
}

static void test_validate_too_long(void) {
    /* 17 chars — should fail, returning index 17 */
    int idx = nord_name_validate("BeatItOnDownTheLi");
    ASSERT_EQ_INT(idx, 17, "17 chars too long");
    PASS("name exceeding max length returns strlen");
}

static void test_validate_plus_rejected(void) {
    int idx = nord_name_validate("organ+piano low");
    ASSERT_EQ_INT(idx, 5, "plus at index 5");
    PASS("plus character is rejected");
}

static void test_validate_special_chars(void) {
    ASSERT_EQ_INT(nord_name_validate("Name!"), 4, "exclamation rejected");
    ASSERT_EQ_INT(nord_name_validate("A@B"), 1, "at-sign rejected");
    ASSERT_EQ_INT(nord_name_validate("A#B"), 1, "hash rejected");
    PASS("special characters are rejected at correct index");
}

static void test_validate_empty(void) {
    ASSERT_EQ_INT(nord_name_validate(""), 0, "empty string invalid");
    PASS("empty name returns 0");
}

static void test_validate_null(void) {
    ASSERT_EQ_INT(nord_name_validate(NULL), 0, "null invalid");
    PASS("null name returns 0");
}

/* ─── nord_make_unique tests ────────────────────────── */

static void test_unique_no_conflict(void) {
    const char *names[] = {"Piano", "Organ", "Strings"};
    char out[256];
    int suffix = nord_make_unique("Brass", names, 3, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 0, "no suffix needed");
    ASSERT_EQ_STR(out, "Brass", "name unchanged");
    PASS("no conflict: name unchanged, suffix=0");
}

static void test_unique_appends_2(void) {
    const char *names[] = {"Piano", "Organ", "Strings"};
    char out[256];
    int suffix = nord_make_unique("Piano", names, 3, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 2, "suffix=2");
    ASSERT_EQ_STR(out, "Piano 2", "appended \" 2\"");
    PASS("one conflict: appends \" 2\"");
}

static void test_unique_appends_3(void) {
    const char *names[] = {"Piano", "Piano 2", "Strings"};
    char out[256];
    int suffix = nord_make_unique("Piano", names, 3, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 3, "suffix=3");
    ASSERT_EQ_STR(out, "Piano 3", "appended \" 3\"");
    PASS("two conflicts: skips \" 2\", appends \" 3\"");
}

static void test_unique_gap_in_suffixes(void) {
    const char *names[] = {"Piano", "Piano 3"};
    char out[256];
    int suffix = nord_make_unique("Piano", names, 2, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 2, "suffix=2 (gap at 2)");
    ASSERT_EQ_STR(out, "Piano 2", "fills gap at \" 2\"");
    PASS("gap in suffixes: uses first available");
}

static void test_unique_skip_self(void) {
    const char *names[] = {"Piano", "Organ"};
    char out[256];
    int suffix = nord_make_unique("Piano", names, 2, 0, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 0, "no suffix when self skipped");
    ASSERT_EQ_STR(out, "Piano", "name unchanged");
    PASS("skip_index: renaming self to same name is OK");
}

static void test_unique_empty_list(void) {
    char out[256];
    int suffix = nord_make_unique("Piano", NULL, 0, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 0, "no suffix needed");
    ASSERT_EQ_STR(out, "Piano", "name unchanged");
    PASS("empty list: no conflict");
}

static void test_unique_truncates_for_suffix(void) {
    /* Buffer of 10 chars: "LongNa" + " 2" + NUL = 9 + NUL = 10 */
    const char *names[] = {"LongName"};
    char out[10];
    int suffix = nord_make_unique("LongName", names, 1, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 2, "suffix=2");
    /* "LongName" (8) + " 2" (2) = 10 chars, needs truncation to fit in 10 with NUL */
    ASSERT(strlen(out) < 10, "output fits in buffer");
    /* Should end with " 2" */
    char *sfx = strstr(out, " 2");
    ASSERT(sfx != NULL, "output contains \" 2\" suffix");
    PASS("long name truncated to fit suffix in buffer");
}

static void test_unique_many_conflicts(void) {
    /* Simulate 10 conflicts: "X", "X 2", "X 3", ..., "X 10" */
    const char *names[] = {
        "X", "X 2", "X 3", "X 4", "X 5",
        "X 6", "X 7", "X 8", "X 9", "X 10"
    };
    char out[256];
    int suffix = nord_make_unique("X", names, 10, -1, out, sizeof(out));
    ASSERT_EQ_INT(suffix, 11, "suffix=11");
    ASSERT_EQ_STR(out, "X 11", "appended \" 11\"");
    PASS("many conflicts: escalates to \" 11\"");
}

/* ─── Main ──────────────────────────────────────────── */

int main(void) {
    printf("nord_names.h — Unit Tests\n");

    header("nord_name_conflicts");
    test_conflicts_no_match();
    test_conflicts_exact_match();
    test_conflicts_first_match();
    test_conflicts_skip_index();
    test_conflicts_skip_only_skips_one();
    test_conflicts_empty_list();
    test_conflicts_empty_name();
    test_conflicts_null_name();
    test_conflicts_case_sensitive();

    header("nord_name_validate");
    test_validate_simple_name();
    test_validate_with_spaces();
    test_validate_with_hyphens();
    test_validate_with_digits();
    test_validate_max_length();
    test_validate_too_long();
    test_validate_plus_rejected();
    test_validate_special_chars();
    test_validate_empty();
    test_validate_null();

    header("nord_make_unique");
    test_unique_no_conflict();
    test_unique_appends_2();
    test_unique_appends_3();
    test_unique_gap_in_suffixes();
    test_unique_skip_self();
    test_unique_empty_list();
    test_unique_truncates_for_suffix();
    test_unique_many_conflicts();

    printf("\n");
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n  %d passed, %d failed\n", pass_count, fail_count);
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n");

    return fail_count > 0 ? 1 : 0;
}
