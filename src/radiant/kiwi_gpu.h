#pragma once
// Static GPU storage for the retained scene (kiwi_scene.h) and the light preview's retained
// shadow volumes (camwnd.cpp).
//
// The callers build index and vertex lists on the CPU and never name a graphics API; they
// hand them to this interface and keep the returned opaque handles.  kiwi_gpu_d3d9.cpp
// implements it on Direct3D 9 (MANAGED pool, so a device Reset keeps the contents).  A Vulkan
// port implements these functions plus its draw, and nothing in the callers changes.

#include <stdint.h>

struct KiwiGpuIndexBuffer;
struct KiwiGpuVertexBuffer;

// Upload `count` 16-bit indices into a new static buffer.  nullptr = no device or the
// allocation failed; the caller draws nothing for that buffer rather than failing.
KiwiGpuIndexBuffer *KiwiGpu_CreateIndexBuffer( const uint16_t *indices, int count );

// Replace `ib`'s contents.  The caller keeps only the returned handle (nullptr on failure)
// and never touches `ib` again: the backend retires it once the GPU no longer reads it, so a
// backend whose frames are in flight (Vulkan) can hand back a fresh buffer, and one that
// draws synchronously (D3D9 here) may rewrite in place.
KiwiGpuIndexBuffer *KiwiGpu_ReplaceIndexBuffer( KiwiGpuIndexBuffer *ib, const uint16_t *indices,
                                                int count );

// Retire a buffer; the same "once the GPU is done" rule.
void KiwiGpu_ReleaseIndexBuffer( KiwiGpuIndexBuffer *ib );

// Upload `count` vertices of `stride` bytes into a new static buffer (`verts` nullptr = room
// only, filled by KiwiGpu_WriteVertexBuffer); nullptr as above.
KiwiGpuVertexBuffer *KiwiGpu_CreateVertexBuffer( const void *verts, int stride, int count );

// Write `bytes` at `byteOffset` into `vb`.  On success the caller keeps only the returned
// handle, as with KiwiGpu_ReplaceIndexBuffer; nullptr = nothing written and `vb` unchanged.
KiwiGpuVertexBuffer *KiwiGpu_WriteVertexBuffer( KiwiGpuVertexBuffer *vb, int byteOffset,
                                                const void *data, int bytes );

// Retire a vertex buffer; the same rule as KiwiGpu_ReleaseIndexBuffer.
void KiwiGpu_ReleaseVertexBuffer( KiwiGpuVertexBuffer *vb );
