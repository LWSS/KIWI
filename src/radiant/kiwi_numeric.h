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
float       KiwiNum_ValueWorld();     // RAW WORLD UNITS (Units_FromDisplay applied)
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
