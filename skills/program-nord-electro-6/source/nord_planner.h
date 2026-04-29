/*
 * nord_planner.h — Optimal move/swap sequence planner for Nord rearrangements.
 *
 * Header-only library (matches nord_addr.h / nord_names.h pattern). Pure
 * compute — no USB, no hardware.
 *
 * Given a "before" snapshot of slot→program-name and an "after" snapshot
 * with the same set of programs, computes the minimum sequence of `nord
 * move` and `nord swap` operations that transforms before into after.
 *
 * Algorithm:
 *   - Build a directed graph on slots: src_slot → dst_slot for each program
 *     whose before-slot ≠ after-slot.
 *   - Each node has in-degree ≤ 1 and out-degree ≤ 1, so the graph is a
 *     disjoint union of simple paths and simple cycles.
 *   - Each path is rooted at a slot empty in `after` (out-edge only) and
 *     terminates at a slot empty in `before` (in-edge only). Process the
 *     path in reverse so each move's destination is empty: `len-1` moves
 *     per path of `len` slots.
 *   - Each k-cycle is resolved with `k-1` swaps using the anchor pattern
 *     swap(P_0, P_1), swap(P_0, P_2), …, swap(P_0, P_{k-1}). Optimal under
 *     the {move, swap} op set (k+1 moves with a temp slot loses to k-1
 *     swaps for all k ≥ 2).
 *
 * Preconditions (else NordPlan.error is set and ops is empty):
 *   - Every program name in `before` also appears in `after` and vice-versa.
 *   - No duplicate names or slots within either snapshot.
 *
 * Programs that don't move (same slot in both states) are silently skipped.
 *
 * Include guard: NORD_PLANNER_H
 */

#ifndef NORD_PLANNER_H
#define NORD_PLANNER_H

#include "nord_addr.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define NORD_PLAN_NUM_BANKS    26
#define NORD_PLAN_SLOTS_PER_BANK 16
#define NORD_PLAN_NUM_SLOTS    (NORD_PLAN_NUM_BANKS * NORD_PLAN_SLOTS_PER_BANK)
#define NORD_PLAN_MAX_NAME_LEN 64

/* Tight upper bound on emitted ops:
 *   - paths contribute (path_edges) moves
 *   - cycles contribute (cycle_edges - 1) swaps
 *   - sum of all edges ≤ NORD_PLAN_NUM_SLOTS (one edge per moving program)
 * so total ops ≤ NORD_PLAN_NUM_SLOTS. The runtime guard exists as a defensive
 * check — exceeding this bound would indicate a planner bug. */
#define NORD_PLAN_MAX_OPS      NORD_PLAN_NUM_SLOTS

typedef enum {
    NORD_OP_MOVE,
    NORD_OP_SWAP,
} NordOpKind;

typedef struct {
    NordOpKind kind;
    BankSpec a;  /* MOVE: src    SWAP: slot a */
    BankSpec b;  /* MOVE: dst    SWAP: slot b */
} NordPlanOp;

typedef enum {
    NORD_PLAN_OK = 0,
    NORD_PLAN_ERR_DUPLICATE_NAME_BEFORE,
    NORD_PLAN_ERR_DUPLICATE_NAME_AFTER,
    NORD_PLAN_ERR_NAME_MISSING_IN_AFTER,
    NORD_PLAN_ERR_NAME_MISSING_IN_BEFORE,
    NORD_PLAN_ERR_TOO_MANY_OPS,
    NORD_PLAN_ERR_PARSE,
} NordPlanError;

/* slot-indexed snapshot. Empty slot = empty string. */
typedef struct {
    char names[NORD_PLAN_NUM_SLOTS][NORD_PLAN_MAX_NAME_LEN];
} NordState;

typedef struct {
    NordPlanOp ops[NORD_PLAN_MAX_OPS];
    int count;
    NordPlanError error;
    char errorDetail[256];
} NordPlan;

/* ─── State construction ─────────────────────────────────────────────── */

static inline void nord_state_init(NordState *s) {
    memset(s, 0, sizeof(*s));
}

static inline int nord_state_slot_idx(BankSpec slot) {
    return slot.bank * NORD_PLAN_SLOTS_PER_BANK + slot.slot;
}

static inline void nord_state_set(NordState *s, BankSpec slot, const char *name) {
    int idx = nord_state_slot_idx(slot);
    if (idx < 0 || idx >= NORD_PLAN_NUM_SLOTS) return;
    if (!name || !name[0]) {
        s->names[idx][0] = '\0';
    } else {
        strncpy(s->names[idx], name, NORD_PLAN_MAX_NAME_LEN - 1);
        s->names[idx][NORD_PLAN_MAX_NAME_LEN - 1] = '\0';
    }
}

