#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "SectorMeshBuilderActor.generated.h"

class UProceduralMeshComponent;
class USceneComponent;
class UMaterialInterface;

// Experimental: builds per-sector meshes (floor, ceiling, and wall strips) from last loaded WAD topology.
// Limitations: holes (islands) not handled yet; sectors with multiple disjoint polygons will build multiple loops.
UCLASS(Blueprintable, hidecategories=(Input, Replication, HLOD, WorldPartition, Rendering))
class AAORIGINAL_API ASectorMeshBuilderActor : public AActor
{
    GENERATED_BODY()
public:
    ASectorMeshBuilderActor();

    UFUNCTION(CallInEditor, Category="Sector Mesh")
    void BuildSectorsFromLastWad();

    // Runtime controls: move floor/ceiling independently (values in Unreal units)
    UFUNCTION(BlueprintCallable, Category="Sector Mesh|Runtime")
    void SetSectorFloorZ(int32 SectorIndex, float NewFloorZ_UU);

    UFUNCTION(BlueprintCallable, Category="Sector Mesh|Runtime")
    void SetSectorCeilingZ(int32 SectorIndex, float NewCeilZ_UU);

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    void ClearChildren();

    // Build a simple floor/ceiling mesh for a polygon
    static void BuildPolygonMesh(const TArray<FVector2D>& Poly, float Z, bool bCeiling, float UnitsToUU, float UVScale, TArray<FVector>& OutVerts, TArray<int32>& OutIndices, TArray<FVector>& OutNormals, TArray<FVector2D>& OutUV0, TArray<FColor>& OutColors, TArray<FProcMeshTangent>& OutTangents);

    // Very simple ear clipping triangulation (no holes). Returns triangle indices into OutVerts (local poly indexing)
    static bool TriangulateEarClip(const TArray<FVector2D>& InPoly, TArray<int32>& OutLocalIndices);

    // Extract oriented edge loops for a sector (each edge oriented so sector is on the left of the edge direction)
    static void BuildSectorLoops(int32 SectorIndex,
                                 const TArray<FIntPoint>& Lines,
                                 const TArray<FIntPoint>& LineSectors,
                                 TArray<TArray<int32>>& OutLoops);

    // Settings
    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    float UnitsToUU = 1.0f; // scale source units to Unreal units

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bClearPrevious = true;

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bBuildWalls = true;

    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bBuildFloorsAndCeilings = true;

    // Prefer BSP subsector polygons for floor/ceiling when available
    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bUseSubsectorPolysForFloors = false;

    // Two-sided walls: also generate a middle band across overlapping height range
    UPROPERTY(EditAnywhere, Category="Sector Mesh")
    bool bBuildMiddleBands = false;

    // Materials
    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    TObjectPtr<UMaterialInterface> FloorMaterial = nullptr;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    TObjectPtr<UMaterialInterface> CeilingMaterial = nullptr;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|Materials")
    TObjectPtr<UMaterialInterface> WallMaterial = nullptr;

    // UV scale controls
    UPROPERTY(EditAnywhere, Category="Sector Mesh|UV")
    float FloorCeilUVScale = 0.01f;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|UV")
    float WallUScale = 0.01f;

    UPROPERTY(EditAnywhere, Category="Sector Mesh|UV")
    float WallVScale = 0.01f;

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
