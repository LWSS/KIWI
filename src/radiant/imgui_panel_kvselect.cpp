// imgui_panel_kvselect.cpp — UI-rework Phase 3: ImGui "Select by key/value" panel over
// the select.cpp core action. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CKeyValueSelectDlg (select.cpp:2716): the SAME core call the MFC OK
// handler makes, with the same widget reads —
//   * Key / Value edits (IDC_KVS_KEY 5402 / IDC_KVS_VALUE 5403), 0x1000-byte buffers like
//     OnOk2's stack buffers (select.cpp:2803).
//   * the two BS_AUTOCHECKBOX flags, verbatim OnCreate labels (select.cpp:2779/2784),
//     feeding KeyValueSelect_Apply's keySubstr / valueSubstr — the two bits that pick one
//     of the eight matcher dispatches inside Select_ByKeyValue_Core.
//   * "Select" = OnOk2 — KeyValueSelect_Apply WITHOUT the trailing ShowWindow( SW_HIDE ).
//   * first open = OnCreate's tail (select.cpp:2791-2797): pre-fill both edits from the
//     entity window's key/value fields via Win_GetEntityKeyValueFields, focus Key.
// The panel-stays-open flow is the sanctioned Phase-3 divergence noted in
// RADIANT_UI_REWORK_PLAN.md: OK (apply + hide) and Cancel/Close (hide) collapse into
// "Select" plus the window's own close box, and Select_ByKeyValue's parent-relative
// GetWindowRect placement (select.cpp:2834-2840) is dropped — HWND-lifetime plumbing with
// no core-action content (ImGui persists its own window rect).
//
// NO HWND/MFC anywhere in this file: the only binds are KeyValueSelect_Apply (declared in
// radiant_ui_actions.h) and Win_GetEntityKeyValueFields, both UI-independent and both
// non-static — the latter extern'd exactly as select.cpp:2636 does, since win_ent.cpp's
// entwnd_* handles are TU-static and reachable only through that accessor.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>

// win_ent.cpp — the entity-window key/value edit text.  Buffers must hold at least
// 0x1000 bytes each (it reads up to 0xFFF chars per field).
extern void Win_GetEntityKeyValueFields( char *keyOut, char *valueOut );

static bool s_showKVSelect = false;

// The dialog's two edit controls.  0x1000 bytes each, matching OnOk2's stack buffers
// (select.cpp:2803) — ImGui reserves the NUL, so the same 0xFFF-char capacity as the MFC
// ::GetWindowTextA( ..., sizeof( key ) - 1 ) reads, and the size Win_GetEntityKeyValueFields
// requires of its outputs.
static char s_key[0x1000]   = { 0 };
static char s_value[0x1000] = { 0 };

// The two BS_AUTOCHECKBOX states (select.cpp:2779/2784).  OnCreate creates both clear.
static bool s_bKeySubstr   = false;   // IDC_KVS_KEYSUB 5404 → keySubstr
static bool s_bValueSubstr = false;   // IDC_KVS_VALSUB 5405 → valueSubstr

static bool s_opened = false;         // first-open latch (OnCreate equivalent)

// Panel toggle, drawn inside the shell window (imgui_shell.cpp).
void ImGuiPanel_KVSelect_MenuItem()
{
    ImGui::Checkbox( "Select by key/value", &s_showKVSelect );
}

void ImGuiPanel_KVSelect_Draw()
{
    if ( !s_showKVSelect )
    {
        // Hiding the panel drops the latch so a re-open re-seeds the fields.  Both MFC
        // paths do this: OnCreate seeds a fresh dialog, and Select_ByKeyValue's
        // still-alive branch (select.cpp:2819-2823) re-seeds before SW_SHOW — so seeding
        // on every open is the behaviour of both, not a divergence.
        s_opened = false;
        return;
    }

    if ( !s_opened )
    {
        // OnCreate tail (select.cpp:2791-2795).  Unconditional overwrite, like the MFC
        // ::SetWindowTextA pair: the accessor clears each buffer first, so an entity
        // window with no key/value text blanks the fields rather than leaving stale text.
        s_opened = true;
        Win_GetEntityKeyValueFields( s_key, s_value );
    }

    if ( ImGui::Begin( "Select by Key/Value", &s_showKVSelect, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // Enter in either edit fires the select — the IDC_KVS_OK BS_DEFPUSHBUTTON
        // equivalent, minus OK's ShowWindow( SW_HIDE ).
        if ( ImGui::IsWindowAppearing() )
            ImGui::SetKeyboardFocusHere();      // OnCreate's ::SetFocus( s_kvsKey )

        ImGui::SetNextItemWidth( 260.0f );
        bool go = ImGui::InputText( "Key", s_key, sizeof( s_key ),
                                    ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::Checkbox( "Key is a substring", &s_bKeySubstr );

        ImGui::SetNextItemWidth( 260.0f );
        go |= ImGui::InputText( "Value", s_value, sizeof( s_value ),
                                ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::Checkbox( "Value is a substring", &s_bValueSubstr );

        go |= ImGui::Button( "Select" );
        if ( go )
        {
            // OnOk2's tail call.  Two empty fields are a no-op INSIDE the action
            // (Select_ByKeyValue_Core's LABEL_2 early-out, select.cpp:2646), so the button
            // needs no guard of its own; the Select_Deselect( 1 ) and the g_nUpdateBits
            // stamp are in there too.
            KeyValueSelect_Apply( s_key, s_value, s_bKeySubstr, s_bValueSubstr );
        }
    }
    ImGuiShell_CloseOnFocusLoss( &s_showKVSelect );
    ImGui::End();
}
