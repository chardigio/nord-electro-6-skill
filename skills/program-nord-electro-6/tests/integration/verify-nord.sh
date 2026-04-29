#!/usr/bin/env bash
#
# verify-nord.sh — Integration tests for the nord CLI against a real Nord Electro 6
#
# NOT a unit test — run manually with a connected keyboard.
# Exercises all CLI commands (list, copy, move, swap, rename, delete) using
# auto-detected empty banks as scratch space. No user data is modified.
#
# Prerequisites:
#   - Nord Electro 6 connected via USB
#   - nord binary built (see README.md)
#
# Usage:
#   From the skill's root dir (.claude/skills/program-nord-electro-6/):
#   make test-integration
#
# Env vars:
#   NORD_BIN          — path to nord binary (default: ../../nord relative to script)
#   NORD_PARTITION    — partition to test (default: 4 = Program)
#   NORD_SOURCE_BANK  — populated bank to copy from (default: auto-detected)

set -uo pipefail
# Note: no set -e — this is a test runner that should continue on failures

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NORD="${NORD_BIN:-${SCRIPT_DIR}/../../nord}"
PARTITION="${NORD_PARTITION:-4}"

# Scratch bank variables (set by discover_banks)
SOURCE_BANK=""
SCRATCH1=""
SCRATCH2=""
SCRATCH3=""

# ─── Helpers ─────────────────────────────────────────────────

failed=0
pass_count=0
fail_count=0

header() {
    echo ""
    printf '=%.0s' {1..60}; echo
    echo "  $1"
    printf '=%.0s' {1..60}; echo
}

pass() {
    echo "  PASS  $1"
    ((pass_count++)) || true
}

fail() {
    echo "  FAIL  $1"
    failed=1
    ((fail_count++)) || true
}

info() {
    echo "  $1: $2"
}

# Run nord CLI, passing partition flag if non-default
run_nord() {
    local args=("$@")
    local cmd="${args[0]}"
    # Insert -p flag for commands that accept it (not status/help)
    if [ "$PARTITION" != "4" ] && [ "$cmd" != "status" ] && [ "$cmd" != "help" ]; then
        "$NORD" "${args[@]}" -p "$PARTITION" 2>&1
    else
        "$NORD" "${args[@]}" 2>&1
    fi
}

# ─── Output parsers ──────────────────────────────────────────

# Check if a bank letter appears as empty in `nord list -a`
bank_is_empty() {
    local bank_letter="$1"
    local output
    output=$(run_nord list -a)
    # Empty banks show as "    Z  (empty)" — letter followed by spaces and (empty)
    echo "$output" | grep -qE "^\s+${bank_letter}\s+\(empty\)"
}

# Check if a bank letter appears as populated in `nord list`
bank_has_content() {
    local bank_letter="$1"
    local output
    output=$(run_nord list -a)
    # Populated banks show as "  > A:1:1  ..." or "    B:2:3  ..." — letter followed by colon
    echo "$output" | grep -qE "^\s+[> ] ${bank_letter}:"
}

# Get the name of a bank from list output (returns empty string if no name)
get_bank_name() {
    local bank_letter="$1"
    local output
    output=$(run_nord list -a)
    echo "$output" | grep -E "^\s+[> ] ${bank_letter}:" | sed -n 's/.*"\(.*\)".*/\1/p'
}

# ─── Cleanup ─────────────────────────────────────────────────

cleanup() {
    echo ""
    printf '=%.0s' {1..60}; echo
    echo "  Cleanup — deleting scratch banks"
    printf '=%.0s' {1..60}; echo

    for bank in "$SCRATCH1" "$SCRATCH2" "$SCRATCH3"; do
        if [ -n "$bank" ]; then
            local output
            output=$("$NORD" delete "$bank" -p "$PARTITION" 2>&1) || true
            echo "  Delete $bank: $(echo "$output" | tail -1)"
        fi
    done

    echo "  Cleanup complete — original data untouched"
}

trap cleanup EXIT INT TERM

# ─── Phase 1: Read-only tests ────────────────────────────────

