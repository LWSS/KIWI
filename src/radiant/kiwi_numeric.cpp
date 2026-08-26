#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Numeric entry and value overlays for modal commands.

#include "stdafx.h"
#include <imgui/imgui.h>

#include "kiwi_numeric.h"
#include "kiwi_command.h"
#include "kiwi_hints.h"     // Shared HUD band and hint visibility.
#include "kiwi_pick.h"
#include "kiwi_snap.h"
#include "kiwi_str.h"        // Shared ASCII case fold.
#include "kiwi_transform.h"  // Transform-gizmo activity queries.
#include "kiwi_units.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// File scope avoids MSVC namespace-linkage mismatches.
extern int Sys_Printf( const char *fmt, ... );                          // win_qe3.cpp:118

namespace
{
    // 24 chars exceeds practical coordinate input; the buffer is a hard cap.
    enum { KNUM_TEXT_MAX = 24 };

    char           s_text[KNUM_MAX_FIELDS][KNUM_TEXT_MAX] = { { 0 } };
    kiwiNumField_t s_fields[KNUM_MAX_FIELDS];
    int            s_fieldCount = 0;
    int            s_focus      = 0;
    bool           s_tabLive    = false;

    // Commands without declarations retain one unnamed editable length field.
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
            return true;                 // Consume input even when full.
        t[n]     = c;
        t[n + 1] = '\0';
        return true;
    }

    // Decimal uniqueness is per numeric run; expressions may contain several numbers.
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
                return false;              // Operators, units, and whitespace end the run.
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

    // Results stay in display inches; KiwiNum_ValueWorldField owns the sole world
    // conversion. Standard precedence, unary signs, parentheses, unit suffixes,
    // and compounds are accepted; incomplete input fails atomically.

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

    // Consumes a unit suffix and returns its multiplier into inches. Longest match
    // wins so short forms cannot prefix-match their longer forms.
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
                continue;                          // Not this suffix.
            // Require a letter boundary rather than prefix-matching an unknown word.
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
        // A unit-bearing piece may be followed by another; a bare piece ends the
        // compound, so "12 12" remains invalid.
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
                total += (float)v;                 // Bare number = inches; end the compound.
                break;
            }
            total += (float)v * mul;
            const char *save = p;
            SkipWs( p );
            if ( !StartsNumber( p ) )
            {
                p = save;                          // Leave whitespace for the expression parser.
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

    // Returns display inches; incomplete input is not a value yet.
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
        if ( v != v )                              // Reject NaN.
            return false;
        if ( v > 1.0e18f || v < -1.0e18f )         // Reject infinities and absurd magnitudes.
            return false;
        if ( outDisplay )
            *outDisplay = v;
        return true;
    }

    // Kind affects formatting only. Length values arrive in world units; other
    // kinds arrive in their natural units.
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

    // Plain input is echoed directly; expressions show an evaluated value and
    // incomplete input stays visible while the command uses its cursor value.
    // The HUD and bubble share this path so units and validity agree.
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
            // FormatValue expects world units for lengths, not display inches.
            char val[48];
            FormatValue( val, sizeof( val ), fd->kind,
                         ( fd->kind == KNUM_LENGTH ) ? Units_FromDisplay( display )
                                                     : display );
            _snprintf( buf, (size_t)bufSize, "%s = %s", s_text[f], val );
        }
        buf[bufSize - 1] = '\0';
        return true;
    }

    // Prefer typed text, then the command's live value. False omits the row.
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

// Lifecycle
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
        // Report truncation here because dropped fields never receive change events.
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

    // Start on the first editable field even when field zero is a readout.
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

// Focus
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

    // First Tab exposes the current field; subsequent Tabs advance.
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

