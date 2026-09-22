#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <libcdvd.h>
#include <libpad.h>
#include <loadfile.h>
#include <ps2_joystick_driver.h>
#include <ps2_usb_driver.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <tamtypes.h>
#include <unistd.h>

#include "ident_db.h"
#include "logger.h"
#include "mecha.h"
#include "verify.h"

#define MECHA_TOOL_VERSION "v0.1.0-alpha"

static char padBuf[256] __attribute__((aligned(64)));
static u8 g_nvram_backup[NVRAM_SIZE_BYTES] = {0};
static int g_nvram_backed_up = 0;
static u8 g_rom_buffer[ROM_SIZE_BYTES] = {0};

static char g_romver[64] = "Unknown";
static char g_cdvd_model[32] = "Unknown";
static u8 g_model_name_valid = 0;

u8 g_mecha_ver[16] = {0};
static u8 g_dsp_ver = 0;
static u8 g_mecha_rtc[16] = {0};
static u8 g_rtc_valid = 0;

static u8 g_console_id[8] = {0};
static u8 g_console_id_valid = 0;
static u8 g_ilink_id[8] = {0};
static u8 g_ilink_id_valid = 0;

static u32 g_serial = 0;
static u8 g_emcs = 0;
static u16 g_model_id = 0;
static char g_dump_dir[160] = "mass:";
static int g_headless = 0;

static const char *get_region_name(u8 region) {
  switch (region) {
  case 0x00:
    return "Japan";
  case 0x01:
    return "USA";
  case 0x02:
    return "Europe";
  case 0x03:
    return "Asia";
  case 0x04:
    return "Russia";
  case 0x05:
    return "China";
  case 0x06:
    return "Mexico";
  case 0x80:
    return "DEX / Test (DTL-H)";
  case 0x81:
    return "DEX / Test (DTL-H)";
  case 0x82:
    return "DEX / Test";
  default:
    return "Unknown";
  }
}

// Pad helpers
static void init_pad(void) {
  padPortOpen(0, 0, padBuf);
  log_printf("[INIT] Gamepad port 0 opened.\n");
}

static u32 read_pad_click(void) {
  static u32 old_pad = 0;
  struct padButtonStatus buttons;
  u32 paddata = 0;

  int state = padGetState(0, 0);
  if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) {
    if (padRead(0, 0, &buttons) != 0) {
      paddata = 0xFFFF ^ buttons.btns;
    }
  }
  u32 clicked = paddata & (~old_pad);
  old_pad = paddata;
  return clicked;
}

static void wait_for_cross(void) {
  if (g_headless) {
    scr_printf("\n >> [HEADLESS] Auto-proceeding...\n");
    log_printf("[HEADLESS] Auto-proceeding past wait_for_cross.\n");
    return;
  }
  scr_printf("\n >> Press CROSS (X) to continue...\n");
  int no_pad_ticks = 0;
  while (1) {
    u32 btn = read_pad_click();
    if (btn & PAD_CROSS)
      break;
    int state = padGetState(0, 0);
    if (state == PAD_STATE_DISCONN) {
      no_pad_ticks++;
      if (no_pad_ticks > 300) {
        scr_printf(" >> [NO GAMEPAD] Auto-proceeding...\n");
        break;
      }
    } else {
      no_pad_ticks = 0;
    }
    for (volatile int d = 0; d < 10000; d++)
      ;
  }
}

// Module loader helper with X-module (Slim/late ROM) fallback
static int load_module_fallback(const char *mod1, const char *mod2) {
  int ret = -1;
  if (mod1) {
    ret = SifLoadModule(mod1, 0, NULL);
  }
  if (ret < 0 && mod2) {
    ret = SifLoadModule(mod2, 0, NULL);
  }
  return ret;
}

// Complete hardware & IOP subsystem initialization for real PS2 & emulators
static void init_ps2_system(void) {
  init_scr();
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("    PS2 MechaCon Diagnostic & CXP103049 Dumper Tool  \n");
  scr_printf("=====================================================\n\n");
  scr_printf(" [*] Initializing PlayStation 2 hardware...\n");

  // 1. Reset IOP to clean BIOS state
  scr_printf(" [*] Resetting I/O Processor (IOP)...\n");
  SifInitRpc(0);
  while (!SifIopReset("", 0)) {
  };
  while (!SifIopSync()) {
  };

  // 2. Re-initialize SIF RPC, IOP Heap, and LoadFile
  SifInitRpc(0);
  SifInitIopHeap();
  SifLoadFileInit();

  // 3. Apply standard SBV patches
  sbv_patch_enable_lmb();
  sbv_patch_disable_prefix_check();
  sbv_patch_fileio();

  // 4. Load PS2SDK Gamepad drivers (sio2man.irx + padman.irx embedded from
  // PS2SDK!)
  scr_printf(" [*] Initializing PS2SDK Gamepad drivers (SIO2 & PAD)...\n");
  enum JOYSTICK_INIT_STATUS joy_status = init_joystick_driver(true);
  log_printf("[INIT] PS2SDK joystick driver status=%d\n", joy_status);

  // 5. Load CDVDMAN & CDVDFSV from ROM (required for MechaCon SCMD & NCMD RPC!)
  scr_printf(" [*] Loading CDVDMAN and CDVDFSV drivers...\n");
  int cdvdman_ret = load_module_fallback("rom0:XCDVDMAN", "rom0:CDVDMAN");
  int cdvdfsv_ret = load_module_fallback("rom0:XCDVDFSV", "rom0:CDVDFSV");
  log_printf("[INIT] CDVDMAN ret=%d, CDVDFSV ret=%d\n", cdvdman_ret,
             cdvdfsv_ret);

  // 6. Initialize USB Mass Storage driver
  scr_printf(" [*] Initializing USB Mass Storage driver...\n");
  enum USB_INIT_STATUS usb_status = init_usb_driver(true);
  log_printf("[INIT] USB driver status=%d\n", usb_status);

  // 7. Initialize EE CDVD & PAD subsystems
  scr_printf(" [*] Connecting CDVD & Controller services...\n");
  mecha_init();
  init_pad();

  scr_printf(" [+] System initialized successfully!\n\n");
}

// Write a full buffer to an already-open fd and log (rather than silently
// swallow) a short/failed write. USB mass storage can be removed, fill up,
// or return a short write mid-dump; without this check a truncated ROM.BIN
// or NVRAM.BIN looks identical to a good one to both the tool and operator.
// Returns 0 on a complete write, -1 otherwise.
static int write_checked(int fd, const void *buf, size_t len, const char *path) {
  ssize_t written = write(fd, buf, len);
  if (written != (ssize_t)len) {
    log_printf("[FILE_ERR] Short write to %s: %ld/%lu bytes\n", path,
               (long)written, (unsigned long)len);
    return -1;
  }
  return 0;
}

