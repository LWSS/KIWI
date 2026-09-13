#pragma once
#include <stddef.h>
#include <stdint.h>
struct GfxLightGrid;
// Return false on a trace error; otherwise set visible using a point sight trace with contents mask 8193.
typedef bool (*LinkerGridSightTrace)(const float *start, const float *end, bool *visible, void *context);
// Selects the renderer's primary light at a point. Traced corners require a callback, never an assumed clear ray.
// The result is unchanged on malformed data or trace failure. The grid must come from the validated BSP importer.
bool Linker_SamplePrimaryLightGrid(const GfxLightGrid *grid, const float *position, unsigned int primaryLightCount,
                                   LinkerGridSightTrace trace, void *context, uint8_t *primaryLight, char *error,
                                   size_t errorSize);
