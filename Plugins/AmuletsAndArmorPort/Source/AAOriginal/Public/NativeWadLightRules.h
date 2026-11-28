#pragma once
#include "CoreMinimal.h"
#include "NativeWadLightRules.generated.h"

class AActor;

USTRUCT(BlueprintType)
struct AAORIGINAL_API FWadSourceIlluminator
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 ThingId = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 SourceType = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 AccessoryData = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Illumination = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 AuthoredSector = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 CenterSector = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") FVector2D AuthoredPosition = FVector2D::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") FVector2D CenterPosition = FVector2D::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") bool bUsesEditedCenter = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") bool bUsedSourceFallback = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TObjectPtr<AActor> BoundActor;
};

USTRUCT(BlueprintType)
struct AAORIGINAL_API FWadSourceLightDirective
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 ThingId = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 MarkerType = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Sector = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 AnimationType = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Rate = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Center = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Radius = 0;
};

USTRUCT(BlueprintType)
struct AAORIGINAL_API FWadSourceLightSector
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Sector = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 AuthoredLight = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") bool bOutside = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") bool bDoor = false;
    // Exact source two-sided adjacency, including authored self neighbors.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<int32> Neighbors;
};

USTRUCT(BlueprintType)
struct AAORIGINAL_API FWadSourceLightingMetadata
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Version = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") FString SourceMapName;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") FString SourceMapSha1;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 ThingCount = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 LineCount = 0;
    // Native safety correction: source LIGHT.C dereferences these missing sides.
    // Keep their source IDs explicit; no fabricated sector adjacency.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<int32> UnaddressableTwoSidedLines;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<FWadSourceLightSector> Sectors;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<FWadSourceIlluminator> Illuminators;
    // Includes disabling terrain directives and overridden glow markers.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<FWadSourceLightDirective> Directives;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<FWadSourceLightDirective> Animations;
};

// Additive checkpoint payload: version0 means genuinely absent/legacy only.
USTRUCT(BlueprintType)
struct AAORIGINAL_API FWadSourceLightState
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") int32 Version = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") FString SourceMapSha1;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<int32> Levels;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") TArray<int32> Phases;
    // Fraction plus retained unserved fixed steps when a frame exceeds the budget.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="WAD|Lighting") double PendingTicks = 0.0;
    UPROPERTY() uint64 CompletedSteps = 0;
};

namespace NativeWadLightRules
{
    inline constexpr int32 MetadataVersion = 1;
    inline constexpr int32 FeedbackStateVersion = 1;
    inline constexpr double FeedbackHz = 70.0;
    inline constexpr int32 MaximumStepsPerFrame = 16;
    inline constexpr double MaximumPendingTicks = 1000000000.0;
    AAORIGINAL_API FString StateDigest(const FWadSourceLightState& State);
    AAORIGINAL_API FWadSourceLightState InitialState(const FWadSourceLightingMetadata& Metadata);
    AAORIGINAL_API bool ValidateState(const FWadSourceLightingMetadata& Metadata,const FWadSourceLightState& State,FString& Error);
    // One-time topology validation/copy; subsequent steps validate only live state/inputs.
    class AAORIGINAL_API FPreparedKernel
    {
    public:
        bool Initialize(const FWadSourceLightingMetadata& Metadata,FString& Error);
        bool Step(FWadSourceLightState& State,TConstArrayView<int32> DoorWeights,int32 Outside,
            TConstArrayView<int32> OwnerCenters,FString& Error) const;
    private:
        bool bReady=false;
        FWadSourceLightingMetadata Metadata;
    };
    AAORIGINAL_API bool IsIlluminatingType(int32 FullType);
    AAORIGINAL_API int32 SourceIllumination(int32 FullType, uint16 AccessoryData);
    AAORIGINAL_API bool IsAnimationDirective(int32 FullType);
    AAORIGINAL_API FWadSourceLightDirective DirectiveFor(int32 ThingId, int32 FullType, int32 Sector);
    AAORIGINAL_API bool ResolveAnimations(TConstArrayView<FWadSourceLightDirective> Directives, int32 SectorCount,
        TArray<FWadSourceLightDirective>& Result, FString& Error);
    AAORIGINAL_API bool Validate(const FWadSourceLightingMetadata& Metadata, FString& Error);
    // One active LIGHT.C recalculation: TWO passes, each followed by ordered
    // signed illumination/clamping. Input is live previous bytes, not baselines.
    // Persistent native overrides and the choice of update cadence are external.
    AAORIGINAL_API bool Recalculate(const FWadSourceLightingMetadata& Metadata, TConstArrayView<int32> Input,
        TConstArrayView<int32> DoorWeights, int32 Outside, TArray<int32>& Result, FString& Error);
    // Source MAP.C applies oscillation AFTER table/illumination. Caller owns live
    // phase persistence and fixed-clock policy; this kernel has no world/timer.
    AAORIGINAL_API bool Animate(const FWadSourceLightingMetadata& Metadata, uint16 DeltaTicks,
        TConstArrayView<int32> InputLights, TConstArrayView<int32> InputPhases,
        TArray<int32>& ResultLights, TArray<int32>& ResultPhases, FString& Error);
}