// GUI Progress Bar
static void draw_progress_bar(int current, int total, const char *label) {
  if (total <= 0) return; // Avoid division by zero on a degenerate/indeterminate total
  const int bar_width = 28;
  int filled = (current * bar_width) / total;
  if (filled > bar_width)
    filled = bar_width;

  scr_setXY(2, 18);
  scr_printf("%-45s\n", label);
  scr_setXY(2, 19);
  scr_printf("[");
  for (int i = 0; i < bar_width; i++) {
    if (i < filled)
      scr_printf("=");
    else if (i == filled)
      scr_printf(">");
    else
      scr_printf(" ");
  }
  int pct = (current * 100) / total;
  scr_printf("] %3d%% (%d/%d)  \n", pct, current, total);

  if (current == total || (current % 64) == 0) {
    log_printf("[PROGRESS] %s: %d/%d (%d%%)\n", label, current, total, pct);
  }
}

// Initial hardware queries via pure SCMD verified against Ghidra
// MechaCon_Analysis.rep
static void query_initial_hardware(void) {
  log_printf("[INIT] Initializing hardware queries via pure SCMD...\n");

  // 1. rom0:ROMVER
  int fd = open("rom0:ROMVER", O_RDONLY);
  if (fd >= 0) {
    int r = read(fd, g_romver, sizeof(g_romver) - 1);
    if (r > 0)
      g_romver[r] = '\0';
    close(fd);
  }
  log_printf("[INIT] BIOS ROMVER: %s\n", g_romver);

  // 2. MechaCon Version & Region (SCMD 0x03-00)
  u8 ver_stat = 0;
  mecha_get_version(g_mecha_ver, &ver_stat);
  log_printf("[INIT] SCMD 0x03-00: Region=0x%02X (%s), Major=%d, Minor=%02d, "
             "Rev=0x%02X (status: 0x%02X)\n",
             g_mecha_ver[0], get_region_name(g_mecha_ver[0]), g_mecha_ver[1],
             g_mecha_ver[2], g_mecha_ver[3], ver_stat);
  log_printf(
      "[INIT] MechaCon Chip Part No: %s\n",
      get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
  if (g_mecha_ver[0] == 0x80 || g_mecha_ver[0] == 0x81) {
    log_printf("[INIT] Note: Region 0x%02X indicates DEX / DTL-H Test Station "
               "(normal, not an error)\n",
               g_mecha_ver[0]);
  }

  // 3. MechaCon DSP Version (SCMD 0x03-01)
  u8 dsp_stat = 0;
  mecha_get_dsp_version(&g_dsp_ver, &dsp_stat);
  log_printf("[INIT] SCMD 0x03-01: DSP Version = 0x%02X (status: 0x%02X)\n",
             g_dsp_ver, dsp_stat);

  // 4. MechaCon Model Name (SCMD 0x17, offset 0 and offset 8) - only exists on v2+
  u8 mn_stat = 0;
  if (g_mecha_ver[1] >= 2 && mecha_read_model_name(g_cdvd_model, &mn_stat) == 0 &&
      strlen(g_cdvd_model) > 0) {
    g_model_name_valid = 1;
    log_printf("[INIT] SCMD 0x17 Model Name: %s\n", g_cdvd_model);
  } else {
    g_model_name_valid = 0;
    strcpy(g_cdvd_model, "N/A (Early MechaCon v1)");
    log_printf(
        "[INIT] SCMD 0x17 skipped/not present on MechaCon v%d (stat: 0x%02X)\n",
        g_mecha_ver[1], mn_stat);
  }

  // 5. Hardware RTC (SCMD 0x08)
  u8 rtc_stat = 0;
  if (mecha_read_rtc(g_mecha_rtc, &rtc_stat) == 0) {
    g_rtc_valid = 1;
    log_printf("[INIT] SCMD 0x08 RTC: %02X %02X %02X %02X %02X %02X %02X %02X "
               "(stat: 0x%02X)\n",
               g_mecha_rtc[0], g_mecha_rtc[1], g_mecha_rtc[2], g_mecha_rtc[3],
               g_mecha_rtc[4], g_mecha_rtc[5], g_mecha_rtc[6], g_mecha_rtc[7],
               rtc_stat);
  } else {
    g_rtc_valid = 0;
    log_printf("[INIT] SCMD 0x08 RTC query returned error (stat: 0x%02X)\n",
               rtc_stat);
  }

  // 6. Console ID (SCMD 0x03-45)
  u8 cid_stat = 0;
  if (mecha_read_console_id(g_console_id, &cid_stat) == 0) {
    g_console_id_valid = 1;
    log_printf("[INIT] SCMD 0x03-45 Console ID: %02X %02X %02X %02X %02X %02X "
               "%02X %02X\n",
               g_console_id[0], g_console_id[1], g_console_id[2],
               g_console_id[3], g_console_id[4], g_console_id[5],
               g_console_id[6], g_console_id[7]);
  } else {
    g_console_id_valid = 0;
    log_printf("[INIT] SCMD 0x03-45 Console ID not loaded / unsupported (stat: "
               "0x%02X)\n",
               cid_stat);
  }

  // 7. i.Link ID (SCMD 0x12)
  u8 ilink_stat = 0;
  if (mecha_read_ilink_id(g_ilink_id, &ilink_stat) == 0) {
    g_ilink_id_valid = 1;
    log_printf(
        "[INIT] SCMD 0x12 i.Link ID: %02X %02X %02X %02X %02X %02X %02X %02X\n",
        g_ilink_id[0], g_ilink_id[1], g_ilink_id[2], g_ilink_id[3],
        g_ilink_id[4], g_ilink_id[5], g_ilink_id[6], g_ilink_id[7]);
  } else {
    g_ilink_id_valid = 0;
    log_printf(
        "[INIT] SCMD 0x12 i.Link ID not loaded / unsupported (stat: 0x%02X)\n",
        ilink_stat);
  }

  // 8. Early NVRAM Header Query: Read Serial & Model ID via SCMD 0x0A so folder
  // name is consistent from boot
  u8 dummy_nvm[512];
  memset(dummy_nvm, 0xFF, sizeof(dummy_nvm));
  u16 w = 0;
  u8 st = 0;
  // Byte order matches mecha_backup_nvram()'s canonical little-endian packing
  // (lo byte first) so extract_*_from_nvram() hits its native-LE fast path
  // instead of relying on its big-endian fallback.
  // Old layout words: 0x0E4 (Model), 0x0E6..0x0E7 (Serial)
  if (mecha_read_nvm_word(0x0E4, &w, &st) == 0) {
    dummy_nvm[0x1C8] = (u8)w;
    dummy_nvm[0x1C9] = (u8)(w >> 8);
  }
  if (mecha_read_nvm_word(0x0E6, &w, &st) == 0) {
    dummy_nvm[0x1CC] = (u8)w;
    dummy_nvm[0x1CD] = (u8)(w >> 8);
  }
  if (mecha_read_nvm_word(0x0E7, &w, &st) == 0) {
    dummy_nvm[0x1CE] = (u8)w;
    dummy_nvm[0x1CF] = (u8)(w >> 8);
  }
  // New layout words: 0x0F8 (Model), 0x0FA..0x0FB (Serial)
  if (mecha_read_nvm_word(0x0F8, &w, &st) == 0) {
    dummy_nvm[0x1F0] = (u8)w;
    dummy_nvm[0x1F1] = (u8)(w >> 8);
  }
  if (mecha_read_nvm_word(0x0FA, &w, &st) == 0) {
    dummy_nvm[0x1F4] = (u8)w;
    dummy_nvm[0x1F5] = (u8)(w >> 8);
  }
  if (mecha_read_nvm_word(0x0FB, &w, &st) == 0) {
    dummy_nvm[0x1F6] = (u8)w;
    dummy_nvm[0x1F7] = (u8)(w >> 8);
  }
  g_model_id = extract_model_id_from_nvram(dummy_nvm);
  g_serial = extract_serial_from_nvram(dummy_nvm, &g_emcs);
  if (g_serial > 0) {
    log_printf("[INIT] SCMD 0x0A Early Serial: %07u, EMCS: 0x%02X, Model ID: "
               "0x%04X (%s)\n",
               g_serial, g_emcs, g_model_id, get_model_id_desc(g_model_id));
  }
}

static int g_storage_ready = 0;

// Compute dynamic destination folder, prioritizing host: (for PCSX2 /
// emulators) then mass: (for real PS2)
static void update_dump_directory(void) {
  u8 major = g_mecha_ver[1];
  u8 minor = g_mecha_ver[2];
  char folder_name[96];

  if (g_serial > 0) {
    snprintf(folder_name, sizeof(folder_name), "MECHA_v%d.%02d_%07u", major,
             minor, g_serial);
  } else if (g_model_name_valid && strlen(g_cdvd_model) > 0 &&
             strncmp(g_cdvd_model, "N/A", 3) != 0) {
    snprintf(folder_name, sizeof(folder_name), "MECHA_v%d.%02d_%s", major,
             minor, g_cdvd_model);
  } else if (g_model_id != 0) {
    snprintf(folder_name, sizeof(folder_name), "MECHA_v%d.%02d_ID%04X", major,
             minor, g_model_id);
  } else {
    snprintf(folder_name, sizeof(folder_name), "MECHA_v%d.%02d_DUMP", major,
             minor);
  }

  g_storage_ready = 0;

  // 1. Priority 1: Check host: (native PC filesystem on PCSX2 / Play! / test
  // environment)
  char target_path[160];
  snprintf(target_path, sizeof(target_path), "host:%s", folder_name);
  int r = mkdir(target_path, 0777);
  if (r == 0 || errno == EEXIST) {
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", target_path);
    g_storage_ready = 1;
    log_printf("[DIR] Destination storage ready: host: (%s)\n", g_dump_dir);
    return;
  }

  snprintf(target_path, sizeof(target_path), "host:/%s", folder_name);
  r = mkdir(target_path, 0777);
  if (r == 0 || errno == EEXIST) {
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", target_path);
    g_storage_ready = 1;
    log_printf("[DIR] Destination storage ready: host:/ (%s)\n", g_dump_dir);
    return;
  }

  // 2. Priority 2: Check mass: (USB flash drive on real PS2)
  snprintf(target_path, sizeof(target_path), "mass:/%s", folder_name);
  r = mkdir(target_path, 0777);
  if (r == 0 || errno == EEXIST) {
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", target_path);
    g_storage_ready = 1;
    log_printf("[DIR] Destination storage ready: mass:/ (%s)\n", g_dump_dir);
    return;
  }

  snprintf(target_path, sizeof(target_path), "mass:%s", folder_name);
  r = mkdir(target_path, 0777);
  if (r == 0 || errno == EEXIST) {
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", target_path);
    g_storage_ready = 1;
    log_printf("[DIR] Destination storage ready: mass: (%s)\n", g_dump_dir);
    return;
  }

  // 3. If neither host: nor mass: is accessible yet (e.g. real PS2 without USB
  // stick inserted)
  snprintf(g_dump_dir, sizeof(g_dump_dir), "mass:/%s", folder_name);
  g_storage_ready = 0;
  log_printf("[DIR] No ready storage device detected yet. Default: %s (insert "
             "USB drive)\n",
             g_dump_dir);
}


// Menu 1: Display Full Console & MechaCon Info
static void show_system_info(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("           Console & MechaCon Identification        \n");
  scr_printf("=====================================================\n\n");

  scr_printf(" [BIOS] rom0:ROMVER   : %s\n", g_romver);
  scr_printf(" [SCMD 0x17] Model    : %s\n", g_cdvd_model);

  if (g_nvram_backed_up) {
    scr_printf(" [NVRAM] Model ID     : 0x%04X (%s)\n", g_model_id,
               get_model_id_desc(g_model_id));
    scr_printf(" [NVRAM] Serial No.   : %07u (EMCS: 0x%02X)\n", g_serial,
               g_emcs);
  } else {
    scr_printf(" [NVRAM] Serial/Model : <Run Dump or NVRAM Backup to decode>\n");
  }

  scr_printf(" [SCMD 0x03-00] Ver   : MD 1.39 v%d.%02d (Rev 0x%02X)\n",
             g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[3]);
  scr_printf(" [SCMD 0x03-00] Region: 0x%02X (%s%s)\n", g_mecha_ver[0],
             get_region_name(g_mecha_ver[0]),
             (g_mecha_ver[0] == 0x80 || g_mecha_ver[0] == 0x81) ? " / DEX"
                                                                : "");
  scr_printf(
      " [CHIP] Part Number   : %s\n",
      get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
  scr_printf(" [SCMD 0x03-01] DSP   : Version 0x%02X\n", g_dsp_ver);

  if (g_rtc_valid) {
    scr_printf(" [SCMD 0x08] RTC Regs : %02X %02X %02X %02X %02X %02X %02X\n",
               g_mecha_rtc[1], g_mecha_rtc[2], g_mecha_rtc[3], g_mecha_rtc[4],
               g_mecha_rtc[5], g_mecha_rtc[6], g_mecha_rtc[7]);
  } else {
    scr_printf(" [SCMD 0x08] RTC Regs : N/A (Failed/Unsupported)\n");
  }

  if (g_console_id_valid) {
    scr_printf(
        " [SCMD 0x03-45] CID   : %02X %02X %02X %02X %02X %02X %02X %02X\n",
        g_console_id[0], g_console_id[1], g_console_id[2], g_console_id[3],
        g_console_id[4], g_console_id[5], g_console_id[6], g_console_id[7]);
  } else {
    scr_printf(" [SCMD 0x03-45] CID   : Not loaded / unsupported\n");
  }

  if (g_ilink_id_valid) {
    scr_printf(
        " [SCMD 0x12] i.Link   : %02X %02X %02X %02X %02X %02X %02X %02X\n\n",
        g_ilink_id[0], g_ilink_id[1], g_ilink_id[2], g_ilink_id[3],
        g_ilink_id[4], g_ilink_id[5], g_ilink_id[6], g_ilink_id[7]);
  } else {
    scr_printf(" [SCMD 0x12] i.Link   : Not loaded / unsupported\n\n");
  }

  scr_printf(" --- Target Chip Detection ---\n");
  scr_printf(
      " Hardware Part : %s\n",
      get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
  if (g_mecha_ver[1] == 3) {
    scr_printf(" Architecture  : CXP103049 Series (SPC970 Core, BGA)\n");
    scr_printf(" Target Status : SPC970 worker +0xB4 exploit enabled.\n");
  } else if (g_mecha_ver[1] == 2) {
    scr_printf(" Architecture  : CXP102064 Series (SPC970 Core, QFP)\n");
    scr_printf(" Target Status : Standard SPC970 worker layout.\n");
  } else if (g_mecha_ver[1] == 1) {
    scr_printf(" Architecture  : CXP101064 Series (Early MD 1.36/1.38)\n");
  } else if (g_mecha_ver[1] >= 5) {
    scr_printf(" Architecture  : Dragon CXR7xxxxx Series (32-bit ARM)\n");
  }

  scr_printf("\n Target Dump Path: %s/\n", g_dump_dir);
  scr_printf(" NVRAM in Memory : %s\n",
             g_nvram_backed_up ? "YES (1024 bytes safe)" : "NO (Use Advanced Tools menu)");

  // Automatically flush and save debug log to USB when viewing diagnostics
  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  if (log_save_to_file(log_path) == 0) {
    scr_printf(" Debug Log File  : [SAVED] %s/DEBUG_LOG.TXT\n", g_dump_dir);
  }

  wait_for_cross();
}

// Menu 2: Backup NVRAM
static void backup_nvram_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("          MechaCon Full NVRAM Backup (1024 B)         \n");
  scr_printf("=====================================================\n\n");

  scr_printf(" [*] Reading all 512 words from EEPROM via SCMD 0x0A...\n\n");

  log_printf("[NVRAM] Starting full backup of 512 words via SCMD 0x0A...\n");
  int read_errs = mecha_backup_nvram(g_nvram_backup, draw_progress_bar);
  // Only trust this buffer for a later restore if every word was actually read;
  // a partial backup contains stale memset(0) words at the failed positions,
  // and other menus check g_nvram_backed_up to decide whether to skip re-reading it.
  g_nvram_backed_up = (read_errs == 0);

  if (read_errs > 0) {
    log_printf("[WARN] NVRAM backup completed with %d word read errors (status "
               "!= 0x00)\n",
               read_errs);
  }

  // Decode Serial Number and Model ID from NVRAM!
  g_serial = extract_serial_from_nvram(g_nvram_backup, &g_emcs);
  g_model_id = extract_model_id_from_nvram(g_nvram_backup);
  log_printf(
      "[NVRAM] Decoded Serial: %07u, EMCS: 0x%02X, Model ID: 0x%04X (%s)\n",
      g_serial, g_emcs, g_model_id, get_model_id_desc(g_model_id));

  // Update target directory on USB
  update_dump_directory();

  // Save NVRAM to target directory
  char nvram_path[256];
  char log_path[256];
  snprintf(nvram_path, sizeof(nvram_path), "%s/NVRAM.BIN", g_dump_dir);
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);

  int saved_mass = 0;
  int fd = open(nvram_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd >= 0) {
    saved_mass = (write_checked(fd, g_nvram_backup, NVRAM_SIZE_BYTES, nvram_path) == 0);
    close(fd);
    if (saved_mass) {
      log_printf("[FILE] Successfully saved %s\n", nvram_path);
    }
  } else {
    log_printf("[WARN] Failed to open %s for writing\n", nvram_path);
  }

  log_save_to_file(log_path);

  scr_setXY(2, 21);
  if (read_errs == 0) {
    scr_printf("[+] NVRAM successfully backed up to memory! (0 errors)\n");
  } else {
    scr_printf("[!] NVRAM backed up with %d word errors (see log)!\n",
               read_errs);
    scr_printf("[!] Incomplete: NOT marked safe for auto-restore. Retry before exploiting.\n");
  }
  scr_printf("    Decoded Serial: %07u | Model ID: 0x%04X (%s)\n", g_serial,
             g_model_id, get_model_id_desc(g_model_id));
  if (saved_mass) {
    scr_printf("[+] Saved to USB: %s\n", nvram_path);
  } else {
    scr_printf("[!] USB not mounted. Memory copy retained safely.\n");
  }

  wait_for_cross();
}



// Full Auto Dump & Verification
static void full_dump_and_verify_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("      Full Auto ROM Dump, Checksum & Restore        \n");
  scr_printf("=====================================================\n\n");

  log_printf("[AUTO_DUMP] Full automated workflow initiated...\n");

  // MechaCon v1 (CXP101064, 1.02..1.08) is NOT supported by this exploit.
  // The SCMD 0x42 8-bit byte counter wraps at 256 bytes, making the worker
  // area (located >280 bytes from buffer base on v1) physically unreachable.
  if (g_mecha_ver[1] == 1) {
    scr_printf(" [!] ERROR: MechaCon v1.%02d is NOT supported by this exploit!\n\n", g_mecha_ver[2]);
    scr_printf("     The SCMD 0x42 byte counter is 8-bit and wraps at 256 bytes.\n");
    scr_printf("     The EEPROM worker area on v1 hardware is located beyond the\n");
    scr_printf("     256-byte write limit, making it physically unreachable.\n\n");
    scr_printf("     Supported firmware: v2.02..v2.14 and v3.00..v3.06\n");
    scr_printf("     Your firmware:      v%d.%02d (Chip: %s)\n\n",
               g_mecha_ver[1], g_mecha_ver[2],
               get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
    log_printf("[AUTO_DUMP] REJECTED: MechaCon v1.%02d is unsupported (worker unreachable).\n",
               g_mecha_ver[2]);
    wait_for_cross();
    return;
  }

  if (!g_nvram_backed_up) {
    scr_printf(" [*] Step 1/4: Backing up NVRAM first for safety...\n");
    int read_errs = mecha_backup_nvram(g_nvram_backup, draw_progress_bar);
    if (read_errs > 0) {
      log_printf("[WARN] Step 1 backup had %d word errors\n", read_errs);
    }
    g_serial = extract_serial_from_nvram(g_nvram_backup, &g_emcs);
    g_model_id = extract_model_id_from_nvram(g_nvram_backup);
    update_dump_directory();

    // Save NVRAM.BIN
    char nvram_path[256];
    snprintf(nvram_path, sizeof(nvram_path), "%s/NVRAM.BIN", g_dump_dir);
    int fd_nvm = open(nvram_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_nvm >= 0) {
      if (write_checked(fd_nvm, g_nvram_backup, NVRAM_SIZE_BYTES, nvram_path) == 0) {
        log_printf("[FILE] Saved NVRAM backup: %s\n", nvram_path);
      }
      close(fd_nvm);
    } else {
      log_printf("[FILE_ERR] Failed to save NVRAM backup: %s\n", nvram_path);
    }

    // The exploit below writes into live EEPROM and relies on g_nvram_backup
    // to restore it afterwards. A partial backup has stale zero words at the
    // failed positions, so restoring from it would corrupt those words on
    // the console instead of restoring them. Refuse to proceed.
    if (read_errs > 0) {
      scr_printf("\n [!] NVRAM backup incomplete (%d word errors). Exploit ABORTED for safety.\n",
                 read_errs);
      scr_printf("     No EEPROM writes were made. Retry the backup (option 1) and try again.\n\n");
      log_printf("[ABORT] NVRAM backup incomplete (%d errors) - refusing to run exploit.\n",
                 read_errs);
      char log_path_abort[256];
      snprintf(log_path_abort, sizeof(log_path_abort), "%s/DEBUG_LOG.TXT", g_dump_dir);
      log_save_to_file(log_path_abort);
      wait_for_cross();
      return;
    }
    g_nvram_backed_up = 1;
  } else {
    scr_printf(" [*] Step 1/4: NVRAM already safely backed up.\n");
  }

  scr_setXY(2, 7);
  int is_v3_device = (g_mecha_ver[1] >= 3);
  u32 expected_chunks = is_v3_device ? 768 : 1024;
  scr_printf(" [*] Step 2/4: Dumping ROM via SCMD 0x42 overflow exploit...\n");
  scr_printf("     Validating pre-flight staging & dumping %u chunks (%s)...\n",
             expected_chunks,
             is_v3_device ? "192 KB, Banks FD-FF" : "256 KB, Banks FC-FF");
  log_printf("[AUTO_DUMP] Starting SPC970 ROM dump exploit (%s)...\n",
             is_v3_device ? "v3 192KB" : "v2 256KB");
  u32 dumped_rom_size = 0;
  int dump_ret = mecha_dump_full_rom(g_rom_buffer, &dumped_rom_size,
                                     g_nvram_backup, draw_progress_bar);
  log_printf("[AUTO_DUMP] ROM dump returned: %d (dumped %u bytes)\n", dump_ret,
             dumped_rom_size);

  if (dump_ret < 0) {
    scr_clear();
    scr_printf("=====================================================\n");
    scr_printf("              ROM DUMP EXPLOIT FAILED                \n");
    scr_printf("=====================================================\n\n");
    scr_printf(" [!] SAFETY ACTIVE: Auto-restoring NVRAM immediately...\n");
    scr_printf("     Please WAIT and do NOT turn off console power!\n\n");
    log_printf("[AUTO_RESTORE] Exploit failed (%d), performing guaranteed "
               "NVRAM restore...\n",
               dump_ret);

    int rest_err = mecha_restore_nvram(g_nvram_backup, draw_progress_bar);
    log_printf("[AUTO_RESTORE] Restoration completed with %d errors\n",
               rest_err);
    scr_setXY(2, 16);
    scr_printf(
        " [+] NVRAM Auto-Restored (%d write errors). Safe to reboot!\n\n",
        rest_err);

    if (dump_ret == -1) {
      scr_printf(" [-] SCMD 0x40/0x42 configuration open/write rejected!\n");
      scr_printf(
          "     This MechaCon chip rejected write mode or block count 0.\n");
    } else if (dump_ret == -2) {
      scr_printf(" [-] Pre-flight Header Validation Failed!\n");
      scr_printf("     Staged data did not match expected ROM signature.\n");
    }
    scr_printf("\n [+] NVRAM is safe and restored to factory settings.\n");
    scr_printf(" [-] Empty 0xFF ROM image was NOT saved to disk.\n\n");
    scr_printf(" Target directory:\n   %s/\n\n", g_dump_dir);

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
    log_save_to_file(log_path);

    wait_for_cross();
    return;
  }

  // Save ROM.BIN with actual dumped size
  char rom_path[256];
  snprintf(rom_path, sizeof(rom_path), "%s/ROM.BIN", g_dump_dir);
  int fd_rom = open(rom_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd_rom >= 0) {
    if (write_checked(fd_rom, g_rom_buffer, dumped_rom_size, rom_path) == 0) {
      log_printf("[FILE] Saved ROM image (%u bytes): %s\n", dumped_rom_size,
                 rom_path);
    } else {
      scr_printf(" [!] Warning: ROM.BIN may be truncated on disk!\n");
    }
    close(fd_rom);
  } else {
    log_printf("[FILE_ERR] Failed to save ROM image: %s\n", rom_path);
  }

  // Step 3: Hardware Checksum Verification
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("         POST Hardware Checksum Verification         \n");
  scr_printf("=====================================================\n\n");
  scr_printf(" [*] Step 3/4: Verifying dump against hardware checksums...\n\n");

  RomVerifyResult res;
  verify_spc970_rom(g_rom_buffer, dumped_rom_size, &res);
  log_printf("[VERIFY] Mirror checksum match: %s\n",
             res.mirror_match ? "YES" : "NO");
  log_printf("[VERIFY] Total sections passed: %d/%d\n", res.passed_count,
             res.total_sections);

  scr_printf(" Mirror Checksum (0xFFFB80 == 0xFFFB92): %s\n",
             res.mirror_match ? "PASS" : "FAIL / PENDING");
  for (int i = 0; i < res.total_sections; i++) {
    scr_printf("  [%d/%d] %-30s : %s (0x%04X)\n", i + 1, res.total_sections,
               res.sections[i].name, res.sections[i].passed ? "PASS" : "FAIL",
               res.sections[i].computed_checksum);
    log_printf("[VERIFY] [%d/%d] %s: %s (computed: 0x%04X, expected: 0x%04X)\n",
               i + 1, res.total_sections, res.sections[i].name,
               res.sections[i].passed ? "PASS" : "FAIL",
               res.sections[i].computed_checksum,
               res.sections[i].expected_checksum);
  }
  scr_printf("\n Result: %d/%d Checksums matched.\n", res.passed_count,
             res.total_sections);

  if (res.all_passed) {
    scr_printf("\n[+] SUCCESS: 100%% INTACT BIT-PERFECT DUMP! ALL POST CHECKS "
               "PASSED!\n");
  } else {
    scr_printf("\n[-] WARNING: POST hardware checksums did not all match!\n");
  }

  // Step 4: Restore Original NVRAM
  scr_printf("\n [*] Step 4/4: Restoring original NVRAM back to EEPROM...\n");
  log_printf("[RESTORE] Restoring NVRAM from backup buffer...\n");
  int write_errs = mecha_restore_nvram(g_nvram_backup, draw_progress_bar);
  if (write_errs > 0) {
    log_printf("[WARN] Restore had %d word write errors\n", write_errs);
  }

  // Write DUMP_INFO.TXT
  char info_path[256];
  snprintf(info_path, sizeof(info_path), "%s/DUMP_INFO.TXT", g_dump_dir);
  FILE *f_info = fopen(info_path, "w");
  if (f_info) {
    fprintf(f_info, "=== PS2 MechaCon Dump Summary ===\n");
    fprintf(f_info, "ROM Dump Size         : %u KB (%u bytes, %s)\n",
            dumped_rom_size / 1024, dumped_rom_size,
            (dumped_rom_size == 196608) ? "v3 Native Banks FD-FF"
                                        : "v2 Full Banks FC-FF");
    fprintf(f_info, "Model Name (SCMD 0x17): %s\n", g_cdvd_model);
    fprintf(
        f_info, "MechaCon Chip Part No : %s\n",
        get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
    fprintf(f_info, "Model ID (NVRAM)      : 0x%04X (%s)\n", g_model_id,
            get_model_id_desc(g_model_id));
    fprintf(f_info, "Serial Number (NVRAM) : %07u (EMCS: 0x%02X)\n", g_serial,
            g_emcs);
    fprintf(f_info, "BIOS ROMVER           : %s\n", g_romver);
    fprintf(f_info,
            "MechaCon Ver (0x03-00): MD 1.39 v%d.%02d (Region: 0x%02X [%s%s], "
            "Rev 0x%02X)\n",
            g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0],
            get_region_name(g_mecha_ver[0]),
            (g_mecha_ver[0] == 0x80 || g_mecha_ver[0] == 0x81) ? " / DEX" : "",
            g_mecha_ver[3]);
    fprintf(f_info, "DSP Version  (0x03-01): 0x%02X\n", g_dsp_ver);
    if (g_rtc_valid) {
      fprintf(f_info,
              "Hardware RTC (0x08)   : %02X %02X %02X %02X %02X %02X %02X\n",
              g_mecha_rtc[1], g_mecha_rtc[2], g_mecha_rtc[3], g_mecha_rtc[4],
              g_mecha_rtc[5], g_mecha_rtc[6], g_mecha_rtc[7]);
    } else {
      fprintf(f_info, "Hardware RTC (0x08)   : N/A\n");
    }
    if (g_console_id_valid) {
      fprintf(
          f_info, "Console ID   (0x03-45): %02X%02X%02X%02X%02X%02X%02X%02X\n",
          g_console_id[0], g_console_id[1], g_console_id[2], g_console_id[3],
          g_console_id[4], g_console_id[5], g_console_id[6], g_console_id[7]);
    } else {
      fprintf(f_info,
              "Console ID   (0x03-45): N/A (Not loaded / early MechaCon)\n");
    }
    if (g_ilink_id_valid) {
      fprintf(f_info,
              "i.Link ID    (0x12)   : %02X%02X%02X%02X%02X%02X%02X%02X\n",
              g_ilink_id[0], g_ilink_id[1], g_ilink_id[2], g_ilink_id[3],
              g_ilink_id[4], g_ilink_id[5], g_ilink_id[6], g_ilink_id[7]);
    } else {
      fprintf(f_info,
              "i.Link ID    (0x12)   : N/A (Not loaded / early MechaCon)\n");
    }
    fprintf(f_info, "\nHardware Checksums (POST Routine 0xFF5170):\n");
    fprintf(f_info, "Mirror Match   : %s\n",
            res.mirror_match ? "PASS" : "FAIL");
    for (int i = 0; i < res.total_sections; i++) {
      fprintf(
          f_info, "Section %d (%s): %s [Computed: 0x%04X, Expected: 0x%04X]\n",
          i + 1, res.sections[i].name, res.sections[i].passed ? "PASS" : "FAIL",
          res.sections[i].computed_checksum, res.sections[i].expected_checksum);
    }
    fprintf(f_info, "Total Matched  : %d/%d\n", res.passed_count,
            res.total_sections);
    fclose(f_info);
    log_printf("[FILE] Generated %s\n", info_path);
  }

  // Step 4: Final NVRAM verification (already restored during dump cycles)
  scr_printf("\n [*] Verifying restored NVRAM integrity...\n");
  int mismatches = mecha_verify_nvram(g_nvram_backup, draw_progress_bar);
  log_printf("[RESTORE] NVRAM verification completed with %d mismatches.\n",
             mismatches);

  if (mismatches == 0) {
    scr_printf("[+] SUCCESS: NVRAM 100%% restored and verified!\n");
  } else {
    scr_printf("[-] WARNING: %d word mismatches during restoration!\n",
               mismatches);
  }

  // Save final debug log to USB
  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  scr_printf("\n[+] All outputs and logs saved to:\n    %s/\n", g_dump_dir);
  wait_for_cross();
}

// Menu 5: Restore NVRAM
static void restore_nvram_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("              Restore NVRAM from Backup              \n");
  scr_printf("=====================================================\n\n");

  if (!g_nvram_backed_up) {
    scr_printf("[-] Error: No NVRAM backup in memory!\n");
    scr_printf("    Please execute option [1] Backup NVRAM in Advanced Tools first.\n");
    wait_for_cross();
    return;
  }

  scr_printf(" [*] Restoring 512 words back to MechaCon EEPROM...\n");
  log_printf("[RESTORE] Manual NVRAM restore initiated...\n");
  int write_errs = mecha_restore_nvram(g_nvram_backup, draw_progress_bar);
  if (write_errs > 0) {
    log_printf("[WARN] Manual restore had %d write errors\n", write_errs);
  }

  scr_printf("\n [*] Verifying restored data...\n");
  int mismatches = mecha_verify_nvram(g_nvram_backup, draw_progress_bar);
  log_printf("[RESTORE] Manual verification: %d mismatches.\n", mismatches);

  if (mismatches == 0) {
    scr_printf("\n[+] SUCCESS: NVRAM verified! 100%% match with backup.\n");
  } else {
    scr_printf("\n[-] WARNING: %d word mismatches detected!\n", mismatches);
  }

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  wait_for_cross();
}

