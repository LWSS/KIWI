#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_numeric.h — RADIANT_UX_DESIGN §13: numeric entry during a modal command.
//
// Grammar v1 was a SINGLE SCALAR (distance / degrees / factor): `G X 64 ⏎`.
// SHAKEOUT E widens that to NAMED FIELDS with Tab cycling, because that is what
// the user asked for after Plasticity: "allow pressing of [tab] to go into the
// length box and type in an exact length.  Pressing [tab] again should cycle to
// the angle too when applicable".
//
// ── FIELDS (shakeout E) ──────────────────────────────────────────────────────
// A command declares its fields ONCE, declaratively, through
// KiwiEditorCommand::NumericFields (kiwi_command.h) — the framework installs them
// in KiwiCmd_Start, between KiwiNum_Reset and Begin, so a command's Begin may
// still relabel a field per stage (KiwiNum_SetFieldLabel; the primitives do this
// as they walk base → height).  A command that declares NOTHING gets the DEFAULT
// single unnamed LENGTH field, which is byte-for-byte the pre-shakeout-E grammar.
//
//   Tab / Shift+Tab   cycle the focus over the EDITABLE fields
//   digits / . / -    edit the FOCUSED field (field 0 until the first Tab, so a
//                     command that never declares fields is unaffected)
//   Backspace         ditto
//   Escape            clears the FOCUSED field first (the framework's Esc ladder,
//                     kiwi_command.cpp), and only then cancels the command
//
// ── KIND IS A DISPLAY FACT, NOT A CONVERSION ────────────────────────────────
// DELIBERATE, and the whole reason the field rework carries no behavioural risk:
// KiwiNum_ValueWorld* ALWAYS applies Units_FromDisplay, exactly as it did before
// shakeout E.  Commands whose scalar is NOT a length (R's degrees, S's factor,
// the array count, the texture steps) already undo that with Units_ToDisplay on
// their side — kiwi_transform.h says so — and every one of those lines is
// untouched.  `kind` therefore only tells the HUD and the §13b bubble HOW TO
// FORMAT a value; it never changes what a command receives.
//
// ── UNITS (§17) ──────────────────────────────────────────────────────────────
// The user types INCHES.  KiwiNum_ValueWorld() is the only value a command ever
// sees, and it has already been through Units_FromDisplay — commands must never
// convert again.  KiwiNum_Text() is the raw typed string, for the HUD only.
//
// Both numpad and main-row keys are accepted (VK_NUMPAD0..9 / VK_DECIMAL /
// VK_SUBTRACT alongside '0'..'9' / VK_OEM_PERIOD / VK_OEM_MINUS).  A key is only
// taken when NO modifier is held, so Ctrl+Z stays undo mid-gesture.  Tab is the
// one exception: it is taken WITH or WITHOUT Shift (Shift+Tab cycles backwards)
// and is always swallowed, so it can never leak into a dialog's focus chain
// while a modal gesture owns the viewport.
//
// ── THE VALUE BUBBLE (§13b, shakeout E) ─────────────────────────────────────
// USER DIRECTIVE: "The distance unit should also be somewhere near the line/
// extrusion/whatever (see pic)" — Plasticity pins a small pill carrying the live
// value next to the geometry being dragged, with the secondary field beside it.
// KiwiNum_DrawBubble does exactly that: it asks the active command where its
// action geometry is (KiwiEditorCommand::BubbleAnchor, defaulting to the last
// snap point), projects that with Pick_WorldToImage and draws one row per field.
// It REPLACES NOTHING — the bottom-centre HUD stays as the verbose line.
// ─────────────────────────────────────────────────────────────────────────────

// ── field kinds (formatting only — see KIND IS A DISPLAY FACT above) ─────────
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
    bool          readOnly;   // display-only in the bubble; Tab skips it
};

#define KNUM_MAX_FIELDS 4

// ── lifecycle ────────────────────────────────────────────────────────────────
// Drop every entry AND the field table (called on command begin/commit/cancel).
// Leaves exactly one editable LENGTH field installed, which is the pre-shakeout-E
// grammar.
void KiwiNum_Reset();

