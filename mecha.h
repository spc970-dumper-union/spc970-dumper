#ifndef MECHA_H
#define MECHA_H

#include <tamtypes.h>

#define NVRAM_SIZE_BYTES    1024
#define NVRAM_SIZE_WORDS    512
#define ROM_SIZE_BYTES_V2   262144  // 256 KB (Banks FC, FD, FE, FF)
#define ROM_SIZE_BYTES_V3   196608  // 192 KB (Active Banks FD, FE, FF)
#define ROM_SIZE_BYTES      ROM_SIZE_BYTES_V2

// ---- SPC970 Config/EEPROM RAM Layout (verified via Ghidra MechaCon_Analysis) ----
// In v2 MechaCon (2.04/2.06), Config Region 2 buffer starts at RAM 0x1940.
// 7 blocks x 16 bytes = 0x70 bytes of valid buffer (0x1940-0x19AF).
// Writes past block 6 overflow into adjacent RAM:
//   Blocks 0-6   (0x1940-0x19AF): Absorbed by valid config buffer
//   Blocks 7-10  (0x19B0-0x19EF): Padding / Drive state
//   Block 11     (0x19F0-0x19FF): Byte 6 = Worker flags (0x19F6, 0x03=busy+pending)
//                                 Bytes 8-11 = Source ROM pointer (0x19F8, 32-bit LE)
//   Block 12     (0x1A00-0x1A0F): Byte 12 = Offset (0x1A0C, 0x00)
//                                 Byte 13 = Word count (0x1A0D, 8-bit!)
//                                 Byte 14 = Worker state (0x1A0E, 0x01=WREN start)
//   Block 13     (0x1A10-0x1A1F): Bytes 0-1 = Target NVRAM word address (0x1A10, 16-bit LE)
// Note: Relative offset from buffer base to worker fields is invariant (0xB6 bytes = 182 bytes)
// across v1 (base 0x1956), v2 (base 0x1940), and v3 (base 0x1906).

#define CONFIG_REGION2_BUFFER_START  0x1940
#define CONFIG_REGION2_VALID_BLOCKS  7
#define CONFIG_OVERFLOW_BLOCK_SIZE   16

// Maximum words copied per worker cycle (worker count register 0x1A0D is 8-bit)
#define EXPLOIT_CHUNK_WORDS          128  // 128 words = 256 bytes per chunk

// EEPROM worker key field addresses in v2 RAM
#define RAM_EEPROM_TASK_FLAGS        0x19F6
#define RAM_EEPROM_SOURCE_PTR        0x19F8
#define RAM_EEPROM_TARGET_WORD       0x1A10

// Status callback for progress reporting
typedef void (*ProgressCallback)(int current, int total, const char *status);

// ---- SCMD primitives (verified against Ghidra MechaCon_Analysis.rep) ----
int mecha_init(void);

// SCMD 0x03, Subcmd 0x00: Returns 4 bytes (out[0]=Region, out[1]=Major, out[2]=Minor, out[3]=Format)
// Note: On DTL-H / DEX consoles, Region byte (out[0]) is 0x80, 0x81, or 0x82.
int mecha_get_version(u8 *out4, u8 *status);

// SCMD 0x03, Subcmd 0x01: Returns DSP version
int mecha_get_dsp_version(u8 *out_dsp, u8 *status);

// SCMD 0x03, Subcmd 0x45: Returns 8 bytes Console ID (status: 0=OK, 0x80=Error/Not loaded)
int mecha_read_console_id(u8 *out_console_id8, u8 *status);

// SCMD 0x08: Reads RTC hardware registers (out_rtc8 receives status + 7 register bytes)
int mecha_read_rtc(u8 *out_rtc8, u8 *status);

// SCMD 0x12: Returns 8 bytes i.Link ID (status: 0=OK, 0x80=Error)
int mecha_read_ilink_id(u8 *out_ilink_id8, u8 *status);

// SCMD 0x17: Reads 16-byte Model Name ASCII string via offsets 0 and 8 (status: 0=OK, 0x80=Error/Unsupported)
int mecha_read_model_name(char *out_model16, u8 *status);

// SCMD 0x40: Config area session open (in[0]=mode: 0=read/1=write, in[1]=region: 0..2, in[2]=block_count)
// Status returned in *status: 0x00=Success, 0x80=Error
int mecha_open_config(u8 mode, u8 region, u8 block_count, u8 *status);

// SCMD 0x41: Read 16-byte config block (returns data chunk; on error MechaCon returns 0x80 + 15 zeros)
int mecha_read_config(u8 *out16, u8 *status);

// SCMD 0x42: Write 16-byte config block (byte 15 is verified checksum; returns 0x00 on OK, 0x80 on error)
int mecha_write_config(const u8 *in16, u8 *status);

// SCMD 0x43: Close config session
int mecha_close_config(u8 *status);

// SCMD 0x0A: EEPROM direct word access (addr: 0..511). Status in *status: 0x00=OK, 0x80=Error
int mecha_read_nvm_word(u16 word_addr, u16 *out_word, u8 *status);

