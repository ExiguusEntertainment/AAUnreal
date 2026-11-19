#include "MapWallBuilderActor.h"
#include "DoomWadLoader.h"
#include "LegacyMapLoader.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/StaticMeshComponent.h"

AMapWallBuilderActor::AMapWallBuilderActor()
{
    PrimaryActorTick.bCanEverTick = false;
}

void AMapWallBuilderActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
}

void AMapWallBuilderActor::ClearSpawned()
{
    UWorld* World = GetWorld();
    if (!World) return;
    if (!bClearPrevious) return;

    for (TWeakObjectPtr<AActor>& Weak : SpawnedActors)
    {
        if (AActor* A = Weak.Get())
        {
            A->Destroy();
        }
    }
    SpawnedActors.Reset();
}

void AMapWallBuilderActor::BuildWallsFromLastWad()
{
    UWorld* World = GetWorld();
    if (!World) return;

    TArray<FVector2D> Verts2D;
    TArray<FIntPoint> Lines;

    bool bHaveGeom = UDoomWadLoader::GetLastWad2DGeometry(Verts2D, Lines);
    if (!bHaveGeom)
    {
        // Try legacy geometry as a fallback
        bHaveGeom = ULegacyMapLoader::GetLastLegacy2DGeometry(Verts2D, Lines);
    }

    if (!bHaveGeom)
    {
        UE_LOG(LogTemp, Warning, TEXT("No geometry available. Load a WAD (AA.LoadWad) or legacy map (AA.LoadLegacyMap) first."));
        return;
    }

    ClearSpawned();

    // Load engine cube mesh safely outside of constructors
    UStaticMesh* CubeMesh = FindObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh)
    {
        CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    }

    if (!CubeMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("Could not load basic cube mesh at /Engine/BasicShapes/Cube.Cube"));
        return;
    }

    FActorSpawnParameters SpawnParams; SpawnParams.Owner = this; SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    for (const FIntPoint& L : Lines)
    {
        if (!Verts2D.IsValidIndex(L.X) || !Verts2D.IsValidIndex(L.Y)) continue;
        const FVector2D A2 = Verts2D[L.X];
        const FVector2D B2 = Verts2D[L.Y];
        const FVector A3(A2.X, A2.Y, 0.0f);
        const FVector B3(B2.X, B2.Y, 0.0f);
        const FVector Mid = (A3 + B3) * 0.5f;
        const float Length = (B3 - A3).Size();
        if (Length <= KINDA_SMALL_NUMBER) continue;

        AStaticMeshActor* SMA = World->SpawnActor<AStaticMeshActor>(Mid + FVector(0,0,WallHeight*0.5f), FRotator::ZeroRotator, SpawnParams);
        if (!SMA) continue;
        UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent();
        SMC->SetStaticMesh(CubeMesh);
        SMC->SetMobility(EComponentMobility::Movable);
        SMC->SetCastShadow(true);

        // Scale cube to a wall slab: cube default is 100x100x100; scale to Length x Thickness x Height
        const FVector Scale(Length / 100.0f, WallThickness / 100.0f, WallHeight / 100.0f);
        SMC->SetRelativeScale3D(Scale);

        // Orient along the line
        const FVector Dir = (B3 - A3).GetSafeNormal();
        const float Yaw = FMath::Atan2(Dir.Y, Dir.X) * (180.0f / PI);
        SMA->SetActorRotation(FRotator(0.0f, Yaw, 0.0f));

        SpawnedActors.Add(SMA);
    }

    UE_LOG(LogTemp, Display, TEXT("Spawned %d wall segments."), SpawnedActors.Num());
}