// Restore NVRAM from USB file (mass:/NVRAM.BIN or mass:/path/NVRAM.BIN)
static void restore_nvram_from_file_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("          Restore NVRAM from USB File (NVRAM.BIN)    \n");
  scr_printf("=====================================================\n\n");

  log_printf("[RESTORE_FILE] NVRAM restore from file initiated...\n");

  // Try multiple paths: target dump dir first, then mass root
  char try_paths[3][256];
  int num_paths = 0;
  snprintf(try_paths[num_paths++], sizeof(try_paths[0]), "%s/NVRAM.BIN", g_dump_dir);
  snprintf(try_paths[num_paths++], sizeof(try_paths[0]), "mass:/NVRAM.BIN");
  snprintf(try_paths[num_paths++], sizeof(try_paths[0]), "mass:NVRAM.BIN");

  int fd = -1;
  const char *found_path = NULL;
  for (int i = 0; i < num_paths; i++) {
    fd = open(try_paths[i], O_RDONLY);
    if (fd >= 0) {
      found_path = try_paths[i];
      break;
    }
  }

  if (fd < 0) {
    scr_printf(" [-] ERROR: Could not find NVRAM.BIN on USB storage!\n\n");
    scr_printf("     Searched:\n");
    for (int i = 0; i < num_paths; i++) {
      scr_printf("       %s\n", try_paths[i]);
    }
    scr_printf("\n     Please copy NVRAM.BIN to your USB drive and try again.\n");
    log_printf("[RESTORE_FILE] NVRAM.BIN not found on any search path.\n");
    wait_for_cross();
    return;
  }

  // Read the file
  u8 file_nvram[NVRAM_SIZE_BYTES];
  ssize_t bytes_read = read(fd, file_nvram, NVRAM_SIZE_BYTES);
  close(fd);

  if (bytes_read != NVRAM_SIZE_BYTES) {
    scr_printf(" [-] ERROR: NVRAM.BIN is %ld bytes, expected exactly %d bytes!\n",
               (long)bytes_read, NVRAM_SIZE_BYTES);
    scr_printf("     File may be corrupt or incomplete. Aborting.\n");
    log_printf("[RESTORE_FILE] File size mismatch: got %ld, expected %d.\n",
               (long)bytes_read, NVRAM_SIZE_BYTES);
    wait_for_cross();
    return;
  }

  scr_printf(" [+] Found: %s (%d bytes)\n\n", found_path, NVRAM_SIZE_BYTES);
  log_printf("[RESTORE_FILE] Loaded %s (%d bytes)\n", found_path, NVRAM_SIZE_BYTES);

  // Decode Serial and Model ID from file for confirmation
  u8 file_emcs = 0;
  u32 file_serial = extract_serial_from_nvram(file_nvram, &file_emcs);
  u16 file_model_id = extract_model_id_from_nvram(file_nvram);
  scr_printf(" File NVRAM Serial : %07u (EMCS: 0x%02X)\n", file_serial, file_emcs);
  scr_printf(" File Model ID     : 0x%04X (%s)\n\n", file_model_id, get_model_id_desc(file_model_id));

  scr_printf(" [!] WARNING: This will overwrite ALL 512 EEPROM words!\n");
  scr_printf("     Press CROSS (X) to proceed, TRIANGLE to cancel.\n");
  log_printf("[RESTORE_FILE] Waiting for user confirmation...\n");

  if (g_headless) {
    scr_printf("\n >> [HEADLESS] Auto-proceeding...\n");
  } else {
    while (1) {
      u32 btn = read_pad_click();
      if (btn & PAD_CROSS) break;
      if (btn & PAD_TRIANGLE) {
        scr_printf("\n [-] Cancelled by user. No changes made.\n");
        log_printf("[RESTORE_FILE] Cancelled by user.\n");
        wait_for_cross();
        return;
      }
      for (volatile int d = 0; d < 10000; d++);
    }
  }

  scr_printf("\n [*] Writing 512 words to MechaCon EEPROM via SCMD 0x0B...\n");
  int write_errs = mecha_restore_nvram(file_nvram, draw_progress_bar);
  if (write_errs > 0) {
    log_printf("[WARN] File restore had %d write errors\n", write_errs);
  }

  scr_printf("\n [*] Verifying restored data...\n");
  int mismatches = mecha_verify_nvram(file_nvram, draw_progress_bar);
  log_printf("[RESTORE_FILE] Verification: %d mismatches.\n", mismatches);

  if (mismatches == 0) {
    scr_printf("\n[+] SUCCESS: NVRAM restored and verified! 100%% match with file.\n");
    // Update in-memory backup to match
    memcpy(g_nvram_backup, file_nvram, NVRAM_SIZE_BYTES);
    g_nvram_backed_up = 1;
    g_serial = file_serial;
    g_emcs = file_emcs;
    g_model_id = file_model_id;
  } else {
    scr_printf("\n[-] WARNING: %d word mismatches detected after restore!\n", mismatches);
  }

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  wait_for_cross();
}

