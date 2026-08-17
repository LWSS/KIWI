#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_numeric.cpp — RADIANT_UX_DESIGN §13 / §13b implementation.
// See kiwi_numeric.h for the field model, the "kind is a display fact" rule and
// what the shakeout-E value bubble is for.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include <imgui/imgui.h>

#include "kiwi_numeric.h"
#include "kiwi_command.h"
#include "kiwi_hints.h"     // ROUND Z, ITEM 5 — the shared bottom band + the chip toggle
#include "kiwi_pick.h"
#include "kiwi_snap.h"
#include "kiwi_str.h"        // KIWI-UX (CLEANUP): KiwiStr_LowerAscii, the one case fold
#include "kiwi_transform.h"  // KIWI-UX (CLEANUP, B-28) — KiwiXform_Is*Active
#include "kiwi_units.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// KIWI-UX (CLEANUP, B-29): the console, for the one diagnostic this file emits.
// FILE SCOPE, not block scope — kiwi_uv.cpp carries the account of why.
extern int Sys_Printf( const char *fmt, ... );                          // win_qe3.cpp:112

namespace
{
    // 24 chars is far past any coordinate a user types; the buffer is a hard cap,
    // not a scroll (one field is still one scalar).
    enum { KNUM_TEXT_MAX = 24 };

    char           s_text[KNUM_MAX_FIELDS][KNUM_TEXT_MAX] = { { 0 } };
    kiwiNumField_t s_fields[KNUM_MAX_FIELDS];
    int            s_fieldCount = 0;
    int            s_focus      = 0;
    bool           s_tabLive    = false;

    // The pre-shakeout-E grammar, installed by KiwiNum_Reset: one editable
    // LENGTH field with no name.  Every command written before shakeout E gets
    // exactly this, so nothing about their entry changes.
    const kiwiNumField_t KNUM_DEFAULT_FIELD = { 0, KNUM_LENGTH, false };

    bool ValidField( int f )
    {
        return f >= 0 && f < s_fieldCount;
    }

    int FocusedField()
    {
        return ValidField( s_focus ) ? s_focus : 0;
    }

    bool Append( int f, char c )
    {
        char *t = s_text[f];
        const size_t n = strlen( t );
        if ( n + 1 >= (size_t)KNUM_TEXT_MAX )
            return true;                 // consumed, but full — ignore the key
        t[n]     = c;
        t[n + 1] = '\0';
        return true;
    }

    // KIWI-UX (ROUND AQ, ITEM 4): "one decimal point" is a property of the number
    // being typed, not of the whole field — an expression may hold several.  Walk
    // back from the end over the current numeric run only.
    bool LastNumberHasDot( int f )
    {
        const char  *t = s_text[f];
        const size_t n = strlen( t );
        for ( size_t i = n; i-- > 0; )
        {
            const char c = t[i];
            if ( c == '.' )
                return true;
            if ( !( c >= '0' && c <= '9' ) )
                return false;              // an operator/space/letter ends the run
        }
        return false;
    }

    bool ParsesToNumber( const char *t )
    {
        // "-", ".", "-." are typed-but-not-a-number.  Require at least one digit.
        for ( const char *p = t; *p; ++p )
            if ( *p >= '0' && *p <= '9' )
                return true;
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AQ, ITEM 4) — THE EXPRESSION EVALUATOR
    // ═════════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "allow basic math operations when typing units.
    // Also allow y/yd/i/in/f/ft for unit specifier.  So I can do ex: 12 * 12
    // (default inch).  OR 12ft * 10 (120ft) OR 10ft6in ALT 10f6i OR 120 + 120
    // (240 inches).  Also allow fractions like 1/8."
    //
    // Everything below works in DISPLAY UNITS (inches).  The single conversion to
    // world units stays exactly where it was — KiwiNum_ValueWorldField's
    // Units_FromDisplay — so this is a strictly better `atof` and nothing about
    // the "kind is a display fact" rule (kiwi_numeric.h) changes.
    //
    // GRAMMAR (standard precedence, chosen over left-to-right because "120 + 120
    // * 2" meaning 360 is what a calculator, a spreadsheet and every CAD input
    // field in existence do — a mapper who wants the other reading has
    // parentheses, and a mapper who assumes left-to-right has no way to ask for
    // precedence at all):
    //
    //     expr    := term  { ('+' | '-') term }
    //     term    := factor { ('*' | '/') factor }
    //     factor  := ['-' | '+'] factor | '(' expr ')' | compound
    //     compound:= number [unit] { number [unit] }        // 10ft6in, 10f6i
    //     unit    := y|yd|yds|yard|yards | f|ft|feet|foot | i|in|ins|inch|inches
    //
    // A bare number is INCHES, which is the display unit, so "120 + 120" is 240 in
    // and "12 * 12" is 144 in.  A COMPOUND is an implicit sum and it only chains
    // while each piece carries a unit — "10ft6in" is 126 in, while "12 12" is not
    // an expression at all and is rejected rather than guessed at.
    //
    // FRACTIONS NEED NO SPECIAL CASE, and that is the point of using the same
    // operator: "1/8" is one divided by eight, i.e. 0.125 in, and "10ft/2" is one
    // hundred and twenty divided by two, i.e. 60 in = 5 ft.  One rule, both
    // readings, no mode.
    //
    // FAILURE IS TOTAL AND SILENT.  Any malformed input — a trailing operator, an
    // unbalanced paren, a division by zero, junk after the expression — returns
    // false, which the callers already treat as "this field has no value yet" and
    // which therefore leaves the gesture on its CURSOR value.  A half-typed
    // "12 *" is exactly that state, so an expression can be typed one character at
    // a time without the preview ever seeing garbage.
    // ═════════════════════════════════════════════════════════════════════════

