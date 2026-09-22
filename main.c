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

static void print_hex_block(u16 base_addr, const u8 *data, int len) {
  for (int i = 0; i < len; i += 16) {
    scr_printf(" %04X: ", base_addr + i);
    for (int j = 0; j < 16; j++) {
      if (i + j < len)
        scr_printf("%02X ", data[i + j]);
      else
        scr_printf("   ");
    }
    scr_printf("|");
    for (int j = 0; j < 16; j++) {
      if (i + j < len) {
        u8 b = data[i + j];
        char c = (b >= 32 && b <= 126) ? (char)b : '.';
        scr_printf("%c", c);
      }
    }
    scr_printf("|\n");
  }
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

// Menu 3: Probe Config Overflow & RAM Snapshot (diagnostic)
static void probe_config_overflow_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("      MechaCon RAM Snapshot & Overflow Diagnostic    \n");
  scr_printf("=====================================================\n\n");

  log_printf("[PROBE] Starting safe read-only RAM snapshot...\n");
  scr_printf(" [*] Step 1: Performing 256-byte Read-Only RAM Snapshot...\n");
  scr_printf(
      "     SCMD 0x40 (read mode 0, region 2, 16 blocks) -> 16x SCMD 0x41\n");

  u8 probe256[256];
  memset(probe256, 0, sizeof(probe256));
  u8 probe_stat = 0;
  int read_errs = mecha_read_ram_probe(2, 16, probe256, &probe_stat);

  if (read_errs < 0) {
    scr_printf(
        " [-] Failed to open config region for reading (stat: 0x%02X)\n\n",
        probe_stat);
    log_printf("[PROBE] Failed to open config session: stat=0x%02X\n",
               probe_stat);
  } else {
    scr_printf(" [+] Snapshot acquired! (%d block read errors)\n", read_errs);
    log_printf("[PROBE] 256-byte RAM snapshot read with %d errors\n",
               read_errs);

    // Analyze blocks 8-15
    int non_zero_overflow = 0;
    for (int i = 112; i < 256; i++) {
      if (probe256[i] != 0)
        non_zero_overflow++;
    }

    if (non_zero_overflow == 0) {
      scr_printf(
          " [!] BLOCKS 8-15 ARE ALL ZEROES (Signature of CXP101064 / 10K!)\n");
      scr_printf(
          "     EEPROM worker fields are NOT at offset +0xB0 in this chip.\n");
      log_printf("[PROBE] Diagnosis: Blocks 8-15 all zero (CXP101064 / MD 1.36 "
                 "layout)\n");
    } else {
      scr_printf(" [+] Active RAM structures detected in overflow region (+%d "
                 "bytes)!\n",
                 non_zero_overflow);
      scr_printf("     Matches CXP102064 / CXP103049 layout signature.\n");
      log_printf(
          "[PROBE] Diagnosis: Active RAM structures found in overflow area\n");
      scr_printf(" Worker region preview (Blocks 11-12):\n");
      print_hex_block(0x00B0, &probe256[0xB0], 32);
    }

    // Save PROBE00.BIN and PROBE00.TXT
    update_dump_directory();
    char probe_bin[256], probe_txt[256];
    snprintf(probe_bin, sizeof(probe_bin), "%s/PROBE00.BIN", g_dump_dir);
    snprintf(probe_txt, sizeof(probe_txt), "%s/PROBE00.TXT", g_dump_dir);

    int fd = open(probe_bin, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
      int wr_ok = (write_checked(fd, probe256, sizeof(probe256), probe_bin) == 0);
      close(fd);
      if (wr_ok) {
        scr_printf(" [+] Saved RAM snapshot: %s\n", probe_bin);
        log_printf("[FILE] Saved %s\n", probe_bin);
      } else {
        scr_printf(" [!] Warning: %s may be truncated!\n", probe_bin);
      }
    }

    FILE *ft = fopen(probe_txt, "w");
    if (ft) {
      fprintf(ft, "SPC970 RAM PROBE SNAPSHOT\n");
      fprintf(ft, "raw_scmd_03_00=%02X %02X %02X %02X\n", g_mecha_ver[0],
              g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[3]);
      fprintf(ft, "probe_bytes=256\n");
      fprintf(ft, "overflow_active_bytes=%d\n", non_zero_overflow);
      fclose(ft);
    }
  }

  // Also take an extended 512-byte snapshot (32 blocks) to find where CXP101064
  // worker is
  scr_printf("\n [*] Step 2: Extended 512-byte RAM Snapshot (32 blocks)...\n");
  u8 probe512[512];
  memset(probe512, 0, sizeof(probe512));
  read_errs = mecha_read_ram_probe(2, 32, probe512, &probe_stat);
  if (read_errs >= 0) {
    char ext_bin[256];
    snprintf(ext_bin, sizeof(ext_bin), "%s/PROBE_EXT512.BIN", g_dump_dir);
    int fd_ext = open(ext_bin, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_ext >= 0) {
      int wr_ok = (write_checked(fd_ext, probe512, sizeof(probe512), ext_bin) == 0);
      close(fd_ext);
      if (wr_ok) {
        scr_printf(" [+] Saved extended snapshot: %s\n", ext_bin);
        log_printf("[FILE] Saved %s\n", ext_bin);
      } else {
        scr_printf(" [!] Warning: %s may be truncated!\n", ext_bin);
      }
    }
  }

  // Step 3: Test SCMD 0x42 Write Acceptance (bounds test)
  scr_printf("\n [*] Step 3: Testing SCMD 0x42 overflow write acceptance...\n");
  u8 open_stat = 0xFF;
  int ret = mecha_open_config(1, 2, 0, &open_stat);
  if (ret != 0) {
    u8 close_st = 0;
    mecha_close_config(&close_st);
    ret = mecha_open_config(1, 2, 0, &open_stat);
  }
  scr_printf(" SCMD 0x40 open (write, reg 2, count 0): stat=0x%02X\n",
             open_stat);
  log_printf(
      "[PROBE] SCMD 0x40 open (write, reg 2, count 0): ret=%d, stat=0x%02X\n",
      ret, open_stat);

  if (ret == 0) {
    u8 test_block[16];
    memset(test_block, 0, 16);
    u8 wr_stat = 0xFF;

    // Block 0
    int w0 = mecha_write_config(test_block, &wr_stat);
    scr_printf(" SCMD 0x42 Block 0 write: ret=%d stat=0x%02X\n", w0, wr_stat);
    log_printf("[PROBE] SCMD 0x42 Block 0 write: ret=%d stat=0x%02X\n", w0,
               wr_stat);

    // Blocks 1 to 6
    for (int b = 1; b <= 6; b++) {
      mecha_write_config(test_block, &wr_stat);
    }

    // Block 7 (first overflow block past 0x70 valid buffer)
    // Must preserve 0xFF, 0x67 in bytes 0-1 so 0x19B0 state flag is retained!
    u8 blk7[16] = {0};
    blk7[0] = 0xFF;
    blk7[1] = 0x67;
    int w7 = mecha_write_config(blk7, &wr_stat);
    scr_printf(" SCMD 0x42 Block 7 (Overflow): ret=%d stat=0x%02X\n", w7,
               wr_stat);
    log_printf("[PROBE] SCMD 0x42 Block 7 (Overflow): ret=%d stat=0x%02X\n", w7,
               wr_stat);
    if (wr_stat == 0x00) {
      scr_printf(
          " [+] OVERFLOW WRITES ACCEPTED! Hardware has no bounds check.\n");
      log_printf("[PROBE] Overflow writes accepted (no bounds check)\n");
    } else {
      scr_printf(
          " [-] OVERFLOW WRITE REJECTED! Hardware returned error 0x%02X.\n",
          wr_stat);
      log_printf("[PROBE] Overflow write rejected: stat=0x%02X\n", wr_stat);
    }

    u8 close_stat = 0;
    mecha_close_config(&close_stat);
    log_printf("[PROBE] SCMD 0x43 close: stat=0x%02X\n", close_stat);
  } else {
    log_printf("[PROBE] SCMD 0x40 write mode 1 rejected: stat=0x%02X\n",
               open_stat);
  }

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  wait_for_cross();
}

