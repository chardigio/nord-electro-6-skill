/*
 * test_planner.c — Unit tests for nord_planner.h
 *
 * Standalone test binary — no USB, no hardware.
 *
 * Build: cd nord-electro-6-skill && make tests/test_planner
 * Run:   cd nord-electro-6-skill && make test
 */

#include "nord_planner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass_count = 0;
static int fail_count = 0;

#define PASS(msg) do { printf("  PASS  %s\n", msg); pass_count++; } while (0)
#define FAIL(msg) do { printf("  FAIL  %s\n", msg); fail_count++; } while (0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while (0)
#define ASSERT_EQ_INT(a, b, msg) do { \
    if ((int)(a) != (int)(b)) { \
        printf("  FAIL  %s (expected %d, got %d)\n", msg, (int)(b), (int)(a)); \
        fail_count++; return; \
    } \
} while (0)

static void header(const char *title) {
    printf("\n");
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n  %s\n", title);
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n");
}

/* Helpers — set state by parsing an addr string for clarity. */
static void state_set(NordState *s, const char *addr, const char *name) {
    BankSpec spec;
    if (!parse_bank_spec(addr, &spec)) {
        fprintf(stderr, "BUG: bad addr %s in test\n", addr);
        exit(2);
    }
    nord_state_set(s, spec, name);
}

/* Simulate executing a plan against `before` state and assert it produces
 * `after`. Catches algorithmic bugs (wrong ordering, wrong direction).
 *
 * For MOVE: dst must be empty; src becomes empty.
 * For SWAP: both slots are exchanged (either may be empty).
 */
static bool simulate(const NordState *before, const NordPlan *plan,
                     NordState *out) {
    *out = *before;
    for (int i = 0; i < plan->count; i++) {
        const NordPlanOp *op = &plan->ops[i];
        int ai = nord_state_slot_idx(op->a);
        int bi = nord_state_slot_idx(op->b);
        if (op->kind == NORD_OP_MOVE) {
            if (out->names[bi][0]) {
                fprintf(stderr, "  simulate: op %d MOVE dst not empty\n", i);
                return false;
            }
            if (!out->names[ai][0]) {
                fprintf(stderr, "  simulate: op %d MOVE src empty\n", i);
                return false;
            }
            strcpy(out->names[bi], out->names[ai]);
            out->names[ai][0] = '\0';
        } else {
            char tmp[NORD_PLAN_MAX_NAME_LEN];
            strcpy(tmp, out->names[ai]);
            strcpy(out->names[ai], out->names[bi]);
            strcpy(out->names[bi], tmp);
        }
    }
    return true;
}

static bool states_equal(const NordState *a, const NordState *b) {
    for (int i = 0; i < NORD_PLAN_NUM_SLOTS; i++) {
        if (strcmp(a->names[i], b->names[i]) != 0) return false;
    }
    return true;
}

/* ─── Tests ─────────────────────────────────────────────────────── */

static NordState g_before, g_after, g_simulated;
static NordPlan  g_plan;

static void reset(void) {
    nord_state_init(&g_before);
    nord_state_init(&g_after);
    memset(&g_plan, 0, sizeof(g_plan));
}

static void test_noop_empty_states(void) {
    reset();
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 0, "0 ops");
    PASS("empty before+after -> 0 ops");
}

static void test_noop_same_state(void) {
    reset();
    state_set(&g_before, "A:1:1", "Grand Piano");
    state_set(&g_before, "B:2:3", "Rhodes Warm");
    state_set(&g_after, "A:1:1", "Grand Piano");
    state_set(&g_after, "B:2:3", "Rhodes Warm");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 0, "0 ops");
    PASS("identical before+after -> 0 ops");
}

