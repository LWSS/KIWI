#pragma once
#include <xanim/xanim.h>

// Shared by the engine's asset dispatcher and native writer round-trip tests.
void DB64_LoadTextAsset(XAssetType type, XAssetHeader *header, bool atStreamStart);