// Install a command's fields.  `count` is clamped to KNUM_MAX_FIELDS — KIWI-UX
// (CLEANUP, B-29): the clamp now prints a console line, because a dropped field
// fails far from here (NumericFieldChanged never fires for it).  Clears the typed
// text and the focus, so it belongs in the command-start path only.
void KiwiNum_SetFields( const kiwiNumField_t *fields, int count );

// Relabel one field WITHOUT disturbing what is typed — the per-stage rename the
// primitives need ("base" → "height") mid-gesture.  `label` must be static.
void KiwiNum_SetFieldLabel( int field, const char *label );

int                   KiwiNum_FieldCount();
const kiwiNumField_t *KiwiNum_Field( int field );      // NULL when out of range

// ── focus ────────────────────────────────────────────────────────────────────
// The field digits currently edit.  0 until the first Tab, so a single-field
// command behaves exactly as it did before shakeout E.
int  KiwiNum_Focus();

// Has the user Tabbed at all during this gesture?  The bubble highlights the
// focused row only once this is true — an un-Tabbed gesture should not look like
// it is waiting for input.
bool KiwiNum_TabLive();

// Move the focus to the next / previous EDITABLE field.  False when there is
// nothing to cycle (0 or 1 editable fields).
bool KiwiNum_TabCycle( bool backwards );

// ── keys ─────────────────────────────────────────────────────────────────────
// Feed one key.  True = CONSUMED by the numeric entry.
bool KiwiNum_Key( int vk, unsigned int mods );

// Drop one field's entry (the framework's first Esc rung).
void KiwiNum_ClearField( int field );

// Drop EVERY field's entry WITHOUT touching the installed field table — what a
// STAGED command needs when it advances a stage.  KiwiNum_Reset would also
// reinstall the default single field and throw the command's own fields away,
// which is exactly the bug this exists to make unwritable (kiwi_primitive.cpp's
// ClearNumeric called Reset before shakeout E, when there were no fields to lose).
void KiwiNum_ClearEntry();

// ── KIWI-UX (ROUND AQ, ITEM 4): THE EXPRESSION PARSER, EXPOSED ──────────────
// Evaluates a typed string in DISPLAY units (inches) and returns false when it is
// not a complete, well-formed expression.  This is THE parser behind every
// KiwiNum_*Value* accessor, published so that any other typed-number box in the
// editor uses the same grammar instead of growing a second atof — the viewcube's
// grid-spacing popup (kiwi_viewcube.cpp) is the first such caller.
//
// The grammar, in one line: standard precedence + - * /, optional parentheses,
// unit suffixes y/yd/i/in/f/ft binding to the number before them, compounds by
// juxtaposition (10ft6in), bare numbers in inches, and fractions as ordinary
// division (1/8 = 0.125 in).  kiwi_numeric.cpp carries the full derivation.
//
// `outDisplay` is INCHES, not world units — feed it to Units_FromDisplay if a
// world value is wanted, which is what KiwiNum_ValueWorldField does.
bool KiwiNum_EvalDisplay( const char *text, float *outDisplay );

// ── values, FOCUSED field (the pre-shakeout-E API, unchanged in meaning) ─────
bool        KiwiNum_Has();            // is anything typed?  ("-"/"." count)
bool        KiwiNum_HasValue();       // does it parse to a number yet?
float       KiwiNum_ValueWorld();     // RAW WORLD UNITS (Units_FromDisplay applied)
const char *KiwiNum_Text();           // raw typed text, never NULL — HUD only

// ── values, BY FIELD ─────────────────────────────────────────────────────────
bool        KiwiNum_HasField     ( int field );
bool        KiwiNum_HasValueField( int field );
float       KiwiNum_ValueWorldField( int field );
const char *KiwiNum_TextField    ( int field );

// ── draw ─────────────────────────────────────────────────────────────────────
// §13 HUD: command name + current value + "in", near the bottom of the camera
// image.  Drawn during the ImGui frame from KiwiVP_DrawCameraOverlay.  No-op when
// no modal command is active.
void KiwiNum_DrawHud( float imgMinX, float imgMinY, float imgW, float imgH );

// §13b value bubble, pinned at the active command's BubbleAnchor.  Same frame,
// same ImDrawList-only rule (it can never take the camera image's hover).
void KiwiNum_DrawBubble( float imgMinX, float imgMinY, float imgW, float imgH );
