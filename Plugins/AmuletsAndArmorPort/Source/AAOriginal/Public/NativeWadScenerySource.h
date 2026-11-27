#pragma once
#include "CoreMinimal.h"

// Text-source metadata only: no editor mutation or generated art in this reader.
struct FWadScenerySourceFrame
{
    TArray<int32> Pictures;
    uint16 Attributes = 0;
};
struct FWadScenerySource
{
    int32 Radius = 0;
    uint16 Attributes = 0;
    uint16 MoveAttributes = 0;
    int32 StanceCount = 0;
    int32 FrameTicks = 0;
    int32 AnimationType = 0;
    int32 NextStance = -1;
    TArray<FWadScenerySourceFrame> Frames;
    TMap<int32, FString> PictureFiles;
};
namespace NativeWadScenerySource
{
    // Exact full IDs only. Creature profiles, inventory definitions, starts,
    // map directives and invisible illumination marker924 never become props.
    AAORIGINAL_API bool LoadOrdinaryCatalog(TMap<int32, FString>& Resources, FString& Error);
    AAORIGINAL_API bool Load(const FString& Resource, FWadScenerySource& Result, FString& Error);
    // Original active BLOCKMAP search (3D_VIEW.C5675) and invalid-sector0
    // setup fallback (OBJMOVE.C1241), only for authored props outside native footprints.
    AAORIGINAL_API bool ResolveMountSector(TConstArrayView<uint8> MapBytes, FIntPoint Position,
        int32& Sector, bool& bUsedSectorZeroFallback, FString& Error);
    AAORIGINAL_API bool ResolveMountSector(const FString& SourceWadPath, FIntPoint Position,
        int32& Sector, bool& bUsedSectorZeroFallback, FString& Error);
}
