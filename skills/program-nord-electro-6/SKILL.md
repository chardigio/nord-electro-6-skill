---
name: program-nord-electro-6
description: Manage program presets on a Nord Electro 6 keyboard over USB. Use only when the user names the Electro 6 ("electro 6", "E6", "my old Nord"). Charlie now plays a Nord Electro 7, so a plain "nord", "setlist", or "arrange programs" request goes to program-nord-electro-7.
---

# Nord Electro 6 Program Manager

Manage program presets on a Nord Electro 6 keyboard connected over USB using the `nord` CLI tool.

## Prerequisites

- **Hardware**: Nord Electro 6 connected via USB
- **Binary**: Compiled `nord` CLI in this skill's directory (`<skill-dir>/nord`)

All commands below assume you are `cd`'d into this skill's directory (the one this SKILL.md lives in). `cd` there once at the start and stay for the duration of the skill.

### Build the CLI

```bash
make
```

### Run Tests

```bash
# Unit tests (no hardware needed)
make test

# Integration tests (requires Nord connected via USB)
make test-integration
```

## CLI Commands

### List Programs

```bash
nord list                  # List populated banks (Program partition)
nord list -a               # Include empty banks
nord list -d               # Show sample dependencies
nord list -v               # Verbose (per-slot sizes, metadata)
nord list A                # Show all 16 slots in bank A
nord list -p 5             # List Live partition instead of Program
```

Output marks the active bank/slot with `>` and shows program names in quotes:

```
Nord Electro 6 — Program (partition 4)

  > A:1:1  "Grand Piano"
    B:2:3  "Rhodes Warm"
    C:1:1  "Organ Perc"

  3 populated / 26 total banks
  Active: Bank A
```

Per-bank view (`nord list A`) shows all 16 slots (4 pages x 4 programs):

```
Nord Electro 6 — Bank A

  > A:1:1  "Grand Piano"
    A:1:2  (empty)
    A:1:3  (empty)
    ...
    A:4:4  (empty)
  Active: A:1:1
```

### Status

```bash
nord status    # Firmware version, protocols, partition summary
```

### Copy

```bash
nord copy A F            # Copy A:1:1 -> F:1:1
nord copy A:1:3 F:2:1    # Copy bank A page 1 prog 3 -> bank F page 2 prog 1
```

- Fails if destination is not empty (status=4). Delete first or pick an empty slot.
- Firmware auto-assigns a unique name if the source name conflicts.

### Move

```bash
nord move A F            # Move A:1:1 -> F:1:1 (source becomes empty)
nord move A:1:3 F:2:1    # Move specific slot
```

### Swap

```bash
nord swap A F            # Swap A:1:1 <-> F:1:1
nord swap A:1:1 A:2:1    # Swap slots within same bank
```

### Rename

```bash
nord rename A "My Piano"         # Rename A:1:1
nord rename A:1:3 "Jazz Rhodes"  # Rename specific slot
```

- Names must be unique across all banks. The CLI checks before sending and suggests alternatives if a conflict is found.

### Delete

```bash
nord delete F            # Delete F:1:1
nord delete F:2:1        # Delete specific slot
```

- Deleting an already-empty slot returns OK (idempotent).

### Plan (offline rearrangement planner)

```bash
nord plan --before before.txt --after after.txt          # shell commands
nord plan --before before.txt --after after.txt --json   # JSON
```

Computes the **optimal sequence of `move` and `swap` ops** to transform a `before` slot snapshot into an `after` snapshot. Runs without the keyboard connected — pure compute.

**State file format**: one program per line, `ADDR  Name`. Lines beginning with `#` and blank lines are ignored. Names with spaces are fine; surrounding double-quotes (as emitted by `nord list`) are stripped.

```
# before.txt
A:1:1  Come Together
A:1:2  Superstition
A:1:3  Moondance
B:1:1  AintNoSunshine
```

**Default output** is one shell command per line, ready to pipe to `bash`:

```
nord swap A:1:1 A:1:3
nord swap A:1:2 B:1:1
```

**`--json`** emits a structured array: `[{"op":"swap","a":"A:1:1","b":"A:1:3"}, ...]`.

**Scope**: rearrangement only — the set of program names in `before` must equal the set in `after`. The planner errors if a name is in only one of them. Handle creates (copy default preset + rename) and deletes separately, before/after invoking the planner.

**How it works**: builds the slot permutation graph, decomposes it into simple paths and cycles, emits `len-1` moves per path (processed from the empty end backward) and `len-1` anchor-swaps per cycle. Optimal for the {move, swap} op set.

## Partitions

| ID | Name | Description |
|----|------|-------------|
| 4 | Program | User programs (default) — 26 banks x 16 slots |
| 5 | Live | Live performance programs — same layout |

Use `-p N` to target a different partition: `nord list -p 5`

## Memory Layout

- **26 banks** (A-Z), each with **16 program slots**
- Slots are addressed as `BANK:PAGE:PROG` — 4 pages x 4 programs
- Address format: `A` (defaults to A:1:1) or `A:2:3` (bank A, page 2, prog 3)
- This matches the physical keyboard layout and `nord list` output

## Setlist Workflow

To arrange programs for a setlist:

1. `nord list` — see current program layout
2. `nord list -d` — check sample dependencies (pianos, organs)
3. Copy/move programs into contiguous banks matching setlist order
4. Use `nord rename` to label programs by song name
5. Verify with `nord list` after changes

**Example — arrange for a 4-song set:**

```bash
# Song 1 needs "Grand Piano" (already at A:1:1) — skip
# Song 2 needs "Rhodes Warm" (at H:1:1) — copy to A:1:2
nord copy H:1:1 A:1:2

# Song 3 needs "Organ Perc" (at M:1:1) — copy to A:1:3
nord copy M:1:1 A:1:3

# Song 4 needs a variation of "Grand Piano" — copy and rename
nord copy A:1:1 A:1:4
nord rename A:1:4 "Piano Ballad"

# Verify
nord list
```

## Protocol Reference

See `PROTOCOL.md` (in this skill's directory) for full USB protocol documentation including message formats, CRC details, and partition structure.

## Source Files

All paths are relative to this skill's directory.

| File | Purpose |
|------|---------|
| `source/nord_cli.c` | CLI tool (commands, argument parsing) |
| `source/nord_usb.h` | USB protocol header (constants, types, API) |
| `source/nord_usb.c` | USB protocol implementation (connection, messages, file ops) |
| `source/nord_names.h` | Program name uniqueness logic (header-only) |
| `source/nord_planner.h` | Rearrangement planner — paths/cycles → moves/swaps (header-only) |
| `tests/test_name_uniqueness.c` | Unit tests for name uniqueness |
| `tests/test_planner.c` | Unit tests for the rearrangement planner |
| `PROTOCOL.md` | Reverse-engineered USB protocol documentation |
| `tests/integration/` | Integration test script (requires hardware) |
