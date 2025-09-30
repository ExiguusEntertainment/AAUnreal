#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DoomWadLoader.generated.h"

USTRUCT(BlueprintType)
struct FDoomWadLoadStats
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 NumVertices = 0;
    UPROPERTY(BlueprintReadOnly) int32 NumLines = 0;
    UPROPERTY(BlueprintReadOnly) int32 NumSides = 0;
    UPROPERTY(BlueprintReadOnly) int32 NumSectors = 0;
    UPROPERTY(BlueprintReadOnly) bool bValid = false;
};

UCLASS()
class AAORIGINAL_API UDoomWadLoader : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="DoomWAD")
    static bool LoadDoomWadMap(const FString& WadAbsolutePath, const FString& MapName);

    UFUNCTION(BlueprintCallable, Category="DoomWAD")
    static FDoomWadLoadStats GetLastLoadStats();
};

// C-style access for tests or other modules (mirrors blueprint API)
extern "C" AAORIGINAL_API void AA_GetLastDoomWadStats(int32& OutVerts, int32& OutLines, int32& OutSides, int32& OutSectors, bool& bValid);