// SCMD 0x0B: EEPROM direct word access (addr: 0..511). Status in *status: 0x00=OK, 0x80=Error
int mecha_write_nvm_word(u16 word_addr, u16 word_data, u8 *status);

// ---- High-level NVRAM & Diagnostic operations ----
int mecha_backup_nvram(u8 *nvram_buf, ProgressCallback cb);
int mecha_restore_nvram(const u8 *nvram_buf, ProgressCallback cb);
int mecha_verify_nvram(const u8 *nvram_buf, ProgressCallback cb);

// Read-only RAM probe via SCMD 0x40 (read mode 0) and sequential SCMD 0x41 calls.
int mecha_read_ram_probe(u8 region, u8 block_count, u8 *out_buf, u8 *status);

// Flexible RAM probe: req_count passed to SCMD 0x40 (0 for underflow), blocks_to_read read via SCMD 0x41
int mecha_read_ram_probe_blocks(u8 region, u8 req_count, int blocks_to_read, u8 *out_buf, u8 *status);

// Safe delay loop
void mecha_delay(int iterations);

// Query any SCMD 0x03 PMAP subcommand (returns 16 bytes)
int mecha_query_scmd03_subcmd(u8 subcmd, u8 *out16, u8 *status);

// Worker layout specification matching verified SPC970-MechaLIBerator
#define WORKER_LAYOUT_COUNT          4
#define LAYOUT_STANDARD_V2           0  // standard-fields (v2: CXP102064 2.04..2.14)
#define LAYOUT_V3_MARKER_00          1  // fields-2-bytes-earlier (v3: CXP103049 marker 00)
#define LAYOUT_V3_MARKER_01          2  // shifted-marker-01-pointer-at-6 (v3: CXP103049 marker 01)
#define LAYOUT_V1_EARLY_V2           3  // early-v1-v202-fields (v1/v2.02: CXP101064 / CXP102064 2.02)

struct worker_layout {
    const char *name;
    u8 flags_block;
    u8 flags_byte;
    u8 source_pointer_offset;
    u8 flags_mutable_end_byte;
    u8 control_block;
    u8 source_offset_byte;
    u8 word_count_byte;
    u8 worker_state_byte;
    u8 destination_block;
    u8 destination_low_byte;
    u8 destination_high_byte;
    u8 checksum_adjust_byte;
    u8 scratch_boundary_byte;
    u8 marker_block;
    u8 marker_byte;
    u8 marker_value;
    u32 layout_signature_salt;
    const u8 *tail_signature;
};

// Probe and buffer full 16 blocks (256 bytes) of Config Region 2 into g_config_window
int mecha_init_config_window(void);

// Automatically detect worker layout from buffered config window:
// Returns: 0 = standard-fields (v2), 1 = fields-2-bytes-earlier (v3 marker 00),
//          2 = shifted-marker-01-pointer-at-6 (v3 marker 01), 3 = early-v1-v202-fields (v1/v2.02)
int mecha_detect_worker_layout(void);

const struct worker_layout *mecha_get_layout(int layout_index);

extern u8 g_config_window[256];
extern int g_detected_worker_layout;

// Perform the SCMD 0x40/0x42 buffer underflow exploit to corrupt the EEPROM worker,
// using the verified two-lap staging protocol from SPC970-MechaLIBerator.
// rom_source_addr: 24-bit ROM address to read from (e.g. 0xFC0000)
// nvram_word_start: first EEPROM word to stage data into (e.g. 0, 128, 256, 384)
// nvram_word_count: number of 16-bit words (e.g. 128)
// layout_mode: layout index (0 = standard, 1 = BGA2 marker 00, 2 = BGA2 marker 01, 3 = early v1/v202)
// Returns 0 on success, negative on error.
int mecha_exploit_stage_chunk(u32 rom_source_addr, u16 nvram_word_start, u16 nvram_word_count, int layout_mode);

// Read back staged ROM data from EEPROM after exploit.
// Reads nvram_word_count words starting from nvram_word_start via SCMD 0x0A.
// Returns number of read errors (0 = perfect).
int mecha_read_staged_data(u16 nvram_word_start, u16 nvram_word_count, u8 *out_buf, ProgressCallback cb);

// Full ROM dump: iterates exploit cycles to dump ROM.
// Dumps 256KB on v2 (Banks FC, FD, FE, FF) or 192KB on v3 (Active Banks FD, FE, FF).
// rom_buf must be at least ROM_SIZE_BYTES.
int mecha_dump_full_rom(u8 *rom_buf, u32 *out_rom_size, const u8 *nvram_backup, ProgressCallback cb);

// Diagnostic structure for worker flush observation
struct worker_flush_diff {
    int total_changed_bytes;
    int overflow_changed_bytes;
    u16 flags_detected_offset;
    u16 ptr_detected_offset;
    u16 dest_detected_offset;
    char summary[256];
};

// Perform a safe hardware-native 4-block flush to observe MechaCon internal worker pointers
int mecha_worker_flush_probe(u8 region, u8 *pre_ram256, u8 *post_ram256, struct worker_flush_diff *diff);

#endif // MECHA_H