    bool IsDigit( char c ) { return c >= '0' && c <= '9'; }

    void SkipWs( const char *&p )
    {
        while ( *p == ' ' || *p == '\t' )
            ++p;
    }

    bool StartsNumber( const char *p )
    {
        return IsDigit( *p ) || ( *p == '.' && IsDigit( p[1] ) );
    }

    // KIWI-UX (CLEANUP, wave-2 leftover): this file's copy of the ASCII case fold
    // is gone — KiwiStr_LowerAscii (kiwi_str.h) is the one spelling.  It computes
    // `c + ( 'a' - 'A' )` where this one computed `c - 'A' + 'a'`: the same +32
    // after the identical ['A','Z'] guard, so it is byte-for-byte the same answer
    // on every input the unit-suffix parser feeds it, including bytes >= 0x80
    // (negative on this signed-char target), which both spellings pass through.

    // A unit suffix at `p`, consumed on a match.  Returns the multiplier INTO
    // INCHES, or 0 when there is no suffix here.  Longest match first, so "in"
    // never resolves as "i" and "ft" never as "f".
    float ParseUnitSuffix( const char *&p )
    {
        struct unitRow_t { const char *s; float mul; };
        static const unitRow_t s_units[] =
        {
            { "yards", 36.0f }, { "yard", 36.0f }, { "yds", 36.0f },
            { "yd",    36.0f }, { "y",    36.0f },
            { "inches", 1.0f }, { "inch",  1.0f }, { "ins",  1.0f },
            { "in",     1.0f }, { "i",     1.0f },
            { "feet",  12.0f }, { "foot", 12.0f }, { "ft",  12.0f },
            { "f",     12.0f },
        };
        for ( int u = 0; u < (int)( sizeof( s_units ) / sizeof( s_units[0] ) ); ++u )
        {
            const char *s = s_units[u].s;
            int         k = 0;
            while ( s[k] && KiwiStr_LowerAscii( p[k] ) == s[k] )
                ++k;
            if ( s[k] )
                continue;                          // ran out of input before the word
            // Do not let "in" swallow the "i" of a longer word we do not know: the
            // next character must not be another letter.
            const char n = KiwiStr_LowerAscii( p[k] );
            if ( n >= 'a' && n <= 'z' )
                continue;
            p += k;
            return s_units[u].mul;
        }
        return 0.0f;
    }

    float ParseExpr( const char *&p, bool &ok );

    float ParseFactor( const char *&p, bool &ok )
    {
        SkipWs( p );
        if ( *p == '-' ) { ++p; return -ParseFactor( p, ok ); }
        if ( *p == '+' ) { ++p; return  ParseFactor( p, ok ); }
        if ( *p == '(' )
        {
            ++p;
            const float v = ParseExpr( p, ok );
            SkipWs( p );
            if ( *p == ')' ) ++p;
            else             ok = false;
            return v;
        }
        if ( !StartsNumber( p ) )
        {
            ok = false;
            return 0.0f;
        }
        // The COMPOUND: one number, optionally a unit, and then — only while every
        // piece so far carried a unit — another number.  A unit-less piece ends it,
        // which is what stops "12 12" from silently becoming 24.
        float total = 0.0f;
        for ( ;; )
        {
            char       *end = nullptr;
            const double v  = strtod( p, &end );
            if ( end == p )
            {
                ok = false;
                return 0.0f;
            }
            p = end;
            const float mul = ParseUnitSuffix( p );
            if ( mul <= 0.0f )
            {
                total += (float)v;                 // bare number = inches, and it ends the chain
                break;
            }
            total += (float)v * mul;
            const char *save = p;
            SkipWs( p );
            if ( !StartsNumber( p ) )
            {
                p = save;                          // not a compound tail — leave the ws alone
                break;
            }
        }
        return total;
    }

