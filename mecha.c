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
// out_model17 must point to at least 17 bytes (16 chars + NUL terminator).
int mecha_read_model_name(char *out_model17, u8 *status) {
    if (!out_model17) return -1;
    memset(out_model17, 0, 17);

    u8 in[1] = { 0x00 };
    u8 out[16] = { 0 };
    int ret = sceCdApplySCmd(0x17, in, 1, out);
    if (status) {
        *status = out[0];
    }
    if (ret != 1 || out[0] != 0x00) {
        return -1; // Unsupported on early consoles or not loaded
    }
    memcpy(&out_model17[0], &out[1], 8);

    in[0] = 0x08;
    memset(out, 0, sizeof(out));
    ret = sceCdApplySCmd(0x17, in, 1, out);
    if (ret == 1 && out[0] == 0x00) {
        memcpy(&out_model17[8], &out[1], 8);
    }
    out_model17[16] = '\0';
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
        // Transport-level failure (audit M-3's "return value") is already the
        // primary signal here; the payload-pattern check below only runs on
        // top of it, requiring BOTH ret==1 status byte 0x80 AND all-zero tail
        // before classifying as an error, to avoid misreading a legitimate
        // {0x80, 0x00 x15} data block as an error packet.
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

        // Store little-endian word to match native hardware byte order
        nvram_buf[w * 2]     = (u8)(word_val & 0xFF);
        nvram_buf[w * 2 + 1] = (u8)((word_val >> 8) & 0xFF);

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
        u16 word_val = (u16)(nvram_buf[w * 2] | (nvram_buf[w * 2 + 1] << 8));

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
        int ret = mecha_read_nvm_word((u16)w, &current_word, &status);

        if (ret != 0 || status != 0x00) {
            // Word could not be read back at all (current_word would read as 0).
            // Skip the comparison instead of counting this as a data mismatch.
            log_printf("[VERIFY_ERR] SCMD 0x0A failed at word %d (status 0x%02X); skipping comparison\n", w, status);
        } else {
            u8 hi = (u8)((current_word >> 8) & 0xFF);
            u8 lo = (u8)(current_word & 0xFF);

            if (nvram_buf[w * 2] != lo || nvram_buf[w * 2 + 1] != hi) {
                mismatches++;
            }
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

static const struct worker_layout known_worker_layouts[WORKER_LAYOUT_COUNT] = {
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
    },
    {
        .name = "cxp101064-qfp-v1",
        .flags_block = 12, .flags_byte = 12,
        .source_pointer_offset = 14, .flags_mutable_end_byte = 14,
        .control_block = 14, .source_offset_byte = 2,
        .word_count_byte = 3, .worker_state_byte = 4,
        .destination_block = 14, .destination_low_byte = 6,
        .destination_high_byte = 7, .checksum_adjust_byte = 13,
        .scratch_boundary_byte = 0,
        .marker_block = 0xff,
        .marker_byte = 0, .marker_value = 0,
        .layout_signature_salt = 0x31303130u,
        .tail_signature = NULL
    },
    {
        .name = "early-cxp102064-v1v2",
        .flags_block = 12, .flags_byte = 12,
        .source_pointer_offset = 14, .flags_mutable_end_byte = 14,
        .control_block = 14, .source_offset_byte = 2,
        .word_count_byte = 3, .worker_state_byte = 4,
        .destination_block = 14, .destination_low_byte = 6,
        .destination_high_byte = 7, .checksum_adjust_byte = 13,
        .scratch_boundary_byte = 0,
        .marker_block = 0xff,
        .marker_byte = 0, .marker_value = 0,
        .layout_signature_salt = 0x31303230u,
        .tail_signature = NULL
    }
};

int mecha_clean_overflow_ram(void) {
    u8 status = 0;
    int ret = mecha_open_config(1, 2, 0, &status);
    if (ret != 0) {
        mecha_close_config(&status);
        mecha_delay(5000);
        ret = mecha_open_config(1, 2, 0, &status);
        if (ret != 0) return -1;
    }
    u8 block[16];
    for (int i = 0; i < 16; i++) {
        memcpy(block, &g_config_window[i * 16], 16);
        if (i == 7) {
            block[0] = 0xFF;
            if (block[1] == 0) block[1] = 0x67;
        } else if (i >= 8) {
            memset(block, 0, 16);
        }
        u8 sum = 0;
        for (int b = 0; b < 15; b++) sum += block[b];
        block[15] = sum;
        if (mecha_write_config_raw(block) != 0) {
            mecha_close_config(&status);
            return -2;
        }
        mecha_delay(150);
    }
    mecha_close_config(&status);
    mecha_delay(2000);
    log_printf("[CONFIG] Cleaned RAM overflow blocks 8-15.\n");
    return 0;
}

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
    if (!layout->tail_signature) return 0;
    return (block == 13 && byte >= layout->scratch_boundary_byte) ||
           (block == 14 && byte < layout->scratch_boundary_byte);
}

