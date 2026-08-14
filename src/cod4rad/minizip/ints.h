/* ints.h — minimal fixed-width int typedefs for minizip */
#ifndef COD2RAD_MINIZIP_INTS_H
#define COD2RAD_MINIZIP_INTS_H

#include <stdint.h>

typedef uint64_t ui64_t;
typedef int64_t  i64_t;
typedef uint32_t ui32_t;
typedef int32_t  i32_t;
typedef uint16_t ui16_t;
typedef int16_t  i16_t;
typedef uint8_t  ui8_t;
typedef int8_t   i8_t;

/* minizip expects z_off64_t from a newer zlib; our zlib is older so provide it here */
#ifndef z_off64_t
typedef int64_t z_off64_t;
#endif

/* z_crc_t is a newer zlib type used by minizip's crypto block; our zlib is older */
#ifndef z_crc_t
typedef uint32_t z_crc_t;
#endif

#endif
