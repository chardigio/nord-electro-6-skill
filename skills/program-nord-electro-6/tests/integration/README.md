# Integration Tests — Local Verification

Standalone scripts that exercise the `nord` CLI against a **real Nord Electro 6** connected over USB. They are _not_ automated tests and are never run in CI.

## Prerequisites

- **Hardware**: Nord Electro 6 connected via USB
- **Binary**: `nord` CLI compiled (see build command below)

### Build the CLI

From the skill's root directory (`.claude/skills/program-nord-electro-6/`):

```bash
make
```

## Usage

From the skill's root directory:

```bash
make test-integration
```

## What It Tests

| Phase | What | Details |
|-------|------|---------|
| **1. Read-only** | `status`, `list`, `list -a`, `list -d`, `list <bank>` | Connection, firmware, bank listing, dependencies, per-slot view |
| **2. Mutations** | `copy`, `rename`, `copy`, `swap`, `move`, `delete` | Full lifecycle using auto-detected empty banks as scratch |
| **3. Edge cases** | Copy to non-empty, delete empty bank | Error handling and idempotent operations |

## Safety Guarantees

- **No user data is modified.** The script auto-detects empty banks to use as scratch space.
- **Source bank is read-only.** It is only ever copied from, never written to.
- **Full cleanup on exit.** A trap handler deletes all scratch banks on normal exit, failure, or Ctrl-C.
- **Aborts if unsafe.** If fewer than 3 empty banks are available, the script exits before any mutations.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NORD_BIN` | `../../nord` (relative to script) | Path to the compiled `nord` binary |
| `NORD_PARTITION` | `4` | Partition to test against (4=Program, 5=Live) |
| `NORD_SOURCE_BANK` | (auto-detected) | Populated bank letter to use as copy source |

A non-zero exit code means something failed.
