# Nord Electro 6 Skill

A Claude Code plugin for managing program presets on a [Nord Electro 6](https://www.nordkeyboards.com/products/nord-electro-6) keyboard over USB.

## Install

### As a Claude Code plugin (recommended)

```bash
claude plugin install nord-electro-6 --source github:chardigio/nord-electro-6-skill
```

### Manual install

Copy the skill to your Claude Code skills directory:

```bash
mkdir -p ~/.claude/skills/program-nord-electro-6
curl -sL https://raw.githubusercontent.com/chardigio/nord-electro-6-skill/main/skills/program-nord-electro-6/SKILL.md \
  -o ~/.claude/skills/program-nord-electro-6/SKILL.md
```

## What It Does

This plugin enables Claude to manage programs on your Nord Electro 6, including:

- Listing all programs across 26 banks (A-Z) with 16 slots each
- Copying, moving, and swapping programs between banks/slots
- Renaming programs with uniqueness validation
- Deleting programs
- Viewing sample dependencies and firmware info

## Requirements

- macOS (uses IOKit for USB communication)
- Nord Electro 6 connected via USB
- A C compiler (clang/gcc) to build the CLI

## Build

```bash
make
```

This compiles the `nord` CLI binary from C source in `source/`.

## Test

```bash
# Unit tests (no hardware needed)
make test

# Integration tests (requires Nord connected via USB)
make test-integration
```

## How It Works

The CLI communicates with the Nord Electro 6 over its vendor USB interface (not MIDI SysEx). The protocol was reverse-engineered from scratch — see [PROTOCOL.md](PROTOCOL.md) for the full documentation including message formats, CRC details, and partition structure.

## Privacy

All communication happens directly over USB between your Mac and the keyboard. No data is sent to external services.

## License

MIT
