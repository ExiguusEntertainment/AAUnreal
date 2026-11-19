#pragma once

#include "CoreMinimal.h"
#include "DoomWadWalls.h"
#include "GameFramework/Actor.h"
#include "NativeWadLevel.generated.h"

class UBoxComponent;
class UStaticMeshComponent;

USTRUCT()
struct FAAWadNativeSector
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category="WAD")
    TObjectPtr<UStaticMeshComponent> Floor;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    TObjectPtr<UStaticMeshComponent> Ceiling;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 Light = 0;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    bool bOutside = false;
};

USTRUCT()
struct FAAWadNativeSide
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Sector = INDEX_NONE;

    UPROPERTY()
    FIntPoint TextureOffset = FIntPoint::ZeroValue;

    UPROPERTY()
    TArray<FString> Textures;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> Bands;
};

USTRUCT()
struct FAAWadNativeLine
{
    GENERATED_BODY()

    UPROPERTY()
    FIntPoint Endpoints = FIntPoint(INDEX_NONE, INDEX_NONE);

    UPROPERTY()
    FIntPoint Sides = FIntPoint(INDEX_NONE, INDEX_NONE);

    UPROPERTY()
    uint16 Flags = 0;

    UPROPERTY()
    TArray<TObjectPtr<UBoxComponent>> Collision;
};

USTRUCT()
struct FAAWadNativeDoor
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 Sector = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 SourceThing = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    float ClosedZ = 0.0f;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    float OpenZ = 0.0f;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    float TravelSeconds = 0.0f;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    float HoldSeconds = 0.0f;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    bool bRequiresUnlock = false;
};

// Owns the saved Lumen geometry and the small amount of topology needed to move it.
UCLASS(Blueprintable)
class AAORIGINAL_API ANativeWadLevel : public AActor
{
    GENERATED_BODY()

public:
    ANativeWadLevel();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    void SetSectorFloorZ(int32 Sector, float Z);

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    void SetSectorCeilingZ(int32 Sector, float Z);

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool SetSectorLightLevel(int32 Sector, int32 Light);

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    void ClearSectorLightOverride(int32 Sector);

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    void SetOutsideLightLevel(int32 Light);

    UFUNCTION(BlueprintPure, Category="WAD|Runtime")
    int32 GetSectorLightLevel(int32 Sector) const;

    int32 GetOutsideLightLevel() const { return OutsideLight; }

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool SetSectorTextureScrollTexels(int32 Sector, bool bCeiling, FVector2D Offset);

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool SetWallTextureScrollTexels(int32 Side, EWadWallBand Band, FVector2D Offset);

    // Requests the WAD-authored open, hold, and close cycle on an unlocked door sector.
    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool UseDoor(int32 Sector);

    bool UseDoorFromView(const FVector& Position, const FVector& Direction, double Reach, const AActor* UserActor);

    bool GetSectorSurfaceHeights(int32 Sector, FVector2D& Heights) const;
    const TArray<FAAWadNativeSector>& GetSectors() const { return Sectors; }
    const TArray<FAAWadNativeDoor>& GetDoors() const { return Doors; }
    bool GetWallLine(int32 Line, FWadWallLine& Wall);
    int32 GetLineCount() const { return Lines.Num(); }

private:
    friend struct FAAWadNativeImport;

    void InitializeTopology();
    void SetSectorHeight(int32 Sector, double Z, bool bCeiling);
    void UpdateLine(int32 Line, bool bPersist);
    void ApplyLight(UStaticMeshComponent& Mesh, int32 Light, bool bPersist) const;
    void ApplySectorLight(int32 Sector, int32 Light);
    void RefreshSectorLights();

    enum class EDoorPhase : uint8 { Opening, Holding, Closing };

    struct FActiveDoor
    {
        EDoorPhase Phase = EDoorPhase::Opening;
        double StartZ = 0.0;
        double Elapsed = 0.0;
    };

    bool UpdateDoor(const FAAWadNativeDoor& Door, FActiveDoor& Active, double DeltaSeconds);
    bool IsDoorObstructed(int32 Sector, double FromZ, double ToZ) const;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    TArray<FAAWadNativeDoor> Doors;

    UPROPERTY(VisibleAnywhere, Category="WAD|Lighting")
    bool bUseSectorLightingRules = false;

    UPROPERTY(EditAnywhere, Category="WAD|Lighting", meta=(ClampMin="0.0", ClampMax="10.0"))
    float SectorAmbientStrength = 1.5f;

    UPROPERTY(EditAnywhere, Category="WAD|Lighting", meta=(ClampMin="0", ClampMax="255"))
    int32 OutsideLight = 255;

    TMap<int32, FActiveDoor> ActiveDoors;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    TArray<FAAWadNativeSector> Sectors;

    UPROPERTY()
    TArray<FAAWadNativeSide> Sides;

    UPROPERTY()
    TArray<FAAWadNativeLine> Lines;

    UPROPERTY()
    TArray<FIntPoint> Vertices;

    FDoomWadMapData WallTopology;
    TArray<FWadSectorHeightRange> Heights;
    TArray<TArray<int32>> LinesBySector;
    TArray<TArray<int32>> LightingNeighbors;
    TMap<int32, int32> DoorBySector;
    TArray<int32> AppliedLights;
    TMap<int32, int32> LightOverrides;
    TMap<uint64, FVector2D> WallScroll;
};
