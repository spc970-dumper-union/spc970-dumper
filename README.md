# SPC970 MechaCon Dumper (`ps2_mecha_tool`)

[![Build & Release](https://github.com/spc970-dumper-union/spc970-dumper/actions/workflows/build.yml/badge.svg)](https://github.com/spc970-dumper-union/spc970-dumper/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-PlayStation%202-003791.svg)](https://github.com/ps2dev/ps2sdk)

A specialized, safety-focused PlayStation 2 utility built with PS2SDK to dump the internal mask ROM of Sony **SPC970** MechaCon microcontrollers (Siemens/Infineon C166 core derivatives) to USB storage.

Supports both **v2** (CXP102064, 256 KiB) and **v3** (CXP103049, 192 KiB) MechaCon revisions with automated layout detection, failsafe NVRAM restoration, and **100% POST hardware checksum verification**.

---

## Features

- **Multi-Revision ROM Dumper**:
  - **MechaCon v2 (CXP102064)**: Dumps complete 256 KiB ROM (`0xFC0000-0xFFFFFF`, 4 banks: FC, FD, FE, FF).
  - **MechaCon v3 (CXP103049)**: Dumps native 192 KiB active ROM (`0xFD0000-0xFFFFFF`, 3 banks: FD, FE, FF). Automatically omits unmapped Bank FC, saving 256 exploit staging cycles (25% faster, 25% less EEPROM write wear).
- **Failsafe Hardware Protection**:
  - Automatically captures a pristine 1,024-byte NVRAM backup to both memory and USB before running any exploit writes.
  - Pre-flight staging verification: validates target ROM entry signature (`0xE600`) before proceeding.
  - Immediate auto-restoration of NVRAM if exploit staging or MechaCon session fails.
  - Word-by-word read-back verification of restored NVRAM with zero mismatch tolerance.
- **POST Hardware Checksum Verification**:
  - Re-implements the exact hardware checksum calculation executed by the Sony MechaCon POST routine (`0xFF5170`).
  - Verifies the dual-mirror checksum table at `0xFFFB80` == `0xFFFB92`.
  - Section-by-section verification (9 sections for v2, 7 sections for v3) guaranteeing 100% bit-perfect dumps.
- **Hardware Telemetry & Identification**:
  - Queries Sony Model Name via `SCMD 0x17` (`SCPH-30000`, `SCPH-39001`, etc.).
  - Extracts Serial Number, EMCS, and 16-bit Model ID from NVRAM, decoded against the **PS2Ident** database.
  - Queries Real-Time Clock hardware via `SCMD 0x08`.
  - Queries Console ID (`SCMD 0x03-45`) and i.Link ID (`SCMD 0x12`).
- **MechaCon RAM Mapping & Subcommand Probe Mode** (`--map`):
  - Dumps live RAM regions (Region 0..3) via SCMD `0x40`/`0x41`.
  - Probes all 256 subcommands under SCMD `0x03` to discover undocumented diagnostic functions.
- **Automated / Headless CI Testing Mode** (`--auto`):
  - Fully scriptable execution without requiring gamepad interaction, ideal for automated test harnesses and emulator regression tests.

---

## Output Structure on USB (`mass:`)

All outputs are organized into a timestamped/serial-named directory:

```
mass:/MECHA_v<Major>.<Minor>_<Serial>/
├── ROM.BIN          (Bit-perfect SPC970 ROM image: 256 KiB for v2, 192 KiB for v3)
├── NVRAM.BIN        (1,024-byte factory EEPROM backup)
├── DUMP_INFO.TXT    (Hardware details, revision, region, and POST checksum breakdown)
└── DEBUG_LOG.TXT    (Complete execution trace, SCMD statuses, and verification logs)
```

When using Mapping Mode (`--map` / Menu [6]):
```
├── REGION0_RAM256.BIN
├── REGION1_RAM256.BIN
├── REGION2_RAM256.BIN
├── MECHA_MAP_REPORT.TXT
└── TARGET_SCMD_REPORT.TXT
```

---

## How the Exploit Works

The SPC970 MechaCon executes an internal background worker to service EEPROM write operations.
When opening OSD Configuration Region 2 (`SCMD 0x40`) for writing with a block count of `0`, the MechaCon's internal block counter underflows to `0xFF` on the first write (`SCMD 0x42`).

By streaming blocks past the valid 7-block buffer, the dumper safely overflows into adjacent MechaCon RAM holding the EEPROM worker task structure:
- **Block 11**: Overwrites the source pointer (`0x19F8`) with the target physical ROM address (`0xFC0000`..`0xFFFFFF`).
- **Block 12**: Overwrites the word count register (`0x1A0D`, 128 words / 256 bytes per chunk) and arm state.
- **Block 13**: Directs destination to NVRAM word address `0x0000`.
- **Trigger**: Armed with trigger bits (`0x03`), the MechaCon worker copies 256 bytes from internal ROM to NVRAM. The dumper then reads the staged ROM chunk via standard `SCMD 0x0A (ReadNvm)`.
- **Restoration**: After all chunks are dumped, the original NVRAM is rewritten and verified word-for-word.

---

## Controls

| Button | Action |
| :--- | :--- |
| **D-Pad Up / Down** | Navigate menu options |
| **CROSS (X)** | Select / Confirm action |
| **TRIANGLE** | Cancel / Return |

---

## Building from Source

### Option A: Using Docker (Recommended)

Requires [Docker](https://www.docker.com/):

```bash
docker run --rm -v "$(pwd):/src" -w /src ghcr.io/ps2homebrew/ps2homebrew:main make clean all
```

Or simply run the included build script:
```bash
./build.sh
```

### Option B: Native PS2DEV Environment

Set up your [ps2dev](https://github.com/ps2dev/ps2dev) toolchain:

```bash
export PS2DEV=/usr/local/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin:$PATH

make clean all
```

The build generates:
- `ps2_mecha_tool.elf` (Debug ELF with symbols)
- `ps2_mecha_tool_stripped.elf` (Stripped ELF for deployment)

---

## Command-Line Arguments

The ELF can be launched from host loaders or automated test runners:

- `host:test.elf --auto` : Automatically initiates Full Dump, validates POST checksums, restores NVRAM, and exits cleanly.
- `host:test.elf --map`  : Automatically dumps all valid RAM regions and probes SCMD 0x03 subcommands.

---

## License

Released under the [MIT License](LICENSE).
