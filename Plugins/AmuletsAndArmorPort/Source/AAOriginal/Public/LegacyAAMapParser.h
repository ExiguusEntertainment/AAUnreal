#pragma once
#include "CoreMinimal.h"

// Minimal modern parser for Amulets & Armor Lxx.MAP files.
// This parser intentionally avoids any dependency on legacy headers.
// It will be incrementally improved as the true format is documented.

struct FAA_MapVertex
{
    int16 X = 0;
    int16 Y = 0;
};

struct FAA_MapLine
{
    int16 From = -1;
    int16 To = -1;
};

struct FAA_MapData
{
    TArray<FAA_MapVertex> Vertices;
    TArray<FAA_MapLine> Lines;
    // Future: Sides, Sectors, etc.
};

namespace AA
{
    class FLegacyAAMapParser
    {
    public:
        // Attempt to parse the given bytes as an A&A .MAP. Returns true on success.
        static bool Parse(const TArray<uint8>& Bytes, FAA_MapData& Out, FString& OutWhyNot);

        // Utility: simple hex dump helper for debugging
        static void HexDumpFirstBytes(const TArray<uint8>& Bytes, int32 MaxBytesToLog = 256);
    };
}