static inline const char *nord_state_get(const NordState *s, BankSpec slot) {
    int idx = nord_state_slot_idx(slot);
    if (idx < 0 || idx >= NORD_PLAN_NUM_SLOTS) return "";
    return s->names[idx];
}

/* ─── Plan computation ──────────────────────────────────────────────── */

static inline void nord_plan_set_error(NordPlan *plan, NordPlanError err,
                                       const char *fmt, ...) {
    plan->error = err;
    plan->count = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(plan->errorDetail, sizeof(plan->errorDetail), fmt, ap);
    va_end(ap);
}

static inline int nord_plan_find_name(const NordState *s, const char *name,
                                      int skip_idx) {
    for (int i = 0; i < NORD_PLAN_NUM_SLOTS; i++) {
        if (i == skip_idx) continue;
        if (s->names[i][0] && strcmp(s->names[i], name) == 0) return i;
    }
    return -1;
}

static inline void nord_plan_compute(const NordState *before,
                                     const NordState *after,
                                     NordPlan *plan) {
    plan->count = 0;
    plan->error = NORD_PLAN_OK;
    plan->errorDetail[0] = '\0';

    /* 1. Reject duplicate names within each snapshot. */
    for (int i = 0; i < NORD_PLAN_NUM_SLOTS; i++) {
        if (!before->names[i][0]) continue;
        int dup = nord_plan_find_name(before, before->names[i], i);
        if (dup >= 0) {
            nord_plan_set_error(plan, NORD_PLAN_ERR_DUPLICATE_NAME_BEFORE,
                "Duplicate name \"%s\" in 'before' (appears at multiple slots)",
                before->names[i]);
            return;
        }
    }
    for (int i = 0; i < NORD_PLAN_NUM_SLOTS; i++) {
        if (!after->names[i][0]) continue;
        int dup = nord_plan_find_name(after, after->names[i], i);
        if (dup >= 0) {
            nord_plan_set_error(plan, NORD_PLAN_ERR_DUPLICATE_NAME_AFTER,
                "Duplicate name \"%s\" in 'after' (appears at multiple slots)",
                after->names[i]);
            return;
        }
    }

    /* 2. Build slot graph: for each name in `before`, find its `after` slot. */
    int next_slot[NORD_PLAN_NUM_SLOTS];
    int prev_slot[NORD_PLAN_NUM_SLOTS];
    for (int i = 0; i < NORD_PLAN_NUM_SLOTS; i++) {
        next_slot[i] = -1;
        prev_slot[i] = -1;
    }

    for (int src_idx = 0; src_idx < NORD_PLAN_NUM_SLOTS; src_idx++) {
        if (!before->names[src_idx][0]) continue;
        int dst_idx = nord_plan_find_name(after, before->names[src_idx], -1);
        if (dst_idx < 0) {
            nord_plan_set_error(plan, NORD_PLAN_ERR_NAME_MISSING_IN_AFTER,
                "Program \"%s\" exists in 'before' but not in 'after'",
                before->names[src_idx]);
            return;
        }
        if (dst_idx == src_idx) continue;  /* already in place */
        next_slot[src_idx] = dst_idx;
        prev_slot[dst_idx] = src_idx;
    }

    /* 3. Every name in `after` must also be in `before`. */
    for (int dst_idx = 0; dst_idx < NORD_PLAN_NUM_SLOTS; dst_idx++) {
        if (!after->names[dst_idx][0]) continue;
        int src_idx = nord_plan_find_name(before, after->names[dst_idx], -1);
        if (src_idx < 0) {
            nord_plan_set_error(plan, NORD_PLAN_ERR_NAME_MISSING_IN_BEFORE,
                "Program \"%s\" exists in 'after' but not in 'before'",
                after->names[dst_idx]);
            return;
        }
    }

    /* 4. Decompose into paths and cycles. */
    bool visited[NORD_PLAN_NUM_SLOTS];
    memset(visited, 0, sizeof(visited));

    /* Paths first: walk from any node with out-edge but no in-edge. */
    for (int s = 0; s < NORD_PLAN_NUM_SLOTS; s++) {
        if (visited[s]) continue;
        if (next_slot[s] == -1 || prev_slot[s] != -1) continue;

        int path[NORD_PLAN_NUM_SLOTS + 1];
        int len = 0;
        int cur = s;
        while (cur != -1 && len <= NORD_PLAN_NUM_SLOTS) {
            path[len++] = cur;
            visited[cur] = true;
            cur = next_slot[cur];
        }
        /* Emit moves in reverse: empty destination first, walk back. */
        for (int i = len - 2; i >= 0; i--) {
            if (plan->count >= NORD_PLAN_MAX_OPS) {
                nord_plan_set_error(plan, NORD_PLAN_ERR_TOO_MANY_OPS,
                    "Plan exceeds %d ops", NORD_PLAN_MAX_OPS);
                return;
            }
            NordPlanOp *op = &plan->ops[plan->count++];
            op->kind = NORD_OP_MOVE;
            op->a.bank = path[i] / NORD_PLAN_SLOTS_PER_BANK;
            op->a.slot = path[i] % NORD_PLAN_SLOTS_PER_BANK;
            op->b.bank = path[i + 1] / NORD_PLAN_SLOTS_PER_BANK;
            op->b.slot = path[i + 1] % NORD_PLAN_SLOTS_PER_BANK;
        }
    }

    /* Cycles: any unvisited node with an out-edge is in a cycle. */
    for (int s = 0; s < NORD_PLAN_NUM_SLOTS; s++) {
        if (visited[s]) continue;
        if (next_slot[s] == -1) continue;

        int cycle[NORD_PLAN_NUM_SLOTS];
        int len = 0;
        int cur = s;
        while (cur != -1 && !visited[cur] && len < NORD_PLAN_NUM_SLOTS) {
            cycle[len++] = cur;
            visited[cur] = true;
            cur = next_slot[cur];
        }
        /* Anchor swaps: swap(cycle[0], cycle[i]) for i = 1..len-1. */
        for (int i = 1; i < len; i++) {
            if (plan->count >= NORD_PLAN_MAX_OPS) {
                nord_plan_set_error(plan, NORD_PLAN_ERR_TOO_MANY_OPS,
                    "Plan exceeds %d ops", NORD_PLAN_MAX_OPS);
                return;
            }
            NordPlanOp *op = &plan->ops[plan->count++];
            op->kind = NORD_OP_SWAP;
            op->a.bank = cycle[0] / NORD_PLAN_SLOTS_PER_BANK;
            op->a.slot = cycle[0] % NORD_PLAN_SLOTS_PER_BANK;
            op->b.bank = cycle[i] / NORD_PLAN_SLOTS_PER_BANK;
            op->b.slot = cycle[i] % NORD_PLAN_SLOTS_PER_BANK;
        }
    }
}

