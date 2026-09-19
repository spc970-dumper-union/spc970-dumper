#ifndef VERIFY_H
#define VERIFY_H

#include <tamtypes.h>

typedef struct {
    const char *name;
    u32 start_offset;
    u32 count; // count * 2 = num_words (16-bit)
    u16 expected_checksum;
    u16 computed_checksum;
    int passed;
} SectionChecksumResult;

typedef struct {
    int mirror_match;
    int all_passed;
    int passed_count;
    int total_sections;
    SectionChecksumResult sections[9];
} RomVerifyResult;

// Verifies a 256KB SPC970 ROM image using routine 0xFF5170 POST algorithm
int verify_spc970_rom(const u8 *rom, u32 rom_len, RomVerifyResult *res);

#endif // VERIFY_H