    float ParseTerm( const char *&p, bool &ok )
    {
        float v = ParseFactor( p, ok );
        while ( ok )
        {
            SkipWs( p );
            if ( *p == '*' )
            {
                ++p;
                v *= ParseFactor( p, ok );
            }
            else if ( *p == '/' )
            {
                ++p;
                const float d = ParseFactor( p, ok );
                if ( !ok || ( d > -1.0e-9f && d < 1.0e-9f ) )
                {
                    ok = false;
                    return 0.0f;
                }
                v /= d;
            }
            else
            {
                break;
            }
        }
        return v;
    }

    float ParseExpr( const char *&p, bool &ok )
    {
        float v = ParseTerm( p, ok );
        while ( ok )
        {
            SkipWs( p );
            if      ( *p == '+' ) { ++p; v += ParseTerm( p, ok ); }
            else if ( *p == '-' ) { ++p; v -= ParseTerm( p, ok ); }
            else                  { break; }
        }
        return v;
    }

    // THE parser.  `outDisplay` comes back in INCHES; false means "not a value
    // yet", which every caller already handles as "use the cursor instead".
    bool EvalDisplay( const char *t, float *outDisplay )
    {
        if ( !t || !*t || !ParsesToNumber( t ) )
            return false;
        bool        ok = true;
        const char *p  = t;
        const float v  = ParseExpr( p, ok );
        SkipWs( p );
        if ( !ok || *p )
            return false;
        if ( v != v )                              // NaN
            return false;
        if ( v > 1.0e18f || v < -1.0e18f )         // inf / absurd
            return false;
        if ( outDisplay )
            *outDisplay = v;
        return true;
    }

    // Kind drives FORMATTING ONLY (kiwi_numeric.h).  `v` arrives in the field's
    // own natural unit: raw world units for a LENGTH, degrees for an ANGLE, a
    // bare multiplier / integer for the other two.
    void FormatValue( char *buf, int bufSize, kiwiNumKind_t kind, float v )
    {
        if ( !buf || bufSize < 1 )
            return;
        switch ( kind )
        {
        case KNUM_ANGLE:
            _snprintf( buf, (size_t)bufSize, "%.1f deg", (double)v );
            break;
        case KNUM_FACTOR:
            _snprintf( buf, (size_t)bufSize, "x%.3f", (double)v );
            break;
        case KNUM_COUNT:
            _snprintf( buf, (size_t)bufSize, "%d", (int)( v + ( v < 0.0f ? -0.5f : 0.5f ) ) );
            break;
        default:
            KiwiUnits_Format( buf, bufSize, v );
            break;
        }
        buf[bufSize - 1] = '\0';
    }

    // ── KIWI-UX (ROUND AQ, ITEM 4): SAY WHAT THE EXPRESSION COMES TO ──────────
    // A typed value used to be echoed with a hardcoded suffix ("%s in"), which was
    // honest while the only legal input was a bare number.  It is a lie for
    // "10ft6in" and useless for "12 * 12".  So: a plain number still echoes exactly
    // as before, an EXPRESSION echoes with its evaluated result beside it, and
    // something that does not evaluate is marked rather than shown as if it were a
    // value.  That mark IS the "invalid expression keeps the field live + a status
    // line" the directive asks for — the field keeps its text, the command keeps its
    // cursor value, and the row says why.
    //
    // KIWI-UX (CLEANUP, B-3): hoisted out of FieldDisplay so the HUD LINE can use it
    // too.  The HUD had its own hardcoded `"%s in"` — the very defect the paragraph
    // above names and fixes — so "10ft6in" printed as "10ft6in in" and "12 *" was
    // presented as if it were a value.  ONE spelling of "how a typed field reads",
    // and the KNUM_LENGTH -> Units_FromDisplay conversion (which the HUD path did not
    // do at all) comes with it.  Returns false when the field has no typed text.
    bool TypedFieldDisplay( int f, char *buf, int bufSize )
    {
        const kiwiNumField_t *fd = KiwiNum_Field( f );
        if ( !fd || !s_text[f][0] )
            return false;

        float       display = 0.0f;
        const bool  valid   = EvalDisplay( s_text[f], &display );
        bool        plain   = true;
        for ( const char *q = s_text[f]; *q; ++q )
            if ( !( ( *q >= '0' && *q <= '9' ) || *q == '.'
                    || ( *q == '-' && q == s_text[f] ) ) )
                { plain = false; break; }

        if ( !valid )
        {
            _snprintf( buf, (size_t)bufSize, "%s  (incomplete)", s_text[f] );
        }
        else if ( plain )
        {
            if ( fd->kind == KNUM_LENGTH )
                _snprintf( buf, (size_t)bufSize, "%s in", s_text[f] );
            else if ( fd->kind == KNUM_ANGLE )
                _snprintf( buf, (size_t)bufSize, "%s deg", s_text[f] );
            else
                _snprintf( buf, (size_t)bufSize, "%s", s_text[f] );
        }
        else
        {
            // The evaluator works in DISPLAY units, and FormatValue wants the
            // field's own natural unit — which for a LENGTH is world units.
            char val[48];
            FormatValue( val, sizeof( val ), fd->kind,
                         ( fd->kind == KNUM_LENGTH ) ? Units_FromDisplay( display )
                                                     : display );
            _snprintf( buf, (size_t)bufSize, "%s = %s", s_text[f], val );
        }
        buf[bufSize - 1] = '\0';
        return true;
    }

