#include "NativeWadPickupMetadata.h"

#include "NativeWadRules.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"

bool NativeWadPickupMetadata::ParseJournalPageName(const FString& Name, int32& Page)
{
    // Import only the finished, extensionless resources used by NOTES.C, never authoring copies.
    if (Name.Len() != 8 || !Name.StartsWith(TEXT("NOTE"), ESearchCase::CaseSensitive)) { return false; }
    for (int32 Index = 4; Index < 8; ++Index)
    { if (Name[Index] < TEXT('0') || Name[Index] > TEXT('9')) { return false; } }
    const int32 Value = FCString::Atoi(*Name.Mid(4));
    if (Value >= MaximumJournalPages) { return false; }
    Page = Value;
    return true;
}

NativeWadPickupMetadata::FAmmo NativeWadPickupMetadata::ResolveAmmo(int32 Type, bool bRecorded,
    uint16 AccessoryData, int32 LegacyIndex, int32 LegacyQuantity)
{
    using namespace NativeWadRules;
    // Prior native saves already recorded partial quivers separately. Retain that exact amount
    // when no explicit source accessory value exists; zero in new source metadata means default.
    if (!bRecorded && LegacyIndex != INDEX_NONE) { return {LegacyIndex, LegacyQuantity}; }
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(CrossbowAmmo); ++Index)
    {
        const auto& Ammo = CrossbowAmmo[Index];
        if (Type == Ammo.Quiver || Type == Ammo.Bolt)
        { return {Index, AccessoryData ? int32(AccessoryData) : Type == Ammo.Quiver ? 12 : 1}; }
    }
    return {};
}

void NativeWadPickupMetadata::WriteSavedDrop(const ANativeWadPickup& Pickup, FWadSavedDrop& Saved)
{
    Saved.Type = Pickup.GetItemType();
    Saved.Position = Pickup.GetActorLocation();
    Saved.AmmoIndex = Pickup.GetAmmoIndex();
    Saved.AmmoQuantity = Pickup.GetAmmoQuantity();
    Saved.bAccessoryDataRecorded = Pickup.HasAccessoryData();
    Saved.AccessoryData = Pickup.GetAccessoryData();
}

void NativeWadPickupMetadata::RestoreSavedDrop(const FWadSavedDrop& Saved, ANativeWadPickup& Pickup)
{
    Pickup.SetAuthoredAmmo(Saved.AmmoIndex, Saved.AmmoQuantity);
    Pickup.RestoreAccessoryData(Saved.AccessoryData, Saved.bAccessoryDataRecorded);
}
