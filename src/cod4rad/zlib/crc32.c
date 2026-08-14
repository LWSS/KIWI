/*
 * crc32.c — minimal CRC-32 implementation for this old zlib fork.
 * Provides the `crc32` symbol that unzip.c requires.
 * Uses the standard Ethernet/ZIP polynomial 0xEDB88320.
 */

#include "zlib.h"

static uLong s_crc_table[256];
static int   s_crc_table_ready = 0;

static void build_crc_table(void)
{
    uLong poly = 0xEDB88320UL;
    int i, j;
    for (i = 0; i < 256; i++)
    {
        uLong c = (uLong)i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_table_ready = 1;
}

uLong ZEXPORT crc32(uLong crc, const Bytef *buf, uInt len)
{
    if (!s_crc_table_ready)
        build_crc_table();
    if (buf == Z_NULL)
        return 0UL;
    crc = crc ^ 0xFFFFFFFFUL;
    while (len--)
    {
        crc = s_crc_table[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}