    // What the bubble shows for one field: the TYPED text while the user is
    // typing (with the §17 unit suffix a length needs), the command's LIVE value
    // otherwise.  False = this field has nothing to say and gets no row.
    bool FieldDisplay( const KiwiEditorCommand *cmd, int f, char *buf, int bufSize )
    {
        const kiwiNumField_t *fd = KiwiNum_Field( f );
        if ( !fd )
            return false;

        if ( TypedFieldDisplay( f, buf, bufSize ) )
            return true;

        float v = 0.0f;
        if ( !cmd || !cmd->NumericFieldValue( f, &v ) )
            return false;
        FormatValue( buf, bufSize, fd->kind, v );
        return true;
    }
}

// ─── lifecycle ───────────────────────────────────────────────────────────────
void KiwiNum_Reset()
{
    for ( int i = 0; i < KNUM_MAX_FIELDS; ++i )
        s_text[i][0] = '\0';
    s_fields[0]  = KNUM_DEFAULT_FIELD;
    s_fieldCount = 1;
    s_focus      = 0;
    s_tabLive    = false;
}

void KiwiNum_SetFields( const kiwiNumField_t *fields, int count )
{
    if ( !fields || count < 1 )
    {
        KiwiNum_Reset();
        return;
    }
    if ( count > KNUM_MAX_FIELDS )
    {
        // KIWI-UX (CLEANUP, B-29): the truncation is now audible.  A dropped field
        // fails FAR from here — NumericFieldChanged for any index past the cap
        // simply never fires, and the command reads as "that field does nothing" —
        // so the one place that knows says so.  Same early-out, one line louder.
        Sys_Printf( "Numeric: a command declared %i fields; only %i (KNUM_MAX_FIELDS) "
                    "are shown, the rest will never receive input.\n",
                    count, KNUM_MAX_FIELDS );
        count = KNUM_MAX_FIELDS;
    }

    for ( int i = 0; i < KNUM_MAX_FIELDS; ++i )
        s_text[i][0] = '\0';
    for ( int i = 0; i < count; ++i )
        s_fields[i] = fields[i];
    s_fieldCount = count;
    s_tabLive    = false;

    // Focus the first EDITABLE field, so a command whose field 0 is a read-only
    // readout still takes digits somewhere sensible without a Tab.
    s_focus = 0;
    for ( int i = 0; i < count; ++i )
        if ( !s_fields[i].readOnly )
        {
            s_focus = i;
            break;
        }
}

void KiwiNum_SetFieldLabel( int field, const char *label )
{
    if ( !ValidField( field ) )
        return;
    s_fields[field].label = label;
}

int KiwiNum_FieldCount()
{
    return s_fieldCount;
}

const kiwiNumField_t *KiwiNum_Field( int field )
{
    return ValidField( field ) ? &s_fields[field] : 0;
}

// ─── focus ───────────────────────────────────────────────────────────────────
int KiwiNum_Focus()
{
    return FocusedField();
}

bool KiwiNum_TabLive()
{
    return s_tabLive;
}

bool KiwiNum_TabCycle( bool backwards )
{
    int editable = 0;
    for ( int i = 0; i < s_fieldCount; ++i )
        if ( !s_fields[i].readOnly )
            ++editable;
    if ( editable < 1 )
        return false;

    // The FIRST Tab does not move: it FOCUSES.  Plasticity's dialog behaves the
    // same way — one Tab puts you in the first box, the next walks on — and it
    // means "Tab, type, Enter" always lands in the field the HUD is naming.
    if ( !s_tabLive )
    {
        s_tabLive = true;
        if ( !s_fields[FocusedField()].readOnly )
            return true;
    }
    if ( editable < 2 )
        return true;

    int f = FocusedField();
    for ( int step = 0; step < s_fieldCount; ++step )
    {
        f = backwards ? ( f - 1 ) : ( f + 1 );
        if ( f < 0 )              f = s_fieldCount - 1;
        if ( f >= s_fieldCount )  f = 0;
        if ( !s_fields[f].readOnly )
        {
            s_focus = f;
            return true;
        }
    }
    return true;
}

