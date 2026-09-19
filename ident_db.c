#include "ident_db.h"

const char *get_model_id_desc(u16 model_id) {
    switch (model_id) {
        case 0xd200: return "DTL-H10000";
        case 0xd201: return "SCPH-10000";
        case 0xd202: return "SCPH-15000/18000";
        case 0xd203: return "SCPH-30001";
        case 0xd204: return "SCPH-30002/R";
        case 0xd205: return "SCPH-30003/R";
        case 0xd206: return "SCPH-30004/R";
        case 0xd207: return "DTL-H30001";
        case 0xd208: return "DTL-H30002";
        case 0xd209: return "COH-H30000";
        case 0xd20a: return "SCPH-18000";
        case 0xd20b: return "COH-H31000";
        case 0xd20c: return "SCPH-30000";
        case 0xd20d: return "DTL-H30000";
        case 0xd20e: return "COH-H31100";
        case 0xd20f: return "SCPH-35001 GT";
        case 0xd210: return "SCPH-35002 GT";
        case 0xd211: return "SCPH-35003 GT";
        case 0xd212: return "SCPH-35004 GT";
        case 0xd213: return "SCPH-35000 GT";
        case 0xd214: return "SCPH-30001/R";
        case 0xd215: return "SCPH-30005 R";
        case 0xd216: return "SCPH-30006 R";
        case 0xd217: return "SCPH-39000";
        case 0xd218: return "SCPH-39001";
        case 0xd219: return "SCPH-39002";
        case 0xd21a: return "SCPH-39003";
        case 0xd21b: return "SCPH-39004";
        case 0xd21d: return "SCPH-37000 L";
        case 0xd21e: return "SCPH-37000 B";
        case 0xd21f: return "SCPH-39008";
        case 0xd220: return "SCPH-39000 TB";
        case 0xd221: return "SCPH-39000 RC";
        case 0xd222: return "SCPH-39006";
        case 0xd225: return "DTL-H10100";
        case 0xd226: return "DTL-H30100";
        case 0xd227: return "DTL-H30101";
        case 0xd228: return "DTL-H30102";
        case 0xd229: return "DTL-H30105";
        case 0xd22a: return "SCPH-39000 S";
        case 0xd22b: return "SCPH-39000 AQ";
        case 0xd22c: return "SCPH-39000 SA";
        case 0xd22d: return "SCPH-39010/N";
        case 0xd301: return "DTL-H50000";
        case 0xd302: return "DTL-H50001";
        case 0xd303: return "DTL-H50002";
        case 0xd304: return "DTL-H50009";
        case 0xd31e: return "DTL-H70002";
        case 0xd322: return "DTL-H75000";
        case 0xd326: return "DTL-H90000";
        case 0xd380: return "DESR-7000";
        case 0xd381: return "DESR-5000";
        case 0xd382: return "DESR-7100";
        case 0xd383: return "DESR-5100";
        case 0xd384: return "DESR-5100/S";
        case 0xd385: return "DESR-7500";
        case 0xd386: return "DESR-5500";
        case 0xd387: return "DESR-7700";
        case 0xd388: return "DESR-5700";
        case 0xd401: return "SCPH-50001/N";
        case 0xd402: return "SCPH-50010/N";
        case 0xd403: return "SCPH-50000";
        case 0xd404: return "SCPH-50000 MB/NH";
        case 0xd405: return "SCPH-50002";
        case 0xd406: return "SCPH-50003";
        case 0xd407: return "SCPH-50004";
        case 0xd408: return "SCPH-50002 SS";
        case 0xd409: return "SCPH-50003 SS";
        case 0xd40a: return "SCPH-50004 SS";
        case 0xd40b: return "SCPH-50001";
        case 0xd40c: return "SCPH-50005/N";
        case 0xd40d: return "SCPH-50006";
        case 0xd40e: return "SCPH-50007";
        case 0xd40f: return "SCPH-50008";
        case 0xd411: return "SCPH-50000 NB";
        case 0xd412: return "SCPH-50000 TSS";
        case 0xd413: return "SCPH-55000 GU";
        case 0xd414: return "SCPH-55000 GT";
        case 0xd415: return "SCPH-50009 SS";
        case 0xd416: return "SCPH-50003 AQ";
        default: return "Unknown Model ID";
    }
}

u32 extract_serial_from_nvram(const u8 *nvram, u8 *emcs_out) {
    if (!nvram) return 0;

    // 1. Try Old layout (words 0x0E6, 0x0E7 -> offsets 0x1CC..0x1CF in big-endian dump)
    u16 w0_old = (u16)((nvram[0x1CC] << 8) | nvram[0x1CD]);
    u16 w1_old = (u16)((nvram[0x1CE] << 8) | nvram[0x1CF]);
    if (w0_old != 0xFFFF && w1_old != 0xFFFF && (w0_old != 0 || w1_old != 0)) {
        if (emcs_out) *emcs_out = (u8)((w1_old >> 8) & 0xFF);
        return (u32)(w0_old | ((w1_old & 0xFF) << 16));
    }

    // 2. Try New layout (words 0x0FA, 0x0FB -> offsets 0x1F4..0x1F7 in big-endian dump)
    u16 w0_new = (u16)((nvram[0x1F4] << 8) | nvram[0x1F5]);
    u16 w1_new = (u16)((nvram[0x1F6] << 8) | nvram[0x1F7]);
    if (w0_new != 0xFFFF && w1_new != 0xFFFF && (w0_new != 0 || w1_new != 0)) {
        if (emcs_out) *emcs_out = (u8)((w1_new >> 8) & 0xFF);
        return (u32)(w0_new | ((w1_new & 0xFF) << 16));
    }

    if (emcs_out) *emcs_out = 0;
    return 0;
}

