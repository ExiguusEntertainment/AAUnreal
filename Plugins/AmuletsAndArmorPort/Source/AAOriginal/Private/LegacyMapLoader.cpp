#include "LegacyMapLoader.h"
#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogLegacyMap, Log, All);

namespace Legacy
{
    static FLegacyMapLoadStats G_LastStats;

    static void SetFail()
    {
        G_LastStats = FLegacyMapLoadStats{}; // resets to zeros
    }

    static bool ParseBuffer(const TArray<uint8>& Data)
    {
        if (Data.Num() < 4 + 4 * 4) return false;
        if (FMemory::Memcmp(Data.GetData(), "AAM1", 4) != 0) return false;
        const uint8* Ptr = Data.GetData() + 4;
        auto ReadU32 = [&Ptr]() -> uint32 { uint32 V; FMemory::Memcpy(&V, Ptr, 4); Ptr += 4; return V; };
        FLegacyMapLoadStats Stats;
        Stats.NumVertices = (int32)ReadU32();
        Stats.NumLines    = (int32)ReadU32();
        Stats.NumSides    = (int32)ReadU32();
        Stats.NumSectors  = (int32)ReadU32();
        Stats.bValid = Stats.NumLines > 0 && Stats.NumSectors > 0;
        G_LastStats = Stats;
        return Stats.bValid;
    }

    static bool LoadInternal(const FString& Path)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            UE_LOG(LogLegacyMap, Error, TEXT("Failed to read legacy map file: %s"), *Path);
            SetFail();
            return false;
        }
        if (!ParseBuffer(Bytes))
        {
            UE_LOG(LogLegacyMap, Warning, TEXT("Unrecognized legacy map format: %s"), *Path);
            SetFail();
            return false;
        }
        UE_LOG(LogLegacyMap, Display, TEXT("Loaded legacy map %s V=%d L=%d Sides=%d Sec=%d"), *Path, G_LastStats.NumVertices, G_LastStats.NumLines, G_LastStats.NumSides, G_LastStats.NumSectors);
        return true;
    }
}

bool ULegacyMapLoader::LoadLegacyMapByNumber(int32 MapNumber)
{
    if (MapNumber < 0) return false;
    const FString Name = FString::Printf(TEXT("l%d.map"), MapNumber);
    // Search typical locations: project dir, Content/TestData, plugin content.
    TArray<FString> Candidates;
    Candidates.Add(Name);
    Candidates.Add(FPaths::ProjectContentDir() / TEXT("TestData") / Name);
    // (Additional plugin paths could be added here if needed.)
    for (const FString& C : Candidates)
    {
        if (FPaths::FileExists(C)) return Legacy::LoadInternal(C);
    }
    UE_LOG(LogLegacyMap, Error, TEXT("Legacy map number %d not found (searched %d paths)"), MapNumber, Candidates.Num());
    Legacy::SetFail();
    return false;
}

bool ULegacyMapLoader::LoadLegacyMapFromPath(const FString& AbsoluteOrRelativePath)
{
    FString Path = AbsoluteOrRelativePath;
    if (!FPaths::FileExists(Path))
    {
        // Try relative to project content testdata
        FString Alt = FPaths::ProjectContentDir() / TEXT("TestData") / FPaths::GetCleanFilename(Path);
        if (FPaths::FileExists(Alt)) Path = Alt;
    }
    if (!FPaths::FileExists(Path))
    {
        UE_LOG(LogLegacyMap, Error, TEXT("Legacy map file not found: %s"), *AbsoluteOrRelativePath);
        Legacy::SetFail();
        return false;
    }
    return Legacy::LoadInternal(Path);
}

FLegacyMapLoadStats ULegacyMapLoader::GetLastLegacyStats()
{
    return Legacy::G_LastStats;
}

extern "C" AAORIGINAL_API void AA_GetLegacyMapStats(int32& OutLines, int32& OutSides, int32& OutSectors, int32& OutVertices, bool& bValid)
{
    const FLegacyMapLoadStats S = ULegacyMapLoader::GetLastLegacyStats();
    OutLines = S.NumLines; OutSides = S.NumSides; OutSectors = S.NumSectors; OutVertices = S.NumVertices; bValid = S.bValid;
}