// ─── keys ────────────────────────────────────────────────────────────────────
bool KiwiNum_Key( int vk, unsigned int mods )
{
    // Tab is taken WITH or WITHOUT Shift and is always swallowed — see the note
    // in kiwi_numeric.h.  It is tested above the modifier gate for exactly that
    // reason (Shift+Tab is a real binding here, not a stray modified key).
    if ( vk == 0x09 )                    // VK_TAB
    {
        KiwiNum_TabCycle( ( mods & 1 ) != 0 );
        return true;
    }

    // ── KIWI-UX (ROUND AQ, ITEM 4): THE SHIFTED OPERATORS ───────────────────
    // '*', '(' and ')' cannot be typed without Shift on a US layout, and the gate
    // below refuses every modified key.  So a NARROW shifted set is taken first,
    // by VK, and everything else modified still falls through untouched.  The
    // numpad (VK_MULTIPLY / VK_ADD / VK_DIVIDE, handled below) and VK_OEM_PLUS
    // give unshifted routes to the same characters, which is what non-US layouts
    // — where these VKs do not carry these glyphs — should use.
    if ( mods == 1 )                     // Shift, and nothing else
    {
        const int f = FocusedField();
        switch ( vk )
        {
        case '8':  return Append( f, '*' );
        case '9':  return Append( f, '(' );
        case '0':  return Append( f, ')' );
        case 0xBB: return Append( f, '+' );   // VK_OEM_PLUS — Shift+'=' is '+'
        default:   break;
        }
        return false;
    }

    if ( mods )                          // Ctrl+Z / Shift+… stay themselves
        return false;

    const int f = FocusedField();

    if ( vk == 0x08 )                    // VK_BACK
    {
        const size_t n = strlen( s_text[f] );
        if ( n )
            s_text[f][n - 1] = '\0';
        return true;                     // consumed even when empty: never a hotkey mid-gesture
    }

    if ( vk >= '0' && vk <= '9' )
        return Append( f, (char)vk );
    if ( vk >= 0x60 && vk <= 0x69 )      // VK_NUMPAD0..VK_NUMPAD9
        return Append( f, (char)( '0' + ( vk - 0x60 ) ) );

    if ( vk == 0xBE || vk == 0x6E )      // VK_OEM_PERIOD / VK_DECIMAL
    {
        // KIWI-UX (ROUND AQ, ITEM 4): one dot per NUMBER, not one per field — an
        // expression legitimately holds several ("1.5ft + 0.25in").  The old
        // whole-field HasDot test would have refused the second one.
        if ( LastNumberHasDot( f ) )
            return true;
        return Append( f, '.' );
    }

    if ( vk == 0xBD || vk == 0x6D )      // VK_OEM_MINUS / VK_SUBTRACT
    {
        // KIWI-UX (ROUND AQ, ITEM 4): minus is now BOTH the leading sign and the
        // subtraction operator, so it is no longer refused once the field has
        // text.  A trailing "-" simply makes the expression not evaluate, which is
        // the same "no value yet" state a trailing "*" produces.
        return Append( f, '-' );
    }

    // ── KIWI-UX (ROUND AQ, ITEM 4): THE REST OF THE EXPRESSION ALPHABET ─────
    if ( vk == 0x6B )                    // VK_ADD (numpad +)
        return Append( f, '+' );
    if ( vk == 0x6A )                    // VK_MULTIPLY (numpad *)
        return Append( f, '*' );
    if ( vk == 0x6F || vk == 0xBF )      // VK_DIVIDE (numpad /) / VK_OEM_2 ('/')
        return Append( f, '/' );
    if ( vk == 0xBB )                    // VK_OEM_PLUS unshifted ('=') — read as '+'
        return Append( f, '+' );
    if ( vk == 0x20 )                    // VK_SPACE — "10ft 6in", and a separator only
        return s_text[f][0] ? Append( f, ' ' ) : false;

    // ── THE UNIT LETTERS, AND WHY THEY ARE GATED ────────────────────────────
    // y / yd / i / in / f / ft, per the directive.  A letter is taken ONLY when it
    // can CONTINUE what is already in the field, i.e. the field is a LENGTH and
    // something has been typed into it.  That gate is what keeps the tool hotkeys
    // alive: with an empty field every one of these letters still falls straight
    // through to the command and to the binding table exactly as it did before, so
    // "F", "T" and the rest keep meaning what they mean when the user is not
    // mid-number.  (Known limit: the funnel's preempt/swap rungs run BEFORE this
    // one, so a letter bound to a swappable verb can still be taken as that verb
    // while a gesture is idle — the numpad and OEM operator routes above are
    // unaffected, and no unit letter is bound to a swap verb today.)
    {
        const kiwiNumField_t *fd = KiwiNum_Field( f );
        if ( fd && fd->kind == KNUM_LENGTH && s_text[f][0] )
        {
            switch ( vk )
            {
            case 'Y': return Append( f, 'y' );
            case 'D': return Append( f, 'd' );
            case 'I': return Append( f, 'i' );
            case 'N': return Append( f, 'n' );
            case 'F': return Append( f, 'f' );
            case 'T': return Append( f, 't' );
            default:  break;
            }
        }
    }

    return false;
}

