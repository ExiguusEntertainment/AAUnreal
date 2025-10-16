#include "LegacyAAMapParser.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogLegacyAAMapParser, Log, All);

namespace
{
    template<typename T>
    static bool ReadAt(const TArray<uint8>& Bytes, int32 Offset, T& Out)
    {
        if (Offset < 0 || Offset + (int32)sizeof(T) > Bytes.Num()) return false;
        FMemory::Memcpy(&Out, Bytes.GetData() + Offset, sizeof(T));
        return true;
    }

    static bool ReadArray(const TArray<uint8>& Bytes, int32 Offset, int32 Count, int32 Stride, TFunctionRef<void(const uint8* P)> Each)
    {
        int64 End = (int64)Offset + (int64)Count * (int64)Stride;
        if (Offset < 0 || End > Bytes.Num()) return false;
        const uint8* Base = Bytes.GetData() + Offset;
        for (int32 i = 0; i < Count; ++i)
        {
            Each(Base + i * Stride);
        }
        return true;
    }
}

bool AA::FLegacyAAMapParser::Parse(const TArray<uint8>& Bytes, FAA_MapData& Out, FString& OutWhyNot)
{
    Out = FAA_MapData{};
    OutWhyNot.Empty();

    // First rough heuristic: many DOS-era maps stored a small header of counts near the start.
    // We try a few candidate layouts conservatively. If none match, we fail with a reason.

    if (Bytes.Num() < 16)
    {
        OutWhyNot = TEXT("File too small to be a valid A&A map");
        return false;
    }

    // Candidate Layout A: [u16 NumVerts][u16 NumLines][...followed by arrays]
    // Values should be reasonable. We'll probe a couple of likely patterns cautiously.
    auto TryLayoutA = [&]() -> bool
    {
        uint16 NumVerts16 = 0, NumLines16 = 0;
        if (!ReadAt(Bytes, 0, NumVerts16) || !ReadAt(Bytes, 2, NumLines16)) return false;
        const int32 NumVerts = (int32)NumVerts16;
        const int32 NumLines = (int32)NumLines16;
        if (NumVerts <= 0 || NumVerts > 65535 || NumLines <= 0 || NumLines > 65535) return false;

        // Compute expected sizes: vertices (int16 x,y) then lines (int16 from,to)
        const int32 VertsOffset = 4;
        const int32 VertStride = 2 * sizeof(int16);
        const int64 VertsEnd = (int64)VertsOffset + (int64)NumVerts * VertStride;
        const int32 LinesOffset = (int32)VertsEnd;
        const int32 LineStride = 2 * sizeof(int16);
        const int64 LinesEnd = (int64)LinesOffset + (int64)NumLines * LineStride;

        if (LinesEnd > Bytes.Num()) return false;

        Out.Vertices.SetNum(NumVerts);
        bool ok = ReadArray(Bytes, VertsOffset, NumVerts, VertStride, [&](const uint8* P)
        {
            int16 x, y; FMemory::Memcpy(&x, P + 0, 2); FMemory::Memcpy(&y, P + 2, 2);
            Out.Vertices.Add({x, y});
        });
        if (!ok) return false;

        Out.Lines.SetNum(NumLines);
        ok = ReadArray(Bytes, LinesOffset, NumLines, LineStride, [&](const uint8* P)
        {
            int16 a, b; FMemory::Memcpy(&a, P + 0, 2); FMemory::Memcpy(&b, P + 2, 2);
            Out.Lines.Add({a, b});
        });
        if (!ok) return false;

        // Sanity: referenced indices should be < NumVerts
        int32 ValidRefs = 0;
        for (const auto& L : Out.Lines) if (L.From >= 0 && L.From < NumVerts && L.To >= 0 && L.To < NumVerts) ++ValidRefs;
        if (ValidRefs < FMath::Max(1, NumLines / 2))
        {
            // Too many invalid refs, likely wrong layout
            Out = FAA_MapData{};
            return false;
        }
        return true;
    };

    if (TryLayoutA())
    {
        UE_LOG(LogLegacyAAMapParser, Display, TEXT("Parsed A&A MAP using LayoutA: V=%d L=%d"), Out.Vertices.Num(), Out.Lines.Num());
        return true;
    }

    // Additional layouts can be added here as we reverse-engineer the spec.

    OutWhyNot = TEXT("Unsupported or unknown A&A .MAP layout (needs spec).");
    return false;
}

void AA::FLegacyAAMapParser::HexDumpFirstBytes(const TArray<uint8>& Bytes, int32 MaxBytesToLog)
{
    const int32 N = FMath::Min(MaxBytesToLog, Bytes.Num());
    FString Line; Line.Reserve(N * 3);
    for (int32 i = 0; i < N; ++i)
    {
        Line += FString::Printf(TEXT("%02X "), Bytes[i]);
        if ((i % 16) == 15) { UE_LOG(LogLegacyAAMapParser, Display, TEXT("%s"), *Line); Line.Reset(); }
    }
    if (!Line.IsEmpty()) UE_LOG(LogLegacyAAMapParser, Display, TEXT("%s"), *Line);
}
