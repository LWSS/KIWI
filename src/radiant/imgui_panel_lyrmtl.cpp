// imgui_panel_lyrmtl.cpp — the Layered Materials tool, over layeredmaterialwnd.cpp.
// Replaces the raw-Win32 WS_EX_PALETTEWINDOW palette (COMCTL32 toolbar + custom-painted
// "LayeredMaterialList" child + the MFC CNameDlg name prompt), cmd 35008 / F4.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

// Include order mirrors kiwi_uveditor.cpp:59-62 — the other TU that walks a Material's
// textureTable down to its GfxImage and hands the result to ImGui.
#include <gfx_d3d/r_material.h>      // Material / MaterialTextureDef / textureTable
#include <gfx_d3d/r_gfx.h>           // GfxImage / GfxTexture / MAPTYPE_2D
#include <d3d9.h>                    // IDirect3DTexture9 (the ImTextureID we hand over)

#include <string.h>

// ── layeredmaterialwnd.cpp bindings (the DlgProc/toolbar drove these same six) ──
extern LRESULT     LayeredMaterialWnd_Commands( int cmd );   // layeredmaterialwnd.cpp:161  LRESULT LayeredMaterialWnd_Commands(int)
extern void        LyrMtlNewMaterial_Apply( const char *name ); // layeredmaterialwnd.cpp:102  void LyrMtlNewMaterial_Apply(const char*)
extern int         LyrMtlSelectLayer_Apply( int layerIndex );   // layeredmaterialwnd.cpp:189  int LyrMtlSelectLayer_Apply(int)
extern const char *LyrMtlWnd_ActiveName();                      // layeredmaterialwnd.cpp:199  const char *LyrMtlWnd_ActiveName()
extern int         LyrMtlWnd_LayerCount();                      // layeredmaterialwnd.cpp:206  int LyrMtlWnd_LayerCount()
extern qtexture_s *LyrMtlWnd_LayerHandle( int i );              // layeredmaterialwnd.cpp:213  qtexture_s *LyrMtlWnd_LayerHandle(int)
extern qtexture_s *Texture_GetHandle( const char *name );       // texwnd.cpp 0x45a8e0

static bool s_show      = false;
static bool s_openName  = false;      // arm the name modal on the next draw
static char s_nameBuf[64] = { 0 };     // the CNameDlg edit; entry->name is char[64]

// The active material's colormap as an ImGui texture, or null.  Same three guards and
// the same MAPTYPE_2D arm as kiwi_uveditor.cpp:1553-1580 / kiwi_skybox.cpp:451-476:
// an unloaded material has no textureTable, the table is hash-sorted so [0] is a coin
// flip (TS_COLOR_MAP is semantic 2), and GfxTexture is a union whose live arm before
// upload is loadDef — handing THAT to ImGui is a crash.
static IDirect3DTexture9 *LayerColorMap( qtexture_s *q )
{
    if ( !q )
        return nullptr;
    if ( !q->next && q->name )
        Texture_GetHandle( q->name );        // lazy registration (texwnd.cpp:320)
    Material *mtl = q->next;
    if ( !mtl || !mtl->textureTable )
        return nullptr;
    for ( int i = 0; i < (int)mtl->textureCount; ++i )
    {
        if ( mtl->textureTable[i].semantic != 2 )        // TS_COLOR_MAP
            continue;
        GfxImage *img = mtl->textureTable[i].u.image;
        if ( !img || img->delayLoadPixels )
            continue;
        if ( img->mapType == MAPTYPE_2D && img->texture.map )
            return img->texture.map;
    }
    return nullptr;
}

// Cmd_OnToggleLayeredMaterials (mainfrm.cpp) and the F4 accelerator.
void ImGuiPanel_LyrMtl_Toggle()
{
    s_show = !s_show;
}

void ImGuiPanel_LyrMtl_MenuItem()
{
    ImGui::Checkbox( "Layered materials", &s_show );
}

