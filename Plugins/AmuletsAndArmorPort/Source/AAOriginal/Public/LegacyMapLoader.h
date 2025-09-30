#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LegacyMapLoader.generated.h"

// Modern reimplementation placeholder for legacy .map loading.
// NOTE: Does not depend on original external legacy headers; only parses
// a simplified custom binary/text structure we define for UE integration.
// Expected minimal format (subject to evolution):
//   Header: 4 bytes magic "AAM1"
//   uint32 NumVertices, NumLines, NumSides, NumSectors
//   Followed optionally by opaque payload we ignore for now.

USTRUCT(BlueprintType)
struct FLegacyMapLoadStats
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 NumLines = 0;
    UPROPERTY(BlueprintReadOnly) int32 NumSides = 0;
    UPROPERTY(BlueprintReadOnly) int32 NumSectors = 0;
    UPROPERTY(BlueprintReadOnly) int32 NumVertices = 0;
    UPROPERTY(BlueprintReadOnly) bool bValid = false;
};

UCLASS()
class AAORIGINAL_API ULegacyMapLoader : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="LegacyMap")
    static bool LoadLegacyMapByNumber(int32 MapNumber);

    UFUNCTION(BlueprintCallable, Category="LegacyMap")
    static bool LoadLegacyMapFromPath(const FString& AbsoluteOrRelativePath);

    UFUNCTION(BlueprintCallable, Category="LegacyMap")
    static FLegacyMapLoadStats GetLastLegacyStats();
};

extern "C" AAORIGINAL_API void AA_GetLegacyMapStats(int32& OutLines, int32& OutSides, int32& OutSectors, int32& OutVertices, bool& bValid);
