#pragma once
// Direct3D 9 view of kiwi_gpu.h's opaque buffers, for the D3D9 editor backend
// (r_ed_scene.cpp) only.  Nothing API-neutral includes this.

#include "kiwi_gpu.h"

struct IDirect3DIndexBuffer9;
struct IDirect3DVertexBuffer9;

IDirect3DIndexBuffer9 *KiwiGpu_D3D9IndexBuffer( const KiwiGpuIndexBuffer *ib );
IDirect3DVertexBuffer9 *KiwiGpu_D3D9VertexBuffer( const KiwiGpuVertexBuffer *vb );
