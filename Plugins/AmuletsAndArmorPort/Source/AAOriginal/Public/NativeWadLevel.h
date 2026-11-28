#pragma once

#include "CoreMinimal.h"
#include "DoomWadWalls.h"
#include "Engine/TimerHandle.h"
#include "NativeWadScript.h"
#include "NativeWadLightRules.h"
#include "GameFramework/Actor.h"
#include "NativeWadLevel.generated.h"

class UBoxComponent;
class UAudioComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;
class UWorld;
class UPrimitiveComponent;
class ANativeWadPlayer;
class ANativeWadPickup;
class ANativeWadGuardian;
class ANativeWadScenery;
class UStaticMesh;
class UTexture2D;
struct FWadLevelCheckpoint;

USTRUCT()
struct FWadAmbientEmitter
{
    GENERATED_BODY()

    UPROPERTY()
    FVector Position = FVector::ZeroVector;

    UPROPERTY()
    int32 Sound = 0;

    UPROPERTY()
    int32 Group = 0;
    UPROPERTY()
    int32 SourceThingId = INDEX_NONE;
    UPROPERTY()
    int32 MarkerType = 0;
    UPROPERTY()
    int32 Radius = 500;
    UPROPERTY()
    int32 MaxVolume = 255;
    UPROPERTY()
    bool bLoop = true;
};

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

    UPROPERTY()
    int32 Activity = 0;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> WalkingFloor;

    UPROPERTY()
    int32 FloorDepth = 0;

    // Original G_3dSectorInfoArray friction; absent old-map property defaults to dirt.
    UPROPERTY()
    int32 SourceFriction = 12;

    UPROPERTY()
    bool bWater = false;
    UPROPERTY()
    bool bLava = false;
    UPROPERTY()
    bool bAcid = false;

    UPROPERTY()
    FVector2D FlowImpulse = FVector2D::ZeroVector;

    UPROPERTY()
    FVector2D FloorScrollRate = FVector2D::ZeroVector;

    // Integer edge data from the original L52 import.
    UPROPERTY()
    TArray<FIntPoint> RepairedBoundary;

    // Start and end points for repaired footprint edges, including fractional intersections.
    UPROPERTY()
    TArray<FVector2D> RepairedBoundaryPoints;
};

USTRUCT()
struct FAAWadNativeSide
{
    GENERATED_BODY()

    UPROPERTY()
    FVector2D ScrollRate = FVector2D::ZeroVector;

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
    int32 Activity = 0;

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

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 RequiredItem = 0;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 LockStrength = -1;
};

USTRUCT()
struct FWadPickupGenerator
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<ANativeWadPickup> Pickup;

    UPROPERTY()
    float Delay = 0.0f;
};

USTRUCT()
struct FWadCreatureGenerator
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<TObjectPtr<ANativeWadGuardian>> Creatures;
    UPROPERTY()
    TArray<TObjectPtr<ANativeWadPickup>> Pickups;
    UPROPERTY()
    TObjectPtr<ANativeWadGuardian> Summoner;

    int32 NumReleases() const { return Creatures.Num() + Pickups.Num(); }
    bool IsExhausted(int32 Released) const { return NumReleases() == 0 || (!bInfinite && Released >= NumReleases()); }

    UPROPERTY()
    bool bInfinite = false;
    UPROPERTY()
    FVector2D SpawnXY = FVector2D::ZeroVector;

    UPROPERTY()
    int32 Sector = INDEX_NONE;

    UPROPERTY()
    float Interval = 0.0f;

    UPROPERTY()
    int32 RandomInterval = 0;

    UPROPERTY()
    int32 MaxActive = 0;

    UPROPERTY()
    int32 SourceType = 0;

    UPROPERTY()
    bool bStartsActive = false;
};

USTRUCT()
struct FAAWadNativeLevelLink
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 Sector = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    int32 SourceThing = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category="WAD")
    TSoftObjectPtr<UWorld> Destination;

    UPROPERTY()
    int32 CampaignForm = 0;
};

// Owns the saved Lumen geometry and the small amount of topology needed to move it.
UCLASS(Blueprintable)
class AAORIGINAL_API ANativeWadLevel : public AActor
{
    GENERATED_BODY()

public:
    ANativeWadLevel();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
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