// Menu 4: Safe Hardware Diagnostic & RAM Mapping (All Regions & Target SCMDs)
static void full_hardware_mapping_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("   MechaCon Hardware Diagnostic & Safe RAM Mapping   \n");
  scr_printf("=====================================================\n\n");

  log_printf("[MAP] Starting safe hardware and RAM mapping (Regions 0-7 & Target SCMDs)...\n");
  update_dump_directory();

  // 1. Ensure NVRAM backup
  if (!g_nvram_backed_up) {
    scr_printf(" [*] Step 1/4: Backing up NVRAM (1024 Bytes)...\n");
    int read_errs = mecha_backup_nvram(g_nvram_backup, draw_progress_bar);
    if (read_errs == 0) {
      g_nvram_backed_up = 1;
      g_serial = extract_serial_from_nvram(g_nvram_backup, &g_emcs);
      g_model_id = extract_model_id_from_nvram(g_nvram_backup);
      update_dump_directory();
    }
    char nvram_path[256];
    snprintf(nvram_path, sizeof(nvram_path), "%s/NVRAM.BIN", g_dump_dir);
    int fd = open(nvram_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
      if (write_checked(fd, g_nvram_backup, NVRAM_SIZE_BYTES, nvram_path) == 0) {
        log_printf("[FILE] Successfully saved %s\n", nvram_path);
      }
      close(fd);
    }
  } else {
    scr_printf(" [*] Step 1/4: NVRAM already backed up.\n");
  }

  // 2. Scan Config Regions 0..7 safely (Dump whatever responds, skip unopened cleanly)
  scr_printf("\n [*] Step 2/4: Scanning Config Regions 0 through 7...\n");

  struct region_scan_result {
    int supported_std;
    int supported_uf;
    u8 stat_std;
    u8 stat_uf;
    int non_zero_low;
    int non_zero_overflow;
    int potential_rom_ptrs;
  } reg_results[8];
  memset(reg_results, 0, sizeof(reg_results));

  for (int r = 0; r < 8; r++) {
    // Standard read: 4 blocks (64 bytes)
    u8 buf64[64];
    memset(buf64, 0, sizeof(buf64));
    u8 st_std = 0xFF;
    int err_std = mecha_read_ram_probe_blocks((u8)r, 4, 4, buf64, &st_std);
    if (err_std == 0 && st_std == 0x00) {
      reg_results[r].supported_std = 1;
      reg_results[r].stat_std = st_std;
      char path64[256];
      snprintf(path64, sizeof(path64), "%s/REGION%d_STD64.BIN", g_dump_dir, r);
      int fd = open(path64, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd >= 0) {
        write_checked(fd, buf64, sizeof(buf64), path64);
        close(fd);
      }

      mecha_delay(2000);

      // 16 blocks (256 bytes) read with explicit block count = 16 (prevents bus hangs)
      u8 buf256[256];
      memset(buf256, 0, sizeof(buf256));
      u8 st_uf = 0xFF;
      int err_uf = mecha_read_ram_probe_blocks((u8)r, 16, 16, buf256, &st_uf);
      if (err_uf == 0 && st_uf == 0x00) {
        reg_results[r].supported_uf = 1;
        reg_results[r].stat_uf = st_uf;
        for (int i = 0; i < 112; i++) {
          if (buf256[i] != 0)
            reg_results[r].non_zero_low++;
        }
        for (int i = 112; i < 256; i++) {
          if (buf256[i] != 0)
            reg_results[r].non_zero_overflow++;
        }
        for (int i = 0; i <= 256 - 4; i++) {
          u32 val = buf256[i] | (buf256[i + 1] << 8) | (buf256[i + 2] << 16);
          if ((val >= 0xFC0000 && val <= 0xFFFFFF) ||
              (val >= 0xFD0000 && val <= 0xFFFFFF)) {
            reg_results[r].potential_rom_ptrs++;
          }
        }
        char path256[256];
        snprintf(path256, sizeof(path256), "%s/REGION%d_RAM256.BIN", g_dump_dir, r);
        int fd = open(path256, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
          write_checked(fd, buf256, sizeof(buf256), path256);
          close(fd);
        }
      }
    } else {
      reg_results[r].stat_std = st_std;
    }

    scr_printf("  Region %d: std=%s (stat 0x%02X) | 256B=%s [NZ low:%d, over:%d, ptrs:%d]\n",
               r, reg_results[r].supported_std ? "OK" : "--", st_std,
               reg_results[r].supported_uf ? "OK" : "--",
               reg_results[r].non_zero_low, reg_results[r].non_zero_overflow,
               reg_results[r].potential_rom_ptrs);
    log_printf("[MAP] Region %d: std=%d (stat=0x%02X), 256B=%d (stat=0x%02X), low_nz=%d, over_nz=%d, ptrs=%d\n",
               r, reg_results[r].supported_std, st_std,
               reg_results[r].supported_uf, reg_results[r].stat_uf,
               reg_results[r].non_zero_low, reg_results[r].non_zero_overflow,
               reg_results[r].potential_rom_ptrs);

    mecha_delay(2000);
  }

  // 3. Query Target Safe Diagnostic SCMDs (No blind sweeps, no motor/laser subcommands)
  scr_printf("\n [*] Step 3/4: Querying Target Diagnostic SCMDs (Safe Set)...\n");
  char scmd_rpt_path[256];
  snprintf(scmd_rpt_path, sizeof(scmd_rpt_path), "%s/TARGET_SCMD_REPORT.TXT", g_dump_dir);
  FILE *fp_scmd = fopen(scmd_rpt_path, "w");
  if (fp_scmd) {
    fprintf(fp_scmd, "=== Target Safe SCMD Diagnostics ===\n");
    fprintf(fp_scmd, "MechaCon: v%d.%02d (Reg 0x%02X, Rev 0x%02X) | Chip: %s\n\n",
            g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0], g_mecha_ver[3],
            get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
  }

  struct target_scmd_probe {
    const char *name;
    u8 cmd;
    u8 in_len;
    u8 in_bytes[4];
    u8 exp_out_len;
  } target_cmds[] = {
    { "SCMD 0x01    (Drive Status)", 0x01, 0, { 0x00 }, 1 },
    { "SCMD 0x03-00 (MechaCon Ver)", 0x03, 1, { 0x00 }, 4 },
    { "SCMD 0x03-01 (DSP Version)",   0x03, 1, { 0x01 }, 1 },
    { "SCMD 0x03-45 (Console ID)",   0x03, 1, { 0x45 }, 8 },
    { "SCMD 0x08    (Hardware RTC)", 0x08, 0, { 0x00 }, 8 },
    { "SCMD 0x0A    (Read NVM w0)",  0x0A, 2, { 0x00, 0x00 }, 3 },
    { "SCMD 0x12    (i.Link ID)",    0x12, 0, { 0x00 }, 8 },
  };
  int num_target_cmds = sizeof(target_cmds) / sizeof(target_cmds[0]);

  for (int i = 0; i < num_target_cmds; i++) {
    u8 out[16] = {0};
    int ret = sceCdApplySCmd(target_cmds[i].cmd, target_cmds[i].in_bytes, target_cmds[i].in_len, out);
    scr_printf("  %-28s: ret=%d (stat=0x%02X)\n", target_cmds[i].name, ret, out[0]);
    log_printf("[MAP_SCMD] %s: ret=%d | %02X %02X %02X %02X %02X %02X %02X %02X\n",
               target_cmds[i].name, ret, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
    if (fp_scmd) {
      fprintf(fp_scmd, "%-28s: ret=%d | ", target_cmds[i].name, ret);
      for (int k = 0; k < target_cmds[i].exp_out_len; k++) {
        fprintf(fp_scmd, "%02X ", out[k]);
      }
      fprintf(fp_scmd, "\n");
    }
    mecha_delay(5000); // 5ms safe delay between SCMDs
  }
  if (fp_scmd) fclose(fp_scmd);
  scr_printf("  Target SCMD queries completed safely.\n");

  // 4. Generate Comprehensive Mapping Report
  scr_printf("\n [*] Step 4/4: Compiling MECHA_MAP_REPORT.TXT...\n");
  char rpt_path[256];
  snprintf(rpt_path, sizeof(rpt_path), "%s/MECHA_MAP_REPORT.TXT", g_dump_dir);
  FILE *fr = fopen(rpt_path, "w");
  if (fr) {
    fprintf(fr, "=====================================================\n");
    fprintf(fr, "      MechaCon Hardware & RAM Mapping Report         \n");
    fprintf(fr, "=====================================================\n\n");
    fprintf(fr, "Model Name (SCMD 0x17): %s\n", g_cdvd_model);
    fprintf(fr, "MechaCon Chip Part No : %s\n", get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
    fprintf(fr, "MechaCon Version      : MD 1.39 v%d.%02d (Region: 0x%02X [%s%s], Rev 0x%02X)\n",
            g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0], get_region_name(g_mecha_ver[0]),
            (g_mecha_ver[0] == 0x80 || g_mecha_ver[0] == 0x81) ? " / DEX" : "",
            g_mecha_ver[3]);
    fprintf(fr, "DSP Version (0x03-01) : 0x%02X\n", g_dsp_ver);
    fprintf(fr, "BIOS ROMVER           : %s\n", g_romver);
    fprintf(fr, "Model ID (NVRAM)      : 0x%04X (%s)\n", g_model_id, get_model_id_desc(g_model_id));
    fprintf(fr, "Serial Number (NVRAM) : %07u (EMCS: 0x%02X)\n\n", g_serial, g_emcs);

    fprintf(fr, "--- Config Regions Sweep (SCMD 0x40 / 0x41) ---\n");
    fprintf(fr, "Region | Std 4-Blk | 256-Byte Read | Low Non-Zero | Overflow Non-Zero | ROM Pointers\n");
    fprintf(fr, "-------+-----------+---------------+--------------+-------------------+-------------\n");
    for (int r = 0; r < 8; r++) {
      fprintf(fr, "  %2d   |    %s     |      %s      |     %3d      |        %3d        |     %3d\n",
              r,
              reg_results[r].supported_std ? "OK" : "--",
              reg_results[r].supported_uf ? "OK" : "--",
              reg_results[r].non_zero_low,
              reg_results[r].non_zero_overflow,
              reg_results[r].potential_rom_ptrs);
    }
    fprintf(fr, "\n--- Diagnostic Findings ---\n");
    int best_region = -1;
    int max_over = 0;
    for (int r = 0; r < 8; r++) {
      if (reg_results[r].non_zero_overflow > max_over) {
        max_over = reg_results[r].non_zero_overflow;
        best_region = r;
      }
    }
    if (best_region >= 0 && max_over > 0) {
      fprintf(fr, "[+] Active overflow structures found in Region %d (+%d bytes)!\n", best_region, max_over);
      fprintf(fr, "    This region is the primary candidate for worker parameters and ROM staging.\n");
    } else {
      fprintf(fr, "[!] No standard overflow structures detected in probed regions.\n");
      fprintf(fr, "    Worker is either isolated, dynamically armed, or uses dedicated command.\n");
    }
    fclose(fr);
    scr_printf(" [+] Report generated: %s\n", rpt_path);
    log_printf("[MAP] Report written to %s\n", rpt_path);
  }

  // Save debug log
  char log_path_out[256];
  snprintf(log_path_out, sizeof(log_path_out), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path_out);

  scr_printf("\n[+] Diagnostic probe complete! All files saved to:\n    %s/\n", g_dump_dir);
  wait_for_cross();
}