void KiwiNum_ClearField( int field )
{
    if ( ValidField( field ) )
        s_text[field][0] = '\0';
}

void KiwiNum_ClearEntry()
{
    for ( int i = 0; i < KNUM_MAX_FIELDS; ++i )
        s_text[i][0] = '\0';
    // The FOCUS is deliberately kept: a staged tool that has just advanced to its
    // height stage should still be typing into the field the user Tabbed to.
}

// ─── values ──────────────────────────────────────────────────────────────────
// KIWI-UX (ROUND AQ, ITEM 4): the published entry point to the one parser.
bool KiwiNum_EvalDisplay( const char *text, float *outDisplay )
{
    return EvalDisplay( text, outDisplay );
}

bool KiwiNum_HasField( int field )
{
    return ValidField( field ) && s_text[field][0] != '\0';
}

bool KiwiNum_HasValueField( int field )
{
    // KIWI-UX (ROUND AQ, ITEM 4): the whole expression must evaluate, not merely
    // contain a digit.  A half-typed "12 *" is therefore "no value yet" and the
    // gesture stays on its cursor value instead of jumping to 12 and back.
    return ValidField( field ) && EvalDisplay( s_text[field], nullptr );
}

float KiwiNum_ValueWorldField( int field )
{
    float display = 0.0f;
    if ( !ValidField( field ) || !EvalDisplay( s_text[field], &display ) )
        return 0.0f;
    // §17: the typed number is INCHES; this is the one conversion boundary, and
    // it applies to EVERY kind (kiwi_numeric.h "KIND IS A DISPLAY FACT").
    // ROUND AQ: `display` now comes from the expression evaluator instead of
    // atof, and the evaluator has already folded every ft/yd suffix down to
    // inches — so this boundary is unmoved and still the only one.
    return Units_FromDisplay( display );
}

const char *KiwiNum_TextField( int field )
{
    return ValidField( field ) ? s_text[field] : "";
}

bool  KiwiNum_Has()        { return KiwiNum_HasField( FocusedField() ); }
bool  KiwiNum_HasValue()   { return KiwiNum_HasValueField( FocusedField() ); }
float KiwiNum_ValueWorld() { return KiwiNum_ValueWorldField( FocusedField() ); }
const char *KiwiNum_Text() { return KiwiNum_TextField( FocusedField() ); }