    UFUNCTION(BlueprintCallable, Category="WAD|Lighting")
    void SetTimeOfDayHours(double Hours);

    bool HasDaySky() const { return DaySky != nullptr; }
    double GetTimeOfDayHours() const { return TimeOfDayHours; }
    UFUNCTION(BlueprintPure, Category="WAD|Runtime")
    int32 GetSectorLightLevel(int32 Sector) const;

    int32 GetOutsideLightLevel() const { return OutsideLight; }
    UFUNCTION(BlueprintPure, Category="WAD|Lighting")
    FWadSourceLightingMetadata GetSourceLightingMetadata() const { return SourceLightingMetadata; }
    UFUNCTION(BlueprintPure, Category="WAD|Lighting")
    bool IsSourceLightFeedbackEnabled() const { return bUseSourceLightFeedback; }
    bool InstallSourceLightingMetadata(const FWadSourceLightingMetadata& Metadata, FString& Error);


    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool SetSectorTextureScrollTexels(int32 Sector, bool bCeiling, FVector2D Offset);

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool SetWallTextureScrollTexels(int32 Side, EWadWallBand Band, FVector2D Offset);

    // Requests the WAD-authored open, hold, and close cycle on an unlocked door sector.
    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool UseDoor(int32 Sector);

    bool UseDoorFromView(const FVector& Position, const FVector& Direction, double Reach, ANativeWadPlayer* Player);
    bool UseActivityFromView(ANativeWadPlayer& Player);
    void CheckActivitySector(ANativeWadPlayer& Player, int32& PreviousSector);
    int32 FindActivitySector(const FVector2D& Position, double Radius = 0.0, int32 PreviousSector = INDEX_NONE) const;
    int32 FindWadSector(const FVector2D& Position) const;
    static constexpr ECollisionChannel WaterSurfaceChannel = ECC_GameTraceChannel2;
    static constexpr ECollisionChannel LavaSurfaceChannel = ECC_GameTraceChannel3;
    TOptional<double> FindWaterSurfaceZ(const FVector& Position) const;
    TOptional<double> FindLavaSurfaceZ(const FVector& Position) const;
    TOptional<double> FindAcidSurfaceZ(const FVector& Position) const;
    UFUNCTION(BlueprintPure, Category="WAD|Terrain")
    bool IsWadSectorAcid(int32 Sector) const;
    UFUNCTION(BlueprintPure, Category="WAD|Terrain")
    int32 GetWadSectorDepth(int32 Sector) const;
    double GetWalkingHeight(int32 Sector) const;
    UFUNCTION(BlueprintPure, Category="WAD|Terrain")
    int32 GetWadSectorFriction(int32 Sector) const;
    int32 GetSupportingWadFriction(const UPrimitiveComponent* Support, const FVector& Position) const;
    bool AttachActorToWadFloor(AActor& Actor, int32 Sector) const;
    FVector2D GetCurrentInput(const FVector& Feet, bool bWaterWalking = false) const;
    int32 CountPlayingAmbientEmitters(int32 Sound) const;
    // Read-only audio inspection for saved-content verification and runtime diagnostics.
    UFUNCTION(BlueprintPure, Category="WAD|Audio")
    FString DescribeWadAmbientAudio() const;
    const TArray<FWadAmbientEmitter>& GetWadAmbientEmitters() const { return AmbientEmitters; }
    // External native activators use the same budgeted interpreter as authored sector/switch callbacks.
    bool RunWadActivity(int32 Activity, ANativeWadPlayer* Player = nullptr);

    UFUNCTION(BlueprintPure, Category="WAD|Scripts")
    FString DescribeWadScriptMissiles() const;

    bool HasWadActivities() const { return !ImportedActivities.IsEmpty(); }
    int32 GetWadScriptBlockCount(int32 Activity) const { return ScriptBlockCounts.FindRef(Activity); }
    int32 GetScriptMissileCount() const { return ScriptMissileCount; }
    int32 GetScriptMissileCount(int32 Type) const { return ScriptMissilesByType.FindRef(Type); }
    int32 GetScriptMissilesBlocked() const { return ScriptMissilesBlocked; }
    void CountBlockedScriptMissile() { ++ScriptMissilesBlocked; }
    void LockScriptDoorsNear(const FVector2D& Impact);
    int32 GetDoorLockStrength(int32 Sector);
    bool ModifyDoorLocksNear(const FVector2D& Impact, bool bLock, double Radius);
    bool WriteCheckpoint(FWadLevelCheckpoint& Checkpoint, bool bDiscardProjectiles = false) const;
    bool RestoreCheckpoint(const FWadLevelCheckpoint& Checkpoint);

