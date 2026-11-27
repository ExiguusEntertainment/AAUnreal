#pragma once

#include "GameFramework/CharacterMovementComponent.h"
#include "NativeWadMovement.generated.h"

UCLASS()
class UNativeWadMovement : public UCharacterMovementComponent
{
    GENERATED_BODY()

protected:
    virtual float GetGravityZ() const override;
    virtual bool ResolvePenetrationImpl(const FVector& Adjustment, const FHitResult& Hit, const FQuat& NewRotation) override;
    virtual FVector NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const override;
};