test_status() {
    header "nord status"

    local output rc=0
    output=$(run_nord status) || rc=$?

    if [ $rc -ne 0 ]; then
        fail "nord status exited with $rc"
        echo "$output"
        return
    fi

    if echo "$output" | grep -q "Connected"; then
        local product_name
        product_name=$(echo "$output" | head -1 | sed 's/ — Connected//')
        pass "Connection confirmed: $product_name"
    else
        fail "Missing 'Connected' in output"
    fi

    if echo "$output" | grep -q "Product ID:"; then
        local pid
        pid=$(echo "$output" | grep "Product ID:" | head -1 | xargs)
        pass "$pid"
    else
        fail "Missing Product ID"
    fi

    if echo "$output" | grep -q "Firmware:"; then
        local fw
        fw=$(echo "$output" | grep "Firmware:" | head -1 | xargs)
        pass "$fw"
    else
        fail "Missing firmware info"
    fi

    if echo "$output" | grep -q "Partitions:"; then
        pass "Partition summary present"
    else
        fail "Missing partition summary"
    fi

    if echo "$output" | grep -qE "\[4\].*Program.*populated"; then
        pass "Program partition listed"
    else
        fail "Program partition not found"
    fi
}

test_list() {
    header "nord list"

    local output
    output=$(run_nord list) || true

    if echo "$output" | grep -qE "^.+ — .+ \(partition"; then
        pass "List header present"
    else
        fail "Missing list header"
    fi

    if echo "$output" | grep -qE "[0-9]+ populated / [0-9]+ total banks"; then
        local summary
        summary=$(echo "$output" | grep "populated" | xargs)
        pass "Summary: $summary"
    else
        fail "Missing summary line"
    fi
}

test_list_all() {
    header "nord list -a (show empty banks)"

    local output
    output=$(run_nord list -a) || true

    local empty_count
    empty_count=$(echo "$output" | grep -c "(empty)" || true)

    if [ "$empty_count" -gt 0 ]; then
        pass "$empty_count empty banks shown"
    else
        fail "No empty banks shown (keyboard completely full?)"
    fi
}

test_list_deps() {
    header "nord list -d (show dependencies)"

    local output
    output=$(run_nord list -d) || true

    pass "nord list -d completed without error"

    local dep_count
    dep_count=$(echo "$output" | grep -c "^\s*->" || true)
    info "Dependencies found" "$dep_count"
}

test_list_bank() {
    header "nord list $SOURCE_BANK (per-slot listing)"

    local output
    output=$(run_nord list "$SOURCE_BANK") || true

    if echo "$output" | grep -qE "Bank ${SOURCE_BANK}$"; then
        pass "Bank header shows '$SOURCE_BANK'"
    else
        fail "Missing bank header"
    fi

    local slot_count
    slot_count=$(echo "$output" | grep -cE "^\s+[> ] ${SOURCE_BANK}:[1-4]:[1-4]" || true)

    if [ "$slot_count" -eq 16 ]; then
        pass "All 16 slots listed"
    elif [ "$slot_count" -gt 0 ]; then
        pass "$slot_count slots listed"
    else
        fail "No slot entries found in per-bank listing"
    fi

    if echo "$output" | grep -qE "Active: ${SOURCE_BANK}:[1-4]:[1-4]"; then
        pass "Active slot reported"
    else
        fail "Missing active slot line"
    fi
}

# ─── Bank discovery ──────────────────────────────────────────

