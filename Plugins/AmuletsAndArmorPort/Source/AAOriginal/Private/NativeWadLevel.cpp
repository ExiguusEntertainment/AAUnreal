#include "NativeWadLevel.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInterface.h"
#include "NativeWadPlayer.h"

namespace
{
    const FName SurfaceTexture(TEXT("SurfaceTexture"));
    constexpr int32 UVTransformIndex = 0;
    constexpr int32 AmbientIndex = 4;
    constexpr double WallMeshSize = 128.0;

    uint64 BandKey(int32 Side, EWadWallBand Band)
    {
        return (uint64(Side) << 2) | uint64(Band);
    }

    FVector2D GetTextureSize(const UStaticMeshComponent& Mesh)
    {
        UTexture* Texture = nullptr;
        if (UMaterialInterface* Material = Mesh.GetMaterial(0);
            Material && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(SurfaceTexture), Texture))
        {
            if (const UTexture2D* Image = Cast<UTexture2D>(Texture))
            {
#if WITH_EDITORONLY_DATA
                if (Image->Source.IsValid())
                {
                    return FVector2D(Image->Source.GetSizeX(), Image->Source.GetSizeY());
                }
#endif
                if (Image->GetSizeX() > 0 && Image->GetSizeY() > 0)
                {
                    return FVector2D(Image->GetSizeX(), Image->GetSizeY());
                }
            }
        }
        return FVector2D(64.0, 64.0);
    }

    void SetUV(UStaticMeshComponent& Mesh, const FVector4& UV, bool bPersist)
    {
        if (bPersist)
        {
            Mesh.SetDefaultCustomPrimitiveDataVector4(UVTransformIndex, UV);
        }
        else
        {
            Mesh.SetCustomPrimitiveDataVector4(UVTransformIndex, UV);
        }
    }

    void CopyTextureName(const FString& Name, char (&Out)[8])
    {
        const auto Ansi = StringCast<ANSICHAR>(*Name);
        FMemory::Memcpy(Out, Ansi.Get(), FMath::Min(Ansi.Length(), int32(sizeof(Out))));
    }
}

ANativeWadLevel::ANativeWadLevel()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("MapOrigin")));
    GetRootComponent()->SetMobility(EComponentMobility::Static);
}

bool ANativeWadLevel::UseDoor(int32 Sector)
{
    return UseDoorForPlayer(Sector, nullptr);
}

bool ANativeWadLevel::UseDoorForPlayer(int32 Sector, ANativeWadPlayer* Player)
{
    InitializeTopology();
    const int32* Index = DoorBySector.Find(Sector);
    FVector2D DoorHeights;
    if (!Index || !GetSectorSurfaceHeights(Sector, DoorHeights)
        || !GetWorld() || !GetWorld()->IsGameWorld() || DoorHeights.X >= Doors[*Index].OpenZ)
    {
        return false;
    }
    const FAAWadNativeDoor& Door = Doors[*Index];
    if (ScriptLockedDoors.Contains(Sector))
    {
        if (Player)
        {
            Player->ShowInteractionMessage(TEXT("Door is locked."));
        }
        return false;
    }
    if (GetDoorLockStrength(Sector) > 0)
    {
        if (!Player)
        {
            return false;
        }
        const bool bNeedsItem = Door.RequiredItem > 0 && !UnlockedItemDoors.Contains(Sector);
        if (!bNeedsItem || !Player->ConsumeItem(Door.RequiredItem))
        {
            Player->ShowInteractionMessage(bNeedsItem ? TEXT("Door requires key to be opened.") : TEXT("Door is locked."));
            return false;
        }
        UnlockedItemDoors.Add(Sector);
        DoorLockStrengths.Add(Sector, 0);
        Player->ShowInteractionMessage(TEXT("Door took item."));
    }

    FActiveDoor& State = ActiveDoors.FindOrAdd(*Index);
    if (State.Phase == EDoorPhase::Holding || DoorHeights.Y >= Doors[*Index].OpenZ)
    {
        State = { EDoorPhase::Holding, DoorHeights.Y, 0.0 };
    }
    else if (State.Phase == EDoorPhase::Closing || State.Elapsed == 0.0)
    {
        State = { EDoorPhase::Opening, DoorHeights.Y, 0.0 };
    }
    SetActorTickEnabled(true);
    return true;
}

