#pragma once

#include "NativeWadTown.h"
#include "NativeWadHudData.generated.h"

UCLASS()
class UNativeWadHudData : public UDataAsset
{
    GENERATED_BODY()

public:
    static constexpr const TCHAR* ObjectPath = TEXT("/Game/WadInterface/Graphics/ClassicHud.ClassicHud");

    UPROPERTY()
    TMap<FName, TObjectPtr<UTexture2D>> Pictures;

    UPROPERTY()
    TMap<int32, TObjectPtr<UTexture2D>> Items;

    UPROPERTY()
    TMap<int32, int32> ItemWeights;

    UPROPERTY()
    TArray<FColor> Palette;

    UPROPERTY()
    FWadTownFont Tiny;
};