// Menu 7: Save Debug Log to USB
static void save_debug_log_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("             Save Debug Log to USB Storage           \n");
  scr_printf("=====================================================\n\n");

  update_dump_directory();

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  int res = log_save_to_file(log_path);

  if (res == 0) {
    scr_printf("[+] Debug log successfully saved to:\n    %s\n\n", log_path);
    scr_printf("    You can copy this file and share it in chat!\n");
  } else {
    scr_printf("[-] Failed to save debug log to USB storage.\n");
    scr_printf("    Please ensure USB is plugged in as FAT32.\n");
  }

  wait_for_cross();
}

static void advanced_tools_menu(void) {
  int sub_selected = 0;
  const int sub_items = 5;

  while (1) {
    scr_clear();
    scr_printf("=====================================================\n");
    scr_printf("        Advanced Tools & Manual Operations           \n");
    scr_printf("=====================================================\n\n");
    scr_printf(" D-Pad Up/Down: Navigate | CROSS (X): Select | TRIANGLE: Back\n\n");

    scr_printf(" %s [1] Standalone NVRAM Backup (1024 Bytes)\n",
               (sub_selected == 0) ? "->" : "  ");
    scr_printf(" %s [2] Restore NVRAM from Backup Buffer\n",
               (sub_selected == 1) ? "->" : "  ");
    scr_printf(" %s [3] Restore NVRAM from USB File (NVRAM.BIN)\n",
               (sub_selected == 2) ? "->" : "  ");
    scr_printf(" %s [4] Export Debug Log to USB Storage\n",
               (sub_selected == 3) ? "->" : "  ");
    scr_printf(" %s [5] Back to Main Menu\n\n",
               (sub_selected == 4) ? "->" : "  ");

    scr_printf("-----------------------------------------------------\n");
    scr_printf(" NVRAM State : %s\n",
               g_nvram_backed_up ? "[BACKED UP]" : "[NOT BACKED UP]");

    u32 btn = 0;
    while (!(btn = read_pad_click())) {
      for (volatile int d = 0; d < 10000; d++)
        ;
    }

    if (btn & PAD_UP) {
      sub_selected = (sub_selected - 1 + sub_items) % sub_items;
    } else if (btn & PAD_DOWN) {
      sub_selected = (sub_selected + 1) % sub_items;
    } else if (btn & PAD_CROSS) {
      log_printf("[UI] User selected advanced menu item [%d]\n", sub_selected + 1);
      switch (sub_selected) {
      case 0:
        backup_nvram_action();
        break;
      case 1:
        restore_nvram_action();
        break;
      case 2:
        restore_nvram_from_file_action();
        break;
      case 3:
        save_debug_log_action();
        break;
      case 4:
        return;
      }
    } else if (btn & PAD_TRIANGLE) {
      return;
    }
  }
}