static void test_two_cycle_uses_one_swap(void) {
    reset();
    state_set(&g_before, "A:1:1", "X");
    state_set(&g_before, "B:1:1", "Y");
    state_set(&g_after, "A:1:1", "Y");
    state_set(&g_after, "B:1:1", "X");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 1, "1 op");
    ASSERT_EQ_INT(g_plan.ops[0].kind, NORD_OP_SWAP, "is swap");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("2-cycle X<->Y -> 1 swap");
}

static void test_three_cycle_uses_two_swaps(void) {
    reset();
    /* A:1:1=X, A:1:2=Y, A:1:3=Z; want A:1:1=Z, A:1:2=X, A:1:3=Y
       So X: A:1:1->A:1:2, Y: A:1:2->A:1:3, Z: A:1:3->A:1:1. 3-cycle. */
    state_set(&g_before, "A:1:1", "X");
    state_set(&g_before, "A:1:2", "Y");
    state_set(&g_before, "A:1:3", "Z");
    state_set(&g_after, "A:1:1", "Z");
    state_set(&g_after, "A:1:2", "X");
    state_set(&g_after, "A:1:3", "Y");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 2, "2 ops");
    ASSERT_EQ_INT(g_plan.ops[0].kind, NORD_OP_SWAP, "op 0 is swap");
    ASSERT_EQ_INT(g_plan.ops[1].kind, NORD_OP_SWAP, "op 1 is swap");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("3-cycle X->Y->Z->X -> 2 swaps (correct end state)");
}

static void test_four_cycle_uses_three_swaps(void) {
    reset();
    state_set(&g_before, "A:1:1", "P0");
    state_set(&g_before, "A:1:2", "P1");
    state_set(&g_before, "A:1:3", "P2");
    state_set(&g_before, "A:1:4", "P3");
    /* σ(A:1:1)=A:1:2, σ(A:1:2)=A:1:3, σ(A:1:3)=A:1:4, σ(A:1:4)=A:1:1 */
    state_set(&g_after, "A:1:2", "P0");
    state_set(&g_after, "A:1:3", "P1");
    state_set(&g_after, "A:1:4", "P2");
    state_set(&g_after, "A:1:1", "P3");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 3, "3 ops");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("4-cycle -> 3 swaps");
}

static void test_simple_path_uses_moves(void) {
    reset();
    /* A:1:1=X, A:1:2=Y, A:1:3=(empty); want A:1:1=(empty), A:1:2=X, A:1:3=Y */
    state_set(&g_before, "A:1:1", "X");
    state_set(&g_before, "A:1:2", "Y");
    state_set(&g_after, "A:1:2", "X");
    state_set(&g_after, "A:1:3", "Y");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 2, "2 ops");
    ASSERT_EQ_INT(g_plan.ops[0].kind, NORD_OP_MOVE, "op 0 is move");
    ASSERT_EQ_INT(g_plan.ops[1].kind, NORD_OP_MOVE, "op 1 is move");
    /* First move must be Y -> A:1:3 (empty dst). */
    ASSERT_EQ_INT(g_plan.ops[0].a.slot, 1, "first move src slot = A:1:2");
    ASSERT_EQ_INT(g_plan.ops[0].b.slot, 2, "first move dst slot = A:1:3");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("path of 2 -> 2 moves in reverse order");
}

static void test_long_path_reverse_order(void) {
    reset();
    /* 4-program path shifting left by one: A1=A, A2=B, A3=C, A4=D
       -> (empty), A, B, C, D at A1=empty, A2=A, A3=B, A4=C, B1=D
       Wait let's do simpler: A1->A2, A2->A3, A3->A4 (A4 was empty before).
       Means before: A1=A, A2=B, A3=C, A4=empty.
       After: A1=empty, A2=A, A3=B, A4=C.
       Path is A1->A2->A3->A4. 3 moves. */
    state_set(&g_before, "A:1:1", "A");
    state_set(&g_before, "A:1:2", "B");
    state_set(&g_before, "A:1:3", "C");
    state_set(&g_after, "A:1:2", "A");
    state_set(&g_after, "A:1:3", "B");
    state_set(&g_after, "A:1:4", "C");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 3, "3 ops");
    /* All moves. */
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(g_plan.ops[i].kind, NORD_OP_MOVE, "move op");
    }
    /* First op must move into the originally-empty A:1:4 slot. */
    ASSERT_EQ_INT(g_plan.ops[0].b.slot, 3, "first move targets A:1:4");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("path of length 3 -> 3 moves, processed from empty end");
}

