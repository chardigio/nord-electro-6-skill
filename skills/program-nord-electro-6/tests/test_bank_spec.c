/*
 * test_bank_spec.c — Unit tests for nord_addr.h (address parsing & formatting)
 *
 * Standalone test binary — no USB or hardware dependencies.
 *
 * Build:
 *   cd nord-electro-6-skill && make tests/test_bank_spec
 *
 * Run:
 *   cd nord-electro-6-skill && make test
 */

#include "nord_addr.h"
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

/* ─── parse_bank_spec: bank:page:prog format ──────── */

static void test_parse_bank_only(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A", &spec), "parse 'A' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 0, "default slot = 0");
    PASS("bank-only 'A' -> bank 0, slot 0");
}

static void test_parse_bank_only_lowercase(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("z", &spec), "parse 'z' succeeds");
    ASSERT_EQ_INT(spec.bank, 25, "bank Z = 25");
    ASSERT_EQ_INT(spec.slot, 0, "default slot = 0");
    PASS("lowercase 'z' -> bank 25, slot 0");
}

static void test_parse_page_prog_A11(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:1:1", &spec), "parse 'A:1:1' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 0, "page 1 prog 1 = slot 0");
    PASS("A:1:1 -> bank 0, slot 0");
}

static void test_parse_page_prog_A12(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:1:2", &spec), "parse 'A:1:2' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 1, "page 1 prog 2 = slot 1");
    PASS("A:1:2 -> bank 0, slot 1");
}

static void test_parse_page_prog_A13(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:1:3", &spec), "parse 'A:1:3' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 2, "page 1 prog 3 = slot 2");
    PASS("A:1:3 -> bank 0, slot 2");
}

static void test_parse_page_prog_A14(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:1:4", &spec), "parse 'A:1:4' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 3, "page 1 prog 4 = slot 3");
    PASS("A:1:4 -> bank 0, slot 3");
}

static void test_parse_page_prog_A21(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:2:1", &spec), "parse 'A:2:1' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 4, "page 2 prog 1 = slot 4");
    PASS("A:2:1 -> bank 0, slot 4");
}

static void test_parse_page_prog_A44(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:4:4", &spec), "parse 'A:4:4' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 15, "page 4 prog 4 = slot 15");
    PASS("A:4:4 -> bank 0, slot 15");
}

static void test_parse_page_prog_F23(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("F:2:3", &spec), "parse 'F:2:3' succeeds");
    ASSERT_EQ_INT(spec.bank, 5, "bank F = 5");
    ASSERT_EQ_INT(spec.slot, 6, "page 2 prog 3 = slot 6");
    PASS("F:2:3 -> bank 5, slot 6");
}

static void test_parse_page_prog_Z44(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("Z:4:4", &spec), "parse 'Z:4:4' succeeds");
    ASSERT_EQ_INT(spec.bank, 25, "bank Z = 25");
    ASSERT_EQ_INT(spec.slot, 15, "page 4 prog 4 = slot 15");
    PASS("Z:4:4 -> bank 25, slot 15");
}

/* ─── parse_bank_spec: legacy bank:slot format ──────── */

static void test_parse_legacy_slot1(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:1", &spec), "parse legacy 'A:1' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 0, "slot 1 = index 0");
    PASS("legacy A:1 -> bank 0, slot 0 (with deprecation warning)");
}

static void test_parse_legacy_slot5(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:5", &spec), "parse legacy 'A:5' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 4, "slot 5 = index 4");
    PASS("legacy A:5 -> bank 0, slot 4 (with deprecation warning)");
}

static void test_parse_legacy_slot16(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:16", &spec), "parse legacy 'A:16' succeeds");
    ASSERT_EQ_INT(spec.bank, 0, "bank A = 0");
    ASSERT_EQ_INT(spec.slot, 15, "slot 16 = index 15");
    PASS("legacy A:16 -> bank 0, slot 15 (with deprecation warning)");
}

/* ─── parse_bank_spec: invalid inputs ──────────────── */

static void test_parse_null(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec(NULL, &spec), "null returns false");
    PASS("null input rejected");
}

static void test_parse_empty(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("", &spec), "empty returns false");
    PASS("empty input rejected");
}

static void test_parse_digit(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("1", &spec), "digit returns false");
    PASS("digit-only input rejected");
}

static void test_parse_page_out_of_range(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("A:5:1", &spec), "page 5 rejected");
    PASS("page > 4 rejected");
}

static void test_parse_page_zero(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("A:0:1", &spec), "page 0 rejected");
    PASS("page 0 rejected");
}

static void test_parse_prog_out_of_range(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("A:1:5", &spec), "prog 5 rejected");
    PASS("prog > 4 rejected");
}

static void test_parse_prog_zero(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("A:1:0", &spec), "prog 0 rejected");
    PASS("prog 0 rejected");
}

static void test_parse_legacy_slot_zero(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("A:0", &spec), "legacy slot 0 rejected");
    PASS("legacy slot 0 rejected");
}

