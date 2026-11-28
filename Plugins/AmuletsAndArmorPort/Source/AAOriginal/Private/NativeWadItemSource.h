#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "NativeWadHudData.h"

struct AAORIGINAL_API FWadAuthoredItem
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

// Validates the complete source INF numeric layout and returns stance0/frame0/angle0.
// Negative picture IDs retain the original mirrored-orientation sign.
AAORIGINAL_API bool ParseWadObjectInitialPicture(TConstArrayView<int32> Values, int32& SignedPicture);

AAORIGINAL_API bool ReadWadAuthoredItems(const TSet<int32>& ThingTypes, TMap<int32, FWadAuthoredItem>& Items, FOutputDevice& Output);
bool ReadWadShopStock(bool bBank, TMap<int32, int32>& Tiers, FOutputDevice& Output);
bool ReadWadStartingEquipment(TArray<FWadStartingEquipment>& Equipment, FOutputDevice& Output);

#endif