void ImGuiPanel_LyrMtl_Draw()
{
    if ( !s_show )
    {
        // The binary's hide path un-toggled Live add first (LayeredMaterialWnd_OnClose /
        // ToggleVisibility 0x4176B0); LayeredMaterialWnd_Commands( 2 ) is that same flip.
        // Done here so every close route — the menu toggle, the checkbox, the title-bar X —
        // carries it.
        if ( lyrMtlWndGlob.liveAddActive )
            LayeredMaterialWnd_Commands( 2 );
        return;
    }

    const char *name  = LyrMtlWnd_ActiveName();
    const int   count = LyrMtlWnd_LayerCount();
    const int   sel   = lyrMtlWndGlob.selectedLayerIndex;

    ImGui::SetNextWindowSize( ImVec2( 300.0f, 420.0f ), ImGuiCond_FirstUseEver );
    if ( ImGui::Begin( "Layered Materials", &s_show ) )
    {
        // The frame caption the binary kept: "(no layered material)" / Editing "<name>".
        if ( name )
            ImGui::Text( "Editing \"%s\"", name );
        else
            ImGui::TextUnformatted( "(no layered material)" );
        ImGui::Separator();

        // The five toolbar buttons, with the binary's TTN_NEEDTEXT strings (sub_417AC0)
        // as tooltips and the enable rules sub_4174E0 applied to each TBSTATE_ENABLED.
        if ( ImGui::Button( "New" ) )
        {
            s_nameBuf[0] = '\0';
            s_openName   = true;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Create a new layered material" );

        ImGui::SameLine();
        ImGui::BeginDisabled( name == nullptr );
        if ( ImGui::Button( "Delete" ) )
            LayeredMaterialWnd_Commands( 1 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Delete the selected layered material" );

        ImGui::SameLine();
        bool live = ( lyrMtlWndGlob.liveAddActive & 1 ) != 0;
        if ( ImGui::Checkbox( "Live add", &live ) )
            LayeredMaterialWnd_Commands( 2 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "When checked, clicking in the texture window adds a layer" );
        ImGui::EndDisabled();

        ImGui::BeginDisabled( name == nullptr || sel < 0 || sel >= count );
        if ( ImGui::Button( "Remove layer" ) )
            LayeredMaterialWnd_Commands( 3 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Remove selected layer from the layered material" );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( name == nullptr || sel + 1 >= count );
        if ( ImGui::Button( "Up" ) )
            LayeredMaterialWnd_Commands( 4 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Move selected layer up" );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( name == nullptr || sel - 1 < 0 );
        if ( ImGui::Button( "Down" ) )
            LayeredMaterialWnd_Commands( 5 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Move selected layer down" );
        ImGui::EndDisabled();

        ImGui::Separator();

        // The layer list.  The native painter drew layer 0 at the BOTTOM (rows top-down
        // over a reversed walk, sub_417D60), so the rows go highest index first.
        if ( ImGui::BeginChild( "##lyrmtl_layers", ImVec2( 0.0f, 0.0f ), ImGuiChildFlags_Borders ) )
        {
            const float rowH = 64.0f;
            for ( int i = count - 1; i >= 0; --i )
            {
                qtexture_s *q = LyrMtlWnd_LayerHandle( i );
                ImGui::PushID( i );
                if ( ImGui::Selectable( "##row", i == sel, 0, ImVec2( 0.0f, rowH ) ) )
                    LyrMtlSelectLayer_Apply( i );

                // Thumbnail then name, aspect-fit into the 64x64 cell the native row used.
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                ImDrawList  *dl     = ImGui::GetWindowDrawList();
                IDirect3DTexture9 *tex = LayerColorMap( q );
                if ( tex && q->width > 0 && q->height > 0 )
                {
                    float w = 64.0f, h = 64.0f;
                    if ( q->width >= q->height )
                        h = 64.0f * (float)q->height / (float)q->width;
                    else
                        w = 64.0f * (float)q->width / (float)q->height;
                    const ImVec2 p0( rowMin.x + 2.0f + ( 64.0f - w ) * 0.5f,
                                     rowMin.y + ( 64.0f - h ) * 0.5f );
                    dl->AddImage( (ImTextureID)(intptr_t)tex, p0,
                                  ImVec2( p0.x + w, p0.y + h ) );
                }
                dl->AddText( ImVec2( rowMin.x + 68.0f, rowMin.y + 24.0f ),
                             ImGui::GetColorU32( ImGuiCol_Text ),
                             ( q && q->name ) ? q->name : "(unnamed)" );
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // CNameDlg, as a modal.  Submitted at panel scope so it centres over the panel.
        if ( s_openName )
        {
            ImGui::OpenPopup( "New Layered Material" );
            s_openName = false;
        }
        if ( ImGui::BeginPopupModal( "New Layered Material", nullptr,
                                     ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            if ( ImGui::IsWindowAppearing() )
                ImGui::SetKeyboardFocusHere();
            bool ok = ImGui::InputText( "Name", s_nameBuf, sizeof( s_nameBuf ),
                                        ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::Separator();
            ok |= ImGui::Button( "OK" );
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel" ) )
                ImGui::CloseCurrentPopup();
            if ( ok )
            {
                // LayeredMaterials_AddEntries validates the name and reports invalid /
                // duplicate through its own MessageBox, so no guard here.
                LyrMtlNewMaterial_Apply( s_nameBuf );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    // NOT run through ImGuiShell_CloseOnFocusLoss: "Live add" makes this a modeless
    // tool palette that must stay open while the operator clicks the texture browser.
    ImGui::End();
}
