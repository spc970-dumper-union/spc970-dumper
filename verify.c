#include "verify.h"
#include <string.h>

static const struct {
    const char *name;
    u32 start_offset;
    u32 count;
} ROM_SECTIONS_V2[9] = {
    { "Bank FC (0xFC0000-0xFC7FFF)", 0x00000, 0x2000 },
    { "Bank FC (0xFC8000-0xFCFFFF)", 0x08000, 0x2000 },
    { "Bank FD (0xFD0000-0xFD7FFF)", 0x10000, 0x2000 },
    { "Bank FD (0xFD8000-0xFDFFFF)", 0x18000, 0x2000 },
    { "Bank FE (0xFE0000-0xFE7FFF)", 0x20000, 0x2000 },
    { "Bank FE (0xFE8000-0xFEFFFF)", 0x28000, 0x2000 },
    { "Bank FF (0xFF0000-0xFF7FFF)", 0x30000, 0x2000 },
    { "Bank FF (0xFF8000-0xFFFB7F)", 0x38000, 0x1EE0 },
    { "Bank FF (0xFFFBA4-0xFFFFFF)", 0x3FBA4, 0x0117 },
};

static const struct {
    const char *name;
    u32 start_offset;
    u32 count;
} ROM_SECTIONS_V3[7] = {
    { "Bank FD (0xFD0000-0xFD7FFF)", 0x10000, 0x2000 },
    { "Bank FD (0xFD8000-0xFDFFFF)", 0x18000, 0x2000 },
    { "Bank FE (0xFE0000-0xFE7FFF)", 0x20000, 0x2000 },
    { "Bank FE (0xFE8000-0xFEFFFF)", 0x28000, 0x2000 },
    { "Bank FF (0xFF0000-0xFF7FFF)", 0x30000, 0x2000 },
    { "Bank FF (0xFF8000-0xFFFB7F)", 0x38000, 0x1EE0 },
    { "Bank FF (0xFFFBA4-0xFFFFFF)", 0x3FBA4, 0x0117 },
};

int verify_spc970_rom(const u8 *rom, u32 rom_len, RomVerifyResult *res) {
    if (!rom || !res) return -1;
    if (rom_len != 262144 && rom_len != 196608) return -1;

    memset(res, 0, sizeof(RomVerifyResult));

    u32 bank_offset = (rom_len == 196608) ? 0x10000 : 0x00000;
    u32 chk_offset = 0x3FB80 - bank_offset;
    u32 ver_offset = 0x3FB32 - bank_offset;

    // Mirror checksum comparison: 2 copies of 18 bytes (9 words each)
    const u8 *c1 = &rom[chk_offset];
    const u8 *c2 = &rom[chk_offset + 18];
    res->mirror_match = (memcmp(c1, c2, 18) == 0);

    // Unpack stored expected words (little endian)
    u16 stored_words[9];
    for (int i = 0; i < 9; i++) {
        stored_words[i] = (u16)(c1[i * 2] | (c1[i * 2 + 1] << 8));
    }

    // Determine whether this is v2 or v3 architecture:
    // If 192KB, it's definitely v3 (Banks FD, FE, FF).
    // On 256KB v3, Bank FC is unmapped/empty and stored_words[7..8] are 0.
    int is_v3 = (rom_len == 196608) || (rom[ver_offset] >= 3) || (stored_words[7] == 0 && stored_words[8] == 0);
    int total_sections = is_v3 ? 7 : 9;

    res->total_sections = total_sections;
    res->passed_count = 0;

    for (int i = 0; i < total_sections; i++) {
        const char *sec_name = is_v3 ? ROM_SECTIONS_V3[i].name : ROM_SECTIONS_V2[i].name;
        u32 sec_start_256k = is_v3 ? ROM_SECTIONS_V3[i].start_offset : ROM_SECTIONS_V2[i].start_offset;
        u32 sec_start = sec_start_256k - bank_offset;
        u32 sec_count = is_v3 ? ROM_SECTIONS_V3[i].count : ROM_SECTIONS_V2[i].count;

        res->sections[i].name = sec_name;
        res->sections[i].start_offset = sec_start_256k;
        res->sections[i].count = sec_count;
        res->sections[i].expected_checksum = stored_words[i];

        u32 acc = 0;
        u32 num_words = sec_count * 2;

        for (u32 w = 0; w < num_words; w++) {
            u16 val = (u16)(rom[sec_start + w * 2] | (rom[sec_start + w * 2 + 1] << 8));
            acc = (acc + val) & 0xFFFF;
        }

        res->sections[i].computed_checksum = (u16)acc;
        if (acc == stored_words[i]) {
            res->sections[i].passed = 1;
            res->passed_count++;
        } else {
            res->sections[i].passed = 0;
        }
    }

    res->all_passed = (res->passed_count == total_sections && res->mirror_match);
    return res->all_passed ? 0 : 1;
}
