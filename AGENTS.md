# AGENTS.md — Development Protocol for `ps2_mecha_tool`

> **Main Reference:** See `../AGENTS.md` for the comprehensive root-level project architecture, emulator harness details, and reverse-engineering findings.

---

## Quick Reference for `ps2_mecha_tool`

### Environment
```bash
export PS2DEV=/Users/imac/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/bin:$PATH
```

### Build
```bash
make clean && make
```

### Distribution Sync
```bash
cp ps2_mecha_tool.elf ../dist_spc970_release/
cp ps2_mecha_tool_stripped.elf ../dist_spc970_release/
cp ps2_mecha_tool.elf ../test_headless/test.elf
```

### Test Harness
```bash
cd ..
PLAY_MECHACON_MODE=v1 Play-/build/tools/AutoTest/autotest test_headless
```

### Core Architecture & Codebase Map
- `main.c`: UI menus (Main Menu: 5 options, Submenu: 6 options), command line parsing (`--auto`, `--v1`, `--dump`, `--info`, `--map`, `--probe`, `--backup`, `--flush`), initialization.
- `mecha.c`: Low-level SCMD communication (`sceCdApplySCmd`), Two-Lap Staging exploit, safe NVRAM backup and restoration, RAM mapping sweeps.
- `mecha.h`: Hardware definitions, worker addresses for v1, v2, v3, memory sizes (256 KiB vs 192 KiB).
- `verify.c` / `verify.h`: Hardware POST checksum verification algorithm (`0xFF5170`), dual-mirror table validation (`0xFFFB80` == `0xFFFB92`), 7 sections for v3, 9 sections for v2/v1.
- `ident_db.c` / `ident_db.h`: PS2 console model identification database, EMCS, serial decoding.
- `logger.c` / `logger.h`: In-memory ring buffer with file flushing to `DEBUG_LOG.TXT`.

### Key Rules
1. **Little-Endian:** SPC970 is little-endian. All words from SCMD 0x0A and staged ROM are `byte[0] = word & 0xFF`, `byte[1] = word >> 8`.
2. **Safety First:** Never trigger exploit without pre-captured 1024-byte NVRAM backup. Always restore on error.
3. **CI/CD:** Commits to `main` automatically build and update the `latest` pre-release on GitHub via `.github/workflows/build.yml`.