// ─── HUD ─────────────────────────────────────────────────────────────────────
void KiwiNum_DrawHud( float imgMinX, float imgMinY, float imgW, float imgH )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return;

    // KIWI-UX (Phase 3): the transform state sits between the op name and the typed
    // value — "Move   faces  axis X  64 in   [12]".  HudStatus() is owned by the
    // command (kiwi_command.h) and already carries its own units; the typed buffer
    // is still raw text plus the "in" suffix §17 requires.
    const char *state = cmd->HudStatus();

    // KIWI-UX (shakeout E): once the user has Tabbed, the HUD names the field the
    // digits are going into — otherwise a two-field command gives no feedback at
    // all about which box is live.
    char fieldTag[40];
    fieldTag[0] = '\0';
    if ( KiwiNum_TabLive() )
    {
        const kiwiNumField_t *fd = KiwiNum_Field( KiwiNum_Focus() );
        if ( fd && fd->label )
        {
            _snprintf( fieldTag, sizeof( fieldTag ), "[%s] ", fd->label );
            fieldTag[sizeof( fieldTag ) - 1] = '\0';
        }
    }

    // ── KIWI-UX (ROUND Z, ITEM 5): THE CHIPS OWN THE GRAMMAR ────────────────
    // USER REPORT (screenshot): this line and the chip strip were drawn on top of
    // each other AND said the same thing — "RMB / Enter confirm · drag adjust · Tab
    // field · Esc cancel" here, "RMB/Enter Confirm | Esc Cancel | Drag Adjust | Tab
    // Field | 0-9 Exact" there.  The shared band (kiwi_hints.h) stops them
    // OVERLAPPING; this stops them being two answers to one question.
    //
    // WHICH ONE YIELDS, and why this one: the chip strip reads its keys LIVE out of
    // g_radiantCommands and shows them as keycaps, so it is the one that cannot go
    // stale when a binding is remapped.  This tail is a hard-coded sentence.  So the
    // tail is dropped WHILE THE CHIPS ARE UP and the line keeps what only it has —
    // the command name, its transform state and the typed value.
    //
    // With the hints toggled OFF (KiwiHints_Show false) the tail comes back in full,
    // because then it is the only place the grammar is written down at all.
    const bool chipsUp = KiwiHints_Show();
    const char *tail   = chipsUp
                       ? ""
                       : "RMB / Enter confirm  ·  drag adjust  ·  Tab field  ·  Esc cancel";

    // KIWI-UX (CLEANUP, B-3): the typed value is rendered by the SAME helper the
    // bubble uses, so an expression reads "12 * 12 = 12 ft" and an unevaluatable one
    // reads "(incomplete)" instead of both being suffixed with a bare " in".
    char typed[96];
    typed[0] = '\0';
    const bool haveTyped = TypedFieldDisplay( KiwiNum_Focus(), typed, sizeof( typed ) );

    char line[400];
    if ( haveTyped )
        _snprintf( line, sizeof( line ), "%s   %s%s%s%s", cmd->Name(),
                   state ? state : "", state ? "   " : "", fieldTag, typed );
    else if ( state )
        // KIWI-UX (shakeout E): the confirm flow changed — an LMB release PAUSES,
        // RMB-click or Enter CONFIRMS.  The line the user reads every gesture has
        // to say so… unless the chips already are (round Z, above).
        _snprintf( line, sizeof( line ), "%s   %s   %s%s",
                   cmd->Name(), state, fieldTag, tail );
    else
        _snprintf( line, sizeof( line ), "%s   %s%s", cmd->Name(), fieldTag, tail );
    line[sizeof( line ) - 1] = '\0';
    // With the tail dropped (above) the formats leave their separator behind, and a
    // box sized on CalcTextSize would carry that as dead width.  Trimmed rather than
    // branched, so the three formats stay one shape.
    for ( int t = (int)strlen( line ) - 1; t >= 0 && line[t] == ' '; --t )
        line[t] = '\0';

    // §19: an INVALID gesture reads RED and says so — the user must never mistake
    // "the geometry stopped following the cursor" for a frozen editor.
    const bool  invalid = cmd->HudInvalid();
    const ImU32 frameCol = invalid ? IM_COL32( 235,  70,  55, 230 )
                                   : IM_COL32(  90, 160, 220, 200 );
    const ImU32 textCol  = invalid ? IM_COL32( 255, 140, 125, 255 )
                         : KiwiNum_Has() ? IM_COL32( 255, 225, 130, 255 )
                                         : IM_COL32( 195, 200, 210, 255 );

    const ImVec2 sz = ImGui::CalcTextSize( line );
    const ImVec2 pad( 9.0f, 5.0f );
    const float  boxW = sz.x + pad.x * 2.0f;
    const float  boxH = sz.y + pad.y * 2.0f;

    // Bottom-centre of the camera image — out of the way of the top-left chips.
    // ROUND Z, ITEM 5: horizontally centred as always; the VERTICAL anchor comes
    // from the shared bottom band (kiwi_hints.h KiwiHud_BandTake), which is what
    // stops this landing on top of the chip strip and the texture readout.  The
    // fallback is the geometry this line used before the band existed.
    float x = imgMinX + ( imgW - boxW ) * 0.5f;
    float y = KiwiHud_BandTake( boxH, imgMinY + imgH - boxH - 12.0f );
    if ( x < imgMinX ) x = imgMinX;
    if ( y < imgMinY ) y = imgMinY;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled( ImVec2( x, y ), ImVec2( x + boxW, y + boxH ),
                       invalid ? IM_COL32( 34, 14, 14, 235 )
                               : IM_COL32( 16, 16, 20, 225 ), 4.0f );
    dl->AddRect( ImVec2( x, y ), ImVec2( x + boxW, y + boxH ),
                 frameCol, 4.0f, 0, invalid ? 2.0f : 1.0f );
    dl->AddText( ImVec2( x + pad.x, y + pad.y ), textCol, line );
}

