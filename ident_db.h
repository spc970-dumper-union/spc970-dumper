#ifndef IDENT_DB_H
#define IDENT_DB_H

#include <tamtypes.h>

// Returns model description from 16-bit Console Model ID (from PS2Ident database)
const char *get_model_id_desc(u16 model_id);

// Extracts 7-digit serial number and EMCS ID from 1024-byte NVRAM dump
u32 extract_serial_from_nvram(const u8 *nvram, u8 *emcs_out);

// Extracts 16-bit Model ID from 1024-byte NVRAM dump
u16 extract_model_id_from_nvram(const u8 *nvram);

// Returns Sony MechaCon Chip Part Number (e.g. "CXP103049-203GG") from SCMD 0x03-00
const char *get_mechacon_chip_desc(u8 major, u8 minor, u8 region);

#endif // IDENT_DB_H
