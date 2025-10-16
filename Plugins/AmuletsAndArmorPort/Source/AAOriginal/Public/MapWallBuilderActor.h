#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MapWallBuilderActor.generated.h"

// Utility actor that can spawn simple wall meshes from the last loaded WAD geometry.
// Place in a level and click the button to build walls (Editor-only action via CallInEditor).
UCLASS(Blueprintable, hidecategories=(Actor, Input, Rendering, Replication, Collision, HLOD, WorldPartition, Cooking))
class AAORIGINAL_API AMapWallBuilderActor : public AActor
{
    GENERATED_BODY()
public:
    AMapWallBuilderActor();

    // Press in editor to generate walls from last-loaded geometry
    UFUNCTION(CallInEditor, Category="Wall Builder")
    void BuildWallsFromLastWad();

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    void ClearSpawned();

    UPROPERTY(EditAnywhere, Category="Wall Builder")
    float WallHeight = 300.0f; // default height in UU

    UPROPERTY(EditAnywhere, Category="Wall Builder")
    float WallThickness = 20.0f; // box thickness

    UPROPERTY(EditAnywhere, Category="Wall Builder")
    bool bClearPrevious = true;

    UPROPERTY(Transient)
    TArray<TWeakObjectPtr<AActor>> SpawnedActors;
};
