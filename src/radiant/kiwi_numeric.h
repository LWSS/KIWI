#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Numeric entry for modal commands.
//
// Commands declare static field tables through NumericFields; no declaration gets
// one unnamed editable length field. Focus starts on the first editable field.
// First Tab activates focus, later Tabs cycle it, and Escape clears focused text
// before the framework cancels the command.
//
// Field kind controls formatting only. KiwiNum_ValueWorld* always applies
// Units_FromDisplay, so non-length commands undo it with Units_ToDisplay. Typed
// lengths and expression results are display inches.
//
// Numeric keys reject modifiers except exact Shifted operators, preserving editor
// chords. Tab accepts optional Shift and is always swallowed during modal gestures.
// The HUD and bubble share formatting; the bubble projects BubbleAnchor or the last
// snap point and never takes hover from the camera image.

// Field kinds
enum kiwiNumKind_t
{
    KNUM_LENGTH = 0,    // formatted with KiwiUnits_Format ("17 ft 3.2 in")
    KNUM_ANGLE,         // degrees      ("36.4 deg")
    KNUM_FACTOR,        // a multiplier ("x1.250")
    KNUM_COUNT          // a bare integer
};

struct kiwiNumField_t
{
    const char   *label;      // "length" / "angle" — STATIC storage, never copied
    kiwiNumKind_t kind;
    bool          readOnly;   // display-only in overlays; Tab skips it
};

#define KNUM_MAX_FIELDS 4

// Lifecycle
// Clear every entry and reinstall one unnamed editable length field.
void KiwiNum_Reset();

// Install up to KNUM_MAX_FIELDS, reporting excess declarations. Clears all text
// and chooses the first editable field, so use only on command start.
void KiwiNum_SetFields( const kiwiNumField_t *fields, int count );
// Change a mode's field table without losing entries in retained fields.
void KiwiNum_UpdateFields( const kiwiNumField_t *fields, int count );

// Relabel without clearing typed text; label must have static storage.
void KiwiNum_SetFieldLabel( int field, const char *label );

int                   KiwiNum_FieldCount();
const kiwiNumField_t *KiwiNum_Field( int field );      // NULL when out of range

// Focus
// The field receiving digits; defaults to the first editable field before Tab.
int  KiwiNum_Focus();

// True after Tab activates focus; only then does the bubble highlight its row.
bool KiwiNum_TabLive();

// First call activates focus; later calls cycle it. False only when none is editable.
bool KiwiNum_TabCycle( bool backwards );

// KIWI: focus one editable field directly, as if Tab had landed on it (the options
// panel's Tab ring visits HUD fields it has no row for).  False when not editable.
bool KiwiNum_FocusField( int field );

// Key input
// Feed one key.  True = CONSUMED by the numeric entry.
bool KiwiNum_Key( int vk, unsigned int mods );

// Drop one field's entry (the framework's first Esc rung).
void KiwiNum_ClearField( int field );

// Clear every entry without replacing field declarations or focus; staged commands
// use this between stages.
void KiwiNum_ClearEntry();

// Expression parser
// Shared evaluator for standard-precedence expressions, parentheses, fractions,
// unit suffixes, and compounds such as 10ft6in. Results are display inches; convert
// with Units_FromDisplay for world values. False means incomplete or malformed.
bool KiwiNum_EvalDisplay( const char *text, float *outDisplay );

// Focused-field values
bool        KiwiNum_Has();            // is anything typed?  ("-"/"." count)
bool        KiwiNum_HasValue();       // does it parse to a number yet?
const char *KiwiNum_Text();           // raw typed text, never NULL — HUD only

// Values by field
bool        KiwiNum_HasField     ( int field );
bool        KiwiNum_HasValueField( int field );
float       KiwiNum_ValueWorldField( int field );
const char *KiwiNum_TextField    ( int field );

// Drawing
// Bottom-camera HUD for command status and typed values; no-op without a command.
void KiwiNum_DrawHud( float imgMinX, float imgMinY, float imgW, float imgH );

// Value bubble at BubbleAnchor, drawn through ImDrawList so it cannot take hover.
void KiwiNum_DrawBubble( float imgMinX, float imgMinY, float imgW, float imgH );

// KIWI (2026-10-22, user: "plasticity just added a new angle tool ... It works like a pie and
// when ctrl is held it snaps to 5deg increments"): Plasticity 2026.2's angle pie.  An angle
// drag turns the cursor AROUND a world anchor instead of sliding it sideways; the camera
// draws a ring on the anchor, the zero spoke, the value spoke, the swept wedge between them,
// a dotted line to the cursor, 5-degree ticks while Ctrl snaps, and a "deg" label.  The pie
// only measures; the command owns the value (its snapping and clamps) and reports it back.
// Begin: the cursor's direction now means valueDeg.  Owned by the active command, so it
// also disappears when that command ends.
void  KiwiAnglePie_Begin( const float *anchor, int imgX, int imgY, float valueDeg );
// valueDeg at Begin plus the signed angle the cursor has swept around the anchor since
// (counter-clockwise on screen = +), counted continuously so whole turns add up.  Unchanged
// while the cursor sits on the centre, where the direction is meaningless.
float KiwiAnglePie_Drag( int imgX, int imgY );
// The value the spoke and label show after the command's snapping and clamps.  `rebase`:
// the cursor's current direction now means valueDeg (a clamp, so travel back answers at once).
void  KiwiAnglePie_Show( float valueDeg, bool rebase );
void  KiwiAnglePie_End();
bool  KiwiAnglePie_Active();
// Camera overlay pass (KiwiVP_DrawCameraOverlay); no-op while no pie is up.
void  KiwiAnglePie_Draw( float imgMinX, float imgMinY, float imgW, float imgH );