// ─── §13b the value bubble, pinned at the ACTION GEOMETRY ────────────────────
void KiwiNum_DrawBubble( float imgMinX, float imgMinY, float imgW, float imgH )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return;

    // WHERE.  The command's own anchor if it has one; otherwise the last snap
    // point, which for the drawing tools IS the moving end of the segment — the
    // exact place Plasticity puts it (kiwi_command.h BubbleAnchor).
    float anchor[3];
    if ( !cmd->BubbleAnchor( anchor ) )
    {
        const snap_result_t &s = KiwiCmd_LastSnap();
        if ( !s.valid )
            return;
        anchor[0] = s.position[0];
        anchor[1] = s.position[1];
        anchor[2] = s.position[2];
    }

    float sx = 0.0f, sy = 0.0f;
    if ( !Pick_WorldToImage( anchor, &sx, &sy ) )
        return;                                  // behind the eye — no bubble

    // WHAT.  One row per field that has something to say.
    struct { char text[64]; bool focused; } rows[KNUM_MAX_FIELDS];
    int   n        = 0;
    float widest   = 0.0f;
    const int nf   = KiwiNum_FieldCount();
    const int foc  = KiwiNum_Focus();
    const bool tab = KiwiNum_TabLive();

    for ( int i = 0; i < nf && n < KNUM_MAX_FIELDS; ++i )
    {
        char body[48];
        if ( !FieldDisplay( cmd, i, body, sizeof( body ) ) )
            continue;
        const kiwiNumField_t *fd = KiwiNum_Field( i );
        // The primary row is bare ("0.609 in"); a secondary row carries its own
        // label, because "36.4 deg" alone is only unambiguous when it is the only
        // number on screen.
        if ( n == 0 || !fd || !fd->label )
            _snprintf( rows[n].text, sizeof( rows[n].text ), "%s", body );
        else
            _snprintf( rows[n].text, sizeof( rows[n].text ), "%s %s", fd->label, body );
        rows[n].text[sizeof( rows[n].text ) - 1] = '\0';
        rows[n].focused = ( tab && i == foc );

        const float w = ImGui::CalcTextSize( rows[n].text ).x;
        if ( w > widest )
            widest = w;
        ++n;
    }
    if ( n <= 0 )
        return;

    const ImVec2 pad( 7.0f, 4.0f );
    const float  lineH = ImGui::GetTextLineHeight();
    const float  boxW  = widest + pad.x * 2.0f;
    const float  boxH  = lineH * (float)n + pad.y * 2.0f + ( n > 1 ? 2.0f * (float)( n - 1 ) : 0.0f );

    // Offset up-and-right of the anchor so the pill never sits ON the geometry it
    // is measuring, then clamped inside the image.
    // ── KIWI-UX (ROUND AN, ITEM 8): CLEAR OF THE GIZMO ──────────────────────
    // USER REPORT, verbatim: "the measurement label gets in the way of the gizmo
    // sometimes."  The gizmo is ~80 screen px of handles centred on the same
    // anchor this bubble pins to; +16/-10 put the box straight onto the Y arrow
    // and the plane squares.  While a transform gizmo is up, the bubble stands
    // off far enough to clear the arrow tips (KGZ reach + head + a margin);
    // otherwise the old tight offset stays (drawing tools have no gizmo and the
    // bubble should hug the point there).
    // KIWI-UX (CLEANUP, B-28): these two used to be BLOCK-SCOPE externs here.
    // Both are declared by kiwi_transform.h, which this file now includes — an
    // owning header beats a re-declaration, and it removes the MSVC
    // namespace-mangling hazard kiwi_uv.cpp documents.
    const bool gizmoUp = KiwiXform_IsMoveActive() || KiwiXform_IsRotateActive();
    const float standoff = gizmoUp ? 96.0f : 16.0f;
    float x = imgMinX + sx + standoff;
    float y = imgMinY + sy - boxH - ( gizmoUp ? 48.0f : 10.0f );
    const float maxX = imgMinX + imgW - boxW - 2.0f;
    const float maxY = imgMinY + imgH - boxH - 2.0f;
    if ( x > maxX ) x = maxX;
    if ( y > maxY ) y = maxY;
    if ( x < imgMinX + 2.0f ) x = imgMinX + 2.0f;
    if ( y < imgMinY + 2.0f ) y = imgMinY + 2.0f;

    const bool  invalid = cmd->HudInvalid();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled( ImVec2( x, y ), ImVec2( x + boxW, y + boxH ),
                       invalid ? IM_COL32( 40, 16, 16, 225 )
                               : IM_COL32( 14, 14, 18, 215 ), 4.0f );
    dl->AddRect( ImVec2( x, y ), ImVec2( x + boxW, y + boxH ),
                 invalid ? IM_COL32( 235, 70, 55, 220 )
                         : IM_COL32( 120, 128, 148, 170 ), 4.0f, 0, 1.0f );

    float ty = y + pad.y;
    for ( int i = 0; i < n; ++i )
    {
        const ImU32 col = invalid        ? IM_COL32( 255, 150, 135, 255 )
                        : rows[i].focused ? IM_COL32( 255, 225, 130, 255 )
                        : ( i == 0 )      ? IM_COL32( 235, 240, 250, 255 )
                                          : IM_COL32( 180, 188, 205, 235 );
        if ( rows[i].focused )
        {
            // A thin caret bar in the margin marks the box the digits go into —
            // the one thing the Tab cycling is useless without.
            dl->AddRectFilled( ImVec2( x + 2.0f, ty + 1.0f ),
                               ImVec2( x + 4.0f, ty + lineH - 1.0f ),
                               IM_COL32( 255, 205, 90, 240 ) );
        }
        dl->AddText( ImVec2( x + pad.x, ty ), col, rows[i].text );
        ty += lineH + 2.0f;
    }
}
