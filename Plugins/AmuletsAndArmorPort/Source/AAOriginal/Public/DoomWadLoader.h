#pragma once
#include "CoreMinimal.h"
#include "DoomWadTypes.h"
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

    // The view is valid until the next load. Returns null if no map is loaded.
    static const FDoomWadMapData* GetLastWadMapData();

    // Triangulate the original linedef boundaries, including holes and separate islands.
    static bool GetLastWadSectorFootprint(int32 SectorIndex, FDoomWadSectorFootprint& OutFootprint);

    // Export source data without changing the map loaded for the preview or builder.
    static bool ExportDoomWadMapJson(const FString& WadPath, const FString& MapName, const FString& OutputPath);

    // Editor/debug helper: returns last-loaded 2D geometry for wall building
    // Vertices are returned in Unreal units (raw WAD ints cast to float). Lines reference vertex indices.
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWad2DGeometry(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines);

    // Experimental: expose topology for wall band generation.
    // OutLineSectors: for each line, X=FrontSectorIdx (or -1), Y=BackSectorIdx (or -1)
    // OutSectorHeights: array sized to NumSectors, X=FloorHeight, Y=CeilingHeight
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadWallTopology(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines, TArray<FIntPoint>& OutLineSectors, TArray<FIntPoint>& OutSectorHeights);

    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadLineSideIndices(TArray<FIntPoint>& OutLineSideIndices);

    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadSectorTextureNames(int32 SectorIndex, FString& OutFloorTexture, FString& OutCeilingTexture);

    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadSideTextureNames(int32 SideIndex, FString& OutUpperTexture, FString& OutLowerTexture, FString& OutMainTexture, FVector2D& OutTextureOffset);

    static void GetWadSideTextureNames(const FWadSideDef& Side, FString& OutUpperTexture, FString& OutLowerTexture, FString& OutMainTexture, FVector2D& OutTextureOffset);

    // Experimental: get per-sector subsector polygons if SEGS/SSECTORS were present.
    // Returns flattened polygon list:
    //  - OutPolyOffsets[i] and OutPolyCounts[i] describe the i-th polygon range in OutPolyVerts
    //  - OutPolyVerts contains all polygon 2D points (Unreal units)
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadSectorSubsectorPolys(int32 SectorIndex, TArray<int32>& OutPolyOffsets, TArray<int32>& OutPolyCounts, TArray<FVector2D>& OutPolyVerts);

    // Experimental: get per-sector boundary loops derived from linedefs.
    // Returns flattened loop list:
    //  - OutLoopOffsets[i] and OutLoopCounts[i] describe the i-th loop range in OutLoopVerts
    //  - OutLoopVerts contains all loop 2D points (Unreal units)
    UFUNCTION(BlueprintCallable, Category="DoomWAD|Debug", meta=(DevelopmentOnly))
    static bool GetLastWadSectorBoundaryLoops(int32 SectorIndex, TArray<int32>& OutLoopOffsets, TArray<int32>& OutLoopCounts, TArray<FVector2D>& OutLoopVerts);
};

// C-style access for tests or other modules (mirrors blueprint API)
extern "C" AAORIGINAL_API void AA_GetLastDoomWadStats(int32& OutVerts, int32& OutLines, int32& OutSides, int32& OutSectors, bool& bValid);
