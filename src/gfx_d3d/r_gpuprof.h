#pragma once
// KIWI: GPU timing for Tracy (Radiant).  The front end brackets a pass with R_GpuZoneScope;
// the backend turns each bracket into two D3D9 timestamp queries, and the results land on
// Tracy's GPU timeline once the GPU has finished that frame.  Everything is a no-op unless a
// profiler is connected, so an unprofiled frame renders exactly as before.

struct GfxRenderCommandExecState;

// The queries live and die with the device (R_CreateForInitOrReset /
// R_ReleaseForShutdownOrReset).
void R_GpuProf_CreateQueries();
void R_GpuProf_ReleaseQueries();

// One GPU frame per editor frame: Begin before its first command list, End after its last
// present.  End hands Tracy every earlier frame the GPU has finished.
void R_GpuProf_FrameBegin();
void R_GpuProf_FrameEnd();

// Front end: an RC_GPU_ZONE marker.  `name` must be a string literal - its address names the
// zone, and the backend reads it after the call returns.
void R_AddCmdGpuZone( const char *name, bool begin );

struct R_GpuZoneScope
{
    explicit R_GpuZoneScope( const char *name ) : m_name( name ) { R_AddCmdGpuZone( name, true ); }
    ~R_GpuZoneScope() { R_AddCmdGpuZone( m_name, false ); }
    R_GpuZoneScope( const R_GpuZoneScope & ) = delete;
    R_GpuZoneScope &operator=( const R_GpuZoneScope & ) = delete;

    const char *m_name;
};

// Backend: the RC_GPU_ZONE handler, and the same bracket for work the backend runs itself
// (the ImGui overlay).  ListEnd closes what a refused end marker left open.
void RB_GpuZoneCmd( GfxRenderCommandExecState *execState );
void RB_GpuProf_ZoneBegin( const char *name );
void RB_GpuProf_ZoneEnd( const char *name );
void RB_GpuProf_ListEnd();