u16 extract_model_id_from_nvram(const u8 *nvram) {
    if (!nvram) return 0;

    // Check old location: word 0x0E4 (offset 0x1C8..0x1C9)
    u16 m_old = (u16)((nvram[0x1C8] << 8) | nvram[0x1C9]);
    if (m_old != 0xFFFF && m_old != 0x0000) return m_old;

    // Check new location: word 0x0F8 (offset 0x1F0..0x1F1)
    u16 m_new = (u16)((nvram[0x1F0] << 8) | nvram[0x1F1]);
    if (m_new != 0xFFFF && m_new != 0x0000) return m_new;

    return 0;
}

const char *get_mechacon_chip_desc(u8 major, u8 minor, u8 region) {
    u32 revision = ((u32)major << 16) | ((u32)minor << 8) | (u32)region;

    if (revision >= 0x050000) {
        revision = revision & 0xfffeff; // Retail and debug chips are identical
        if (revision != 0x050607)
            revision = revision & 0xffff00; // Mexico unit is unique
    }

    switch (revision) {
        case 0x010200: return "CXP101064-605R";
        case 0x010300: return "CXP101064-602R"; // DTL-T10000 and retail models, Japan region locked
        case 0x010900: return "CXP102064-751R"; // only DTL-T10000
        case 0x02040A: return "CXP102064-651R"; // only Arcade machines
        case 0x020501:
        case 0x020502:
        case 0x020503: return "CXP102064-702R"; // DTL-H3000x
        case 0x020701:
        case 0x020702:
        case 0x020703: return "CXP102064-703R"; // DTL-H3000x, DTL-H3010x
        case 0x020900:
        case 0x020901:
        case 0x020902:
        case 0x020903:
        case 0x020904: return "CXP102064-704R"; // DTL-H3000x, DTL-H3010x
        case 0x020D00:
        case 0x020D01:
        case 0x020D02:
        case 0x020D04:
        case 0x020D05: return "CXP102064-705R/-752R"; // DTL-H3000x, DTL-H3010x, DTL-T10000

        // Japanese region only v1-v2
        case 0x010600: return "CXP102064-001R (Unconfirmed)";
        case 0x010700: return "CXP102064-003R";
        case 0x010800: return "CXP102064-002R";
        case 0x020000: return "CXP102064-004R (Unconfirmed)";
        case 0x020200: return "CXP102064-005R";
        case 0x020800: return "CXP102064-006R";
        case 0x020C00: return "CXP102064-007R";
        case 0x020E00: return "CXP102064-008R";

        // US region only
        case 0x020401: return "CXP102064-101R";
        case 0x020601: return "CXP102064-102R";
        case 0x020C01: return "CXP102064-103R";
        case 0x020E01: return "CXP102064-104R";

        // EU region only
        case 0x020402: return "CXP102064-201R";
        case 0x020602: return "CXP102064-202R";
        case 0x020C02: return "CXP102064-203R";
        case 0x020E02: return "CXP102064-204R";

        // Australia region only
        case 0x020403: return "CXP102064-301R";
        case 0x020603: return "CXP102064-302R";
        case 0x020C03: return "CXP102064-303R";
        case 0x020E03: return "CXP102064-304R";

        // Japan region only
        case 0x030200: return "CXP103049-001GG";
        case 0x030600: return "CXP103049-002GG";
        case 0x030800: return "CXP103049-003GG";

        // US region only
        case 0x030001: return "CXP103049-101GG";
        case 0x030201: return "CXP103049-102GG";
        case 0x030601: return "CXP103049-103GG";

        // EU region only
        case 0x030002: return "CXP103049-201GG";
        case 0x030202: return "CXP103049-202GG";
        case 0x030602: return "CXP103049-203GG";

        // Australia region only
        case 0x030003: return "CXP103049-301GG";
        case 0x030203: return "CXP103049-302GG";
        case 0x030603: return "CXP103049-303GG";

        // Asia region only
        case 0x030404: return "CXP103049-401GG";
        case 0x030604: return "CXP103049-402GG";
        case 0x030804: return "CXP103049-403GG";

        // Russia region only
        case 0x030605: return "CXP103049-501GG";

        // Dragon (SCPH-5000x / Slim / PSX)
        case 0x050000: return "CXR706080-101GG";
        case 0x050200: return "CXR706080-102GG";
        case 0x050400: return "CXR706080-103GG";
        case 0x050600: return "CXR706080-104GG";
        case 0x050C00: return "CXR706080-105GG/CXR706F080-1GG";
        case 0x050607: return "CXR706080-106GG";
        case 0x050A00: return "CXR706080-702GG";
        case 0x050E00: return "CXR706080-703GG";
        case 0x060000: return "CXR716080-101GG";
        case 0x060200: return "CXR716080-102GG";
        case 0x060400: return "CXR716080-103GG";
        case 0x060600: return "CXR716080-104GG";
        case 0x060A00: return "CXR716080-106GG";
        case 0x060C00: return "CXR726080-301GB";

        default:
            if (major == 3) return "CXP103049 Series (Unknown Rev)";
            if (major == 2) return "CXP102064 Series (Unknown Rev)";
            if (major == 1) return "CXP101064 Series (Unknown Rev)";
            if (major >= 5) return "CXR Dragon Series (Unknown Rev)";
            return "Unknown MechaCon Chip";
    }
}