// Menu 5: Full Auto Dump & Verification
static void full_dump_and_verify_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("      Full Auto ROM Dump, Checksum & Restore        \n");
  scr_printf("=====================================================\n\n");

  log_printf("[AUTO_DUMP] Full automated workflow initiated...\n");

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

// Menu 6: EEPROM Worker Discovery & Flush Diagnostics
static void worker_flush_diagnostics_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("   EEPROM Worker Discovery & Flush Diagnostics      \n");
  scr_printf("=====================================================\n\n");

  log_printf("[DISCOVERY] Worker flush discovery action initiated...\n");

  // Step 1: Ensure NVRAM backup exists
  if (!g_nvram_backed_up) {
    scr_printf(" [*] Step 1/3: Backing up NVRAM first for safety...\n");
    int read_errs = mecha_backup_nvram(g_nvram_backup, draw_progress_bar);
    // Only trust this buffer for a later restore if every word was actually read.
    g_nvram_backed_up = (read_errs == 0);
    if (read_errs > 0) {
      log_printf("[WARN] NVRAM backup had %d word errors\n", read_errs);
    }
    g_serial = extract_serial_from_nvram(g_nvram_backup, &g_emcs);
    g_model_id = extract_model_id_from_nvram(g_nvram_backup);
    update_dump_directory();

    char nvram_path[256];
    snprintf(nvram_path, sizeof(nvram_path), "%s/NVRAM.BIN", g_dump_dir);
    int fd_nvm = open(nvram_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_nvm >= 0) {
      write_checked(fd_nvm, g_nvram_backup, NVRAM_SIZE_BYTES, nvram_path);
      close(fd_nvm);
    }
  } else {
    scr_printf(" [*] Step 1/3: NVRAM is already safely backed up.\n");
  }

  update_dump_directory();

  // Step 2: Region 2 Controlled Flush Test
  scr_printf("\n [*] Step 2/3: Performing Region 2 (0x1940) Controlled Flush...\n");
  scr_printf("     Writing 4 original blocks back to trigger internal worker...\n");

  u8 pre_r2[256];
  memset(pre_r2, 0, sizeof(pre_r2));
  u8 post_r2[256];
  memset(post_r2, 0, sizeof(post_r2));
  struct worker_flush_diff diff_r2;
  memset(&diff_r2, 0, sizeof(diff_r2));

  int ret_r2 = mecha_worker_flush_probe(2, pre_r2, post_r2, &diff_r2);
  if (ret_r2 == 0) {
    scr_printf(" [+] Region 2 Flush Completed! Total RAM bytes changed: %d\n", diff_r2.total_changed_bytes);
    scr_printf("     Overflow Area Changes (Blocks 8-15): %d bytes\n", diff_r2.overflow_changed_bytes);

    if (diff_r2.flags_detected_offset > 0) {
      scr_printf(" [!] WORKER FLAGS DETECTED AT RAM: 0x%04X!\n", diff_r2.flags_detected_offset);
    }

    // Save PRE and POST binaries
    char pre_path[256], post_path[256];
    snprintf(pre_path, sizeof(pre_path), "%s/FLUSH_PRE_REG2.BIN", g_dump_dir);
    snprintf(post_path, sizeof(post_path), "%s/FLUSH_POST_REG2.BIN", g_dump_dir);
    int fd = open(pre_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write_checked(fd, pre_r2, 256, pre_path); close(fd); }
    fd = open(post_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write_checked(fd, post_r2, 256, post_path); close(fd); }
  } else {
    scr_printf(" [-] Region 2 Flush Probe failed with code %d\n", ret_r2);
  }

  // Step 3: Region 1 Controlled Flush Test
  scr_printf("\n [*] Step 3/3: Performing Region 1 (0x18D0) Controlled Flush...\n");
  u8 pre_r1[256];
  memset(pre_r1, 0, sizeof(pre_r1));
  u8 post_r1[256];
  memset(post_r1, 0, sizeof(post_r1));
  struct worker_flush_diff diff_r1;
  memset(&diff_r1, 0, sizeof(diff_r1));

  int ret_r1 = mecha_worker_flush_probe(1, pre_r1, post_r1, &diff_r1);
  if (ret_r1 == 0) {
    scr_printf(" [+] Region 1 Flush Completed! Total RAM bytes changed: %d\n", diff_r1.total_changed_bytes);
    scr_printf("     Overflow Area Changes (Blocks 8-15): %d bytes\n", diff_r1.overflow_changed_bytes);

    char pre_path[256], post_path[256];
    snprintf(pre_path, sizeof(pre_path), "%s/FLUSH_PRE_REG1.BIN", g_dump_dir);
    snprintf(post_path, sizeof(post_path), "%s/FLUSH_POST_REG1.BIN", g_dump_dir);
    int fd = open(pre_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write_checked(fd, pre_r1, 256, pre_path); close(fd); }
    fd = open(post_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write_checked(fd, post_r1, 256, post_path); close(fd); }
  } else {
    scr_printf(" [-] Region 1 Flush Probe failed with code %d\n", ret_r1);
  }

  // Generate WORKER_DISCOVERY.TXT report
  char rpt_path[256];
  snprintf(rpt_path, sizeof(rpt_path), "%s/WORKER_DISCOVERY.TXT", g_dump_dir);
  FILE *fr = fopen(rpt_path, "w");
  if (fr) {
    fprintf(fr, "=====================================================\n");
    fprintf(fr, "      MechaCon EEPROM Worker Discovery Report        \n");
    fprintf(fr, "=====================================================\n\n");
    fprintf(fr, "MechaCon Chip Part No : %s\n", get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
    fprintf(fr, "MechaCon Version      : MD 1.39 v%d.%02d (Region: 0x%02X, Rev 0x%02X)\n\n",
            g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0], g_mecha_ver[3]);
    fprintf(fr, "--- Region 2 Flush (Base: 0x1940) ---\n");
    fprintf(fr, "Outcome         : %s\n", (ret_r2 == 0) ? "SUCCESS" : "FAILED");
    fprintf(fr, "Total Changes   : %d bytes\n", diff_r2.total_changed_bytes);
    fprintf(fr, "Overflow Changes: %d bytes (Blocks 8-15: 0x19C0-0x1A3F)\n", diff_r2.overflow_changed_bytes);
    for (int i = 0; i < 256; i++) {
      if (pre_r2[i] != post_r2[i]) {
        fprintf(fr, "  RAM 0x%04X (blk %2d, byte %2d): 0x%02X -> 0x%02X\n",
                0x1940 + i, i / 16, i % 16, pre_r2[i], post_r2[i]);
      }
    }

    fprintf(fr, "\n--- Region 1 Flush (Base: 0x18D0) ---\n");
    fprintf(fr, "Outcome         : %s\n", (ret_r1 == 0) ? "SUCCESS" : "FAILED");
    fprintf(fr, "Total Changes   : %d bytes\n", diff_r1.total_changed_bytes);
    fprintf(fr, "Overflow Changes: %d bytes\n", diff_r1.overflow_changed_bytes);
    for (int i = 0; i < 256; i++) {
      if (pre_r1[i] != post_r1[i]) {
        fprintf(fr, "  RAM 0x%04X (blk %2d, byte %2d): 0x%02X -> 0x%02X\n",
                0x18D0 + i, i / 16, i % 16, pre_r1[i], post_r1[i]);
      }
    }
    fclose(fr);
    scr_printf("\n [+] Telemetry report saved: %s\n", rpt_path);
    log_printf("[DISCOVERY] Report saved to %s\n", rpt_path);
  }

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  scr_printf(" [+] Flush discovery complete! All files saved to USB:\n    %s/\n", g_dump_dir);
  wait_for_cross();
}

