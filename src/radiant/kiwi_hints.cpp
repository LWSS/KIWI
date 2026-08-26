#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// Contextual hint strips: current key grammar at bottom-left and selection verbs
// at bottom-right. A live command suppresses selection verbs and adds HudPrompts.
// Command-backed chips read g_radiantCommands live; literal prompt keys belong to
// modal/input paths that have no command-table row.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_hints.h"
#include "radiant_frame.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_grid.h"
#include "kiwi_lollipop.h"
#include "kiwi_region.h"
#include "kiwi_conselect.h"
#include "kiwi_selection.h"
#include "kiwi_sun.h"
#include "kiwi_transform.h"
#include "kiwi_units.h"
#include "radiant_registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// Keep external bindings outside the anonymous namespace so MSVC preserves their
// linkage to definitions in other translation units.
extern bool        KiwiPatchVerts_Active();                                 // kiwi_patchverts.cpp
// Terrain-paint armed gate from patchdialog.cpp:229 (0x401D50); it matches the
// cursor-ring and Alt+LMB routing gate.
extern int         sub_401D50();                                            // patchdialog.cpp:229

namespace
{
    // Cap each strip at 12 chips and two rows to bound bottom-band height.
    enum { KHINT_MAX_CHIPS = 12, KHINT_MAX_ROWS = 2 };

    // Requested layout diverges from Plasticity 0.6: prompts left, verbs right.
    constexpr bool KHINT_PROMPTS_LEFT = true;

    int s_show = -1;                    // -1 = not read from the profile yet

    // 32 fits long modifier chords plus palette wrapping; LiveKey and storage share it.
    enum { KHINT_KEYCAP_MAX = 32 };

    struct chip_t
    {
        char  key[KHINT_KEYCAP_MAX];
        char  label[40];
        // Cached once for both the measure and draw passes.
        float kw;                       // keycap box width (text + 2 * KHINT_PAD_X)
        float w;                        // whole chip: keycap + gap + label
    };

    // Compact live binding text; CommandList_Mods targets a much wider panel.
    // Keep the ported key-name table faithful and localize missing VK_OEM names here.
    const char *OemKeyName( unsigned vk )
    {
        switch ( vk )
        {
        case 0xBA: return ";";
        case 0xBB: return "=";
        case 0xBF: return "/";
        case 0xC0: return "~";
        case 0xDE: return "'";
        default:   return 0;
        }
    }

    bool LiveKey( int commandId, char *out, size_t outSize )
    {
        const RadiantCommand *table = 0;
        const int count = Radiant_GetCommandTable( &table );
        if ( !table )
            return false;
        for ( int i = 0; i < count; ++i )
        {
            if ( table[i].commandId != commandId || !table[i].vk )
                continue;
            char keybuf[8];
            const char *key = OemKeyName( table[i].vk );
            if ( !key )
                key = CommandList_KeyName( table[i], keybuf );
            char mods[24];
            mods[0] = '\0';
            if ( table[i].mods & 1 ) strcat( mods, "Shift+" );
            if ( table[i].mods & 2 ) strcat( mods, "Alt+" );
            if ( table[i].mods & 4 ) strcat( mods, "Ctrl+" );
            if ( table[i].mods & 8 ) strcat( mods, "Win+" );
            _snprintf( out, outSize, "%s%s", mods, key ? key : "?" );
            out[outSize - 1] = '\0';
            return true;
        }
        return false;
    }

    void AddChip( chip_t *chips, int *n, const char *key, const char *label )
    {
        if ( *n >= KHINT_MAX_CHIPS )
            return;
        _snprintf( chips[*n].key,   sizeof( chips[*n].key ),   "%s", key );
        _snprintf( chips[*n].label, sizeof( chips[*n].label ), "%s", label );
        chips[*n].key  [sizeof( chips[*n].key   ) - 1] = '\0';
        chips[*n].label[sizeof( chips[*n].label ) - 1] = '\0';
        chips[*n].kw = 0.0f;            // filled by MeasureChips
        chips[*n].w  = 0.0f;
        ++( *n );
    }