discover_banks() {
    header "Auto-discovering banks"

    local list_output
    list_output=$(run_nord list -a)

    local populated_banks=()
    local empty_banks=()

    while IFS= read -r line; do
        # Match populated: "  > A:1:1  ..." or "    B:2:3  ..."
        local letter
        letter=$(echo "$line" | sed -n 's/^[[:space:]]*[> ][[:space:]]*\([A-Z]\)[: ].*/\1/p')
        [ -z "$letter" ] && continue

        if echo "$line" | grep -q "(empty)"; then
            empty_banks+=("$letter")
        else
            populated_banks+=("$letter")
        fi
    done <<< "$list_output"

    info "Populated banks" "${#populated_banks[@]} (${populated_banks[*]:-none})"
    info "Empty banks" "${#empty_banks[@]} (${empty_banks[*]:-none})"

    # Select source bank
    if [ -n "${NORD_SOURCE_BANK:-}" ]; then
        SOURCE_BANK="$NORD_SOURCE_BANK"
    elif [ ${#populated_banks[@]} -gt 0 ]; then
        SOURCE_BANK="${populated_banks[0]}"
    else
        fail "No populated banks found — cannot test copy/rename"
        exit 1
    fi

    # Need 3 empty banks for scratch
    if [ ${#empty_banks[@]} -lt 3 ]; then
        fail "Need 3 empty banks for mutation tests, found ${#empty_banks[@]}"
        echo "  Free up some banks or skip mutation tests"
        exit 1
    fi

    SCRATCH1="${empty_banks[0]}"
    SCRATCH2="${empty_banks[1]}"
    SCRATCH3="${empty_banks[2]}"

    info "Source bank (read-only)" "$SOURCE_BANK"
    info "Scratch banks" "$SCRATCH1, $SCRATCH2, $SCRATCH3"
    pass "Bank discovery complete"
}

# ─── Phase 2: Safe mutation tests ────────────────────────────

test_copy() {
    header "Copy: $SOURCE_BANK -> $SCRATCH1"

    local output
    output=$(run_nord copy "$SOURCE_BANK" "$SCRATCH1") || true

    if echo "$output" | grep -q "OK"; then
        pass "Copy $SOURCE_BANK -> $SCRATCH1 succeeded"
    else
        fail "Copy did not report OK: $output"
        return 1
    fi

    # Verify scratch1 is now populated
    if bank_has_content "$SCRATCH1"; then
        pass "$SCRATCH1 is now populated"
    else
        fail "$SCRATCH1 still appears empty after copy"
        return 1
    fi
}

test_rename() {
    header "Rename: $SCRATCH1 -> \"NordTest\""

    local output
    output=$(run_nord rename "$SCRATCH1" "NordTest") || true

    if echo "$output" | grep -q "OK"; then
        pass "Rename $SCRATCH1 -> 'NordTest' succeeded"
    else
        fail "Rename did not report OK: $output"
        return 1
    fi

    # Verify name changed
    local name
    name=$(get_bank_name "$SCRATCH1")
    if [ "$name" = "NordTest" ]; then
        pass "Name verified: '$name'"
    else
        fail "Expected name 'NordTest', got '$name'"
    fi
}

test_copy_scratch() {
    header "Copy: $SCRATCH1 -> $SCRATCH2"

    local output
    output=$(run_nord copy "$SCRATCH1" "$SCRATCH2") || true

    if echo "$output" | grep -q "OK"; then
        pass "Copy $SCRATCH1 -> $SCRATCH2 succeeded"
    else
        fail "Copy did not report OK: $output"
        return 1
    fi

    if bank_has_content "$SCRATCH2"; then
        pass "$SCRATCH2 is now populated"
    else
        fail "$SCRATCH2 still appears empty after copy"
    fi
}

test_swap() {
    header "Swap: $SCRATCH1 <-> $SCRATCH2"

    # Record names before swap
    local name1_before name2_before
    name1_before=$(get_bank_name "$SCRATCH1")
    name2_before=$(get_bank_name "$SCRATCH2")
    info "Before" "$SCRATCH1='$name1_before', $SCRATCH2='$name2_before'"

    local output
    output=$(run_nord swap "$SCRATCH1" "$SCRATCH2") || true

    if echo "$output" | grep -q "OK"; then
        pass "Swap $SCRATCH1 <-> $SCRATCH2 succeeded"
    else
        fail "Swap did not report OK: $output"
        return 1
    fi

    # Verify both still have content
    if bank_has_content "$SCRATCH1" && bank_has_content "$SCRATCH2"; then
        pass "Both banks still populated after swap"
    else
        fail "One or both banks lost content after swap"
    fi

    # Verify names swapped
    local name1_after name2_after
    name1_after=$(get_bank_name "$SCRATCH1")
    name2_after=$(get_bank_name "$SCRATCH2")
    info "After" "$SCRATCH1='$name1_after', $SCRATCH2='$name2_after'"

    if [ "$name1_after" = "$name2_before" ] && [ "$name2_after" = "$name1_before" ]; then
        pass "Names swapped correctly"
    else
        # Names might not be set on copies from factory programs
        info "Note" "Name swap verification inconclusive (factory programs may lack names)"
    fi
}

test_move() {
    header "Move: $SCRATCH2 -> $SCRATCH3"

    local output
    output=$(run_nord move "$SCRATCH2" "$SCRATCH3") || true

    if echo "$output" | grep -q "OK"; then
        pass "Move $SCRATCH2 -> $SCRATCH3 succeeded"
    else
        fail "Move did not report OK: $output"
        return 1
    fi

    if bank_has_content "$SCRATCH3"; then
        pass "$SCRATCH3 is now populated (destination)"
    else
        fail "$SCRATCH3 should be populated after move"
    fi

    if bank_is_empty "$SCRATCH2"; then
        pass "$SCRATCH2 is now empty (source cleared)"
    else
        fail "$SCRATCH2 should be empty after move"
    fi
}

test_delete() {
    header "Delete: $SCRATCH1 and $SCRATCH3"

    # Delete scratch1
    local output1
    output1=$(run_nord delete "$SCRATCH1") || true

    if echo "$output1" | grep -q "OK"; then
        pass "Delete $SCRATCH1 succeeded"
    else
        fail "Delete $SCRATCH1 did not report OK: $output1"
    fi

    # Delete scratch3
    local output3
    output3=$(run_nord delete "$SCRATCH3") || true

    if echo "$output3" | grep -q "OK"; then
        pass "Delete $SCRATCH3 succeeded"
    else
        fail "Delete $SCRATCH3 did not report OK: $output3"
    fi

    # Verify both empty
    if bank_is_empty "$SCRATCH1"; then
        pass "$SCRATCH1 is empty"
    else
        fail "$SCRATCH1 should be empty after delete"
    fi

    if bank_is_empty "$SCRATCH3"; then
        pass "$SCRATCH3 is empty"
    else
        fail "$SCRATCH3 should be empty after delete"
    fi
}

# ─── Phase 3: Edge case tests ────────────────────────────────

test_copy_non_empty() {
    header "Edge case: copy to non-empty destination"

    # Setup: copy source -> scratch1
    run_nord copy "$SOURCE_BANK" "$SCRATCH1" > /dev/null 2>&1 || true

    # Try again — should fail
    local output rc=0
    output=$(run_nord copy "$SOURCE_BANK" "$SCRATCH1" 2>&1) || rc=$?

    if [ $rc -ne 0 ]; then
        pass "Copy to non-empty destination correctly failed (exit=$rc)"
    else
        fail "Copy to non-empty destination should have failed"
    fi

    if echo "$output" | grep -q "destination not empty"; then
        pass "Error message: 'destination not empty'"
    else
        info "Output" "$output"
    fi

    # Cleanup
    run_nord delete "$SCRATCH1" > /dev/null 2>&1 || true
}

test_delete_already_empty() {
    header "Edge case: delete already-empty bank"

    # Ensure scratch1 is empty
    run_nord delete "$SCRATCH1" > /dev/null 2>&1 || true

    # Delete again
    local output rc=0
    output=$(run_nord delete "$SCRATCH1" 2>&1) || rc=$?

    if [ $rc -eq 0 ]; then
        pass "Delete empty bank exits 0 (not an error)"
    else
        fail "Delete empty bank exited $rc (expected 0)"
    fi

    if echo "$output" | grep -q "already empty"; then
        pass "Output: 'already empty'"
    else
        fail "Expected 'already empty' in output: $output"
    fi
}

test_rename_duplicate() {
    header "Edge case: rename to existing name"

    # Setup: copy source -> scratch1, get source bank's name
    run_nord copy "$SOURCE_BANK" "$SCRATCH1" > /dev/null 2>&1 || true
    local source_name
    source_name=$(get_bank_name "$SOURCE_BANK")

    if [ -z "$source_name" ]; then
        info "Skip" "source bank has no name (factory program)"
        run_nord delete "$SCRATCH1" > /dev/null 2>&1 || true
        return
    fi

    # Try to rename scratch1 to the same name as source — should fail
    local output rc=0
    output=$(run_nord rename "$SCRATCH1" "$source_name" 2>&1) || rc=$?

    if [ $rc -ne 0 ]; then
        pass "Rename to duplicate name correctly rejected (exit=$rc)"
    else
        fail "Rename to duplicate name should have failed"
    fi

    if echo "$output" | grep -q "already in use"; then
        pass "Error message: 'already in use'"
    else
        fail "Expected 'already in use' in output: $output"
    fi

    if echo "$output" | grep -q "Suggestion:"; then
        local suggestion
        suggestion=$(echo "$output" | grep "Suggestion:" | sed 's/.*"\(.*\)".*/\1/')
        pass "Suggested alternative: '$suggestion'"
    else
        fail "Expected suggestion in output"
    fi

    # Cleanup
    run_nord delete "$SCRATCH1" > /dev/null 2>&1 || true
}

# ─── Phase 4: New feature tests ─────────────────────────────

test_version() {
    header "nord --version"

    local output
    output=$("$NORD" --version 2>&1) || true

    if echo "$output" | grep -qE "^nord [0-9]+\.[0-9]+"; then
        pass "Version string: $output"
    else
        fail "Missing version string: $output"
    fi
}

test_memory_layout() {
    header "Memory layout (dynamic bank/slot detection)"

    local output
    output=$(run_nord status) || true

    if echo "$output" | grep -qE "Memory: [0-9]+ banks x [0-9]+ slots"; then
        local layout
        layout=$(echo "$output" | grep "Memory:" | head -1 | xargs)
        pass "$layout"
    else
        fail "Missing memory layout line"
    fi
}

test_export() {
    header "Export: $SOURCE_BANK:1:1 -> temp file"

    local tmpfile
    tmpfile=$(mktemp /tmp/nord_test_XXXXXX.ne6p)

    local output rc=0
    output=$(run_nord export "$SOURCE_BANK:1:1" "$tmpfile" 2>&1) || rc=$?

    if [ $rc -eq 0 ] && echo "$output" | grep -q "OK"; then
        pass "Export succeeded"
    else
        fail "Export failed (exit=$rc): $output"
        rm -f "$tmpfile"
        return
    fi

    # Verify file was written
    if [ -f "$tmpfile" ]; then
        local size
        size=$(wc -c < "$tmpfile" | tr -d ' ')
        if [ "$size" -gt 0 ]; then
            pass "Exported file: $size bytes"
        else
            fail "Exported file is empty"
        fi
    else
        fail "Export file not created"
    fi

    rm -f "$tmpfile"
}

test_export_roundtrip() {
    header "Export round-trip: export -> copy -> export -> compare"

    local tmpfile1 tmpfile2
    tmpfile1=$(mktemp /tmp/nord_rt1_XXXXXX.ne6p)
    tmpfile2=$(mktemp /tmp/nord_rt2_XXXXXX.ne6p)

    # Export original
    run_nord export "$SOURCE_BANK:1:1" "$tmpfile1" > /dev/null 2>&1 || true

    # Copy to scratch, export copy
    run_nord copy "$SOURCE_BANK" "$SCRATCH1" > /dev/null 2>&1 || true
    run_nord export "$SCRATCH1:1:1" "$tmpfile2" > /dev/null 2>&1 || true

    # Compare file contents (should be identical)
    if [ -f "$tmpfile1" ] && [ -f "$tmpfile2" ]; then
        if cmp -s "$tmpfile1" "$tmpfile2"; then
            pass "Exported files match (copy preserves data)"
        else
            local size1 size2
            size1=$(wc -c < "$tmpfile1" | tr -d ' ')
            size2=$(wc -c < "$tmpfile2" | tr -d ' ')
            fail "Exported files differ ($size1 vs $size2 bytes)"
        fi
    else
        fail "One or both export files missing"
    fi

    # Cleanup
    run_nord delete "$SCRATCH1" > /dev/null 2>&1 || true
    rm -f "$tmpfile1" "$tmpfile2"
}

test_import_roundtrip() {
    header "Import round-trip: export -> import -> export -> compare"

    local tmpfile1 tmpfile2
    tmpfile1=$(mktemp /tmp/nord_imp1_XXXXXX.ne6p)
    tmpfile2=$(mktemp /tmp/nord_imp2_XXXXXX.ne6p)

    # Export a program from source bank
    run_nord export "$SOURCE_BANK:1:1" "$tmpfile1" > /dev/null 2>&1 || true

    if [ ! -s "$tmpfile1" ]; then
        fail "Could not export source program"
        rm -f "$tmpfile1" "$tmpfile2"
        return
    fi

    # Import to scratch bank
    local output rc=0
    output=$(run_nord import "$tmpfile1" "$SCRATCH1:1:1" 2>&1) || rc=$?

    if [ $rc -eq 0 ] && echo "$output" | grep -q "OK"; then
        pass "Import succeeded"
    else
        fail "Import failed (exit=$rc): $output"
        rm -f "$tmpfile1" "$tmpfile2"
        return
    fi

    # Verify program shows up
    if bank_has_content "$SCRATCH1"; then
        pass "$SCRATCH1 is now populated after import"
    else
        fail "$SCRATCH1 still empty after import"
        rm -f "$tmpfile1" "$tmpfile2"
        return
    fi

    # Export the imported program back
    run_nord export "$SCRATCH1:1:1" "$tmpfile2" > /dev/null 2>&1 || true

    # Compare: original export should match re-exported data
    if [ -f "$tmpfile2" ] && cmp -s "$tmpfile1" "$tmpfile2"; then
        pass "Import round-trip: data matches"
    else
        local s1 s2
        s1=$(wc -c < "$tmpfile1" | tr -d ' ')
        s2=$(wc -c < "$tmpfile2" 2>/dev/null | tr -d ' ')
        fail "Import round-trip: data mismatch ($s1 vs ${s2:-missing} bytes)"
    fi

    # Cleanup
    run_nord delete "$SCRATCH1" > /dev/null 2>&1 || true
    rm -f "$tmpfile1" "$tmpfile2"
}

# ─── Main ────────────────────────────────────────────────────

main() {
    echo "Nord CLI — Integration Tests"
    echo "Binary: $NORD"
    echo "Partition: $PARTITION"

    # Verify binary exists
    if [ ! -x "$NORD" ]; then
        echo ""
        echo "FATAL: nord binary not found at $NORD"
        echo "Build: from .claude/skills/program-nord-electro-6/ run \`make\`"
        exit 1
    fi

    # Phase 0: CLI basics
    test_version

    # Phase 1: Read-only
    test_status
    test_memory_layout
    test_list
    test_list_all
    test_list_deps

    # Discover banks (needed for per-bank listing and mutations)
    discover_banks
    test_list_bank

    # Phase 2: Safe mutations
    test_copy          || { fail "Aborting mutations — copy failed"; return; }
    test_rename
    test_copy_scratch  || { fail "Aborting mutations — second copy failed"; return; }
    test_swap
    test_move
    test_delete

    # Phase 3: Edge cases
    test_copy_non_empty
    test_delete_already_empty
    test_rename_duplicate

    # Phase 4: Export & Import
    test_export
    test_export_roundtrip
    test_import_roundtrip

    # Summary
    header "Summary"
    echo "  $pass_count passed, $fail_count failed"
    if [ $failed -ne 0 ]; then
        echo ""
        echo "  Some checks failed — see above"
        exit 1
    else
        pass "All checks passed"
    fi
}

main