int main(int argc, char *argv[]) {
  log_init();
  init_ps2_system();

  log_printf("[BOOT] PS2 MechaCon Tool startup. argc=%d\n", argc);
  for (int i = 0; i < argc; i++) {
    log_printf("[BOOT] argv[%d] = '%s'\n", i, argv[i] ? argv[i] : "NULL");
  }

  int auto_action = 0;
  for (int i = 1; i < argc; i++) {
    if (!argv[i])
      continue;
    if (strcmp(argv[i], "--auto") == 0 || strcmp(argv[i], "--dump") == 0 || strcmp(argv[i], "--v1") == 0)
      auto_action = 3;
    else if (strcmp(argv[i], "--backup") == 0)
      auto_action = 2;
    else if (strcmp(argv[i], "--info") == 0 || strcmp(argv[i], "--ident") == 0)
      auto_action = 1;
  }

  if (auto_action > 0) {
    g_headless = 1;
    log_printf("[BOOT] Headless mode activated: auto-action [%d]\n",
               auto_action);
  }

  scr_printf(" [*] Querying MechaCon hardware...\n");
  query_initial_hardware();
  update_dump_directory();

  if (auto_action == 1) {
    show_system_info();
    return 0;
  } else if (auto_action == 2) {
    backup_nvram_action();
    return 0;
  } else if (auto_action == 3) {
    full_dump_and_verify_action();
    return 0;
  }

  int selected = 0;
  const int menu_items = 4;

  while (1) {
    scr_clear();
    scr_printf("=====================================================\n");
    scr_printf("       PS2 MechaCon ROM Dumper %s           \n", MECHA_TOOL_VERSION);
    scr_printf("=====================================================\n");
    scr_printf(
        " Chip   : %-16s | Firmware: v%d.%02d (Reg 0x%02X)\n",
        get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]),
        g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]);
    scr_printf(" Console: %-16s | Target: %s/\n\n",
               g_model_name_valid ? g_cdvd_model : get_model_id_desc(g_model_id),
               g_dump_dir);
    scr_printf(" D-Pad Up/Down: Navigate | CROSS (X): Select\n\n");

    scr_printf(" %s [1] DUMP MECHACON ROM (Recommended)\n",
               (selected == 0) ? "->" : "  ");
    scr_printf("      -> Auto Exploit, POST Verify & NVRAM Safe Backup\n\n");

    scr_printf(" %s [2] Hardware Diagnostics & Export Debug Log\n",
               (selected == 1) ? "->" : "  ");
    scr_printf("      -> View full console identity and save DEBUG_LOG.TXT to USB\n\n");

    scr_printf(" %s [3] Advanced Tools & Manual Operations...\n",
               (selected == 2) ? "->" : "  ");
    scr_printf("      -> NVRAM backup, restore from memory or USB file\n\n");

    scr_printf(" %s [4] Exit to OSD / Browser\n\n",
               (selected == 3) ? "->" : "  ");

    scr_printf("-----------------------------------------------------\n");
    if (g_mecha_ver[1] == 1) {
      scr_printf(" [!] MechaCon v1.%02d detected - ROM dump NOT supported\n", g_mecha_ver[2]);
    } else if (g_storage_ready) {
      scr_printf(" Status: Ready to dump to %s/\n", g_dump_dir);
    } else {
      scr_printf(" Status: %s/ [!] Insert USB flash drive (FAT32/exFAT)\n", g_dump_dir);
    }
    scr_printf(" NVRAM State : %s\n",
               g_nvram_backed_up ? "[BACKED UP]" : "[NOT BACKED UP]");

    u32 btn = 0;
    while (!(btn = read_pad_click())) {
      for (volatile int d = 0; d < 10000; d++)
        ;
    }

    if (btn & PAD_UP) {
      selected = (selected - 1 + menu_items) % menu_items;
    } else if (btn & PAD_DOWN) {
      selected = (selected + 1) % menu_items;
    } else if (btn & PAD_CROSS) {
      log_printf("[UI] User selected menu item [%d]\n", selected + 1);
      switch (selected) {
      case 0:
        full_dump_and_verify_action();
        break;
      case 1:
        show_system_info();
        break;
      case 2:
        advanced_tools_menu();
        break;
      case 3:
        return 0;
      }
    }
  }

  return 0;
}
