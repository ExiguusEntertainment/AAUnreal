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
        int32 InSector, int32 InThingId);
    uint16 GetSourceAngle() const;
    void SetSourceAngle(uint16 Angle);
    void SetPickup(ANativeWadPickup* Item);
    ANativeWadPickup* GetPickup() const;
    int32 GetSourceThingId() const { return ThingId; }

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

    double FrameTime = 0.0;
    int32 AnimationStep = 0;
    int32 CurrentFrame = INDEX_NONE;
    int32 CurrentAngle = INDEX_NONE;
    FRandomStream Random;
};