static int worker_layout_matches(const struct worker_layout *layout) {
    if (!layout->tail_signature) return 0;
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
    for (int i = 0; i < WORKER_LAYOUT_COUNT; i++) {
        if (worker_layout_matches(&known_worker_layouts[i])) {
            if (match_index != -1) {
                match_index = -1; // Ambiguous
                break;
            }
            match_index = i;
        }
    }

    if (match_index == -1) {
        // Safe fallback using firmware version and chip generation
        if (g_mecha_ver[1] == 1 && g_mecha_ver[2] <= 3) {
            // CXP101064 (v1.02, v1.03): QFP v1 with 0x1956 base
            match_index = LAYOUT_V1_CXP101064; // 3
        } else if ((g_mecha_ver[1] == 1 && g_mecha_ver[2] >= 6) ||
                   (g_mecha_ver[1] == 2 && g_mecha_ver[2] <= 2)) {
            // Early CXP102064 (v1.06..v1.08, v2.02): 0x1940 base with 0x1A0C worker
            match_index = LAYOUT_EARLY_CXP102064; // 4
        } else if (g_mecha_ver[1] >= 3 || (g_mecha_ver[1] == 2 && g_mecha_ver[2] >= 14)) {
            match_index = LAYOUT_V3_MARKER_00; // 1
        } else {
            match_index = LAYOUT_STANDARD_V2; // 0
        }
    }

    g_detected_worker_layout = match_index;
    log_printf("[CONFIG] Selected worker layout: %d (%s)\n",
               g_detected_worker_layout, known_worker_layouts[g_detected_worker_layout].name);
    return g_detected_worker_layout;
}

const struct worker_layout *mecha_get_layout(int layout_index) {
    if (layout_index < 0 || layout_index >= WORKER_LAYOUT_COUNT) return NULL;
    return &known_worker_layouts[layout_index];
}