    bool GetSectorSurfaceHeights(int32 Sector, FVector2D& Heights) const;
    const TArray<FAAWadNativeSector>& GetSectors() const { return Sectors; }
    const TArray<FAAWadNativeDoor>& GetDoors() const { return Doors; }
    const TArray<FAAWadNativeLevelLink>& GetLevelLinks() const { return LevelLinks; }
    int32 FindLevelLinkAt(const FVector2D& Position) const;
    bool GetWallLine(int32 Line, FWadWallLine& Wall);
    int32 GetLineCount() const { return Lines.Num(); }
    int32 GetWadLineFlags(int32 Index) const { return Lines.IsValidIndex(Index) ? int32(Lines[Index].Flags) : 0; }
    uint32 GetWallGeometryRevision() const { return WallGeometryRevision; }

    // Read the saved lower/middle/upper picture names without exposing mutable topology.
    UFUNCTION(BlueprintPure, Category="WAD|Surfaces")
    TArray<FString> GetWadSideTextureNames(int32 Side) const;

private:
    friend struct FAAWadNativeImport;
    uint32 WallGeometryRevision = 0;

    void InitializeTopology();
    void InitializeWaterSurfaces();
    bool UseDoorForPlayer(int32 Sector, ANativeWadPlayer* Player);
    void ActivatePickupGenerator(int32 Id);
    void TickPickupGenerators(float DeltaSeconds);
    void ActivateCreatureGenerator(int32 Id);
    void TickCreatureGenerators(float DeltaSeconds);
    bool HasPendingCreatureGenerators() const;
    void SetSectorHeight(int32 Sector, double Z, bool bCeiling);
    void UpdateLine(int32 Line, bool bPersist);
    void UpdateLineCollisionOnly(int32 Index);
    void UpdateLineCollision(int32 Index, FWadWallLine& Wall);
    void ApplyLight(UStaticMeshComponent& Mesh, int32 Light, bool bPersist) const;
    void ApplySectorLight(int32 Sector, int32 Light);
    void RefreshSectorLights();
    void UpdateDaySky();
    bool ContainsSectorPoint(int32 Sector, const FVector2D& LocalPosition) const;
    const FWadScriptActivity* FindWadActivity(int32 Activity) const;
    void RunWadScriptCommands(int32 Activity, int32 StartCommand, TWeakObjectPtr<ANativeWadPlayer> Player, int32& Budget);
    void ScheduleScriptContinuation(int32 Activity, int32 NextCommand, TWeakObjectPtr<ANativeWadPlayer> Player, float Delay);
    void StartActivitySlide(int32 Sector, double Target, int32 Ticks, bool bCeiling, bool bPauseOnCrush, int32 CompletionActivity);
    void TickActivitySlides(float DeltaSeconds);
    void SetActivitySwitch(int32 Side, bool bUp);
    void SetActivityWallTexture(int32 Side, FName Texture);
    UStaticMeshComponent* FindActivitySideBand(int32 Side) const;
    void PlayActivitySound(const FWadScriptCommand& Command);
    void LaunchActivityMissile(const FWadScriptCommand& Command);
    void TickSurfaceScroll(float DeltaSeconds);
    void TickAmbientEmitters(float DeltaSeconds);
    void TeleportActivityPlayer(ANativeWadPlayer& Player, const FVector2D& Destination);

    struct FActivitySlide
    {
        int32 Sector;
        double From;
        double To;
        double Duration;
        double Elapsed;
        bool bCeiling;
        bool bPauseOnCrush;
        int32 CompletionActivity;
    };

    struct FPendingScriptDelay
    {
        FTimerHandle Handle;
        int32 Activity;
        int32 NextCommand;
        TWeakObjectPtr<ANativeWadPlayer> Player;
    };

    UPROPERTY()
    TArray<int32> ActivitySectors;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> ActivitySwitchUp;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> ActivitySwitchDown;

