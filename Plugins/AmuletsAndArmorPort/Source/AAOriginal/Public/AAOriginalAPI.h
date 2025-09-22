#pragma once

#include "CoreMinimal.h"

// Compute CRC32 of a buffer (standard IEEE polynomial). Returns 0 for null or empty input.
AAORIGINAL_API uint32 AA_ComputeCRC32(const uint8* Data, int32 Length);

// Returns a semantic version string for the upstream integration (e.g., "0.0.1").
AAORIGINAL_API const TCHAR* AA_GetVersion();