static void test_parse_legacy_slot_17(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("A:17", &spec), "legacy slot 17 rejected");
    PASS("legacy slot > 16 rejected");
}

static void test_parse_trailing_junk(void) {
    BankSpec spec;
    ASSERT(!parse_bank_spec("AB", &spec), "trailing letter rejected");
    PASS("trailing characters rejected");
}

/* ─── format_bank_spec ──────────────────────────────── */

static void test_format_slot0(void) {
    char buf[16];
    format_bank_spec(buf, sizeof(buf), 0, 0);
    ASSERT_EQ_STR(buf, "A:1:1", "slot 0 formats as A:1:1");
    PASS("format bank 0 slot 0 -> A:1:1");
}

static void test_format_slot1(void) {
    char buf[16];
    format_bank_spec(buf, sizeof(buf), 0, 1);
    ASSERT_EQ_STR(buf, "A:1:2", "slot 1 formats as A:1:2");
    PASS("format bank 0 slot 1 -> A:1:2");
}

static void test_format_slot4(void) {
    char buf[16];
    format_bank_spec(buf, sizeof(buf), 0, 4);
    ASSERT_EQ_STR(buf, "A:2:1", "slot 4 formats as A:2:1");
    PASS("format bank 0 slot 4 -> A:2:1");
}

static void test_format_slot15(void) {
    char buf[16];
    format_bank_spec(buf, sizeof(buf), 0, 15);
    ASSERT_EQ_STR(buf, "A:4:4", "slot 15 formats as A:4:4");
    PASS("format bank 0 slot 15 -> A:4:4");
}

static void test_format_bank5_slot6(void) {
    char buf[16];
    format_bank_spec(buf, sizeof(buf), 5, 6);
    ASSERT_EQ_STR(buf, "F:2:3", "bank 5 slot 6 formats as F:2:3");
    PASS("format bank 5 slot 6 -> F:2:3");
}

static void test_format_bank25_slot15(void) {
    char buf[16];
    format_bank_spec(buf, sizeof(buf), 25, 15);
    ASSERT_EQ_STR(buf, "Z:4:4", "bank 25 slot 15 formats as Z:4:4");
    PASS("format bank 25 slot 15 -> Z:4:4");
}

/* ─── Round-trip: parse then format ───────────────── */

static void test_roundtrip_A11(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("A:1:1", &spec), "parse succeeds");
    char buf[16];
    format_bank_spec(buf, sizeof(buf), spec.bank, spec.slot);
    ASSERT_EQ_STR(buf, "A:1:1", "roundtrip A:1:1");
    PASS("roundtrip A:1:1");
}

static void test_roundtrip_F23(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("F:2:3", &spec), "parse succeeds");
    char buf[16];
    format_bank_spec(buf, sizeof(buf), spec.bank, spec.slot);
    ASSERT_EQ_STR(buf, "F:2:3", "roundtrip F:2:3");
    PASS("roundtrip F:2:3");
}

static void test_roundtrip_Z44(void) {
    BankSpec spec;
    ASSERT(parse_bank_spec("Z:4:4", &spec), "parse succeeds");
    char buf[16];
    format_bank_spec(buf, sizeof(buf), spec.bank, spec.slot);
    ASSERT_EQ_STR(buf, "Z:4:4", "roundtrip Z:4:4");
    PASS("roundtrip Z:4:4");
}

/* ─── Main ──────────────────────────────────────────── */

int main(void) {
    printf("nord_addr.h — Unit Tests\n");

    header("parse_bank_spec: bank:page:prog format");
    test_parse_bank_only();
    test_parse_bank_only_lowercase();
    test_parse_page_prog_A11();
    test_parse_page_prog_A12();
    test_parse_page_prog_A13();
    test_parse_page_prog_A14();
    test_parse_page_prog_A21();
    test_parse_page_prog_A44();
    test_parse_page_prog_F23();
    test_parse_page_prog_Z44();

    header("parse_bank_spec: legacy bank:slot format");
    test_parse_legacy_slot1();
    test_parse_legacy_slot5();
    test_parse_legacy_slot16();

    header("parse_bank_spec: invalid inputs");
    test_parse_null();
    test_parse_empty();
    test_parse_digit();
    test_parse_page_out_of_range();
    test_parse_page_zero();
    test_parse_prog_out_of_range();
    test_parse_prog_zero();
    test_parse_legacy_slot_zero();
    test_parse_legacy_slot_17();
    test_parse_trailing_junk();

    header("format_bank_spec");
    test_format_slot0();
    test_format_slot1();
    test_format_slot4();
    test_format_slot15();
    test_format_bank5_slot6();
    test_format_bank25_slot15();

    header("round-trip: parse -> format");
    test_roundtrip_A11();
    test_roundtrip_F23();
    test_roundtrip_Z44();

    printf("\n");
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n  %d passed, %d failed\n", pass_count, fail_count);
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n");

    return fail_count > 0 ? 1 : 0;
}
