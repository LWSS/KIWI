#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Texture browser colour filter (texwnd.cpp).  Each material is classified by the mean of
// its colour map into one bucket; picking a bucket shows only that colour.  Classification
// is lazy and cached by material name; the Textures toolbar pumps it while a colour is picked.

enum kiwiTexColorBucket_t
{
    KTEXCOL_ALL = 0,        // no colour filter
    KTEXCOL_RED,
    KTEXCOL_ORANGE,
    KTEXCOL_YELLOW,
    KTEXCOL_GREEN,
    KTEXCOL_CYAN,
    KTEXCOL_BLUE,
    KTEXCOL_PURPLE,
    KTEXCOL_PINK,
    KTEXCOL_BROWN,
    KTEXCOL_WHITE,
    KTEXCOL_GREY,
    KTEXCOL_BLACK,
    KTEXCOL_COUNT
};

struct kiwiTexColor_t
{
    const char *name;
    float       r, g, b;    // the button swatch
};

const kiwiTexColor_t *TexWnd_ColorInfo( int bucket );     // NULL when out of range
int  TexWnd_GetColorFilter();
void TexWnd_SetColorFilter( int bucket );                 // re-homes the scroll

// Classifies materials the other filters accept for up to budgetMs; true when every one is
// classified.  Unclassified materials stay hidden while a colour is picked.
bool TexWnd_PumpColorFilter( double budgetMs, int *outDone, int *outTotal );

// "In Use & View": only materials on brush faces / patches the 3D camera can see (facing it,
// inside the frustum, below a live section cut; no occlusion).  Pump once per frame.
bool TexWnd_GetViewFilter();
void TexWnd_SetViewFilter( bool on );
void TexWnd_PumpViewFilter();
