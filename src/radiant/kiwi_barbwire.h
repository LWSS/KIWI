#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-09-16, user: "I want a barbwire tool that applies barbwire strips across a
// construction line ... extract the barbwire strips from those models and then configure
// it so that we can infinitely lay down barbwire along construction lines and splines ...
// it should not be high poly, keep it like the original cod4 barbwire").
//
// The stock CoD4 barbwire models (mil_barbedwire2/4/6/7/8) are post + strand assemblies;
// there is no plain strip.  This tool READS one of them from the search path, keeps only
// the connected components that run the whole length of the model (the strands - posts and
// the coil wraps around them span a few units and drop out), and treats that strand set as
// a repeating tile: x along the wire, y sideways, z up.  The selected construction objects
// (lines, polylines, splines, arcs, circles - whatever KiwiCon_VertWorld tessellates) are
// walked by arc length and the tile is bent onto them: every strand vertex lands at
// P(s) + side*y + up*(z + offset) in a frame that follows the curve, so the wire follows a
// spline as a true sweep rather than a chain of straight props.  Alternate tiles are
// mirrored end-for-end so the strand ends meet exactly at each join (the tile's two ends
// are different cross-sections; a mirror makes each join a strand meeting ITSELF).
//
// Output = ordinary v25 xmodel/xmodelparts/xmodelsurfs files under raw/ named
// kiwi_bw_<hash>, ONE per curve by default (`pieceLen` 0; a value splits very long runs,
// since a static model is lit from one light-grid sample and LODs by one distance),
// placed as misc_model entities with `kiwi_barbwire "<construction name>"` so a
// re-run on the same curve replaces its previous pieces (and deletes their files).  The
// source materials (mtl_barbed_wire_a_masked and friends) are referenced by name; the
// textures are the shipped ones.  No collision surfaces are written: CoD4 barbwire is
// clipped with brushes, like the stock models on the map compilers' side.
//
// Hooked into: palette + Construct panel button + Selection menu (KIWI_CMD_BARBWIRE),
// one classic undo record per run, the outliner (they are entities), the test DSL
// (`construct ...` to draw a curve headless, `barbwire [zoffset]` to lay).

#include <stddef.h>

struct kiwiBarbwireOpts_t
{
    int   source      = 0;       // KiwiBarbwire_SourceName() index (the stock model)
    // Wires kept from the top of the source (its strands cluster by height into the
    // physical wires); 1 = the top strand alone, centred ON the curve.  0 = every wire
    // at its post height with the curve as the ground line.
    int   strands     = 1;
    float zOffset     = 0.0f;    // strands raised (+) / lowered (-) against the curve
    // Split the run into several xmodels above this length; 0 = the whole curve is ONE
    // xmodel (the user's preference - a single strand is light enough that one light-grid
    // sample and one LOD distance for the whole run do not show).
    float pieceLen    = 0.0f;
    // Sideways meander (units) layered onto the tile's own waviness so the repeat does
    // not read as a perfectly articulated pattern; `seed` picks the meander.
    float wobble      = 2.0f;
    int   seed        = 0;
    bool  mirrorTiles = true;    // alternate tiles flipped so strand ends meet
    bool  replace     = true;    // a re-run on the same curve replaces its pieces
};

// The stock models the strands can be lifted from (index = opts.source).
int         KiwiBarbwire_SourceCount();
const char *KiwiBarbwire_SourceName ( int i );     // "mil_barbedwire7"
const char *KiwiBarbwire_SourceLabel( int i );     // what the dialog shows

// Persisted dialog defaults (kiwi_radiant.ini [Barbwire]).
void KiwiBarbwire_GetOpts( kiwiBarbwireOpts_t *out );
void KiwiBarbwire_SetOpts( const kiwiBarbwireOpts_t &o );

// A construction object is selected (palette greying / Construct panel button).
bool KiwiBarbwire_CanExecute();

void KiwiBarbwire_RegisterCommands();
bool KiwiBarbwire_DispatchInstant( unsigned int cmdId );   // opens the dialog
void KiwiBarbwire_BuildMenu( void *frameMenu );            // Selection menu row
// The options dialog: a MODAL POPUP drawn at top-level window scope every frame, like
// KiwiPlastBridge_Draw (early-out when idle).
void KiwiBarbwire_Draw();

// Lay barbwire along every selected construction object with `opts` (the dialog's
// "Lay" button and the `barbwire` test verb).  Prints what it made; false + `err`
// when nothing was laid.
bool KiwiBarbwire_LayNow( const kiwiBarbwireOpts_t &opts, char *err, size_t errSz );
