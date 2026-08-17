#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_cmdoptions.cpp — RADIANT_UX_DESIGN §62.3.  See kiwi_cmdoptions.h for the
// whole design note and the Plasticity citations.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_cmdoptions.h"
#include "kiwi_command.h"
#include "kiwi_numeric.h"
#include "kiwi_units.h"

#include <imgui/imgui.h>

#include <math.h>
#include <stdio.h>

// ── shell bridge (verified against its definition) ──────────────────────────
extern bool ImGuiShell_CameraImageRect( float *x, float *y, float *w, float *h );  // imgui_shell.cpp
extern int  g_nUpdateBits;                                                         // engine_stubs.cpp:773

namespace
{
    // Geometry.  Deliberately narrow: this is a strip of controls beside a live
    // gesture, not a properties editor, and every pixel it takes is a pixel of the
    // model the user is looking at.  Plasticity's own dialog is `w-96` = 24rem =
    // 384 px (Dialog.tsx:32); 250 is the same idea at KIWI's smaller font.
    const float KOPT_WIDTH   = 250.0f;
    const float KOPT_INSET   = 12.0f;    // from the camera image's left edge
    const float KOPT_TOPFRAC = 0.16f;    // down from the image's top

    // A segmented button group.  ImGui has no such widget, so it is a row of
    // Buttons with the selected one pushed to the "active" colour — which is what
    // Plasticity's hidden-radio + styled-label idiom renders as
    // (FilletDialog.tsx:78-81).
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
            // The ID is the LABEL — every row's labels are distinct within one
            // command, and pushing the option index as an ID scope keeps two
            // options that happen to share a label ("On"/"Off") apart.
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

    // `-  N  +`.  The buttons are square-ish and the value sits between them, which
    // is the shape the directive asked for in as many words ("density stepper").
    // ── KIWI-UX (ROUND AN, ITEM 3): SLIDER, with the stepper kept at its ends ──
    // USER DIRECTIVE, verbatim: "make the density a slider so it's easier to
    // adjust.  Make the topend something like 128, but allow typing in the field
    // as well for any value."  The slider is DRAG-ONLY on purpose — this panel
    // holds no text entry (it would steal Enter/Esc/Tab from the command,
    // §62.3); typing goes through the command's own Tab numeric field, which is
    // the same state by construction (OptionChanged routes into
    // NumericFieldChanged).  The -/+ steppers stay for single-step nudges.
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

    // ── THE FLAGS, AND WHY EACH ONE IS HERE ─────────────────────────────────
    // NoNavFocus + NoFocusOnAppearing  — Plasticity's `tabIndex={-1}`
    //     (Dialog.tsx:45-47): the panel must never take the keyboard from the
    //     viewport, or Enter/Esc/Tab would stop reaching the command.
    // NoSavedSettings + NoDocking + NoCollapse + NoResize + AlwaysAutoResize —
    //     it is a transient, not a window the user owns.  The transient flag set
    //     is the one kiwi_palette.cpp:229-231 and kiwi_addmenu.cpp:241-243 use.
    // NO `p_open` — there is no ✕: the panel leaves when the gesture does.
    // NO ImGuiShell_CloseOnFocusLoss — that helper is for pop-outs the user
    //     opened; closing this on focus loss would hide a live gesture's controls
    //     the moment the user clicked in the viewport, which is every gesture.
    const ImGuiWindowFlags flags =
          ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_AlwaysAutoResize;

    // "###CmdOptions" keeps ONE window identity while the caption changes with the
    // command — the same idiom imgui_panel_patchdensity.cpp:47-48 uses.
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

        // The `enabledBy` gate — Plasticity's `class="disabled"` row
        // (FilletDialog.tsx:73).  A disabled row is DRAWN, not hidden: a control
        // that vanishes teaches nothing about why it is unavailable.
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
            // ── THE ONE PLACE THIS PANEL TOUCHES THE NUMERIC LAYER ──────────
            // Read through NumericFieldValue and write through
            // NumericFieldChanged, so the slider and the Tab-and-type path are
            // the SAME state — there is no second store.  The write is in WORLD
            // units (Units_FromDisplay), which is that handler's stated contract
            // (kiwi_command.h: "`world` is already in RAW WORLD UNITS … never
            // re-convert it"), and the read is converted back for display for the
            // same reason.
            //
            // KiwiNum_ClearField after the write is load-bearing: if the user had
            // typed into this field, the typed text would otherwise still be
            // showing in the HUD bubble and would win the next time the command
            // re-read it.  Dragging the slider is a statement that the typed entry
            // is finished with.
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

    // ── THE FOOTER, and it is Plasticity's (Dialog.tsx:44-47) ───────────────
    // Cancel then OK, both calling exactly what the keyboard calls — KiwiCmd_Cancel
    // is Esc's rung and KiwiCmd_Confirm IS Enter's (kiwi_command.cpp:1351 feeds
    // VK_RETURN), which is also what the RMB release calls.  So "still allows
    // enter/rightclick completion" is not a special case here: the button is the
    // same code path, and nothing about Enter or RMB changed.
    //
    // BOTH are latched and acted on AFTER ImGui::End(): confirming ENDS the
    // gesture, and running that inside the window's Begin/End would tear down the
    // command whose Name() this window's ID string came from, mid-frame.
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