static void prepare_stage_block(int index, u32 rom_address, u16 nvram_word, int word_count,
                                const struct worker_layout *layout, u8 block[16]) {
    memcpy(block, &g_config_window[index * 16], 16);

    if (index == 7) {
        // Guarantee 0x19B0.0 is preserved (system state flag)
        block[0] = 0xFF;
        if (block[1] == 0) block[1] = 0x67;
    }

    if (index == layout->flags_block) {
        block[layout->flags_byte] &= 0xFC; // Idle mask (clear bits 0 and 1)
        if (layout->source_pointer_offset <= 12) {
            block[layout->source_pointer_offset]     = (u8)(rom_address & 0xFF);
            block[layout->source_pointer_offset + 1] = (u8)((rom_address >> 8) & 0xFF);
            block[layout->source_pointer_offset + 2] = (u8)((rom_address >> 16) & 0xFF);
            block[layout->source_pointer_offset + 3] = (u8)((rom_address >> 24) & 0xFF);
        } else {
            // Pointer starts at byte 14 of Block 12, byte 15 is checksum byte of Block 12
            block[layout->source_pointer_offset] = (u8)(rom_address & 0xFF);
            if (layout->checksum_adjust_byte < 15) {
                block[layout->checksum_adjust_byte] = 0;
                u8 partial_sum = 0;
                for (int i = 0; i < 15; i++) partial_sum += block[i];
                block[layout->checksum_adjust_byte] = (u8)(((u8)((rom_address >> 8) & 0xFF)) - partial_sum);
            }
        }
    }

    if (layout->source_pointer_offset > 12 && index == (layout->flags_block + 1)) {
        // High 16 bits of pointer in Block 13 (bytes 0-1)
        block[0] = (u8)((rom_address >> 16) & 0xFF);
        block[1] = (u8)((rom_address >> 24) & 0xFF);
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
    if (layout_mode < 0 || layout_mode >= WORKER_LAYOUT_COUNT) return -1;
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
            if (block[1] == 0) block[1] = 0x67;
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
    if (layout->source_pointer_offset > 12 && layout->checksum_adjust_byte < 15) {
        block[layout->checksum_adjust_byte] = 0;
        u8 partial_sum = 0;
        for (int i = 0; i < 15; i++) partial_sum += block[i];
        block[layout->checksum_adjust_byte] = (u8)(((u8)((rom_source_addr >> 8) & 0xFF)) - partial_sum);
    }
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
    u8 final_stat = 0xFF;
    int retries_used = 0;
    for (int retry = 0; retry < 1000; retry++) {
        retries_used = retry;
        u8 close_stat = 0xFF;
        mecha_close_config(&close_stat);
        final_stat = close_stat;
        if (close_stat == 0x00) {
            close_done = 1;
            break;
        }
        if (close_stat != 0x01) {
            log_printf("[EXPLOIT] SCMD 0x43 close error: stat=0x%02X (retry %d)\n", close_stat, retry);
            break;
        }
        mecha_delay(5000); // 5ms per poll
    }

    if (final_stat != 0x00 || retries_used > 5) {
        log_printf("[EXPLOIT] Staging completion: close_stat=0x%02X, retries=%d\n", final_stat, retries_used);
    }

    if (!close_done) {
        log_printf("[EXPLOIT] SCMD 0x43 close timed out\n");
        return -60;
    }

    // Ensure physical NVRAM write settling delay
    mecha_delay(2000);

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

        // Store as native little-endian to match ROM byte order
        out_buf[w * 2]     = (u8)(word_val & 0xFF);
        out_buf[w * 2 + 1] = (u8)((word_val >> 8) & 0xFF);

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
    if (mecha_init_config_window() != 0) {
        // g_config_window would stay at its memset(0) state, which would make
        // mecha_clean_overflow_ram()/prepare_stage_block() write zeroed blocks
        // over live EEPROM config data (drive calibration, etc). Refuse instead.
        log_printf("[EXPLOIT_ERR] Failed to buffer config window; refusing to touch EEPROM. Aborting dump.\n");
        return -3;
    }

    // Step 2: Detect active worker layout
    int active_layout = mecha_detect_worker_layout();

    mecha_clean_overflow_ram();
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

    u16 orig_w0 = (u16)(nvram_backup[0] | (nvram_backup[1] << 8));
    u16 orig_w1 = (u16)(nvram_backup[2] | (nvram_backup[3] << 8));

    int is_valid = 0;
    if (pre_ret == 0) {
        if (preview_words[0] == 0x00E6 || preview_words[0] == 0xE600) {
            is_valid = 1; // Standard SPC970 opcode signature
        } else if (preview_words[0] == 0x76A6 || (preview_words[0] == 0x0000 && preview_words[1] == 0xA676)) {
            is_valid = 1; // CXP103049 alternate signature
        } else if ((preview_words[0] != orig_w0 || preview_words[1] != orig_w1) &&
                   preview_words[1] != 0xFFFF && preview_words[0] != 0x0000) {
            is_valid = 1; // Confirmed data transferred from ROM to NVRAM
        }
    }

    // If initial layout didn't validate, restore NVRAM and try alternative layouts
    if (!is_valid) {
        for (int cand = 0; cand < WORKER_LAYOUT_COUNT; cand++) {
            if (cand == active_layout) continue;
            mecha_restore_nvram(nvram_backup, NULL);
            mecha_clean_overflow_ram();
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
                if (preview_words[0] == 0x00E6 || preview_words[0] == 0xE600 ||
                    preview_words[0] == 0x76A6 ||
                    (preview_words[0] == 0x0000 && preview_words[1] == 0xA676) ||
                    ((preview_words[0] != orig_w0 || preview_words[1] != orig_w1) &&
                     preview_words[1] != 0xFFFF && preview_words[0] != 0x0000)) {
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
        mecha_clean_overflow_ram();
        log_printf("[FAILSAFE] NVRAM automatically restored.\n");
        return (pre_ret != 0) ? pre_ret : -2;
    }

    log_printf("[AUTO_DUMP] Pre-flight SUCCESS! Validated with Layout %d. Dumping %d chunks (%u KB)...\n",
               active_layout, total_chunks, total_rom_bytes / 1024);

    // Read staged chunk 0
    int read_err0 = mecha_read_staged_data(0, chunk_words, &rom_buf[0], NULL);
    if (read_err0 > 0) {
        log_printf("[READ_ERR] Chunk 0 readback had %d word errors; region left as 0xFF\n", read_err0);
    }

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

        int read_err = mecha_read_staged_data(0, chunk_words, &rom_buf[buf_offset], NULL);
        if (read_err > 0) {
            log_printf("[READ_ERR] Chunk %d readback had %d word errors; region left as 0xFF\n", chunk, read_err);
            consecutive_errors++;
            if (consecutive_errors >= 5) {
                log_printf("[EXPLOIT_ERR] 5 consecutive readback failures! Aborting dump and restoring NVRAM...\n");
                mecha_restore_nvram(nvram_backup, NULL);
                log_printf("[FAILSAFE] NVRAM restored after abort.\n");
                return -101;
            }
        }
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

    mecha_clean_overflow_ram();

    return 0;
}

int mecha_worker_flush_probe(u8 region, u8 *pre_ram256, u8 *post_ram256, struct worker_flush_diff *diff) {
    if (!pre_ram256 || !post_ram256) return -1;
    if (diff) memset(diff, 0, sizeof(*diff));

    // Step 1: Read baseline RAM (16 blocks = 256 bytes) via SCMD 0x40 (read) + 16x SCMD 0x41
    u8 st = 0;
    int err1 = mecha_read_ram_probe_blocks(region, 16, 16, pre_ram256, &st);
    if (err1 != 0 || st != 0x00) {
        log_printf("[FLUSH_PROBE] Failed to read baseline RAM for region %d: err=%d, stat=0x%02X\n", region, err1, st);
        return -1;
    }
    mecha_delay(2000);

    // Step 2: Open region in WRITE mode with count = 4 blocks (the 4 original blocks)
    st = 0;
    int ret = mecha_open_config(1, region, 4, &st);
    if (ret != 0 || st != 0x00) {
        log_printf("[FLUSH_PROBE] SCMD 0x40 open (write, reg %d, count 4) failed: ret=%d, stat=0x%02X\n", region, ret, st);
        mecha_close_config(&st);
        return -2;
    }

    // Step 3: Write the 4 original blocks
    for (int b = 0; b < 4; b++) {
        u8 blk[16];
        memcpy(blk, &pre_ram256[b * 16], 16);
        u8 wr_st = 0;
        int wr_ret = mecha_write_config(blk, &wr_st);
        if (wr_ret != 0 || wr_st != 0x00) {
            log_printf("[FLUSH_PROBE] Block %d write failed: ret=%d, stat=0x%02X\n", b, wr_ret, wr_st);
            mecha_close_config(&st);
            return -3;
        }
        mecha_delay(200);
    }

    // On block 3, count reaches 0 -> firmware executes Config_Flush_To_NVRAM_Worker!
    // Wait for worker completion via delay + SCMD 0x40 polling
    int flushed = 0;
    for (int r = 0; r < 200; r++) {
        u8 chk_st = 0xFF;
        mecha_open_config(0, region, 0, &chk_st);
        if (chk_st == 0x00) {
            mecha_close_config(&chk_st);
            flushed = 1;
            break;
        }
        mecha_close_config(&chk_st);
        mecha_delay(5000);
    }
    mecha_close_config(&st);
    mecha_delay(5000);

    if (!flushed) {
        log_printf("[FLUSH_PROBE] Region %d worker flush timed out after 200 retries; results would be unreliable\n", region);
        return -5;
    }

    // Step 4: Immediately read post-flush RAM (256 bytes)
    int err2 = mecha_read_ram_probe_blocks(region, 16, 16, post_ram256, &st);
    if (err2 != 0 || st != 0x00) {
        log_printf("[FLUSH_PROBE] Failed to read post-flush RAM: err=%d, stat=0x%02X\n", err2, st);
        return -4;
    }

    // Step 5: Diff pre vs post
    int total_changed = 0;
    int overflow_changed = 0;
    u16 base_ram = (region == 0) ? 0x1890 : (region == 1 ? 0x18D0 : 0x1940);
    for (int i = 0; i < 256; i++) {
        if (pre_ram256[i] != post_ram256[i]) {
            total_changed++;
            if (i >= 112) overflow_changed++;
            u16 ram_addr = base_ram + i;
            log_printf("[FLUSH_DIFF] Reg %d RAM 0x%04X (blk %d, byte %d): 0x%02X -> 0x%02X\n",
                       region, ram_addr, i / 16, i % 16, pre_ram256[i], post_ram256[i]);
            if (diff) {
                if (post_ram256[i] == 0x01 || post_ram256[i] == 0x02 || post_ram256[i] == 0x03) {
                    diff->flags_detected_offset = ram_addr;
                }
            }
        }
    }

    if (diff) {
        diff->total_changed_bytes = total_changed;
        diff->overflow_changed_bytes = overflow_changed;
        snprintf(diff->summary, sizeof(diff->summary),
                 "Region %d flush completed. %d bytes modified (overflow: %d).",
                 region, total_changed, overflow_changed);
    }

    log_printf("[FLUSH_PROBE] Region %d flush finished: %d bytes changed (overflow: %d).\n",
               region, total_changed, overflow_changed);
    return 0;
}

// ---------------------------------------------------------------------------
// EXPERIMENTAL: extended write-reach probe.
//
// mecha_read_ram_probe_blocks() (the READ-mode SCMD 0x40/0x41 primitive) was
// confirmed on real CXP101064 (v1.02) hardware to wrap back to block 0 after
// 16 blocks (256 bytes) - requesting more blocks just re-reads the same 256
// bytes, it does not walk further into RAM. That rules out "read further" as
// a way to locate the v1 worker structure.
//
// The WRITE side (SCMD 0x40 write mode + SCMD 0x42) has only ever been probed
// up to block 7 (probe_config_overflow_action()'s Step 3: "no bounds check"
// on this chip up to that point) or used up to block 15 (mecha_exploit_stage_
// chunk()'s hardcoded `index < 16` loop). Whether the write session ALSO
// wraps at 256 bytes, or keeps accepting blocks further out, was unknown.
// This answers that directly, and as safely as the primitive allows:
//
//   - Blocks 0-15 are rewritten verbatim from g_config_window (caller must
//     have called mecha_init_config_window() first) so the already-understood
//     region is left exactly as found.
//   - Blocks 16..(16+max_extra_blocks-1) get an all-zero payload (checksum
//     computed normally by mecha_write_config()). Zero is deliberate: it
//     matches the "idle" convention mecha_clean_overflow_ram() already uses
//     for blocks 8-15, minimizing the chance some written byte is misread as
//     a live flag/trigger outside the 16 known blocks.
//   - The worker trigger sequence (flags byte = 0x03) that mecha_exploit_
//     stage_chunk() uses is NEVER written here, so the EEPROM-copy worker is
//     never armed - this only characterizes how far SCMD 0x42 accepts writes,
//     nothing here should ever reach EEPROM.
//   - The loop stops at the first non-0x00 status (or transport failure), so
//     it never pushes further than the hardware itself accepts.
//
// Returns the number of extra blocks (beyond block 15) accepted with status
// 0x00, or a negative code if opening the session or rewriting the known-safe
// blocks 0-15 failed. Callers should wrap this with the same NVRAM backup/
// restore safety net as the real exploit even though no worker is armed here.
int mecha_probe_extended_write_reach(u8 region, int max_extra_blocks, ProgressCallback cb) {
    if (max_extra_blocks <= 0) return -1;
    if (max_extra_blocks > 128) max_extra_blocks = 128; // sane upper bound

    u8 status = 0;
    int ret = mecha_open_config(1, region, 0, &status);
    if (ret != 0) {
        log_printf("[REACH_PROBE] SCMD 0x40 open (write, reg %d, count 0) failed: ret=%d, status=0x%02X\n",
                   region, ret, status);
        mecha_close_config(&status);
        return -2;
    }

    // Blocks 0-15: rewrite verbatim from the buffered window (same convention
    // as mecha_exploit_stage_chunk's Lap 1, minus the trigger byte).
    for (int index = 0; index < 16; index++) {
        u8 block[16];
        memcpy(block, &g_config_window[index * 16], 16);
        if (index == 7) {
            block[0] = 0xFF;
            if (block[1] == 0) block[1] = 0x67;
        }
        if (mecha_write_config_raw(block) != 0) {
            log_printf("[REACH_PROBE] Block %d (known region) write failed - aborting before touching new territory\n", index);
            mecha_close_config(&status);
            return -3;
        }
        mecha_delay(200);
    }

    // Blocks 16+: neutral all-zero payload, one block at a time, stopping at
    // the first rejection.
    int accepted = 0;
    for (int extra = 0; extra < max_extra_blocks; extra++) {
        u8 block[16] = { 0 };
        u8 wr_stat = 0xFF;
        int wr_ret = mecha_write_config(block, &wr_stat);
        int block_index = 16 + extra;
        log_printf("[REACH_PROBE] Block %d (offset 0x%03X from buffer base): ret=%d stat=0x%02X\n",
                   block_index, block_index * 16, wr_ret, wr_stat);
        if (cb) cb(extra + 1, max_extra_blocks, "Probing extended write reach...");
        if (wr_ret != 0 || wr_stat != 0x00) {
            log_printf("[REACH_PROBE] Rejected at block %d (offset 0x%03X) - stopping, nothing further was written\n",
                       block_index, block_index * 16);
            break;
        }
        accepted++;
        mecha_delay(200);
    }

    if (accepted == max_extra_blocks) {
        log_printf("[REACH_PROBE] Still accepting writes at the requested limit (%d extra blocks) - reach may extend further\n",
                   max_extra_blocks);
    }

    u8 close_stat = 0;
    mecha_close_config(&close_stat);
    log_printf("[REACH_PROBE] SCMD 0x43 close: stat=0x%02X. Extra blocks accepted past block 15: %d (0x%03X bytes)\n",
               close_stat, accepted, accepted * 16);
    mecha_delay(2000);

    return accepted;
}

// ---------------------------------------------------------------------------
// EXPERIMENTAL: deep worker candidate scan.
//
// mecha_probe_extended_write_reach() confirmed real CXP101064 (v1.02)
// hardware accepts SCMD 0x42 writes at least ~32 blocks (512 bytes) past the
// known 16-block window, with no rejection observed. This sweeps candidate
// block positions in that now-confirmed-reachable zone for a worker struct
// using the SAME compact single-block(+spillover) field pattern already
// proven on CXP102064 chips by LAYOUT_V1_CXP101064/LAYOUT_EARLY_CXP102064
// (flags@byte12, ROM-pointer LSB@byte14 + mid-byte via checksum, ROM-pointer
// high bytes@(block+1) bytes0-1, word_count@byte3, worker_state@byte4,
// destination NVRAM word@byte6-7) - just relocated to a different block,
// on the theory the same C struct simply landed at a different offset in
// this older firmware build.
//
// For each candidate block:
//   - Blocks 0-15 are rewritten verbatim from g_config_window; every other
//     block up to the candidate (and its spillover block) gets the same
//     neutral all-zero payload mecha_probe_extended_write_reach() already
//     validated as accepted and inert.
//   - The candidate's fields are armed (idle flags) via the same two-lap
//     protocol mecha_exploit_stage_chunk() uses, then triggered (flags=0x03)
//     exactly like the real exploit.
//   - NVRAM words 0-7 are read back via SCMD 0x0A and compared against the
//     pre-existing baseline using the same signature checks
//     mecha_dump_full_rom() uses for its own pre-flight validation.
//   - NVRAM is restored from nvram_backup after EVERY candidate, hit or not,
//     before moving on - never accumulates risk across candidates.
//
// Returns the block index (>=16) of the first candidate whose trigger
// produced a plausible ROM-signature/changed preview, or -1 if none of the
// candidates in [first_trial_block, last_trial_block] did. On a hit,
// out_preview_words (if non-NULL) receives the 8 preview words that matched.
static void deep_scan_block_base(int index, u8 out16[16]) {
    if (index < 16) {
        memcpy(out16, &g_config_window[index * 16], 16);
        if (index == 7) {
            out16[0] = 0xFF;
            if (out16[1] == 0) out16[1] = 0x67;
        }
    } else {
        memset(out16, 0, 16);
    }
}

// Same overlay math as prepare_stage_block(), but sourcing the "before
// overlay" 16 bytes from deep_scan_block_base() so it stays safe for block
// indices past g_config_window's 256-byte (16-block) size.
static void prepare_deep_scan_block(int index, u32 rom_address, u16 nvram_word,
                                     int word_count, const struct worker_layout *layout,
                                     u8 block[16]) {
    deep_scan_block_base(index, block);

    if (index == layout->flags_block) {
        block[layout->flags_byte] &= 0xFC; // Idle mask
        if (layout->source_pointer_offset <= 12) {
            block[layout->source_pointer_offset]     = (u8)(rom_address & 0xFF);
            block[layout->source_pointer_offset + 1] = (u8)((rom_address >> 8) & 0xFF);
            block[layout->source_pointer_offset + 2] = (u8)((rom_address >> 16) & 0xFF);
            block[layout->source_pointer_offset + 3] = (u8)((rom_address >> 24) & 0xFF);
        } else {
            block[layout->source_pointer_offset] = (u8)(rom_address & 0xFF);
            if (layout->checksum_adjust_byte < 15) {
                block[layout->checksum_adjust_byte] = 0;
                u8 partial_sum = 0;
                for (int i = 0; i < 15; i++) partial_sum += block[i];
                block[layout->checksum_adjust_byte] = (u8)(((u8)((rom_address >> 8) & 0xFF)) - partial_sum);
            }
        }
    }

    if (layout->source_pointer_offset > 12 && index == (layout->flags_block + 1)) {
        block[0] = (u8)((rom_address >> 16) & 0xFF);
        block[1] = (u8)((rom_address >> 24) & 0xFF);
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
            block[layout->checksum_adjust_byte] = 0;
            u8 partial_sum = 0;
            for (int i = 0; i < 15; i++) partial_sum += block[i];
            block[layout->checksum_adjust_byte] = (u8)(((u8)(nvram_word >> 8)) - partial_sum);
        }
    }

    u8 sum = 0;
    for (int i = 0; i < 15; i++) sum += block[i];
    block[15] = sum;
}

int mecha_scan_deep_worker_candidates(u32 rom_test_addr, int first_trial_block, int last_trial_block,
                                       const u8 *nvram_backup, u16 out_preview_words[8],
                                       ProgressCallback cb) {
    if (!nvram_backup) return -1;
    if (first_trial_block < 16) first_trial_block = 16;
    if (last_trial_block < first_trial_block) return -1;

    u16 orig_w0 = (u16)(nvram_backup[0] | (nvram_backup[1] << 8));
    u16 orig_w1 = (u16)(nvram_backup[2] | (nvram_backup[3] << 8));

    int candidates = last_trial_block - first_trial_block + 1;
    int tried = 0;

    for (int tb = first_trial_block; tb <= last_trial_block; tb++) {
        tried++;
        if (cb) cb(tried, candidates, "Scanning deep worker candidates...");

        struct worker_layout candidate = known_worker_layouts[LAYOUT_V1_CXP101064];
        candidate.flags_block = (u8)tb;
        candidate.control_block = (u8)tb;
        candidate.destination_block = (u8)tb;

        int total_blocks = tb + 2; // covers the pointer-high-byte spillover block

        // Arming+triggering a wrong candidate (unlike the all-zero write-reach
        // probe) has been observed on real CXP101064 hardware to leave the
        // Config-session handshake rejecting SCMD 0x40 open with stat=0x80 for
        // every subsequent attempt - a short single retry does not recover it.
        // Retry with real backoff; if it still won't open, the MechaCon is
        // wedged and burning through the remaining candidates would only
        // produce false "no hit" data for blocks that were never actually
        // tested, so abort the whole scan instead of continuing silently.
        u8 status = 0;
        int ret = -1;
        int open_attempt;
        for (open_attempt = 0; open_attempt < 20; open_attempt++) {
            ret = mecha_open_config(1, 2, 0, &status);
            if (ret == 0) break;
            mecha_close_config(&status);
            mecha_delay(20000);
        }
        if (ret != 0) {
            log_printf("[DEEP_SCAN] Block %d: SCMD 0x40 open failed after %d retries (stat=0x%02X) - "
                       "MechaCon appears wedged. Aborting scan: blocks %d-%d were NOT tested. "
                       "Power-cycle the console before retrying.\n",
                       tb, open_attempt, status, tb, last_trial_block);
            return -2; // distinct from -1 ("no hit"): scan was cut short, range is incomplete
        }
        if (open_attempt > 0) {
            log_printf("[DEEP_SCAN] Block %d: SCMD 0x40 open recovered after %d retries\n", tb, open_attempt);
        }

        u8 block[16];
        int write_failed = 0;

        // Lap 1: arm (idle flags) across the full candidate range
        for (int index = 0; index < total_blocks; index++) {
            prepare_deep_scan_block(index, rom_test_addr, 0, EXPLOIT_CHUNK_WORDS, &candidate, block);
            if (mecha_write_config_raw(block) != 0) {
                write_failed = 1;
                log_printf("[DEEP_SCAN] Block %d: Lap 1 write failed at index %d\n", tb, index);
                break;
            }
            mecha_delay(200);
        }

        // Lap 2: rewrite everything before the flags block again (same
        // pattern mecha_exploit_stage_chunk uses)
        if (!write_failed) {
            for (int index = 0; index < tb; index++) {
                deep_scan_block_base(index, block);
                u8 sum = 0;
                for (int i = 0; i < 15; i++) sum += block[i];
                block[15] = sum;
                if (mecha_write_config_raw(block) != 0) {
                    write_failed = 1;
                    log_printf("[DEEP_SCAN] Block %d: Lap 2 write failed at index %d\n", tb, index);
                    break;
                }
                mecha_delay(200);
            }
        }

        if (write_failed) {
            mecha_close_config(&status);
            mecha_delay(2000);
            mecha_restore_nvram(nvram_backup, NULL);
            continue;
        }

        // Trigger: re-send the flags block with bits 0-1 set (busy+pending)
        prepare_deep_scan_block(tb, rom_test_addr, 0, EXPLOIT_CHUNK_WORDS, &candidate, block);
        block[candidate.flags_byte] = (block[candidate.flags_byte] & 0xFC) | 0x03;
        if (candidate.source_pointer_offset > 12 && candidate.checksum_adjust_byte < 15) {
            block[candidate.checksum_adjust_byte] = 0;
            u8 partial_sum = 0;
            for (int i = 0; i < 15; i++) partial_sum += block[i];
            block[candidate.checksum_adjust_byte] = (u8)(((u8)((rom_test_addr >> 8) & 0xFF)) - partial_sum);
        }
        u8 sum = 0;
        for (int i = 0; i < 15; i++) sum += block[i];
        block[15] = sum;

        int trigger_ok = (mecha_write_config_raw(block) == 0);
        if (!trigger_ok) {
            log_printf("[DEEP_SCAN] Block %d: trigger write failed\n", tb);
        }

        // Poll close (same bounded retry loop as mecha_exploit_stage_chunk)
        int close_done = 0;
        u8 final_stat = 0xFF;
        for (int retry = 0; retry < 1000; retry++) {
            u8 close_stat = 0xFF;
            mecha_close_config(&close_stat);
            final_stat = close_stat;
            if (close_stat == 0x00) { close_done = 1; break; }
            if (close_stat != 0x01) break;
            mecha_delay(5000);
        }
        mecha_delay(2000);

        u16 preview[8] = { 0 };
        for (int i = 0; i < 8; i++) {
            u8 st = 0;
            mecha_read_nvm_word((u16)i, &preview[i], &st);
        }
        log_printf("[DEEP_SCAN] Block %d (offset 0x%03X): trigger_ok=%d close_stat=0x%02X "
                   "preview=%04X %04X %04X %04X %04X %04X %04X %04X\n",
                   tb, tb * 16, trigger_ok, final_stat,
                   preview[0], preview[1], preview[2], preview[3],
                   preview[4], preview[5], preview[6], preview[7]);

        int hit = 0;
        if (trigger_ok && close_done) {
            if (preview[0] == 0x00E6 || preview[0] == 0xE600) {
                hit = 1; // Standard SPC970 opcode signature
            } else if (preview[0] == 0x76A6 || (preview[0] == 0x0000 && preview[1] == 0xA676)) {
                hit = 1; // CXP103049 alternate signature
            } else if ((preview[0] != orig_w0 || preview[1] != orig_w1) &&
                       preview[1] != 0xFFFF && preview[0] != 0x0000) {
                hit = 1; // Confirmed data transferred from ROM to NVRAM
            }
        }

        // ALWAYS restore immediately, hit or not, before trying the next candidate
        int rest_err = mecha_restore_nvram(nvram_backup, NULL);
        if (rest_err != 0) {
            log_printf("[DEEP_SCAN] Block %d: NVRAM restore had %d word errors\n", tb, rest_err);
        }

        if (hit) {
            log_printf("[DEEP_SCAN] *** HIT at block %d (offset 0x%03X)! ***\n", tb, tb * 16);
            if (out_preview_words) memcpy(out_preview_words, preview, sizeof(preview));
            return tb;
        }
    }

    return -1;
}