int32 ANativeWadLevel::GetDoorLockStrength(int32 Sector)
{
    InitializeTopology();
    const int32* Index = DoorBySector.Find(Sector);
    if (!Index || ScriptLockedDoors.Contains(Sector)) { return NativeWadRules::AbsoluteDoorLock; }
    if (const int32* Strength = DoorLockStrengths.Find(Sector)) { return *Strength; }
    if (UnlockedItemDoors.Contains(Sector)) { return 0; }
    const FAAWadNativeDoor& Door = Doors[*Index];
    // Unrefreshed native maps retain the earlier conservative locked behavior.
    return Door.LockStrength >= 0 ? Door.LockStrength : Door.bRequiresUnlock ? NativeWadRules::AbsoluteDoorLock : 0;
}

bool ANativeWadLevel::ModifyDoorLocksNear(const FVector2D& Impact, bool bLock, double Radius)
{
    if (!FMath::IsFinite(Radius) || Radius <= 0.0) { return false; }
    InitializeTopology();
    const FVector Local = GetActorTransform().InverseTransformPosition(FVector(Impact, GetActorLocation().Z));
    const FBox Box(FVector(Local.X - Radius, Local.Y - Radius, -1), FVector(Local.X + Radius, Local.Y + Radius, 1));
    TSet<int32> Nearby;
    const int32 CenterSector = FindWadSector(Impact);
    if (DoorBySector.Contains(CenterSector)) { Nearby.Add(CenterSector); }
    for (const FAAWadNativeLine& Line : Lines)
    {
        const FVector Start(FVector2D(Vertices[Line.Endpoints.X]), 0);
        const FVector End(FVector2D(Vertices[Line.Endpoints.Y]), 0);
        if (!FMath::LineBoxIntersection(Box, Start, End, End - Start)) { continue; }
        for (const int32 Side : {Line.Sides.X, Line.Sides.Y})
        {
            const int32 Sector = Sides.IsValidIndex(Side) ? Sides[Side].Sector : INDEX_NONE;
            if (DoorBySector.Contains(Sector)) { Nearby.Add(Sector); }
        }
    }
    bool bChanged = false;
    for (const int32 Sector : Nearby)
    {
        const int32 Before = GetDoorLockStrength(Sector);
        const int32 After = NativeWadRules::AdjustDoorLockStrength(Before, bLock);
        if (After != Before) { DoorLockStrengths.Add(Sector, After); bChanged = true; }
    }
    return bChanged;
}

void ANativeWadLevel::LockScriptDoorsNear(const FVector2D& Impact)
{
    // Script missile 4012 uses SERVERSH.C's ordinary +10 lock in a radius-100 XY box.
    ModifyDoorLocksNear(Impact, true, 100.0);
}

bool ANativeWadLevel::UseDoorFromView(const FVector& Position, const FVector& Direction, double Reach, ANativeWadPlayer* Player)
{
    if (!GetWorld() || !GetWorld()->IsGameWorld() || !FMath::IsFinite(Reach) || Reach <= 0.0)
    {
        return false;
    }
    InitializeTopology();
    const FTransform& MapTransform = GetActorTransform();
    const FVector End = Position + Direction.GetSafeNormal() * Reach;
    const FVector StartLocal = MapTransform.InverseTransformPosition(Position);
    const FVector Delta = MapTransform.InverseTransformPosition(End) - StartLocal;
    const FVector2D Start(StartLocal.X, StartLocal.Y);
    const FVector2D Ray(Delta.X, Delta.Y);
    double Closest = 1.0;
    int32 ClosestDoor = INDEX_NONE;
    for (const FAAWadNativeDoor& Door : Doors)
    {
        if (!LinesBySector.IsValidIndex(Door.Sector))
        {
            continue;
        }
        for (const int32 LineIndex : LinesBySector[Door.Sector])
        {
            const FAAWadNativeLine& Line = Lines[LineIndex];
            const int32 Front = WallTopology.GetSectorForSide(Line.Sides.X);
            const int32 Back = WallTopology.GetSectorForSide(Line.Sides.Y);
            const int32 Other = Front == Door.Sector ? Back : Front;
            if ((Line.Flags & 0x0004) == 0 || Other == Door.Sector || !Heights.IsValidIndex(Other)
                || !Vertices.IsValidIndex(Line.Endpoints.X) || !Vertices.IsValidIndex(Line.Endpoints.Y))
            {
                continue;
            }
            const FVector2D A(Vertices[Line.Endpoints.X]);
            const FVector2D Edge = FVector2D(Vertices[Line.Endpoints.Y]) - A;
            const double Denominator = FVector2D::CrossProduct(Ray, Edge);
            if (FMath::IsNearlyZero(Denominator))
            {
                continue;
            }
            const double AlongRay = FVector2D::CrossProduct(A - Start, Edge) / Denominator;
            const double AlongEdge = FVector2D::CrossProduct(A - Start, Ray) / Denominator;
            const double Z = StartLocal.Z + Delta.Z * AlongRay;
            if (AlongRay >= 0.0 && AlongRay < Closest && AlongEdge >= 0.0 && AlongEdge <= 1.0
                && Z >= Heights[Other].Floor && Z <= Heights[Other].Ceiling && Z >= Door.ClosedZ)
            {
                Closest = AlongRay;
                ClosestDoor = Door.Sector;
            }
        }
    }
    if (ClosestDoor == INDEX_NONE)
    {
        return false;
    }
    // Test the path up to the door boundary, allowing for its thin collision box.
    const FVector BeforeDoor = FMath::Lerp(Position, End, FMath::Max(0.0, Closest - 0.5 / Reach));
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(UseNativeWadDoor), true, Player);
    Query.bFindInitialOverlaps = false;
    return !GetWorld()->LineTraceSingleByChannel(Hit, Position, BeforeDoor, ECC_Visibility, Query) && UseDoorForPlayer(ClosestDoor, Player);
}

