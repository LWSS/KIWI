#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Material-header cache for the editor's boot.
//
// KIWI (2026-09-19, user: "the startup is taking 20 seconds ... I think it's loading
// materials, the original did this too, but we need to fix it" / "it only happens on the 1st
// editor startup in a while").  Load_Materials (texwnd.cpp 0x45ae40) opens EVERY file under
// materials/ - 4,349 of them in this tree - to read a 40-byte header for the texture browser.
// With a warm file cache that is nothing (measured: the whole boot is under a second); on the
// first start after a reboot each open is a disk seek plus a first-touch antivirus scan, and
// 4,349 of those are the 20 seconds.  The original does the same.
//
// The cache turns that into ONE directory enumeration (FindFirstFileEx hands back every name
// with its size and write time, no file is opened) plus ONE small file,
// <basepath>\kiwi_cache\material_headers.bin.  A header is served from the cache only while
// the material file's size AND last-write time are what they were when it was cached; any
// other file is read the original way and the cache is rewritten.  Files that are not loose
// files in a search-path directory (inside an .iwd) always take the original path.
//
//   KiwiMatHdr_Begin( sizeof( MaterialInfoRaw ) );
//   for each name:  switch ( KiwiMatHdr_Get( name, &raw ) ) { HIT / BAD / MISS -> read + Put }
//   KiwiMatHdr_End();          // writes the cache back if anything changed, prints the tally

enum kiwiMatHdr_t
{
    KMATHDR_MISS = 0,   // not cached (or stale): read the file, then KiwiMatHdr_Put
    KMATHDR_HIT  = 1,   // *rawOut filled from the cache
    KMATHDR_BAD  = 2    // cached as "unreadable / shorter than a header": skip it, as the read would
};

void         KiwiMatHdr_Begin( unsigned rawSize );
kiwiMatHdr_t KiwiMatHdr_Get( const char *name, void *rawOut );
void         KiwiMatHdr_Put( const char *name, const void *raw );     // raw == nullptr records KMATHDR_BAD
void         KiwiMatHdr_End();
