#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Floating option panel for live editor commands; see kiwi_cmdoptions.h.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_cmdoptions.h"
#include "kiwi_command.h"
#include "kiwi_numeric.h"
#include "kiwi_ported.h"
#include "kiwi_units.h"

#include <imgui/imgui.h>

#include <math.h>
#include <stdio.h>

extern bool ImGuiShell_CameraImageRect( float *x, float *y, float *w, float *h );  // imgui_shell.cpp

namespace
{
    // Keep the panel narrow; 250 px fits compact controls without obscuring the model.
    const float KOPT_WIDTH   = 250.0f;
    const float KOPT_INSET   = 12.0f;    // from the camera image's left edge
    const float KOPT_TOPFRAC = 0.16f;    // down from the image's top

    // Keyboard entry into the panel (KIWI 2026-09-28): the row Tab last opened, and the row
    // to open on the next draw (Tab from the key funnel, or a double-click on a slider).
    // Opening = SetKeyboardFocusHere before the slider, which activates its text input.
    const void *s_forCmd     = nullptr;  // the command these indices belong to
    int         s_tabRow     = -1;
    int         s_pendingRow = -1;

    bool NumericRow( const kiwiOption_t &o ) { return o.kind == KOPT_INT || o.kind == KOPT_NUMFIELD; }

    bool RowEnabled( KiwiEditorCommand *cmd, const kiwiOption_t *opts, int count, int i )
    {
        const kiwiOption_t &o = opts[i];
        return !( o.enabledBy >= 0 && o.enabledBy < count ) || cmd->OptionValue( o.enabledBy ) != 0;
    }

    // Open this row for typing on the draw about to submit it; a double-click asks again.
    void BeforeNumeric( int row )
    {
        if ( s_pendingRow != row )
            return;
        ImGui::SetKeyboardFocusHere();
        s_tabRow     = row;
        s_pendingRow = -1;
    }
    // The first click of the pair already moved the slider; the typed value replaces it.
    void AfterNumeric( int row )
    {
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem )
          && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
            s_pendingRow = row;
    }

    // ImGui has no segmented widget; active-styled buttons emulate one.
    bool EnumRow( const kiwiOption_t &o, int cur, int *outValue, bool enabled )
    {
        bool changed = false;
        const ImGuiStyle &st = ImGui::GetStyle();
        const float avail = ImGui::GetContentRegionAvail().x;
        const int   n     = ( o.choiceCount > 0 ) ? o.choiceCount : 1;
        const float bw    = ( avail - st.ItemSpacing.x * (float)( n - 1 ) ) / (float)n;

        for ( int i = 0; i < n; ++i )
        {
            if ( i )
                ImGui::SameLine();
            const bool on = ( i == cur );
            if ( on )
            {
                ImGui::PushStyleColor( ImGuiCol_Button,        st.Colors[ImGuiCol_ButtonActive] );
                ImGui::PushStyleColor( ImGuiCol_ButtonHovered, st.Colors[ImGuiCol_ButtonActive] );
            }
            const char *label = ( o.choices && o.choices[i] ) ? o.choices[i] : "?";
            // Labels supply button IDs; PushID(option index) isolates identical labels across rows.
            if ( ImGui::Button( label, ImVec2( bw, 0.0f ) ) && enabled && !on )
            {
                *outValue = i;
                changed   = true;
            }
            if ( on )
                ImGui::PopStyleColor( 2 );
        }
        return changed;
    }

    // Slider with steppers.  KIWI (2026-09-28): typeable - double-click, Ctrl+click or Tab
    // opens it as a text field (it was drag-only); the steppers are not Tab stops, so Tab
    // inside a typed field goes straight to the next numeric row.
    bool IntRow( int row, int cur, int lo, int hi, int *outValue, bool enabled )
    {
        bool changed = false;
        const float bw = ImGui::GetFrameHeight();
        ImGui::PushItemFlag( ImGuiItemFlags_NoTabStop, true );
        if ( ImGui::Button( "-", ImVec2( bw, 0.0f ) ) && enabled && cur > lo )
        {
            *outValue = cur - 1;
            changed   = true;
        }
        ImGui::PopItemFlag();
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -( bw + ImGui::GetStyle().ItemSpacing.x ) );
        int v = cur;
        BeforeNumeric( row );
        if ( ImGui::SliderInt( "##i", &v, lo, hi, "%d",
                               ImGuiSliderFlags_AlwaysClamp ) && enabled && v != cur )
        {
            if ( v < lo ) v = lo;
            if ( v > hi ) v = hi;
            *outValue = v;
            changed   = true;
        }
        AfterNumeric( row );
        ImGui::SameLine();
        ImGui::PushItemFlag( ImGuiItemFlags_NoTabStop, true );
        if ( ImGui::Button( "+", ImVec2( bw, 0.0f ) ) && enabled && cur < hi )
        {
            *outValue = cur + 1;
            changed   = true;
        }
        ImGui::PopItemFlag();
        return changed;
    }
}