/* ─── State file parsing ─────────────────────────────────────────────
 *
 * Each line is `ADDR  NAME` — one or more whitespace chars between them.
 * NAME runs to the end of the line (trimmed); surrounding double-quotes
 * are stripped if present, so output of `nord list` (`A:1:1  "Foo"`) is
 * directly usable as input. Lines beginning with `#` (after optional
 * whitespace) and blank lines are ignored.
 */

static inline bool nord_plan_parse_line(const char *line, BankSpec *spec,
                                        char *name, size_t name_sz,
                                        bool *is_blank) {
    *is_blank = false;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n' || *line == '\r' || *line == '#') {
        *is_blank = true;
        return true;
    }
    /* Read address token. */
    char addr[32];
    int i = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != '\n'
           && *line != '\r' && i < (int)sizeof(addr) - 1) {
        addr[i++] = *line++;
    }
    addr[i] = '\0';
    if (!parse_bank_spec(addr, spec)) return false;
    /* Skip whitespace between addr and name. */
    while (*line == ' ' || *line == '\t') line++;
    /* Copy rest of line into name. */
    int j = 0;
    while (*line && *line != '\n' && *line != '\r' && j < (int)name_sz - 1) {
        name[j++] = *line++;
    }
    name[j] = '\0';
    /* Trim trailing whitespace. */
    while (j > 0 && (name[j - 1] == ' ' || name[j - 1] == '\t')) {
        name[--j] = '\0';
    }
    /* Strip surrounding double-quotes (matches `nord list` output). */
    if (j >= 2 && name[0] == '"' && name[j - 1] == '"') {
        memmove(name, name + 1, j - 2);
        name[j - 2] = '\0';
        j -= 2;
    }
    return j > 0;
}

static inline bool nord_plan_load_state(FILE *f, NordState *state,
                                        char *errbuf, size_t errsz) {
    nord_state_init(state);
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        BankSpec spec;
        char name[NORD_PLAN_MAX_NAME_LEN];
        bool is_blank = false;
        if (!nord_plan_parse_line(line, &spec, name, sizeof(name), &is_blank)) {
            snprintf(errbuf, errsz,
                "Parse error on line %d: %s", lineno, line);
            return false;
        }
        if (is_blank) continue;
        int idx = nord_state_slot_idx(spec);
        if (state->names[idx][0]) {
            snprintf(errbuf, errsz,
                "Line %d: duplicate slot %c:%d:%d",
                lineno, 'A' + spec.bank,
                spec.slot / 4 + 1, spec.slot % 4 + 1);
            return false;
        }
        nord_state_set(state, spec, name);
    }
    return true;
}

#endif /* NORD_PLANNER_H */