// Menu: EXPERIMENTAL Extended Write-Reach Probe
//
// The known worker layouts (mecha.h/mecha.c) all land inside the first 16
// blocks (256 bytes) past the Config Region buffer base, which is as far as
// the real exploit (mecha_exploit_stage_chunk) and the read-only RAM probes
// (mecha_read_ram_probe*) ever reach. On some chips (e.g. real CXP101064
// v1.02/v1.03 hardware) none of those layouts land on anything live, and the
// read-mode probe has been confirmed to wrap back to block 0 after 16 blocks
// rather than walking further into RAM - so it can't be used to look deeper.
//
// This runs mecha_probe_extended_write_reach(), which never arms the EEPROM
// worker, to find out whether the WRITE side also stops at block 15 or keeps
// accepting blocks further out. Still touches live MechaCon RAM, so NVRAM is
// backed up first and restored afterward exactly like every other action
// here that opens a write-mode Config session.
static void extended_write_reach_probe_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("   EXPERIMENTAL: Extended Write-Reach Probe          \n");
  scr_printf("=====================================================\n\n");
  scr_printf(" [!] This writes neutral (all-zero) blocks past the known\n");
  scr_printf("     16-block window to see how far SCMD 0x42 accepts writes.\n");
  scr_printf("     The EEPROM-copy worker is never armed by this probe.\n\n");

  log_printf("[REACH_PROBE] Extended write-reach probe initiated...\n");

  // Step 1: Ensure NVRAM backup exists
  if (!g_nvram_backed_up) {
    scr_printf(" [*] Step 1/3: Backing up NVRAM first for safety...\n");
    int read_errs = mecha_backup_nvram(g_nvram_backup, draw_progress_bar);
    g_nvram_backed_up = (read_errs == 0);
    if (read_errs > 0) {
      log_printf("[WARN] NVRAM backup had %d word errors\n", read_errs);
      scr_printf(" [!] NVRAM backup incomplete (%d errors). Aborting for safety.\n", read_errs);
      wait_for_cross();
      return;
    }
    g_serial = extract_serial_from_nvram(g_nvram_backup, &g_emcs);
    g_model_id = extract_model_id_from_nvram(g_nvram_backup);
    update_dump_directory();

    char nvram_path[256];
    snprintf(nvram_path, sizeof(nvram_path), "%s/NVRAM.BIN", g_dump_dir);
    int fd_nvm = open(nvram_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_nvm >= 0) {
      write_checked(fd_nvm, g_nvram_backup, NVRAM_SIZE_BYTES, nvram_path);
      close(fd_nvm);
    }
  } else {
    scr_printf(" [*] Step 1/3: NVRAM is already safely backed up.\n");
  }
  update_dump_directory();

  // Step 2: Buffer the live Config Region 2 window (blocks 0-15) so the probe
  // can rewrite them verbatim before touching new territory.
  scr_printf("\n [*] Step 2/3: Buffering live Config Region 2 window...\n");
  if (mecha_init_config_window() != 0) {
    scr_printf(" [-] Failed to buffer config window. Aborting - nothing was written.\n");
    log_printf("[REACH_PROBE] Aborted: mecha_init_config_window() failed.\n");
    wait_for_cross();
    return;
  }

  // Step 3: Run the probe against Region 2, then Region 1 (map sweeps have
  // shown a handful of non-zero overflow bytes in Region 1 on some v1 units).
  scr_printf("\n [*] Step 3/3: Probing extended write reach (Region 2, then Region 1)...\n");
  const int max_extra_blocks = 32; // 32*16 = 512 bytes past block 15
  int accepted_r2 = mecha_probe_extended_write_reach(2, max_extra_blocks, draw_progress_bar);
  scr_printf(" Region 2: ");
  if (accepted_r2 < 0) {
    scr_printf("probe setup failed (code %d)\n", accepted_r2);
  } else {
    scr_printf("%d extra blocks accepted (0x%03X bytes past block 15)\n",
               accepted_r2, accepted_r2 * 16);
  }

  // Re-buffer before touching Region 1, since Region 1 has its own 16-block
  // window and mecha_init_config_window() only ever reads Region 2.
  u8 region1_window[256];
  u8 r1_status = 0;
  int r1_read = mecha_read_ram_probe(1, 16, region1_window, &r1_status);
  int accepted_r1 = -9;
  if (r1_read == 0 && r1_status == 0x00) {
    memcpy(g_config_window, region1_window, 256);
    accepted_r1 = mecha_probe_extended_write_reach(1, max_extra_blocks, draw_progress_bar);
  } else {
    log_printf("[REACH_PROBE] Skipping Region 1: baseline read failed (err=%d, stat=0x%02X)\n",
               r1_read, r1_status);
  }
  scr_printf(" Region 1: ");
  if (accepted_r1 < 0) {
    scr_printf("skipped/failed (code %d)\n", accepted_r1);
  } else {
    scr_printf("%d extra blocks accepted (0x%03X bytes past block 15)\n",
               accepted_r1, accepted_r1 * 16);
  }

  // Always restore NVRAM and clean overflow RAM afterward, exactly like every
  // other action that opens a write-mode Config session.
  scr_printf("\n [*] Restoring NVRAM and cleaning overflow RAM...\n");
  int rest_err = mecha_restore_nvram(g_nvram_backup, draw_progress_bar);
  mecha_clean_overflow_ram();
  log_printf("[REACH_PROBE] Post-probe NVRAM restore: %d errors\n", rest_err);

  // Save a small report
  update_dump_directory();
  char rpt_path[256];
  snprintf(rpt_path, sizeof(rpt_path), "%s/WRITE_REACH_PROBE.TXT", g_dump_dir);
  FILE *fr = fopen(rpt_path, "w");
  if (fr) {
    fprintf(fr, "=== Extended Write-Reach Probe (EXPERIMENTAL) ===\n");
    fprintf(fr, "MechaCon: v%d.%02d (Reg 0x%02X, Rev 0x%02X) | Chip: %s\n\n",
            g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0], g_mecha_ver[3],
            get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
    fprintf(fr, "Region 2: extra blocks accepted past block 15 = %d (0x%03X bytes)\n",
            accepted_r2, accepted_r2 < 0 ? 0 : accepted_r2 * 16);
    fprintf(fr, "Region 1: extra blocks accepted past block 15 = %d (0x%03X bytes)\n",
            accepted_r1, accepted_r1 < 0 ? 0 : accepted_r1 * 16);
    fprintf(fr, "\nSee DEBUG_LOG.TXT [REACH_PROBE] lines for the per-block status trace.\n");
    fclose(fr);
    scr_printf(" [+] Saved: %s\n", rpt_path);
  }

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  scr_printf("\n [+] Probe complete! All files saved to:\n    %s/\n", g_dump_dir);
  wait_for_cross();
}