void KiwiCmdOpts_Draw()
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return;
    const kiwiOption_t *opts = 0;
    const int count = cmd->CommandOptions( &opts );
    if ( count <= 0 || !opts )
        return;
    if ( s_forCmd != (const void *)cmd )
    {
        s_forCmd     = cmd;              // a new command: its rows are not the old indices
        s_tabRow     = -1;
        s_pendingRow = -1;
    }

    float ix, iy, iw, ih;
    if ( !ImGuiShell_CameraImageRect( &ix, &iy, &iw, &ih ) )
        return;                       // no camera image yet — nothing to anchor to

    ImGui::SetNextWindowPos( ImVec2( ix + KOPT_INSET, iy + ih * KOPT_TOPFRAC ),
                             ImGuiCond_Always );
    ImGui::SetNextWindowSize( ImVec2( KOPT_WIDTH, 0.0f ), ImGuiCond_Always );

    // Keep this transient panel out of keyboard navigation so command keys reach the
    // gesture. The gesture owns its state and lifetime, including focus changes.
    const ImGuiWindowFlags flags =
          ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_AlwaysAutoResize;

    // A stable ### suffix preserves window identity while the command caption changes.
    char title[96];
    _snprintf( title, sizeof( title ), "%s###CmdOptions", cmd->Name() );
    title[sizeof( title ) - 1] = '\0';

    if ( !ImGui::Begin( title, 0, flags ) )
    {
        ImGui::End();
        return;
    }

    for ( int i = 0; i < count; ++i )
    {
        const kiwiOption_t &o = opts[i];

        // Draw gated options disabled instead of hiding why they are unavailable.
        bool enabled = true;
        if ( o.enabledBy >= 0 && o.enabledBy < count )
            enabled = ( cmd->OptionValue( o.enabledBy ) != 0 );

        ImGui::PushID( i );
        if ( !enabled )
            ImGui::BeginDisabled();

        if ( o.label && o.label[0] )
            ImGui::TextUnformatted( o.label );

        int   value   = 0;
        bool  changed = false;

        switch ( o.kind )
        {
        case KOPT_ENUM:
            ImGui::PushItemFlag( ImGuiItemFlags_NoTabStop, true );   // Tab walks numbers only
            changed = EnumRow( o, cmd->OptionValue( i ), &value, enabled );
            ImGui::PopItemFlag();
            break;

        case KOPT_TOGGLE:
        {
            bool on = ( cmd->OptionValue( i ) != 0 );
            ImGui::PushItemFlag( ImGuiItemFlags_NoTabStop, true );
            if ( ImGui::Checkbox( "##t", &on ) && enabled )
            {
                value   = on ? 1 : 0;
                changed = true;
            }
            ImGui::PopItemFlag();
            break;
        }

        case KOPT_INT:
            changed = IntRow( i, cmd->OptionValue( i ), (int)o.lo, (int)o.hi, &value, enabled );
            break;

        case KOPT_NUMFIELD:
        {
            // Slider and Tab entry share the command's numeric-field state. Writes follow
            // NumericFieldChanged's Units_FromDisplay contract; non-length handlers compensate.
            // Clear typed text first so it cannot override the slider value on the next read.
            float world = 0.0f;
            if ( !cmd->NumericFieldValue( o.field, &world ) )
                break;
            // KIWI: only a length shows in display units; a factor / count / angle is its own value.
            const kiwiNumField_t *defs = 0;
            const int nDefs = cmd->NumericFields( &defs );
            const bool length = ( o.field >= nDefs || !defs ) || defs[o.field].kind == KNUM_LENGTH;
            float shown = length ? Units_ToDisplay( world ) : world;
            ImGui::SetNextItemWidth( -1.0f );
            BeforeNumeric( i );
            if ( ImGui::SliderFloat( "##n", &shown, o.lo, o.hi, "%.2f",
                                     ImGuiSliderFlags_AlwaysClamp ) && enabled )
            {
                if ( shown < o.lo ) shown = o.lo;
                if ( shown > o.hi ) shown = o.hi;
                KiwiNum_ClearField( o.field );
                cmd->NumericFieldChanged( o.field, true, Units_FromDisplay( shown ) );
                g_nUpdateBits |= 1;
            }
            AfterNumeric( i );
            break;
        }
        }

        if ( changed )
        {
            cmd->OptionChanged( i, value );
            g_nUpdateBits |= 1;
        }

        if ( !enabled )
            ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::Spacing();
    }

    // Buttons use the shared cancel/confirm ladders. Latch clicks until after End():
    // either action can destroy the active command backing this window.
    ImGui::Separator();
    bool doCancel = false, doConfirm = false;
    {
        const float half = ( ImGui::GetContentRegionAvail().x
                           - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        doCancel = ImGui::Button( "Cancel", ImVec2( half, 0.0f ) );
        ImGui::SameLine();
        doConfirm = ImGui::Button( "Confirm", ImVec2( half, 0.0f ) );
    }

    ImGui::End();

    if ( doCancel )
        KiwiCmd_Cancel();
    else if ( doConfirm )
        KiwiCmd_Confirm();
}

bool KiwiCmdOpts_TabFocus( bool backwards )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return false;
    const kiwiOption_t *opts = 0;
    const int count = cmd->CommandOptions( &opts );
    if ( count <= 0 || !opts )
        return false;
    if ( s_forCmd != (const void *)cmd )
    {
        s_forCmd = cmd;
        s_tabRow = s_pendingRow = -1;
    }
    const int from = ( s_pendingRow >= 0 ) ? s_pendingRow : s_tabRow;
    for ( int step = 1; step <= count; ++step )
    {
        int i = backwards ? from - step : from + step;
        i = ( ( i % count ) + count ) % count;
        if ( from < 0 && !backwards )
            i = step - 1;                 // first Tab: the first numeric row
        if ( NumericRow( opts[i] ) && RowEnabled( cmd, opts, count, i ) )
        {
            s_pendingRow = i;
            g_nUpdateBits |= 1;
            return true;
        }
    }
    return false;
}
