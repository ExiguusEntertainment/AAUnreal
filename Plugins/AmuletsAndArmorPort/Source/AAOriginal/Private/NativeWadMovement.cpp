#include "NativeWadMovement.h"

#include "Components/PrimitiveComponent.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"

FVector UNativeWadMovement::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
    FVector Result = Super::NewFallVelocity(InitialVelocity, Gravity, DeltaTime);
    if (const auto* Player = Cast<ANativeWadPlayer>(CharacterOwner); Player && Player->HasFeatherfall())
    {
        Result.Z = FMath::Max(Result.Z, -NativeWadRules::FeatherfallSpeed);
    }
    return Result;
}

bool UNativeWadMovement::ResolvePenetrationImpl(const FVector& Adjustment, const FHitResult& Hit, const FQuat& NewRotation)
{
    if (Super::ResolvePenetrationImpl(Adjustment, Hit, NewRotation))
    {
        return true;
    }
    if (!Hit.bStartPenetrating || !Cast<ANativeWadLevel>(Hit.GetActor()) || !UpdatedPrimitive)
    {
        return false;
    }

    // Use only the position from immediately before this movement update. A spawn or teleport can have a stale one.
    const FVector LastLocation = GetLastUpdateLocation();
    const FVector Retreat = LastLocation - UpdatedPrimitive->GetComponentLocation();
    if (Retreat.IsNearlyZero() || Retreat.SizeSquared() > FMath::Square(UpdatedPrimitive->Bounds.SphereRadius)
        || OverlapTest(LastLocation, NewRotation, UpdatedPrimitive->GetCollisionObjectType(),
            UpdatedPrimitive->GetCollisionShape(), GetOwner()))
    {
        return false;
    }

    const bool bMoved = MoveUpdatedComponent(Retreat, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
    bJustTeleported |= bMoved;
    return bMoved;
}