static void test_path_then_cycle(void) {
    reset();
    /* Path: A:1:1 X -> A:1:2 (empty in before; Y stays after path stuff?)
       Cycle: B:1:1 P <-> B:1:2 Q
       Before: A:1:1=X, B:1:1=P, B:1:2=Q
       After:  A:1:2=X, B:1:1=Q, B:1:2=P (A:1:1 ends empty) */
    state_set(&g_before, "A:1:1", "X");
    state_set(&g_before, "B:1:1", "P");
    state_set(&g_before, "B:1:2", "Q");
    state_set(&g_after, "A:1:2", "X");
    state_set(&g_after, "B:1:1", "Q");
    state_set(&g_after, "B:1:2", "P");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 2, "1 move + 1 swap = 2 ops");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("path + 2-cycle in same plan");
}

static void test_complex_setlist_simulation(void) {
    reset();
    /* Realistic-ish: 8 programs, several cycles + a path. */
    const char *names[] = {"S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8"};
    /* Before: S1..S8 at A:1:1..A:2:4. */
    const char *before_addrs[] = {"A:1:1","A:1:2","A:1:3","A:1:4",
                                   "A:2:1","A:2:2","A:2:3","A:2:4"};
    /* After: shuffled with one removed (S8 stays put), with B:1:1 as a path target. */
    const char *after_addrs[]  = {"A:2:4","A:1:1","A:2:1","A:1:3",
                                   "A:1:2","B:1:1","A:1:4","A:2:3"};
    for (int i = 0; i < 8; i++) {
        state_set(&g_before, before_addrs[i], names[i]);
        state_set(&g_after, after_addrs[i], names[i]);
    }
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT(g_plan.count > 0, "produces some ops");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    printf("    (complex plan size: %d ops)\n", g_plan.count);
    PASS("complex 8-program shuffle -> simulated correctly");
}

static void test_disjoint_cycles(void) {
    reset();
    /* Two independent 2-cycles: A:1:1 <-> A:1:2, B:1:1 <-> B:1:2 */
    state_set(&g_before, "A:1:1", "P");
    state_set(&g_before, "A:1:2", "Q");
    state_set(&g_before, "B:1:1", "R");
    state_set(&g_before, "B:1:2", "S");
    state_set(&g_after, "A:1:1", "Q");
    state_set(&g_after, "A:1:2", "P");
    state_set(&g_after, "B:1:1", "S");
    state_set(&g_after, "B:1:2", "R");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    ASSERT_EQ_INT(g_plan.count, 2, "2 swaps for 2 disjoint 2-cycles");
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("two disjoint 2-cycles -> 2 swaps");
}

static void test_many_moves_long_path(void) {
    reset();
    /* All 16 slots in bank A filled in before, shifted to bank B in after. */
    char nm[16][8];
    for (int i = 0; i < 16; i++) {
        snprintf(nm[i], sizeof(nm[i]), "P%d", i);
        BankSpec a = {.bank = 0, .slot = i};
        BankSpec b = {.bank = 1, .slot = i};
        nord_state_set(&g_before, a, nm[i]);
        nord_state_set(&g_after, b, nm[i]);
    }
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_OK, "no error");
    /* 16 disjoint paths of length 2 (A:i -> B:i) -> 16 moves. */
    ASSERT_EQ_INT(g_plan.count, 16, "16 moves");
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ_INT(g_plan.ops[i].kind, NORD_OP_MOVE, "move op");
    }
    ASSERT(simulate(&g_before, &g_plan, &g_simulated), "simulate ok");
    ASSERT(states_equal(&g_simulated, &g_after), "simulated == after");
    PASS("16 parallel A->B moves -> 16 moves");
}

