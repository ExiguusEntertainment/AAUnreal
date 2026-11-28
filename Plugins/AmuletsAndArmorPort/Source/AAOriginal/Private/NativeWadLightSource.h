#pragma once
#include "CoreMinimal.h"
#include "DoomWadTypes.h"
#include "NativeWadLightRules.h"

// Pure source builder. Edited positions are local source XY, never snapped or
// written back to their actors. Binding pointers are assigned by the importer.
AAORIGINAL_API bool BuildWadSourceLighting(const FDoomWadMapData& Map, TConstArrayView<uint8> OriginalMapBytes,
    const FString& SourceMapName, const TMap<int32,FVector2D>& EditedCenters,
    FWadSourceLightingMetadata& Result, FString& Error);
