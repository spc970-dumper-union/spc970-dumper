# CLAUDE.md — Claude Code Guidelines for `ps2_mecha_tool`

Refer to [`AGENTS.md`](./AGENTS.md) and [`../AGENTS.md`](../AGENTS.md) for full architecture and hardware documentation.

## Commands
```bash
# Setup Environment
export PS2DEV=/Users/imac/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/bin:$PATH

# Build
make clean && make

# Sync & Test
cp ps2_mecha_tool.elf ../dist_spc970_release/
cp ps2_mecha_tool_stripped.elf ../dist_spc970_release/
cp ps2_mecha_tool.elf ../test_headless/test.elf
cd .. && PLAY_MECHACON_MODE=v1 Play-/build/tools/AutoTest/autotest test_headless
```

## Critical Invariants
- **Endianness:** SPC970 is little-endian. Never swap word byte ordering.
- **Safety:** Always backup NVRAM before exploit writes, and auto-restore if any error occurs.
- **Headless:** Do not remove CLI args in `main.c` (`--auto`, `--v1`, etc.).
- **CI Releases:** Pushing to `main` triggers GitHub Actions rolling pre-release `latest`.