/* ─── Validation tests ─────────────────────────────────────────────── */

static void test_error_name_only_in_before(void) {
    reset();
    state_set(&g_before, "A:1:1", "Ghost");
    state_set(&g_before, "A:1:2", "Stays");
    state_set(&g_after, "A:1:2", "Stays");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_ERR_NAME_MISSING_IN_AFTER, "missing-after error");
    PASS("name only in before -> NAME_MISSING_IN_AFTER");
}

static void test_error_name_only_in_after(void) {
    reset();
    state_set(&g_before, "A:1:1", "Stays");
    state_set(&g_after, "A:1:1", "Stays");
    state_set(&g_after, "A:1:2", "NewProgram");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_ERR_NAME_MISSING_IN_BEFORE, "missing-before error");
    PASS("name only in after -> NAME_MISSING_IN_BEFORE");
}

static void test_error_duplicate_name_before(void) {
    reset();
    state_set(&g_before, "A:1:1", "Dup");
    state_set(&g_before, "A:1:2", "Dup");
    state_set(&g_after, "A:1:1", "Dup");
    state_set(&g_after, "A:1:2", "Dup");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_ERR_DUPLICATE_NAME_BEFORE, "dup-before error");
    PASS("duplicate name in before -> DUPLICATE_NAME_BEFORE");
}

static void test_error_duplicate_name_after(void) {
    reset();
    state_set(&g_before, "A:1:1", "X");
    state_set(&g_before, "A:1:2", "Y");
    state_set(&g_after, "A:1:1", "X");
    state_set(&g_after, "A:1:2", "X");
    nord_plan_compute(&g_before, &g_after, &g_plan);
    ASSERT_EQ_INT(g_plan.error, NORD_PLAN_ERR_DUPLICATE_NAME_AFTER, "dup-after error");
    PASS("duplicate name in after -> DUPLICATE_NAME_AFTER");
}

/* ─── Parser tests ─────────────────────────────────────────────────── */

static void test_parse_basic_line(void) {
    BankSpec spec;
    char name[NORD_PLAN_MAX_NAME_LEN];
    bool blank;
    bool ok = nord_plan_parse_line("A:1:1  Grand Piano\n", &spec, name, sizeof(name), &blank);
    ASSERT(ok, "parse ok");
    ASSERT(!blank, "not blank");
    ASSERT_EQ_INT(spec.bank, 0, "bank A");
    ASSERT_EQ_INT(spec.slot, 0, "slot 0");
    ASSERT(strcmp(name, "Grand Piano") == 0, "name correct");
    PASS("parse basic 'A:1:1  Grand Piano'");
}

static void test_parse_rejects_gt_marker(void) {
    BankSpec spec;
    char name[NORD_PLAN_MAX_NAME_LEN];
    bool blank;
    bool ok = nord_plan_parse_line("  > A:2:3  \"Rhodes Warm\"\n", &spec, name, sizeof(name), &blank);
    /* '>' isn't a valid bank letter; `nord list` markers must be stripped
       upstream. Parser must reject and not silently accept. */
    ASSERT(!ok, "leading '>' marker -> parse failure");
    PASS("parse rejects '>' marker from raw `nord list` output");
}

static void test_parse_empty_name(void) {
    BankSpec spec;
    char name[NORD_PLAN_MAX_NAME_LEN];
    bool blank;
    /* Address with no name — should fail (not silently treat as blank). */
    bool ok = nord_plan_parse_line("A:1:1\n", &spec, name, sizeof(name), &blank);
    ASSERT(!ok, "addr-only line returns false");
    ASSERT(!blank, "addr-only is not blank");
    /* Trailing whitespace after addr but still no name. */
    ok = nord_plan_parse_line("A:1:1     \n", &spec, name, sizeof(name), &blank);
    ASSERT(!ok, "addr + trailing whitespace returns false");
    PASS("parse rejects address with empty name");
}

