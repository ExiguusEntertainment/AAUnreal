#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "NativeWadHudData.h"

struct FWadAuthoredItem
{
    FWadInventoryDetails Details;
    FString Picture;
    int32 Weight = 0;
    int32 Radius = 0;
    bool bPassable = true;
    bool bHanging = false;
    bool bRaised = false;

    FString AssetName() const;
    FString WorldMaterialPackage() const;
};

bool ReadWadAuthoredItems(const TSet<int32>& ThingTypes, TMap<int32, FWadAuthoredItem>& Items, FOutputDevice& Output);
bool ReadWadShopStock(bool bBank, TMap<int32, int32>& Tiers, FOutputDevice& Output);

#endif