    // Only planar placement tools expose this world-space plane readout; omit the
    // default major plane at zero so ordinary work does not grow the strip.
    void AddWorkingPlaneChip( chip_t *chips, int *n )
    {
        if ( !KiwiCon_PlanePlacement() )
            return;
        const kconPlane_t &p = KiwiCon_ActivePlane();
        int axis = -1;
        for ( int k = 0; k < 3; ++k )
            if ( fabsf( p.normal[k] ) > KCON_PLANE_PARALLEL )
                axis = k;
        if ( axis < 0 )
        {
            AddChip( chips, n, "Plane", "tilted" );
            return;
        }
        const float h = p.origin[axis];
        if ( fabsf( h ) < 0.01f )
            return;                    // suppress near-zero world-unit offsets
        char hs[24];
        KiwiUnits_Format( hs, (int)sizeof( hs ), h );
        char label[40];
        _snprintf( label, sizeof( label ), "%s %s",
                   ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ", hs );
        label[sizeof( label ) - 1] = '\0';
        AddChip( chips, n, "Plane", label );
    }

    // Patch-vertex mode has no live-command status, so lead with its multi-point
    // grammar and keep the exit gesture last.
    void AddPatchVertsChip( chip_t *chips, int *n )
    {
        if ( !KiwiPatchVerts_Active() )
            return;
        AddChip( chips, n, "Shift+LMB", "Add point" );
        AddChip( chips, n, "Ctrl+LMB",  "Remove point" );
        AddChip( chips, n, "Drag box",  "Take several" );
        AddChip( chips, n, "V / Esc",   "Leave patch vertex mode" );
    }

    // Unbound verbs fall back to the palette's live key, their actual entry path.
    void AddVerb( chip_t *chips, int *n, int commandId, const char *label )
    {
        // AddChip alone owns the capacity check.
        char key[KHINT_KEYCAP_MAX];
        if ( !LiveKey( commandId, key, sizeof( key ) ) )
        {
            char pal[KHINT_KEYCAP_MAX];
            if ( LiveKey( KIWI_CMD_PALETTE, pal, sizeof( pal ) ) )
                _snprintf( key, sizeof( key ), "(%s)", pal );
            else
                _snprintf( key, sizeof( key ), "%s", "(palette)" );
            key[sizeof( key ) - 1] = '\0';
        }
        AddChip( chips, n, key, label );
    }

    // Shared grammar precedes command-specific HudPrompts. Click tools place
    // points; paused drag tools confirm with RMB or Enter.
    int BuildCommandPrompts( chip_t *chips, KiwiEditorCommand *cmd )
    {
        int n = 0;
        if ( KiwiDrop_Active() )
        {
            AddChip( chips, &n, "Drop", "to ground" );
            AddChip( chips, &n, "G",    "Move gizmo" );
            AddChip( chips, &n, "Ctrl", "Snap" );
            AddChip( chips, &n, "Esc",  "Cancel" );
            return n;
        }
        if ( cmd && cmd->PreemptIdle() )
        {
            AddChip( chips, &n, "LMB",       "Pick" );
            AddChip( chips, &n, "Shift+LMB", "Add" );
            AddChip( chips, &n, "Ctrl+LMB",  "Remove" );
        }
        const bool clicks = ( cmd && cmd->WantsClicks() );
        AddChip( chips, &n, "RMB/Enter", "Confirm" );
        AddChip( chips, &n, "Esc",       "Cancel" );
        // The lollipop exposes one draggable ball, not a generic adjustment.
        const bool lolli = KiwiLollipop_Active();
        AddChip( chips, &n, clicks ? "LMB" : "Drag",
                            clicks ? "Place point" : ( lolli ? "Drag the ball" : "Adjust" ) );
        AddChip( chips, &n, "X/Y/Z",     "Axis" );
        // Only the MOVE command accepts a plane lock (kiwi_transform.cpp
        // HandleAxisKey's `allowPlane`; R and S pass false).
        if ( cmd && cmd->Name() && strcmp( cmd->Name(), "Move" ) == 0 )
            AddChip( chips, &n, "Shift+XYZ", "Plane" );
        AddChip( chips, &n, "Tab",       "Field" );
        AddChip( chips, &n, "0-9",       "Exact" );
        // State every SnapContext: Ctrl enables transform snapping, disables
        // construction snapping, and is irrelevant to always-snapped tools.
        if ( cmd )
        {
            switch ( cmd->SnapContext() )
            {
            case KiwiEditorCommand::KSNAPCTX_CONSTRUCT:
                AddChip( chips, &n, "Ctrl", "Free" );
                break;
            case KiwiEditorCommand::KSNAPCTX_ALWAYS:
                AddChip( chips, &n, "Snap", "On" );
                break;
            default:
                AddChip( chips, &n, "Ctrl", "Snap" );
                break;
            }
        }
        // Surface the persistent, non-default grid state during a gesture.
        if ( !KiwiGrid_SnapEnabled() )
            AddChip( chips, &n, "Grid",  "Snap OFF" );
        AddWorkingPlaneChip( chips, &n );
        // The MOVABLE PIVOT is command-local (it has no table row — kiwi_keymap.h),
        // so it is a literal, offered on exactly the two commands that implement it.
        if ( cmd && cmd->Name()
          && ( strcmp( cmd->Name(), "Move" ) == 0 || strcmp( cmd->Name(), "Rotate" ) == 0 ) )
            AddChip( chips, &n, "V", "Pivot" );

        // Append command-specific keys after the shared grammar.
        if ( cmd )
        {
            const kiwiPrompt_t *extra = 0;
            const int extraCount = cmd->HudPrompts( &extra );
            for ( int i = 0; i < extraCount && extra; ++i )
                AddChip( chips, &n, extra[i].key ? extra[i].key : "?",
                                    extra[i].label ? extra[i].label : "" );
        }
        return n;
    }

    // With nothing selected, creation verbs use the current command bindings.
    int BuildCreatePrompts( chip_t *chips )
    {
        int n = 0;
        AddPatchVertsChip( chips, &n );
        AddChip( chips, &n, "LMB",   "Pick" );
        AddChip( chips, &n, "Shift", "Add" );
        AddChip( chips, &n, "Ctrl",  "Remove" );
        AddVerb( chips, &n, KIWI_CMD_DRAW_LINE,     "Line" );
        AddVerb( chips, &n, KIWI_CMD_DRAW_RECT,     "Rect" );
        AddVerb( chips, &n, KIWI_CMD_DRAW_CIRCLE,   "Circle" );
        AddVerb( chips, &n, KIWI_CMD_PRIM_BOX,      "Box" );
        AddVerb( chips, &n, KIWI_CMD_PRIM_CYLINDER, "Cylinder" );
        AddVerb( chips, &n, KIWI_CMD_ADD_MENU,      "Add menu" );
        // With nothing selected, Focus frames the visible world.
        AddVerb( chips, &n, KIWI_CMD_FOCUS_SELECTION, "Frame all" );
        // Literal: viewport input owns Alt+MMB, so it has no command-table row.
        AddChip( chips, &n, "Alt+MMB",              "Swipe views" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE,       "Commands" );
        return n;
    }

    // Selection prompts stay kind-agnostic; kind-specific verbs use the other strip.
    int BuildSelectionPrompts( chip_t *chips, bool construction )
    {
        int n = 0;
        AddPatchVertsChip( chips, &n );
        // Literal: viewport input owns Alt+LMB; expose it only while paint is armed.
        if ( sub_401D50() )
            AddChip( chips, &n, "Alt+LMB", "Paint terrain" );
        AddChip( chips, &n, "LMB",   "Pick" );
        AddChip( chips, &n, "Shift", "Add" );
        AddChip( chips, &n, "Ctrl",  "Remove" );
        AddChip( chips, &n, "Del",   "Delete" );
        AddChip( chips, &n, "Esc",   "Deselect" );
        if ( !construction )
        {
            AddChip( chips, &n, "1-5",      "Mode" );
            AddChip( chips, &n, "Ctrl+1-4", "Convert" );
        }
        // Focus is kind-agnostic and belongs with universal selection prompts.
        AddVerb( chips, &n, KIWI_CMD_FOCUS_SELECTION, "Focus" );
        return n;
    }

    // Construction selection lives outside selection_t and needs its own verbs.
    // Delete and H are literals because the input funnel owns their unbound rows.
    int BuildConstructionVerbs( chip_t *chips )
    {
        int n = 0;
        AddVerb( chips, &n, KIWI_CMD_CONSTRUCT_JOIN,   "Join" );
        AddVerb( chips, &n, KIWI_CMD_TRIM,             "Trim" );
        AddVerb( chips, &n, KIWI_CMD_OFFSET_CURVE,     "Offset" );
        AddVerb( chips, &n, KIWI_CMD_FILLET_CURVE,     "Fillet" );
        AddVerb( chips, &n, KIWI_CMD_MOVE,             "Move" );
        AddVerb( chips, &n, KIWI_CMD_EXTRUDE_REGION,   "Extrude region" );
        AddChip( chips, &n, "Del",                     "Delete" );
        AddChip( chips, &n, "H",                       "Hide" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE,          "More" );
        return n;
    }

    // Region selection is KIWI-owned and invisible to KiwiXform_DominantKind.
    int BuildRegionPrompts( chip_t *chips )
    {
        int n = 0;
        AddChip( chips, &n, "LMB",   "Pick" );
        AddChip( chips, &n, "Shift", "Add" );
        AddChip( chips, &n, "Ctrl",  "Remove" );
        AddChip( chips, &n, "Drag", "the ball" );
        AddChip( chips, &n, "Esc",  "Deselect" );
        AddVerb( chips, &n, KIWI_CMD_FOCUS_SELECTION, "Focus" );
        return n;
    }

    // Sun selection is KIWI-owned and invisible to KiwiXform_DominantKind.
    // Pitch/yaw degrees are the helper's only live drag readout.
    int BuildSunPrompts( chip_t *chips )
    {
        int n = 0;
        float pitch = 0.0f, yaw = 0.0f;
        if ( KiwiSun_Angles( &pitch, &yaw ) )
        {
            char label[40];
            _snprintf( label, sizeof( label ), "%.1f / %.1f deg", pitch, yaw );
            label[sizeof( label ) - 1] = '\0';
            AddChip( chips, &n, "Sun", label );
        }
        // Literal: viewport input owns the drag grammar and its snap modifier.
        AddChip( chips, &n, "Shift+LMB", "Add" );
        AddChip( chips, &n, "Ctrl+LMB",  "Remove" );
        AddChip( chips, &n, "Drag",      "Orbit the sun" );
        AddChip( chips, &n, "Ctrl+drag", "Snap 5 deg" );
        AddChip( chips, &n, "Esc",  "Deselect" );
        return n;
    }

    int BuildSunVerbs( chip_t *chips )
    {
        int n = 0;
        // Sun changes do not reach baked lighting until BSP and light are rebuilt.
        AddChip( chips, &n, "!", "Needs BSP + light recompile" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE, "More" );
        return n;
    }

    int BuildRegionVerbs( chip_t *chips )
    {
        int n = 0;
        AddVerb( chips, &n, KIWI_CMD_EXTRUDE_REGION, "Extrude" );
        AddVerb( chips, &n, KIWI_CMD_PALETTE,        "More" );
        return n;
    }

    int BuildSelectionVerbs( chip_t *chips, sel_kind_t kind )
    {
        int n = 0;
        switch ( kind )
        {
        case SEL_OBJECT:
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Move" );
            AddVerb( chips, &n, KIWI_CMD_ROTATE,        "Rotate" );
            AddVerb( chips, &n, KIWI_CMD_SCALE,         "Scale" );
            // Frequent verbs lead because two-row overflow drops the tail.
            AddVerb( chips, &n, KIWI_CMD_DUPLICATE,     "Duplicate" );
            // Cut picks its line during the gesture; no preselected line is required.
            AddVerb( chips, &n, KIWI_CMD_CUT,           "Cut" );
            // Boolean stays beside Cut; both combine a solid with another operand.
            AddVerb( chips, &n, KIWI_CMD_BOOLEAN,       "Boolean" );
            AddVerb( chips, &n, KIWI_CMD_ARRAY_LINEAR,  "Array" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_FACE,  "To faces" );
            AddVerb( chips, &n, 32934,                  "Isolate" );  // classic HideUnSelected
            AddVerb( chips, &n, 33003,                  "Delete" );   // classic Delete Selection
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        case SEL_FACE:
            // Push/Pull auto-starts when a face is clicked in face mode.
            AddVerb( chips, &n, KIWI_CMD_EXTRUDE_FACE,  "Extrude" );
            AddVerb( chips, &n, KIWI_CMD_MATCH_FACE,    "Match" );
            AddVerb( chips, &n, KIWI_CMD_JOIN,          "Join" );
            // The split acts on the brush; a convex plane brush cannot keep a divided face.
            AddVerb( chips, &n, KIWI_CMD_SPLIT_FACE,    "Split brush" );
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Push/Pull (auto)" );
            AddVerb( chips, &n, KIWI_CMD_INSET_FACE,    "Inset" );
            // The Delete funnel owns this unbound row; keep Del explicit in the label.
            AddVerb( chips, &n, KIWI_CMD_REMOVE_FACE,   "Remove face (Del)" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_EDGE,  "To edges" );
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        case SEL_EDGE:
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Move edge" );
            // Bare B is bound to the curve-fillet row and contextually redirects
            // to the unbound edge tool; D toggles bevel/fillet within that tool.
            AddVerb( chips, &n, KIWI_CMD_FILLET_CURVE,  "Bevel / Fillet (D)" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_FACE,  "To faces" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_OBJECT,"To object" );
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        default:
            AddVerb( chips, &n, KIWI_CMD_MOVE,          "Move point" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_EDGE,  "To edges" );
            AddVerb( chips, &n, KIWI_CMD_SELCONV_OBJECT,"To object" );
            AddVerb( chips, &n, KIWI_CMD_PALETTE,       "More" );
            break;
        }
        return n;
    }

    // Greedy layout wraps upward and drops the tail after two rows; alignLeft
    // selects the anchored edge.
    const float KHINT_PAD_X   = 5.0f;    // inside a keycap
    const float KHINT_GAP     = 4.0f;    // keycap -> label
    const float KHINT_CHIPGAP = 10.0f;   // chip -> chip
    const float KHINT_ROWGAP  = 3.0f;
    const float KHINT_EDGE    = 10.0f;   // inset from the image edge

    // Cache measurements for both layout passes and DrawChip.
    void MeasureChips( chip_t *chips, int n )
    {
        for ( int i = 0; i < n; ++i )
        {
            chips[i].kw = ImGui::CalcTextSize( chips[i].key ).x + KHINT_PAD_X * 2.0f;
            chips[i].w  = chips[i].kw
                        + ( ( chips[i].label[0] != '\0' )
                            ? ( KHINT_GAP + ImGui::CalcTextSize( chips[i].label ).x )
                            : 0.0f );
        }
    }

    float ChipWidth( const chip_t &c ) { return c.w; }

    // Reservation and rendering must share exactly one row-height calculation.
    float StripLineH() { return ImGui::GetTextLineHeight() + 3.0f; }

    void DrawChip( ImDrawList *dl, const chip_t &c, float x, float y, float lineH )
    {
        const float kw = c.kw;          // cached by MeasureChips
        dl->AddRectFilled( ImVec2( x, y ), ImVec2( x + kw, y + lineH ),
                           IM_COL32( 44, 48, 58, 225 ), 3.0f );
        dl->AddRect      ( ImVec2( x, y ), ImVec2( x + kw, y + lineH ),
                           IM_COL32( 96, 102, 118, 170 ), 3.0f, 0, 1.0f );
        dl->AddText( ImVec2( x + KHINT_PAD_X, y ), IM_COL32( 236, 240, 248, 245 ), c.key );
        if ( c.label[0] != '\0' )
            dl->AddText( ImVec2( x + kw + KHINT_GAP, y ),
                         IM_COL32( 186, 194, 208, 230 ), c.label );
    }

    // `bottomY` is the lowest row's bottom edge. The measure pass reuses the exact
    // greedy fill so band reservation and drawing cannot disagree.
    int DrawStrip( const chip_t *chips, int n, bool alignLeft,
                   float leftX, float rightX, float bottomY, float maxW,
                   bool measure )
    {
        if ( n <= 0 )
            return 0;

        ImDrawList *dl    = ImGui::GetWindowDrawList();
        const float lineH = StripLineH();

        // Greedy fill; rows are later drawn top-to-bottom, anchored at bottomY.
        int   rowStart[KHINT_MAX_ROWS + 1];
        float rowW     [KHINT_MAX_ROWS];
        int   rows = 0;
        int   i    = 0;
        rowStart[0] = 0;
        while ( i < n && rows < KHINT_MAX_ROWS )
        {
            float w = 0.0f;
            const int start = i;
            while ( i < n )
            {
                const float cw = ChipWidth( chips[i] ) + ( i > start ? KHINT_CHIPGAP : 0.0f );
                if ( w + cw > maxW && i > start )
                    break;
                w += cw;
                ++i;
            }
            if ( i == start )                 // defensive no-progress guard
                break;
            rowW[rows]        = w;
            rowStart[rows + 1] = i;
            ++rows;
        }
        if ( rows <= 0 || measure )
            return rows;

        // Row `r` (0 = the first-filled row) sits ABOVE the later ones, so the
        // reading order is top-to-bottom and the last row hugs the bottom edge.
        for ( int r = 0; r < rows; ++r )
        {
            const float y = bottomY - (float)( rows - r ) * ( lineH + KHINT_ROWGAP ) + KHINT_ROWGAP;
            float x = alignLeft ? leftX : ( rightX - rowW[r] );
            for ( int k = rowStart[r]; k < rowStart[r + 1]; ++k )
            {
                DrawChip( dl, chips[k], x, y, lineH );
                x += ChipWidth( chips[k] ) + KHINT_CHIPGAP;
            }
        }
        return rows;
    }

    // A frame stamp self-clears the bottom band without an end-of-frame hook;
    // -1 means it has never been opened.
    int   s_bandFrame  = -1;
    float s_bandBottom = 0.0f;    // the bottom edge the NEXT box gets
    float s_bandFloor  = 0.0f;    // never climb above the image
    const float KHUD_BAND_EDGE = 12.0f;   // the inset kiwi_numeric/kiwi_uv both used
    const float KHUD_BAND_GAP  = 6.0f;    // between stacked rows
}

void KiwiHud_BandBegin( float imgMinY, float imgH )
{
    s_bandFrame  = ImGui::GetFrameCount();
    s_bandBottom = imgMinY + imgH - KHUD_BAND_EDGE;
    s_bandFloor  = imgMinY;
}

float KiwiHud_BandTake( float boxH, float fallbackTop )
{
    if ( s_bandFrame != ImGui::GetFrameCount() )
        return fallbackTop;
    float top = s_bandBottom - boxH;
    // Out of room: clamp to the image top; subsequent boxes visibly pile there.
    if ( top < s_bandFloor )
        top = s_bandFloor;
    s_bandBottom = top - KHUD_BAND_GAP;
    return top;
}

// ─── toggle ──────────────────────────────────────────────────────────────────
bool KiwiHints_Show()
{
    if ( s_show < 0 )
        s_show = Radiant_ProfileGetInt( "KiwiUX", "ShowHints", 1 ) ? 1 : 0;
    return s_show != 0;
}

void KiwiHints_SetShow( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_show == v )
        return;
    s_show = v;
    Radiant_ProfileSetInt( "KiwiUX", "ShowHints", v );
}