    UPROPERTY()
    TMap<int32, FWadSoundSample> ActivitySounds;

    UPROPERTY()
    TMap<FName, TObjectPtr<UMaterialInterface>> ActivityWallMaterials;

    UPROPERTY()
    TMap<int32, TObjectPtr<UTexture2D>> ActivityJournalPages;

    UPROPERTY()
    TArray<FWadAmbientEmitter> AmbientEmitters;

    UPROPERTY()
    TMap<int32, FWadSoundSample> AmbientSamples;

    UPROPERTY(Transient)
    TMap<int32, TObjectPtr<UAudioComponent>> PlayingEmitters;

    UPROPERTY(Transient)
    TSet<int32> CompletedAmbientOneShots;
    float AmbientUpdateTime = 0.0f;

    UPROPERTY()
    TObjectPtr<UStaticMesh> ScriptMissileMesh;

    UPROPERTY()
    TMap<int32, FWadPickupGenerator> PickupGenerators;

    UPROPERTY()
    TMap<int32, FWadCreatureGenerator> CreatureGenerators;

    UPROPERTY()
    TMap<int32, TObjectPtr<ANativeWadScenery>> ScriptObjects;

    TMap<int32, FWadCreatureGeneratorState> CreatureGeneratorStates;
    TMap<int32, float> PendingPickupGenerators;
    TSet<int32> FiredPickupGenerators;
    TSet<int32> UnlockedItemDoors;
    TSet<int32> ScriptLockedDoors;
    TMap<int32, int32> DoorLockStrengths;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> ScriptMissileArt;

    UPROPERTY()
    TMap<int32, TObjectPtr<UMaterialInterface>> ScriptMissileMaterials;

    TMap<int32, bool> ActivitySwitchStates;
    TMap<int32, FName> ActivityWallTextures;
    TArray<FActivitySlide> ActivitySlides;
    TMap<uint64, FPendingScriptDelay> PendingScriptDelays;
    uint64 NextScriptDelay = 0;
    int32 ScriptMissileCount = 0;
    TMap<int32, int32> ScriptMissilesByType;
    int32 ScriptMissilesBlocked = 0;
    int32 ScriptVariables[256] = {};
    bool ScriptFlags[8] = {};
    TMap<int32, int32> ScriptBlockCounts;

    UPROPERTY()
    TArray<FWadScriptActivity> ImportedActivities;

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

    UPROPERTY(VisibleAnywhere, Category="WAD")
    TArray<FAAWadNativeLevelLink> LevelLinks;

    UPROPERTY(VisibleAnywhere, Category="WAD|Lighting")
    TObjectPtr<UStaticMeshComponent> DaySky;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> DaySkyMaterial;

    UPROPERTY(EditAnywhere, Category="WAD|Lighting", meta=(ClampMin="0.0", ClampMax="24.0"))
    double TimeOfDayHours = 12.0;

    bool bRunDayClock = false;

    UPROPERTY(VisibleAnywhere, Category="WAD|Lighting")
    bool bUseSectorLightingRules = false;

    UPROPERTY(EditAnywhere, Category="WAD|Lighting", meta=(ClampMin="0.0", ClampMax="10.0"))
    float SectorAmbientStrength = 1.5f;

    UPROPERTY(EditAnywhere, Category="WAD|Lighting", meta=(ClampMin="0", ClampMax="255"))
    int32 OutsideLight = 255;

    TMap<int32, FActiveDoor> ActiveDoors;

    UPROPERTY()
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
    // Additive source metadata only; old and refreshed maps remain legacy by default.
    UPROPERTY()
    FWadSourceLightingMetadata SourceLightingMetadata;
    UPROPERTY()
    bool bUseSourceLightFeedback = false;

    TArray<TArray<int32>> LightingNeighbors;
    TMap<int32, int32> DoorBySector;
    TArray<int32> AppliedLights;
    TMap<int32, int32> LightOverrides;
    TMap<uint64, FVector2D> WallScroll;
    double SurfaceScrollSeconds = 0.0;
    UPROPERTY()
    TArray<int32> ScrollingSectors;
    UPROPERTY()
    TArray<int32> ScrollingSides;
    UPROPERTY()
    TArray<int32> FlowSectors;
};
