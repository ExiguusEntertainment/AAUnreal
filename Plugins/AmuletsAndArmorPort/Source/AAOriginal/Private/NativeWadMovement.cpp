#include "NativeWadMovement.h"

#include "Components/PrimitiveComponent.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"

void UNativeWadMovement::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
    const auto* Player = Cast<ANativeWadPlayer>(CharacterOwner);
    if (!Player || !HasValidData() || !IsMovingOnGround() || DeltaTime < MIN_TICK_TIME || HasAnimRootMotion()
        || (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy && !bWasSimulatingRootMotion))
    { Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration); return; }
    const int32 SourceFriction = Player->GetWadGroundFriction(CurrentFloor.HitResult.GetComponent());
    const double Retention = NativeWadRules::SourceGroundRetention(SourceFriction, DeltaTime);
    const double InputFactor = SourceFriction / double(NativeWadRules::SourceNormalFriction)
        * NativeWadRules::SourceGroundInputFactor(SourceFriction, DeltaTime);
    const FVector SavedAcceleration = Acceleration;
    const float SavedBrakingFriction = BrakingFriction;
    Velocity.X *= Retention;
    Velocity.Y *= Retention;
    Acceleration.X *= InputFactor;
    Acceleration.Y *= InputFactor;
    BrakingFriction = 0.0f;
    // No additional UE steering drag or constant braking deceleration. Keep its
    // collision movement, speed caps and requested movement machinery. This is a
    // continuous approximation, not original fixed-point/frame-order parity.
    Super::CalcVelocity(DeltaTime, 0.0f, false, 0.0f);
    BrakingFriction = SavedBrakingFriction;
    Acceleration = SavedAcceleration;
}

float UNativeWadMovement::GetGravityZ() const
{
    const auto* Player = Cast<ANativeWadPlayer>(CharacterOwner);
    const double Multiplier = Player && Player->HasLowGravity() ? 0.5 : 1.0;
    // Source sector gravity defaults to 1024. Custom sector gravity import and
    // its animation changes remain separate terrain work.
    return float(-NativeWadRules::SourceGravityAcceleration(NativeWadRules::SourceNormalGravity) * Multiplier);
}

FVector UNativeWadMovement::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
    FVector Result = Super::NewFallVelocity(InitialVelocity, Gravity, DeltaTime);
    if (const auto* Player = Cast<ANativeWadPlayer>(CharacterOwner); Player && Player->HasFeatherfall())
    {
        Result.Z = FMath::Max(Result.Z, -NativeWadRules::FeatherfallSpeed);
    }
    return Result;
}

void UNativeWadMovement::ProcessLanded(const FHitResult& Hit, float RemainingTime, int32 Iterations)
{
    // ProcessLanded runs before walking physics clears the incoming velocity.
    // Use only a real downward, walkable collision. Spawn/teleport positions and
    // depenetration corrections do not by themselves constitute a landing impact.
    if (auto* Player = Cast<ANativeWadPlayer>(CharacterOwner); Player && IsFalling() && !bJustTeleported
        && Hit.bBlockingHit && !Hit.bStartPenetrating && IsWalkable(Hit) && Velocity.Z < 0.0)
    {
        const int32 SourceZV = NativeWadRules::SourceZVelocityFromNative(Velocity.Z);
        const int32 RawDamage = NativeWadRules::SourceLandingDamage(SourceZV);
        if (RawDamage > 0)
        {
            if (Player->GetStatusPower(NativeWadRules::ShockAbsorbStatus) != 0)
            {
                UE_LOG(LogTemp, Display, TEXT("WAD Shock Absorption prevented landing impact: source ZV %d, raw normal damage %d."), SourceZV, RawDamage);
            }
            else
            {
                const int32 Before = Player->GetHealth();
                // Original StatsTakeDamage(NORMAL) still applies armor and the
                // ordinary invulnerability gate; fall damage bypasses neither.
                Player->ReceiveTypedHit(RawDamage, NativeWadRules::NormalDamage);
                UE_LOG(LogTemp, Display, TEXT("WAD landing impact: source ZV %d, raw normal damage %d, health %d -> %d."),
                    SourceZV, RawDamage, Before, Player->GetHealth());
            }
        }
    }
    Super::ProcessLanded(Hit, RemainingTime, Iterations);
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
