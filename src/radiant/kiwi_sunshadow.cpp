#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "kiwi_sunshadow.h"
#include "kiwi_sun.h"
#include "prefs.h"
#include "radiant_frame.h"

extern int g_nUpdateBits;

bool KiwiSunPreview_Enabled()
{
    return g_PrefsDlg->preview_sun_aswell != 0;
}

void KiwiSunPreview_SetEnabled( bool enabled )
{
    const int value = enabled ? 1 : 0;
    if ( g_PrefsDlg->preview_sun_aswell != value )
    {
        g_PrefsDlg->preview_sun_aswell = value;
        Prefs_SavePrefs( g_PrefsDlg );
        g_nUpdateBits |= W_CAMERA;
    }

    // The menu and ImGui surfaces are views of this same persisted field.
    Radiant_CheckMenu( 36108, enabled );
}

const char *KiwiSunPreview_Status()
{
    if ( !g_PrefsDlg->enable_light_preview )
        return "light preview disabled (View -> Enable light preview)";
    if ( !KiwiSunPreview_Enabled() )
        return "sun preview disabled (View -> Preview sun as well)";
    if ( !KiwiSun_Exists() )
        return "no sun placed (Sun tab -> Place Sun)";
    return "Sun shadows: stencil (CSM-equivalent hard shadows; runtime CSM needs the game shaders)";
}
