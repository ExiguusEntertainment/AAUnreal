#pragma once
#include "CoreMinimal.h"
#include "DoomWadWalls.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "SectorMeshBuilderActor.generated.h"

class UProceduralMeshComponent;
class USceneComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

// Experimental: builds per-sector meshes (floor, ceiling, and wall strips) from last loaded WAD topology.
UCLASS(Blueprintable, hidecategories=(Input, Replication, HLOD, WorldPartition, Rendering))
class AAORIGINAL_API ASectorMeshBuilderActor : public AActor
{
    GENERATED_BODY()
public:
    ASectorMeshBuilderActor();

    UFUNCTION(CallInEditor, Category="Sector Mesh")
    void BuildSectorsFromLastWad();

    void SetBuildWallsEnabled(bool bEnabled) { bBuildWalls = bEnabled; }
    void SetBuildFloorsEnabled(bool bEnabled) { bBuildFloorsAndCeilings = bEnabled; }
    void SetBuildCeilingsEnabled(bool bEnabled) { bBuildCeilings = bEnabled; }
    void SetFloorVertexColor(const FColor& InColor) { FloorVertexColor = InColor; }
    void SetCeilingVertexColor(const FColor& InColor) { CeilingVertexColor = InColor; }
    void SetWallVertexColor(const FColor& InColor) { WallVertexColor = InColor; }

    // Runtime controls: move floor/ceiling independently (values in Unreal units)
    UFUNCTION(BlueprintCallable, Category="Sector Mesh|Runtime")
    void SetSectorFloorZ(int32 SectorIndex, float NewFloorZ_UU);

    UFUNCTION(BlueprintCallable, Category="Sector Mesh|Runtime")
    void SetSectorCeilingZ(int32 SectorIndex, float NewCeilZ_UU);

    // Offsets from the imported position, in source texels. Follows the original
    // floor scroll direction on both horizontal surfaces and the sidedef direction on walls.
    UFUNCTION(BlueprintCallable, Category="Sector Mesh|Runtime")
    bool SetSectorTextureScrollTexels(int32 SectorIndex, bool bCeiling, FVector2D OffsetTexels);

    UFUNCTION(BlueprintCallable, Category="Sector Mesh|Runtime")
    bool SetWallTextureScrollTexels(int32 SideIndex, EWadWallBand Band, FVector2D OffsetTexels);

    // Actor-relative Z in Unreal units, read from the two generated surfaces.
    bool GetSectorSurfaceHeights(int32 SectorIndex, FVector2D& OutHeights) const;

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    void ClearChildren();
    void BuildWalls(const FDoomWadMapData& Map);
    void BuildWallBatch(int32 BatchIndex);
    void SetSectorSurfaceZ(int32 SectorIndex, float NewZ_UU, bool bCeiling);

    // The builder owns the data needed to move its sectors even after another WAD is loaded.
    FDoomWadMapData WallMap;
    TArray<FWadSectorHeightRange> SectorHeights;
    TArray<TArray<int32>> WallBatchesBySector;
    TArray<TArray<int32>> WallBatchesBySide;
    TArray<TArray<TWeakObjectPtr<UProceduralMeshComponent>>> WallBatches;
    double BuiltUnitsToUU = 1.0;

    struct FWallTextureScroll
    {
        FVector2D Offset = FVector2D::ZeroVector;
        TArray<TWeakObjectPtr<UMaterialInstanceDynamic>, TInlineAllocator<2>> Materials;
    };
    TMap<uint64, FWallTextureScroll> WallTextureScrolls;

    UPROPERTY(Transient)
    TMap<FString, TObjectPtr<UMaterialInterface>> WallMaterials;

    // Settings
    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    float UnitsToUU = 1.0f; // scale source units to Unreal units

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bClearPrevious = true;

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bBuildWalls = true;

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bBuildFloorsAndCeilings = true;

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bBuildCeilings = true;

    // Materials
    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    TObjectPtr<UMaterialInterface> FloorMaterial = nullptr;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    TObjectPtr<UMaterialInterface> CeilingMaterial = nullptr;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    TObjectPtr<UMaterialInterface> WallMaterial = nullptr;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    FColor FloorVertexColor = FColor::Red;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    FColor CeilingVertexColor = FColor::Blue;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    FColor WallVertexColor = FColor(255, 224, 96, 255);

    // Debug: only build a specific sector if >= 0
    UPROPERTY(EditAnywhere, Category="Sector Mesh|Debug")
    int32 SectorFilter = -1;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> SceneRoot;

    // Per-sector movable roots and PMCs for floors/ceilings (index = sector)
    UPROPERTY(Transient)
    TArray<TObjectPtr<USceneComponent>> FloorRoots;

    UPROPERTY(Transient)
    TArray<TObjectPtr<USceneComponent>> CeilRoots;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UProceduralMeshComponent>> FloorPMCs;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UProceduralMeshComponent>> CeilPMCs;

    UPROPERTY(Transient)
    TArray<TWeakObjectPtr<AActor>> Spawned; // kept for parity with other builder; currently unused as we attach PMCs to this actor
};