// Key input
bool KiwiNum_Key( int vk, unsigned int mods )
{
    // Tab accepts optional Shift and must precede modifier filtering.
    if ( vk == 0x09 )                    // VK_TAB
    {
        KiwiNum_TabCycle( ( mods & 1 ) != 0 );
        return true;
    }

    // '*', '(', ')' and '+' require Shift on a US layout, and the gate
    // below rejects other modified keys. Numpad routes remain usable.
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
        // Enforce one decimal point per number, not per expression.
        if ( LastNumberHasDot( f ) )
            return true;
        return Append( f, '.' );
    }

    if ( vk == 0xBD || vk == 0x6D )      // VK_OEM_MINUS / VK_SUBTRACT
    {
        // Minus is both a leading sign and an operator; a trailing minus is incomplete.
        return Append( f, '-' );
    }

    // Remaining expression keys.
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

    // Consume unit letters only after text exists in a length field; otherwise they
    // remain tool hotkeys. The outer funnel handles preempt/swap first, so adding a
    // unit-letter binding there would steal the letter before numeric entry sees it.
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
    // Keep focus so a staged tool continues in the field the user selected.
}

// Values
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
    // Require the whole expression so partial input cannot move the preview.
    return ValidField( field ) && EvalDisplay( s_text[field], nullptr );
}

float KiwiNum_ValueWorldField( int field )
{
    float display = 0.0f;
    if ( !ValidField( field ) || !EvalDisplay( s_text[field], &display ) )
        return 0.0f;
    // Evaluator results are display inches; this is the sole world conversion,
    // regardless of field kind.
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

// HUD
void KiwiNum_DrawHud( float imgMinX, float imgMinY, float imgW, float imgH )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return;

    // Command status carries its own units and precedes the typed-field display.
    const char *state = cmd->HudStatus();

    // Once Tab activates focus, name the field receiving input.
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

    // Live, remappable hint chips own the key grammar while visible. Restore the
    // hard-coded tail only when hints are hidden.
    const bool chipsUp = KiwiHints_Show();
    const char *tail   = chipsUp
                       ? ""
                       : "RMB / Enter confirm  ·  drag adjust  ·  Tab field  ·  Esc cancel";

    // Share the bubble formatter so units and invalid state agree.
    char typed[96];
    typed[0] = '\0';
    const bool haveTyped = TypedFieldDisplay( KiwiNum_Focus(), typed, sizeof( typed ) );

    char line[400];
    if ( haveTyped )
        _snprintf( line, sizeof( line ), "%s   %s%s%s%s", cmd->Name(),
                   state ? state : "", state ? "   " : "", fieldTag, typed );
    else if ( state )
        // The tail explains pause/confirm only when the chips are hidden.
        _snprintf( line, sizeof( line ), "%s   %s   %s%s",
                   cmd->Name(), state, fieldTag, tail );
    else
        _snprintf( line, sizeof( line ), "%s   %s%s", cmd->Name(), fieldTag, tail );
    line[sizeof( line ) - 1] = '\0';
    // Dropping the tail leaves separators; trim them before measuring the box.
    for ( int t = (int)strlen( line ) - 1; t >= 0 && line[t] == ' '; --t )
        line[t] = '\0';

    // Invalid gestures stay visibly distinct from a frozen editor.
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

    // The shared bottom band avoids hints and texture readouts; fallback preserves
    // the standalone placement.
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

// Value bubble
void KiwiNum_DrawBubble( float imgMinX, float imgMinY, float imgW, float imgH )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return;

    // Prefer the command's action anchor; drawing tools fall back to the last snap.
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

    // One row per field that has something to show.
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
        // Only secondary rows need labels to disambiguate multiple values.
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

    // Drawing tools hug the anchor; active transform gizmos need 96 px clearance
    // for their roughly 80 px handles. Clamp either placement inside the image.
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
            // Mark the row receiving digits after Tab activates focus.
            dl->AddRectFilled( ImVec2( x + 2.0f, ty + 1.0f ),
                               ImVec2( x + 4.0f, ty + lineH - 1.0f ),
                               IM_COL32( 255, 205, 90, 240 ) );
        }
        dl->AddText( ImVec2( x + pad.x, ty ), col, rows[i].text );
        ty += lineH + 2.0f;
    }
}
