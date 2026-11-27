#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NativeWadScenery.generated.h"

class ANativeWadLevel;
class ANativeWadPickup;
class UBoxComponent;
class UMaterialInterface;
class UStaticMeshComponent;

USTRUCT()
struct FWadSceneryView
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<UMaterialInterface> Material;

    UPROPERTY()
    FIntPoint Size = FIntPoint::ZeroValue;

    UPROPERTY()
    bool bMirrored = false;
};

USTRUCT()
struct FWadSceneryFrame
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<FWadSceneryView> Views;
};

USTRUCT()
struct FWadSceneryDefinition
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<FWadSceneryFrame> Frames;

    UPROPERTY()
    int32 Radius = 0;

    UPROPERTY()
    uint16 SourceAttributes = 0;

    UPROPERTY()
    int32 FrameTicks = 0;

    UPROPERTY()
    int32 AnimationType = 0;

    UPROPERTY()
    bool bPassable = false;

    UPROPERTY()
    bool bHanging = false;

    UPROPERTY()
    bool bRaised = false;

    UPROPERTY()
    bool bStaysAboveLiquid = false;
};

UCLASS()
class AAORIGINAL_API ANativeWadScenery : public AActor
{
    GENERATED_BODY()

public:
    ANativeWadScenery();
    virtual void Tick(float DeltaSeconds) override;
    void Configure(ANativeWadLevel& Level, UStaticMesh* Plane, const FWadSceneryDefinition& InDefinition,
        int32 InSector, int32 InThingId, bool bPreserveOverrides = false, int32 InSourceType = 0);
    uint16 GetSourceAngle() const;
    void SetSourceAngle(uint16 Angle);
    void SetPickup(ANativeWadPickup* Item);
    UFUNCTION(BlueprintPure, Category = "WAD|Scenery")
    ANativeWadPickup* GetPickup() const;
    UFUNCTION(BlueprintPure, Category = "WAD|Scenery")
    int32 GetSourceThingId() const { return ThingId; }
    UFUNCTION(BlueprintPure, Category = "WAD|Scenery")
    int32 GetSourceType() const { return SourceType; }
    UFUNCTION(BlueprintPure, Category = "WAD|Scenery")
    int32 GetSourceMountSector() const { return Sector; }
    UFUNCTION(BlueprintPure, Category = "WAD|Scenery")
    TArray<int32> GetSourceSceneryInfo() const
    {
        return {SourceType, ThingId, Definition.Radius, Definition.SourceAttributes, Definition.FrameTicks,
            Definition.AnimationType, Definition.Frames.Num(), int32(Definition.bPassable), int32(Definition.bHanging),
            int32(Definition.bRaised), int32(Definition.bStaysAboveLiquid)};
    }
    const FWadSceneryDefinition& GetSceneryDefinition() const { return Definition; }
    int32 GetAnimationFrame() const { return CurrentFrame; }
    int32 GetMagicMapRadius() const { return Definition.Radius; }
    bool IsMagicMapInvisible() const { return (Definition.SourceAttributes & 0x0004) != 0; }

private:
    virtual void BeginPlay() override;
    void InitializeMount();
    void UpdateElevation();
    void SetView(int32 Frame, int32 Angle);

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> Sprite;

    UPROPERTY()
    TObjectPtr<UBoxComponent> Obstacle;

    UPROPERTY()
    TObjectPtr<ANativeWadLevel> WadLevel;

    UPROPERTY()
    TObjectPtr<ANativeWadPickup> Pickup;

    UPROPERTY()
    FWadSceneryDefinition Definition;

    UPROPERTY()
    int32 Sector = INDEX_NONE;

    UPROPERTY()
    int32 ThingId = INDEX_NONE;

    UPROPERTY()
    int32 SourceType = 0;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> SpriteMaterialOverride;

    UPROPERTY()
    double MountOffset = 0.0;

    UPROPERTY()
    double LastMountedZ = 0.0;

    UPROPERTY()
    bool bHasMountedZ = false;

    double FrameTime = 0.0;
    int32 AnimationStep = 0;
    int32 CurrentFrame = INDEX_NONE;
    int32 CurrentAngle = INDEX_NONE;
    FRandomStream Random;
};