static void test_parse_quoted_name(void) {
    BankSpec spec;
    char name[NORD_PLAN_MAX_NAME_LEN];
    bool blank;
    bool ok = nord_plan_parse_line("F:2:1  \"Default Wurli\"\n", &spec, name, sizeof(name), &blank);
    ASSERT(ok, "parse ok");
    ASSERT(strcmp(name, "Default Wurli") == 0, "quotes stripped");
    PASS("parse strips double-quotes");
}

static void test_parse_blank_and_comment(void) {
    BankSpec spec;
    char name[NORD_PLAN_MAX_NAME_LEN];
    bool blank;
    bool ok = nord_plan_parse_line("\n", &spec, name, sizeof(name), &blank);
    ASSERT(ok, "blank ok");
    ASSERT(blank, "is blank");
    ok = nord_plan_parse_line("# this is a comment\n", &spec, name, sizeof(name), &blank);
    ASSERT(ok, "comment ok");
    ASSERT(blank, "comment treated as blank");
    ok = nord_plan_parse_line("  # indented comment\n", &spec, name, sizeof(name), &blank);
    ASSERT(ok, "indented comment ok");
    ASSERT(blank, "indented comment treated as blank");
    PASS("blank and # comment lines treated as blank");
}

static void test_parse_state_file(void) {
    /* Round-trip via tmpfile. */
    FILE *f = tmpfile();
    if (!f) { FAIL("tmpfile failed"); return; }
    fputs("# header comment\n", f);
    fputs("A:1:1  Grand Piano\n", f);
    fputs("\n", f);
    fputs("B:2:3  \"Rhodes Warm\"\n", f);
    fputs("Z:4:4  Last\n", f);
    rewind(f);

    NordState s;
    char errbuf[256] = {0};
    bool ok = nord_plan_load_state(f, &s, errbuf, sizeof(errbuf));
    fclose(f);
    if (!ok) {
        printf("    load error: %s\n", errbuf);
        FAIL("load_state");
        return;
    }
    BankSpec a; parse_bank_spec("A:1:1", &a);
    BankSpec b; parse_bank_spec("B:2:3", &b);
    BankSpec z; parse_bank_spec("Z:4:4", &z);
    ASSERT(strcmp(nord_state_get(&s, a), "Grand Piano") == 0, "A:1:1 -> Grand Piano");
    ASSERT(strcmp(nord_state_get(&s, b), "Rhodes Warm") == 0, "B:2:3 -> Rhodes Warm");
    ASSERT(strcmp(nord_state_get(&s, z), "Last") == 0, "Z:4:4 -> Last");
    PASS("round-trip through nord_plan_load_state");
}

/* ─── Main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("nord_planner.h — Unit Tests\n");

    header("plan computation: noop");
    test_noop_empty_states();
    test_noop_same_state();

    header("plan computation: cycles");
    test_two_cycle_uses_one_swap();
    test_three_cycle_uses_two_swaps();
    test_four_cycle_uses_three_swaps();

    header("plan computation: paths");
    test_simple_path_uses_moves();
    test_long_path_reverse_order();

    header("plan computation: mixed");
    test_path_then_cycle();
    test_disjoint_cycles();
    test_many_moves_long_path();
    test_complex_setlist_simulation();

    header("validation errors");
    test_error_name_only_in_before();
    test_error_name_only_in_after();
    test_error_duplicate_name_before();
    test_error_duplicate_name_after();

    header("state file parsing");
    test_parse_basic_line();
    test_parse_rejects_gt_marker();
    test_parse_empty_name();
    test_parse_quoted_name();
    test_parse_blank_and_comment();
    test_parse_state_file();

    printf("\n");
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n  %d passed, %d failed\n", pass_count, fail_count);
    for (int i = 0; i < 50; i++) printf("=");
    printf("\n");

    return fail_count > 0 ? 1 : 0;
}