// Menu: EXPERIMENTAL Deep Worker Candidate Scan
//
// Only meaningful after Extended Write-Reach Probe has confirmed writes are
// accepted at least up to the requested range here. Unlike that probe, this
// one DOES arm and trigger the EEPROM-copy worker on each candidate (that's
// the only way to tell whether a candidate position is real) - the safety
// net is that NVRAM is restored immediately after every single candidate,
// hit or not, before the next one runs. See mecha_scan_deep_worker_candidates()
// in mecha.c for the full rationale.
static void deep_worker_scan_action(void) {
  scr_clear();
  scr_printf("=====================================================\n");
  scr_printf("   EXPERIMENTAL: Deep Worker Candidate Scan          \n");
  scr_printf("=====================================================\n\n");
  scr_printf(" [!] This ARMS and TRIGGERS the EEPROM worker at each candidate\n");
  scr_printf("     block, unlike the write-reach probe. NVRAM is restored\n");
  scr_printf("     after every single candidate, hit or not.\n");
  scr_printf(" [!] Only run this after Extended Write-Reach Probe confirmed\n");
  scr_printf("     writes are accepted at least this far out.\n\n");

  log_printf("[DEEP_SCAN] Deep worker candidate scan initiated...\n");

  // Step 1: Ensure NVRAM backup exists
  if (!g_nvram_backed_up) {
    scr_printf(" [*] Step 1/3: Backing up NVRAM first for safety...\n");
    int read_errs = mecha_backup_nvram(g_nvram_backup, draw_progress_bar);
    g_nvram_backed_up = (read_errs == 0);
    if (read_errs > 0) {
      log_printf("[WARN] NVRAM backup had %d word errors\n", read_errs);
      scr_printf(" [!] NVRAM backup incomplete (%d errors). Aborting for safety.\n", read_errs);
      wait_for_cross();
      return;
    }
    g_serial = extract_serial_from_nvram(g_nvram_backup, &g_emcs);
    g_model_id = extract_model_id_from_nvram(g_nvram_backup);
    update_dump_directory();

    char nvram_path[256];
    snprintf(nvram_path, sizeof(nvram_path), "%s/NVRAM.BIN", g_dump_dir);
    int fd_nvm = open(nvram_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_nvm >= 0) {
      write_checked(fd_nvm, g_nvram_backup, NVRAM_SIZE_BYTES, nvram_path);
      close(fd_nvm);
    }
  } else {
    scr_printf(" [*] Step 1/3: NVRAM is already safely backed up.\n");
  }
  update_dump_directory();

  // Step 2: Buffer the live Config Region 2 window (blocks 0-15)
  scr_printf("\n [*] Step 2/3: Buffering live Config Region 2 window...\n");
  if (mecha_init_config_window() != 0) {
    scr_printf(" [-] Failed to buffer config window. Aborting - nothing was written.\n");
    log_printf("[DEEP_SCAN] Aborted: mecha_init_config_window() failed.\n");
    wait_for_cross();
    return;
  }

  // Step 3: Run the scan. Range chosen to bracket the estimated target
  // offset with margin, while staying well inside the ~32-block reach
  // already confirmed accepted on this chip.
  const int first_block = 16;
  const int last_block = 27; // covers offsets 0x100-0x1BF (256-447 bytes past base)
  const u32 rom_test_addr = (g_mecha_ver[1] >= 3) ? 0xFD0000 : 0xFC0000;

  scr_printf("\n [*] Step 3/3: Scanning blocks %d-%d (offsets 0x%03X-0x%03X)...\n",
             first_block, last_block, first_block * 16, last_block * 16);
  u16 preview[8] = { 0 };
  int hit_block = mecha_scan_deep_worker_candidates(rom_test_addr, first_block, last_block,
                                                      g_nvram_backup, preview, draw_progress_bar);

  if (hit_block >= 0) {
    scr_printf("\n [+][+][+] HIT at block %d (offset 0x%03X)! [+][+][+]\n", hit_block, hit_block * 16);
    scr_printf("     Preview: %04X %04X %04X %04X %04X %04X %04X %04X\n",
               preview[0], preview[1], preview[2], preview[3],
               preview[4], preview[5], preview[6], preview[7]);
    log_printf("[DEEP_SCAN] Result: HIT at block %d (offset 0x%03X)\n", hit_block, hit_block * 16);
  } else if (hit_block == -2) {
    scr_printf("\n [!] MechaCon Config-session got wedged mid-scan - NOT all of\n");
    scr_printf("     blocks %d-%d were actually tested (see log for which one).\n", first_block, last_block);
    scr_printf("     NVRAM itself is fine (restored below), but you should\n");
    scr_printf("     POWER-CYCLE the console before running this again.\n");
    log_printf("[DEEP_SCAN] Result: ABORTED EARLY (wedged) - range %d-%d incomplete\n", first_block, last_block);
  } else {
    scr_printf("\n [-] No hit in blocks %d-%d. See DEBUG_LOG.TXT [DEEP_SCAN] lines\n", first_block, last_block);
    scr_printf("     for the per-candidate preview trace (widen the range and retry).\n");
    log_printf("[DEEP_SCAN] Result: no hit in blocks %d-%d\n", first_block, last_block);
  }

  // Belt-and-suspenders: the scan already restores after every candidate,
  // but confirm/re-settle NVRAM state once more before returning to the menu.
  scr_printf("\n [*] Confirming NVRAM restore and cleaning overflow RAM...\n");
  int rest_err = mecha_restore_nvram(g_nvram_backup, draw_progress_bar);
  mecha_clean_overflow_ram();
  int mismatches = mecha_verify_nvram(g_nvram_backup, draw_progress_bar);
  log_printf("[DEEP_SCAN] Final NVRAM restore: %d errors, %d mismatches\n", rest_err, mismatches);
  if (mismatches == 0) {
    scr_printf(" [+] NVRAM confirmed 100%% restored.\n");
  } else {
    scr_printf(" [!] WARNING: %d word mismatches after restore - check DEBUG_LOG.TXT!\n", mismatches);
  }

  // Save a report
  update_dump_directory();
  char rpt_path[256];
  snprintf(rpt_path, sizeof(rpt_path), "%s/DEEP_SCAN_REPORT.TXT", g_dump_dir);
  FILE *fr = fopen(rpt_path, "w");
  if (fr) {
    fprintf(fr, "=== Deep Worker Candidate Scan (EXPERIMENTAL) ===\n");
    fprintf(fr, "MechaCon: v%d.%02d (Reg 0x%02X, Rev 0x%02X) | Chip: %s\n\n",
            g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0], g_mecha_ver[3],
            get_mechacon_chip_desc(g_mecha_ver[1], g_mecha_ver[2], g_mecha_ver[0]));
    fprintf(fr, "Scanned blocks %d-%d (offsets 0x%03X-0x%03X) at ROM test addr 0x%06X\n",
            first_block, last_block, first_block * 16, last_block * 16, (unsigned)rom_test_addr);
    if (hit_block >= 0) {
      fprintf(fr, "\nHIT at block %d (offset 0x%03X)\n", hit_block, hit_block * 16);
      fprintf(fr, "Preview words 0-7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
              preview[0], preview[1], preview[2], preview[3],
              preview[4], preview[5], preview[6], preview[7]);
    } else if (hit_block == -2) {
      fprintf(fr, "\nABORTED EARLY: MechaCon Config-session got wedged mid-scan.\n");
      fprintf(fr, "Not all blocks in %d-%d were tested - see DEBUG_LOG.TXT [DEEP_SCAN]\n", first_block, last_block);
      fprintf(fr, "for the last block that was actually attempted. Power-cycle the\n");
      fprintf(fr, "console before running this scan again.\n");
    } else {
      fprintf(fr, "\nNo hit in range (all blocks %d-%d were tested).\n", first_block, last_block);
    }
    fprintf(fr, "Post-scan NVRAM restore: %d errors, %d mismatches\n", rest_err, mismatches);
    fprintf(fr, "\nSee DEBUG_LOG.TXT [DEEP_SCAN] lines for the full per-candidate trace.\n");
    fclose(fr);
    scr_printf(" [+] Saved: %s\n", rpt_path);
  }

  char log_path[256];
  snprintf(log_path, sizeof(log_path), "%s/DEBUG_LOG.TXT", g_dump_dir);
  log_save_to_file(log_path);

  scr_printf("\n [+] Scan complete! All files saved to:\n    %s/\n", g_dump_dir);
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
  const int sub_items = 8;

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
    scr_printf(" %s [3] Config Overflow Quick Probe (Diagnostic)\n",
               (sub_selected == 2) ? "->" : "  ");
    scr_printf(" %s [4] EEPROM Worker Discovery & Flush Diagnostics\n",
               (sub_selected == 3) ? "->" : "  ");
    scr_printf(" %s [5] Extended Write-Reach Probe (EXPERIMENTAL)\n",
               (sub_selected == 4) ? "->" : "  ");
    scr_printf(" %s [6] Deep Worker Candidate Scan (EXPERIMENTAL)\n",
               (sub_selected == 5) ? "->" : "  ");
    scr_printf(" %s [7] Export Debug Log to USB Storage\n",
               (sub_selected == 6) ? "->" : "  ");
    scr_printf(" %s [8] Back to Main Menu\n\n",
               (sub_selected == 7) ? "->" : "  ");

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
        probe_config_overflow_action();
        break;
      case 3:
        worker_flush_diagnostics_action();
        break;
      case 4:
        extended_write_reach_probe_action();
        break;
      case 5:
        deep_worker_scan_action();
        break;
      case 6:
        save_debug_log_action();
        break;
      case 7:
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
      auto_action = 4;
    else if (strcmp(argv[i], "--probe") == 0)
      auto_action = 3;
    else if (strcmp(argv[i], "--backup") == 0)
      auto_action = 2;
    else if (strcmp(argv[i], "--info") == 0 || strcmp(argv[i], "--ident") == 0)
      auto_action = 1;
    else if (strcmp(argv[i], "--map") == 0)
      auto_action = 5;
    else if (strcmp(argv[i], "--flush") == 0)
      auto_action = 6;
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
    probe_config_overflow_action();
    return 0;
  } else if (auto_action == 4) {
    full_dump_and_verify_action();
    return 0;
  } else if (auto_action == 5) {
    full_hardware_mapping_action();
    return 0;
  } else if (auto_action == 6) {
    worker_flush_diagnostics_action();
    return 0;
  }

  int selected = 0;
  const int menu_items = 5;

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

    scr_printf(" %s [3] Full Hardware & RAM Mapping (All Regions)\n",
               (selected == 2) ? "->" : "  ");
    scr_printf("      -> Safe scan of Regions 0-7, RAM dumps & SCMD report\n\n");

    scr_printf(" %s [4] Advanced Tools & Manual Operations...\n",
               (selected == 3) ? "->" : "  ");
    scr_printf("      -> Standalone NVRAM backup, restore, worker & flush probes\n\n");

    scr_printf(" %s [5] Exit to OSD / Browser\n\n",
               (selected == 4) ? "->" : "  ");

    scr_printf("-----------------------------------------------------\n");
    if (g_storage_ready) {
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
        full_hardware_mapping_action();
        break;
      case 3:
        advanced_tools_menu();
        break;
      case 4:
        return 0;
      }
    }
  }

  return 0;
}
