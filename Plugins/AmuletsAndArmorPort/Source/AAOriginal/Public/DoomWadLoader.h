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

    // Editor/debug helper: returns last-loaded 2D geometry for wall building
    // Vertices are returned in Unreal units (raw WAD ints cast to float). Lines reference vertex indices.
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWad2DGeometry(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines);

    // Experimental: expose topology for wall band generation.
    // OutLineSectors: for each line, X=FrontSectorIdx (or -1), Y=BackSectorIdx (or -1)
    // OutSectorHeights: array sized to NumSectors, X=FloorHeight, Y=CeilingHeight
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadWallTopology(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines, TArray<FIntPoint>& OutLineSectors, TArray<FIntPoint>& OutSectorHeights);

    // Experimental: get per-sector subsector polygons if SEGS/SSECTORS were present.
    // Returns flattened polygon list:
    //  - OutPolyOffsets[i] and OutPolyCounts[i] describe the i-th polygon range in OutPolyVerts
    //  - OutPolyVerts contains all polygon 2D points (Unreal units)
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadSectorSubsectorPolys(int32 SectorIndex, TArray<int32>& OutPolyOffsets, TArray<int32>& OutPolyCounts, TArray<FVector2D>& OutPolyVerts);
};

// C-style access for tests or other modules (mirrors blueprint API)
extern "C" AAORIGINAL_API void AA_GetLastDoomWadStats(int32& OutVerts, int32& OutLines, int32& OutSides, int32& OutSectors, bool& bValid);
