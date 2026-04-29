# Program Nord Electro 6

A Claude Code skill for managing program presets on a Nord Electro 6 keyboard over USB.

## What It Does

This skill enables Claude to manage programs on your Nord Electro 6, including:

- Listing all programs across 26 banks (A-Z) with 16 slots each
- Copying, moving, and swapping programs between banks/slots
- Renaming programs with uniqueness validation
- Deleting programs
- Viewing sample dependencies and firmware info

## Requirements

- macOS with a Nord Electro 6 connected via USB
- The `nord` CLI binary compiled from source (see build instructions in SKILL.md)

## Example Uses

- "List all my Nord programs"
- "Copy the program in bank A to bank F"
- "Rename bank C to 'Jazz Rhodes'"
- "Show me what samples bank A depends on"
