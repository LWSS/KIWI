/*
 * crt_patches.h — CRT compatibility patches for cod2rad64
 * Copied from cod2map_win_final.c (same IW codebase)
 *
 * These patches ensure the reconstructed code produces identical output
 * to the original binary. MSVC 2022 ucrt differs from the MSVC 2005 CRT
 * that was statically linked in cod2rad64_original.exe.
 */

#ifndef CRT_PATCHES_H
#define CRT_PATCHES_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MSVC6_CUTOFF 8
#define MSVC6_STKSIZ 125

/*
================
msvc6_shortsort

Selection sort for small partitions, faithful MSVC6 CRT copy.
Original cod2rad at 0x43B2E0.
================
*/
static void msvc6_shortsort(char *lo, char *hi, size_t width,
                            int (*comp)(const void *, const void *))
{
    char *p, *max;
    while (hi > lo) {
        max = lo;
        for (p = lo + width; p <= hi; p += width) {
            if (comp(p, max) > 0)
                max = p;
        }
        if (max != hi) {
            size_t i;
            for (i = 0; i < width; i++) {
                char tmp = max[i];
                max[i] = hi[i];
                hi[i] = tmp;
            }
        }
        hi -= width;
    }
}

/*
================
msvc6_swap

Byte-by-byte element swap, faithful MSVC6 CRT copy.
================
*/
static void msvc6_swap(char *a, char *b, size_t width)
{
    if (a != b) {
        while (width--) {
            char tmp = *a;
            *a++ = *b;
            *b++ = tmp;
        }
    }
}

/*
================
msvc6_qsort

Quicksort with median-of-three pivot and explicit stack, faithful MSVC6 CRT copy.
Original cod2rad at 0x43B3B0. Modern MSVC qsort produces different order
for equal elements.
================
*/
static void msvc6_qsort(void *base, size_t num, size_t width,
                         int (*comp)(const void *, const void *))
{
    char *lo, *hi;
    char *mid;
    char *loguy, *higuy;
    size_t size;
    char *lostk[MSVC6_STKSIZ], *histk[MSVC6_STKSIZ];
    int stkptr = 0;

    if (num < 2 || width == 0)
        return;

    lo = (char *)base;
    hi = (char *)base + width * (num - 1);

recurse:
    size = (hi - lo) / width + 1;

    if (size <= MSVC6_CUTOFF) {
        msvc6_shortsort(lo, hi, width, comp);
    } else {
        mid = lo + (size / 2) * width;

        if (comp(lo, mid) > 0)
            msvc6_swap(lo, mid, width);
        if (comp(lo, hi) > 0)
            msvc6_swap(lo, hi, width);
        if (comp(mid, hi) > 0)
            msvc6_swap(mid, hi, width);

        loguy = lo;
        higuy = hi;

        for (;;) {
            if (mid > loguy) {
                do {
                    loguy += width;
                } while (loguy < mid && comp(loguy, mid) <= 0);
            }
            if (mid <= loguy) {
                do {
                    loguy += width;
                } while (loguy <= hi && comp(loguy, mid) <= 0);
            }

            do {
                higuy -= width;
            } while (higuy > mid && comp(higuy, mid) > 0);

            if (higuy < loguy)
                break;

            msvc6_swap(loguy, higuy, width);

            if (mid == higuy)
                mid = loguy;
        }

        higuy += width;
        if (mid < higuy) {
            do {
                higuy -= width;
            } while (higuy > mid && comp(higuy, mid) == 0);
        }
        if (mid >= higuy) {
            do {
                higuy -= width;
            } while (higuy > lo && comp(higuy, mid) == 0);
        }

        if (higuy - lo >= hi - loguy) {
            if (lo < higuy) {
                lostk[stkptr] = lo;
                histk[stkptr] = higuy;
                ++stkptr;
            }
            if (loguy < hi) {
                lo = loguy;
                goto recurse;
            }
        } else {
            if (loguy < hi) {
                lostk[stkptr] = loguy;
                histk[stkptr] = hi;
                ++stkptr;
            }
            if (lo < higuy) {
                hi = higuy;
                goto recurse;
            }
        }
    }

    --stkptr;
    if (stkptr >= 0) {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;
    }
}

#define qsort msvc6_qsort

/*
================
fix_exponent_format

Restores MSVC6 3-digit exponents, e.g. "1.00179e-05" -> "1.00179e-005".
Original cod2rad uses %g format extensively.
================
*/
static void fix_exponent_format(char *buf, unsigned int *plen)
{
    unsigned int len = *plen;
    unsigned int i = 0;
    while (i + 3 < len) {
        if ((buf[i] == 'e' || buf[i] == 'E') &&
            (buf[i+1] == '+' || buf[i+1] == '-') &&
            buf[i+2] >= '0' && buf[i+2] <= '9' &&
            buf[i+3] >= '0' && buf[i+3] <= '9') {
            if (i + 4 >= len || buf[i+4] < '0' || buf[i+4] > '9') {
                if (len + 1 < 0x7D00) {
                    memmove(buf + i + 3, buf + i + 2, len - (i + 2) + 1);
                    buf[i+2] = '0';
                    len++;
                    i += 5;
                } else {
                    i += 4;
                }
            } else {
                i += 5;
            }
        } else {
            i++;
        }
    }
    *plen = len;
}

/*
================
crt_patches_init

Restore MSVC CRT printf rounding and 3-digit exponents.
Disable FMA3 to prevent rounding differences in ucrt math.
Call early in main().
================
*/
static void crt_patches_init(void)
{
#ifdef _MSC_VER
#include <corecrt_stdio_config.h>
    _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS &= ~_CRT_INTERNAL_PRINTF_STANDARD_ROUNDING;
    _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS |= _CRT_INTERNAL_PRINTF_LEGACY_THREE_DIGIT_EXPONENTS;
    _set_FMA3_enable(0);
#endif
}

#endif /* CRT_PATCHES_H */
