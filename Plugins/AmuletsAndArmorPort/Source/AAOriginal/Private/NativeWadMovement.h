#pragma once

#include "GameFramework/CharacterMovementComponent.h"
#include "NativeWadMovement.generated.h"

UCLASS()
class UNativeWadMovement : public UCharacterMovementComponent
{
    GENERATED_BODY()

protected:
    virtual bool ResolvePenetrationImpl(const FVector& Adjustment, const FHitResult& Hit, const FQuat& NewRotation) override;
};