void ANativeWadLevel::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bRunDayClock)
    {
        // The source phase wraps after 524,288 simulation ticks at 70 Hz.
        TimeOfDayHours = FMath::Fmod(TimeOfDayHours + DeltaSeconds * (24.0 * 70.0 / 524288.0), 24.0);
        UpdateDaySky();
    }
    for (auto It = ActiveDoors.CreateIterator(); It; ++It)
    {
        if (UpdateDoor(Doors[It.Key()], It.Value(), DeltaSeconds))
        {
            It.RemoveCurrent();
        }
    }
    TickActivitySlides(DeltaSeconds);
    TickPickupGenerators(DeltaSeconds);
    TickCreatureGenerators(DeltaSeconds);
    TickSurfaceScroll(DeltaSeconds);
    TickAmbientEmitters(DeltaSeconds);
    if (ActiveDoors.IsEmpty() && ActivitySlides.IsEmpty() && PendingPickupGenerators.IsEmpty() && !bRunDayClock
        && ScrollingSectors.IsEmpty() && ScrollingSides.IsEmpty() && AmbientEmitters.IsEmpty() && !HasPendingCreatureGenerators())
    {
        SetActorTickEnabled(false);
    }
}

int32 ANativeWadLevel::FindLevelLinkAt(const FVector2D& Position) const
{
    const FVector2D Local(GetActorTransform().InverseTransformPosition(FVector(Position, GetActorLocation().Z)));
    for (int32 Index = 0; Index < LevelLinks.Num(); ++Index)
    {
        if (ContainsSectorPoint(LevelLinks[Index].Sector, Local))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

bool ANativeWadLevel::ContainsSectorPoint(int32 Sector, const FVector2D& LocalPosition) const
{
    if (!LinesBySector.IsValidIndex(Sector))
    {
        return false;
    }
    const auto CrossesRay = [&LocalPosition](const auto& A, const auto& B)
    {
        return (A.Y > LocalPosition.Y) != (B.Y > LocalPosition.Y)
            && LocalPosition.X < A.X + (LocalPosition.Y - A.Y) * (B.X - A.X) / (B.Y - A.Y);
    };
    const auto ContainsRepairedPoint = [&CrossesRay](const auto& Points)
    {
        bool bInside = false;
        for (int32 Index = 0; Index + 1 < Points.Num(); Index += 2)
        {
            bInside ^= CrossesRay(Points[Index], Points[Index + 1]);
        }
        return bInside;
    };
    const FAAWadNativeSector& Entry = Sectors[Sector];
    if (!Entry.RepairedBoundaryPoints.IsEmpty())
    {
        return ContainsRepairedPoint(Entry.RepairedBoundaryPoints);
    }
    if (!Entry.RepairedBoundary.IsEmpty())
    {
        return ContainsRepairedPoint(Entry.RepairedBoundary);
    }
    bool bInside = false;
    for (const int32 LineIndex : LinesBySector[Sector])
    {
        const FWadLineDef& Line = WallTopology.Lines[LineIndex];
        if (WallTopology.GetSectorForSide(Line.Side[0]) == WallTopology.GetSectorForSide(Line.Side[1]))
        {
            continue;
        }
        const FIntPoint A = Vertices[Line.From];
        const FIntPoint B = Vertices[Line.To];
        bInside ^= CrossesRay(A, B);
    }
    return bInside;
}

int32 ANativeWadLevel::FindWadSector(const FVector2D& Position) const
{
    const FVector2D Local(GetActorTransform().InverseTransformPosition(FVector(Position, GetActorLocation().Z)));
    for (int32 Sector = 0; Sector < Sectors.Num(); ++Sector)
    {
        if (ContainsSectorPoint(Sector, Local))
        {
            return Sector;
        }
    }
    return INDEX_NONE;
}

double ANativeWadLevel::GetWalkingHeight(int32 Sector) const
{
    if (const FAAWadNativeSector* Entry = Sectors.IsValidIndex(Sector) ? &Sectors[Sector] : nullptr; Entry && Entry->Floor)
    {
        return Entry->Floor->GetRelativeLocation().Z - Entry->FloorDepth;
    }
    return 0.0;
}

void ANativeWadLevel::InitializeWaterSurfaces()
{
    for (const FAAWadNativeSector& Sector : Sectors)
    {
        if ((Sector.bWater || Sector.bLava) && Sector.Floor && Sector.WalkingFloor)
        {
            Sector.Floor->SetCollisionObjectType(Sector.bLava ? LavaSurfaceChannel : WaterSurfaceChannel);
            Sector.Floor->SetCollisionResponseToAllChannels(ECR_Ignore);
            Sector.Floor->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
            Sector.Floor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        }
    }
}

TOptional<double> ANativeWadLevel::FindWaterSurfaceZ(const FVector& Position) const
{
    const int32 Index = FindWadSector(FVector2D(Position));
    if (const FAAWadNativeSector* Sector = Sectors.IsValidIndex(Index) ? &Sectors[Index] : nullptr;
        Sector && Sector->bWater && Sector->Floor && Sector->WalkingFloor)
    {
        return Sector->Floor->GetComponentLocation().Z;
    }
    return {};
}

TOptional<double> ANativeWadLevel::FindLavaSurfaceZ(const FVector& Position) const
{
    const int32 Index = FindWadSector(FVector2D(GetActorTransform().InverseTransformPosition(Position)));
    if (const FAAWadNativeSector* Sector = Sectors.IsValidIndex(Index) ? &Sectors[Index] : nullptr;
        Sector && Sector->bLava && Sector->Floor && Sector->WalkingFloor)
    {
        return Sector->Floor->GetComponentLocation().Z;
    }
    return {};
}

FVector2D ANativeWadLevel::GetCurrentInput(const FVector& Feet, bool bWaterWalking) const
{
    const FVector Local = GetActorTransform().InverseTransformPosition(Feet);
    for (int32 Sector : FlowSectors)
    {
        const FAAWadNativeSector& Entry = Sectors[Sector];
        if ((!Entry.bWater || (!bWaterWalking && Local.Z <= Entry.Floor->GetRelativeLocation().Z + 2.5))
            && ContainsSectorPoint(Sector, FVector2D(Local)))
        {
            // Keep current strength relative to the movement speed of the native first-person character.
            constexpr double InputPerSourceImpulse = 0.0475;
            return FVector2D(GetActorTransform().TransformVectorNoScale(FVector(Entry.FlowImpulse, 0))) * InputPerSourceImpulse;
        }
    }
    return FVector2D::ZeroVector;
}

void ANativeWadLevel::TickSurfaceScroll(float DeltaSeconds)
{
    SurfaceScrollSeconds += DeltaSeconds;
    for (int32 Index : ScrollingSectors)
    {
        SetSectorTextureScrollTexels(Index, false, SurfaceScrollSeconds * Sectors[Index].FloorScrollRate);
    }
    for (int32 Index : ScrollingSides)
    {
        for (EWadWallBand Band : {EWadWallBand::Lower, EWadWallBand::Middle, EWadWallBand::Upper})
        {
            SetWallTextureScrollTexels(Index, Band, SurfaceScrollSeconds * Sides[Index].ScrollRate);
        }
    }
}

void ANativeWadLevel::TeleportActivityPlayer(ANativeWadPlayer& Player, const FVector2D& Destination)
{
    // The original teleport command preserves yaw and places the player on the destination floor.
    const FVector WorldXY = GetActorTransform().TransformPosition(FVector(Destination, 0));
    const int32 Sector = FindWadSector(FVector2D(WorldXY));
    if (Sector == INDEX_NONE)
    {
        return;
    }
    const FVector Start = GetActorTransform().TransformPosition(FVector(Destination,
        GetWalkingHeight(Sector) + ANativeWadPlayer::CapsuleHalfHeight + 2.0));
    if (Player.TeleportTo(Start, Player.GetActorRotation()))
    {
        Player.GetCharacterMovement()->StopMovementImmediately();
        UE_LOG(LogTemp, Display, TEXT("WAD activity teleported the player into sector %d at %s."), Sector, *Start.ToString());
    }
}

bool ANativeWadLevel::IsDoorObstructed(int32 Sector, double FromZ, double ToZ) const
{
    if (ToZ >= FromZ)
    {
        return false;
    }
    UStaticMeshComponent* Ceiling = Sectors[Sector].Ceiling;
    const FVector Start = Ceiling->GetComponentLocation();
    const FVector End = Start + GetActorTransform().TransformVector(FVector(0.0, 0.0, ToZ - FromZ));
    // Sector geometry belongs to this actor. Only blockers from outside the map actor can reverse the door.
    FComponentQueryParams Query(SCENE_QUERY_STAT(WadDoorObstruction), this);
    TArray<FHitResult> Hits;
    return GetWorld()->ComponentSweepMultiByChannel(Hits, Ceiling, Start, End,
        Ceiling->GetComponentQuat(), ECC_Pawn, Query);
}

bool ANativeWadLevel::UpdateDoor(const FAAWadNativeDoor& Door, FActiveDoor& State, double DeltaSeconds)
{
    FVector2D DoorHeights;
    if (!GetSectorSurfaceHeights(Door.Sector, DoorHeights))
    {
        return true;
    }

    // Carry any extra frame time across the normal phase boundaries.
    while (DeltaSeconds > UE_SMALL_NUMBER)
    {
        const bool bHolding = State.Phase == EDoorPhase::Holding;
        const double Duration = bHolding ? Door.HoldSeconds : Door.TravelSeconds;
        const double Step = FMath::Min(DeltaSeconds, Duration - State.Elapsed);
        State.Elapsed += Step;
        DeltaSeconds -= Step;
        if (!bHolding)
        {
            const bool bClosing = State.Phase == EDoorPhase::Closing;
            const double Target = bClosing ? FMath::Max(double(Door.ClosedZ), DoorHeights.X) : Door.OpenZ;
            const double Next = FMath::Lerp(State.StartZ, Target, State.Elapsed / Duration);
            if (bClosing && IsDoorObstructed(Door.Sector, DoorHeights.Y, Next))
            {
                State = { EDoorPhase::Opening, DoorHeights.Y, 0.0 };
                return false;
            }
            SetSectorHeight(Door.Sector, Next, true);
            DoorHeights.Y = Next;
        }
        if (State.Elapsed < Duration)
        {
            break;
        }
        if (State.Phase == EDoorPhase::Closing)
        {
            return true;
        }
        State = { bHolding ? EDoorPhase::Closing : EDoorPhase::Holding, DoorHeights.Y, 0.0 };
    }
    return false;
}

bool ANativeWadLevel::AttachActorToWadFloor(AActor& Actor, int32 Sector) const
{
    return Sectors.IsValidIndex(Sector) && Sectors[Sector].Floor
        && Actor.AttachToComponent(Sectors[Sector].Floor, FAttachmentTransformRules::KeepWorldTransform);
}

bool ANativeWadLevel::GetSectorSurfaceHeights(int32 Sector, FVector2D& OutHeights) const
{
    if (!Sectors.IsValidIndex(Sector) || !Sectors[Sector].Floor || !Sectors[Sector].Ceiling)
    {
        return false;
    }
    OutHeights = FVector2D(Sectors[Sector].Floor->GetRelativeLocation().Z, Sectors[Sector].Ceiling->GetRelativeLocation().Z);
    return true;
}

void ANativeWadLevel::InitializeTopology()
{
    if (!Heights.IsEmpty())
    {
        return;
    }
    WallTopology.Sectors.SetNumZeroed(Sectors.Num());
    Heights.SetNum(Sectors.Num());
    LinesBySector.SetNum(Sectors.Num());
    LightingNeighbors.SetNum(Sectors.Num());
    for (int32 Index = 0; Index < Doors.Num(); ++Index)
    {
        DoorBySector.Add(Doors[Index].Sector, Index);
    }
    for (int32 Index = 0; Index < Sectors.Num(); ++Index)
    {
        FVector2D Range;
        if (GetSectorSurfaceHeights(Index, Range))
        {
            Heights[Index] = { Range.X, Range.Y };
        }
    }
    WallTopology.Vertices.Reserve(Vertices.Num());
    for (const FIntPoint& Vertex : Vertices)
    {
        WallTopology.Vertices.Add({int16(Vertex.X), int16(Vertex.Y)});
    }
    for (const FAAWadNativeSide& Side : Sides)
    {
        FWadSideDef& Record = WallTopology.Sides.AddZeroed_GetRef();
        Record.Sector = int16(Side.Sector);
        Record.TexXOffset = int16(Side.TextureOffset.X);
        Record.TexYOffset = int16(Side.TextureOffset.Y);
        if (Side.Textures.Num() == 3)
        {
            CopyTextureName(Side.Textures[uint8(EWadWallBand::Lower)], Record.LowerTex);
            CopyTextureName(Side.Textures[uint8(EWadWallBand::Middle)], Record.MainTex);
            CopyTextureName(Side.Textures[uint8(EWadWallBand::Upper)], Record.UpperTex);
        }
    }
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const FAAWadNativeLine& Line = Lines[Index];
        FWadLineDef& Record = WallTopology.Lines.AddZeroed_GetRef();
        Record.From = int16(Line.Endpoints.X);
        Record.To = int16(Line.Endpoints.Y);
        Record.Flags = Line.Flags;
        Record.Side[0] = int16(Line.Sides.X);
        Record.Side[1] = int16(Line.Sides.Y);
        const int32 Front = WallTopology.GetSectorForSide(Record.Side[0]);
        const int32 Back = WallTopology.GetSectorForSide(Record.Side[1]);
        if ((Line.Flags & 0x0004) && Front != Back && LightingNeighbors.IsValidIndex(Front) && LightingNeighbors.IsValidIndex(Back))
        {
            LightingNeighbors[Front].AddUnique(Back);
            LightingNeighbors[Back].AddUnique(Front);
        }
        for (const int32 Side : Record.Side)
        {
            const int32 Sector = WallTopology.GetSectorForSide(Side);
            if (LinesBySector.IsValidIndex(Sector))
            {
                LinesBySector[Sector].AddUnique(Index);
            }
        }
    }
    for (TArray<int32>& Neighbors : LightingNeighbors)
    {
        Neighbors.Sort();
    }
}

TArray<FString> ANativeWadLevel::GetWadSideTextureNames(int32 Side) const
{
    return Sides.IsValidIndex(Side) ? Sides[Side].Textures : TArray<FString>();
}

bool ANativeWadLevel::GetWallLine(int32 Line, FWadWallLine& Wall)
{
    InitializeTopology();
    return BuildWadWallLine(WallTopology, Line, Wall, Heights);
}

void ANativeWadLevel::SetSectorFloorZ(int32 Sector, float Z)
{
    SetSectorHeight(Sector, Z, false);
}

void ANativeWadLevel::SetSectorCeilingZ(int32 Sector, float Z)
{
    SetSectorHeight(Sector, Z, true);
}

void ANativeWadLevel::SetSectorHeight(int32 Sector, double Z, bool bCeiling)
{
    if (!Sectors.IsValidIndex(Sector) || !FMath::IsFinite(Z))
    {
        return;
    }
    UStaticMeshComponent* Mesh = bCeiling ? Sectors[Sector].Ceiling : Sectors[Sector].Floor;
    if (!Mesh)
    {
        return;
    }
    InitializeTopology();
    FWadSectorHeightRange& Range = Heights[Sector];
    const double NewHeight = bCeiling ? FMath::Max(Z, Range.Floor) : FMath::Min(Z, Range.Ceiling);
    double& Height = bCeiling ? Range.Ceiling : Range.Floor;
    if (Height == NewHeight)
    {
        return;
    }
    Height = NewHeight;
    FVector Location = Mesh->GetRelativeLocation();
    Location.Z = Height;
    Mesh->SetRelativeLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
    if (!bCeiling && Sectors[Sector].WalkingFloor)
    {
        Location.Z -= Sectors[Sector].FloorDepth;
        Sectors[Sector].WalkingFloor->SetRelativeLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
    }
    for (const int32 Line : LinesBySector[Sector])
    {
        UpdateLine(Line, false);
    }
    if (bCeiling && bUseSectorLightingRules && DoorBySector.Contains(Sector))
    {
        RefreshSectorLights();
    }
}

void ANativeWadLevel::ApplyLight(UStaticMeshComponent& Mesh, int32 Light, bool bPersist) const
{
    const float Baseline = SectorAmbientStrength * FMath::Lerp(0.015f, 0.65f, float(FMath::Clamp(Light, 0, 255)) / 255.0f);
    if (bPersist)
    {
        Mesh.SetDefaultCustomPrimitiveDataFloat(AmbientIndex, Baseline);
    }
    else
    {
        Mesh.SetCustomPrimitiveDataFloat(AmbientIndex, Baseline);
    }
}

bool ANativeWadLevel::SetSectorLightLevel(int32 Sector, int32 Light)
{
    if (!Sectors.IsValidIndex(Sector) || !Sectors[Sector].Floor || !Sectors[Sector].Ceiling)
    {
        return false;
    }
    const int32 Value = FMath::Clamp(Light, 0, 255);
    LightOverrides.Add(Sector, Value);
    ApplySectorLight(Sector, Value);
    return true;
}

void ANativeWadLevel::ApplySectorLight(int32 Sector, int32 Light)
{
    InitializeTopology();
    if (AppliedLights.IsEmpty())
    {
        AppliedLights.Init(INDEX_NONE, Sectors.Num());
    }
    if (AppliedLights[Sector] == Light)
    {
        return;
    }
    AppliedLights[Sector] = Light;
    const FAAWadNativeSector& Entry = Sectors[Sector];
    if (Entry.Floor && Entry.Ceiling)
    {
        ApplyLight(*Entry.Floor, Light, false);
        ApplyLight(*Entry.Ceiling, Light, false);
    }
    for (const int32 Line : LinesBySector[Sector])
    {
        for (const int32 Side : { Lines[Line].Sides.X, Lines[Line].Sides.Y })
        {
            if (Sides.IsValidIndex(Side) && Sides[Side].Sector == Sector)
            {
                for (UStaticMeshComponent* Mesh : Sides[Side].Bands)
                {
                    if (Mesh)
                    {
                        ApplyLight(*Mesh, Light, false);
                    }
                }
            }
        }
    }
}

bool ANativeWadLevel::SetSectorTextureScrollTexels(int32 Sector, bool bCeiling, FVector2D Offset)
{
    if (!Sectors.IsValidIndex(Sector) || Offset.ContainsNaN())
    {
        return false;
    }
    UStaticMeshComponent* Mesh = bCeiling ? Sectors[Sector].Ceiling : Sectors[Sector].Floor;
    if (!Mesh || !Mesh->IsVisible())
    {
        return false;
    }
    const FVector2D Size = GetTextureSize(*Mesh);
    SetUV(*Mesh, FVector4(1.0, 1.0, -FMath::Fmod(Offset.X, Size.X) / Size.X,
        -FMath::Fmod(Offset.Y, Size.Y) / Size.Y), false);
    return true;
}

bool ANativeWadLevel::SetWallTextureScrollTexels(int32 Side, EWadWallBand Band, FVector2D Offset)
{
    const int32 Index = uint8(Band);
    if (!Sides.IsValidIndex(Side) || !Sides[Side].Bands.IsValidIndex(Index) || !Sides[Side].Bands[Index] || Offset.ContainsNaN())
    {
        return false;
    }
    const uint64 Key = BandKey(Side, Band);
    const FVector2D Previous = WallScroll.FindRef(Key);
    WallScroll.Add(Key, Offset);
    UStaticMeshComponent& Mesh = *Sides[Side].Bands[Index];
    const TArray<float>& UV = Mesh.GetCustomPrimitiveData().Data;
    if (Mesh.IsVisible() && UV.IsValidIndex(UVTransformIndex + 3))
    {
        const FVector2D Size = GetTextureSize(Mesh);
        const FVector2D Shift(
            (FMath::Fmod(Offset.X, Size.X) - FMath::Fmod(Previous.X, Size.X)) / Size.X,
            (FMath::Fmod(Offset.Y, Size.Y) - FMath::Fmod(Previous.Y, Size.Y)) / Size.Y);
        Mesh.SetCustomPrimitiveDataVector2(UVTransformIndex + 2, FVector2D(UV[UVTransformIndex + 2], UV[UVTransformIndex + 3]) + Shift);
    }
    return true;
}

void ANativeWadLevel::UpdateLine(int32 Index, bool bPersist)
{
    FWadWallLine Wall;
    if (!GetWallLine(Index, Wall))
    {
        return;
    }
    FAAWadNativeLine& Line = Lines[Index];
    for (const int32 Side : { Line.Sides.X, Line.Sides.Y })
    {
        if (!Sides.IsValidIndex(Side))
        {
            continue;
        }
        for (int32 Band = 0; Band < Sides[Side].Bands.Num(); ++Band)
        {
            UStaticMeshComponent* Mesh = Sides[Side].Bands[Band];
            if (Mesh && Mesh->IsVisible() && !Wall.Surfaces.ContainsByPredicate([Side, Band](const FWadWallSurface& Surface)
                { return Surface.SideIndex == Side && uint8(Surface.Band) == Band; }))
            {
                Mesh->SetVisibility(false);
            }
        }
    }
    for (const FWadWallSurface& Surface : Wall.Surfaces)
    {
        const FAAWadNativeSide& Side = Sides[Surface.SideIndex];
        UStaticMeshComponent* Mesh = Side.Bands[uint8(Surface.Band)];
        if (!Mesh)
        {
            continue;
        }
        const FVector2D From = Surface.bBack ? Wall.To : Wall.From;
        const FVector2D Edge = (Surface.bBack ? Wall.From : Wall.To) - From;
        const double Length = Edge.Size();
        const double Height = Surface.Top - Surface.Bottom;
        const FVector2D Size = GetTextureSize(*Mesh);
        const FVector2D Scroll = WallScroll.FindRef(BandKey(Surface.SideIndex, Surface.Band));
        Mesh->SetRelativeTransform(FTransform(FVector(Edge, 0.0).Rotation(), FVector(From, Surface.Bottom),
            FVector(Length / WallMeshSize, 1.0, Height / WallMeshSize)));
        SetUV(*Mesh, FVector4(Length / Size.X, Height / Size.Y,
            (Side.TextureOffset.X + FMath::Fmod(Scroll.X, Size.X)) / Size.X,
            (Side.TextureOffset.Y + Surface.VOrigin - Surface.Bottom + FMath::Fmod(Scroll.Y, Size.Y)) / Size.Y), bPersist);
        Mesh->SetVisibility(true);
    }

    const FVector2D Edge = Wall.To - Wall.From;
    const FVector2D Middle = (Wall.To + Wall.From) * 0.5;
    const FRotator Rotation = FVector(Edge, 0.0).Rotation();
    const int32 Front = WallTopology.GetSectorForSide(Line.Sides.X);
    const int32 Back = WallTopology.GetSectorForSide(Line.Sides.Y);
    const int32 FrontDepth = Sectors.IsValidIndex(Front) ? Sectors[Front].FloorDepth : 0;
    const int32 BackDepth = Sectors.IsValidIndex(Back) ? Sectors[Back].FloorDepth : 0;
    if (FrontDepth || BackDepth)
    {
        TArray<FWadSectorHeightRange> CollisionHeights = Heights;
        if (FrontDepth)
        {
            CollisionHeights[Front].Floor -= FrontDepth;
        }
        if (BackDepth && Back != Front)
        {
            CollisionHeights[Back].Floor -= BackDepth;
        }
        FWadWallLine CollisionWall;
        if (BuildWadWallLine(WallTopology, Index, CollisionWall, CollisionHeights))
        {
            Wall.Collision = MoveTemp(CollisionWall.Collision);
        }
    }
    for (int32 SpanIndex = 0; SpanIndex < Wall.Collision.Num(); ++SpanIndex)
    {
        if (!Line.Collision.IsValidIndex(SpanIndex))
        {
            UBoxComponent* NewBox = NewObject<UBoxComponent>(this, NAME_None, RF_Transactional);
            NewBox->SetMobility(EComponentMobility::Movable);
            NewBox->SetupAttachment(GetRootComponent());
            NewBox->SetCanEverAffectNavigation(false);
            NewBox->SetGenerateOverlapEvents(false);
            NewBox->SetCollisionObjectType(ECC_WorldStatic);
            NewBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            AddInstanceComponent(NewBox);
            NewBox->RegisterComponent();
            Line.Collision.Add(NewBox);
        }
        const FWadWallCollision& Span = Wall.Collision[SpanIndex];
        UBoxComponent& Box = *Line.Collision[SpanIndex];
        Box.SetBoxExtent(FVector(Edge.Size() * 0.5, 0.25, (Span.Top - Span.Bottom) * 0.5), false);
        Box.SetRelativeLocationAndRotation(FVector(Middle, (Span.Bottom + Span.Top) * 0.5), Rotation);
        // Creature-only blockers stay available for a game-specific collision channel.
        Box.SetCollisionResponseToAllChannels(Span.bCreatureOnly ? ECR_Ignore : ECR_Block);
        Box.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    }
    for (int32 SpanIndex = Wall.Collision.Num(); SpanIndex < Line.Collision.Num(); ++SpanIndex)
    {
        Line.Collision[SpanIndex]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }
}