// ─── draw ────────────────────────────────────────────────────────────────────
void KiwiHints_Draw( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !KiwiHints_Show() )
        return;

    // Reject undersized viewports before building or measuring read-only chip state.
    // Each side gets 46%, nominally leaving a center gutter between the strips.
    const float maxW   = imgW * 0.46f;
    const float leftX  = imgMinX + KHINT_EDGE;
    const float rightX = imgMinX + imgW - KHINT_EDGE;
    if ( maxW < 80.0f || imgH < 120.0f )
        return;                                  // no room — draw nothing

    chip_t prompts[KHINT_MAX_CHIPS];
    chip_t verbs  [KHINT_MAX_CHIPS];
    int    nP = 0;
    int    nV = 0;

    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( cmd )
    {
        // A live gesture owns the keyboard, so suppress selection verbs.
        nP = BuildCommandPrompts( prompts, cmd );
    }
    else
    {
        sel_kind_t kind;
        const bool haveBrushSel = KiwiXform_DominantKind( &kind );
        const bool haveConSel   = !KiwiConSel_Empty();
        // Brush wins because classic commands act on it; region normally clears construction.
        const bool haveRegion   = KiwiRegion_HasSelection();
        // Sun wins because clicking elsewhere clears it, making it the most recent target.
        if ( KiwiSun_Selected() )
        {
            nP = BuildSunPrompts( prompts );
            nV = BuildSunVerbs  ( verbs );
        }
        else if ( haveBrushSel )
        {
            // Classic commands act on brush selection before other KIWI selections.
            nP = BuildSelectionPrompts( prompts, false );
            nV = BuildSelectionVerbs  ( verbs, kind );
        }
        else if ( haveRegion )
        {
            nP = BuildRegionPrompts( prompts );
            nV = BuildRegionVerbs  ( verbs );
        }
        else if ( haveConSel )
        {
            nP = BuildSelectionPrompts( prompts, true );
            nV = BuildConstructionVerbs( verbs );
        }
        else
        {
            nP = BuildCreatePrompts( prompts );
        }
    }

    if ( nP <= 0 && nV <= 0 )
        return;

    MeasureChips( prompts, nP );
    MeasureChips( verbs,   nV );

    // Reserve one shared band slot at the taller side's height.
    const float lineH = StripLineH();
    const int   rowsP = DrawStrip( prompts, nP,  KHINT_PROMPTS_LEFT,
                                   leftX, rightX, 0.0f, maxW, true );
    const int   rowsV = DrawStrip( verbs,   nV, !KHINT_PROMPTS_LEFT,
                                   leftX, rightX, 0.0f, maxW, true );
    const int   rows  = ( rowsP > rowsV ) ? rowsP : rowsV;
    if ( rows <= 0 )
        return;

    const float stripH  = (float)rows * ( lineH + KHINT_ROWGAP ) - KHINT_ROWGAP;
    const float bottomY = KiwiHud_BandTake( stripH, imgMinY + imgH - KHINT_EDGE )
                        + stripH;

    DrawStrip( prompts, nP,  KHINT_PROMPTS_LEFT, leftX, rightX, bottomY, maxW, false );
    DrawStrip( verbs,   nV, !KHINT_PROMPTS_LEFT, leftX, rightX, bottomY, maxW, false );
}
