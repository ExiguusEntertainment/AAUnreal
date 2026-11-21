#pragma once

#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"

namespace NativeWadBillboard
{
    inline FVector FaceView(UStaticMeshComponent& Sprite, const APlayerController& Controller)
    {
        FVector Eye;
        FRotator View;
        Controller.GetPlayerViewPoint(Eye, View);
        const FVector Facing = (Eye - Sprite.GetComponentLocation()).GetSafeNormal2D();
        if (!Facing.IsNearlyZero())
        {
            const FVector Right = FVector::CrossProduct(FVector::UpVector, Facing);
            Sprite.SetWorldRotation(FRotationMatrix::MakeFromXY(Right, -FVector::UpVector).Rotator());
        }
        return Facing;
    }
}
