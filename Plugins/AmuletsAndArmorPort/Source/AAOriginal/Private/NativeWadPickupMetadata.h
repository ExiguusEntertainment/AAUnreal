#pragma once

#include "CoreMinimal.h"

class ANativeWadPickup;
struct FWadSavedDrop;

namespace NativeWadPickupMetadata
{
    inline constexpr int32 JournalObjectType = 373;
    inline constexpr int32 MaximumJournalPages = 1024;
    struct FAmmo { int32 Index = INDEX_NONE; int32 Quantity = 0; };
    AAORIGINAL_API bool ParseJournalPageName(const FString& Name, int32& Page);
    AAORIGINAL_API FAmmo ResolveAmmo(int32 Type, bool bRecorded, uint16 AccessoryData,
        int32 LegacyIndex = INDEX_NONE, int32 LegacyQuantity = 0);
    AAORIGINAL_API void WriteSavedDrop(const ANativeWadPickup& Pickup, FWadSavedDrop& Saved);
    AAORIGINAL_API void RestoreSavedDrop(const FWadSavedDrop& Saved, ANativeWadPickup& Pickup);
}
