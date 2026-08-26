#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Floating option panel for live editor commands; see kiwi_cmdoptions.h.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_cmdoptions.h"
#include "kiwi_command.h"
#include "kiwi_numeric.h"
#include "kiwi_units.h"

#include <imgui/imgui.h>

#include <math.h>
#include <stdio.h>

extern bool ImGuiShell_CameraImageRect( float *x, float *y, float *w, float *h );  // imgui_shell.cpp
extern int  g_nUpdateBits;                                                         // engine_stubs.cpp:773

namespace
{
    // Keep the panel narrow; 250 px fits compact controls without obscuring the model.
    const float KOPT_WIDTH   = 250.0f;
    const float KOPT_INSET   = 12.0f;    // from the camera image's left edge
    const float KOPT_TOPFRAC = 0.16f;    // down from the image's top

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

    // Drag-only slider preserves command keys; typed values use the shared Tab field,
    // while the steppers provide single-step nudges.
    bool IntRow( int cur, int lo, int hi, int *outValue, bool enabled )
    {
        bool changed = false;
        const float bw = ImGui::GetFrameHeight();
        if ( ImGui::Button( "-", ImVec2( bw, 0.0f ) ) && enabled && cur > lo )
        {
            *outValue = cur - 1;
            changed   = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -( bw + ImGui::GetStyle().ItemSpacing.x ) );
        int v = cur;
        if ( ImGui::SliderInt( "##i", &v, lo, hi, "%d",
                               ImGuiSliderFlags_NoInput ) && enabled && v != cur )
        {
            if ( v < lo ) v = lo;
            if ( v > hi ) v = hi;
            *outValue = v;
            changed   = true;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "+", ImVec2( bw, 0.0f ) ) && enabled && cur < hi )
        {
            *outValue = cur + 1;
            changed   = true;
        }
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
            changed = EnumRow( o, cmd->OptionValue( i ), &value, enabled );
            break;

        case KOPT_TOGGLE:
        {
            bool on = ( cmd->OptionValue( i ) != 0 );
            if ( ImGui::Checkbox( "##t", &on ) && enabled )
            {
                value   = on ? 1 : 0;
                changed = true;
            }
            break;
        }

        case KOPT_INT:
            changed = IntRow( cmd->OptionValue( i ), (int)o.lo, (int)o.hi, &value, enabled );
            break;

        case KOPT_NUMFIELD:
        {
            // Slider and Tab entry share the command's numeric-field state. Writes follow
            // NumericFieldChanged's Units_FromDisplay contract; non-length handlers compensate.
            // Clear typed text first so it cannot override the slider value on the next read.
            float world = 0.0f;
            if ( !cmd->NumericFieldValue( o.field, &world ) )
                break;
            float shown = Units_ToDisplay( world );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::SliderFloat( "##n", &shown, o.lo, o.hi, "%.2f" ) && enabled )
            {
                if ( shown < o.lo ) shown = o.lo;
                if ( shown > o.hi ) shown = o.hi;
                KiwiNum_ClearField( o.field );
                cmd->NumericFieldChanged( o.field, true, Units_FromDisplay( shown ) );
                g_nUpdateBits |= 1;
            }
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
