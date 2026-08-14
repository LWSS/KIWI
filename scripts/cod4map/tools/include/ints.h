#pragma once

// Compatibility shim required by the donor's older minizip headers.  The
// reconstructed source references the original Mod Tools integer typedef
// header but does not include that header in the donor tree.
typedef unsigned __int64 ui64_t;
