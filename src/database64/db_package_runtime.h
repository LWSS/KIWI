#pragma once

// Called before map parsing, on the engine's main thread. Loose maps remain valid.
void DB64_LoadMapPackage(const char *bspName);
