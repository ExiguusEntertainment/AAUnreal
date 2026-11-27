#include "NativeWadIdentification.h"

#include "Engine/GameInstance.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadHudData.h"
#include "NativeWadPlayer.h"

bool ANativeWadPlayer::HasIdentifiedItemType(int32 Type) const
{
    // Direct Editor previews retain their existing revealed names until character knowledge is initialized.
    return NativeWadIdentification::ValidType(Type) && (!bItemIdentificationInitialized || IdentifiedItemTypes.Contains(Type));
}

void ANativeWadPlayer::StoreItemIdentification()
{
    if (auto* Instance = GetGameInstance())
    {
        if (auto* Travel = Instance->GetSubsystem<UNativeWadTravelState>())
        {
            Travel->bItemIdentificationRecorded = bItemIdentificationInitialized;
            Travel->IdentifiedItemTypes = IdentifiedItemTypes;
        }
    }
}

void ANativeWadPlayer::InitializeItemIdentification(bool bKnowCarriedItems)
{
    IdentifiedItemTypes.Reset();
    bItemIdentificationInitialized = true;
    if (bKnowCarriedItems)
    {
        for (int32 Type : CarriedItems)
        { if (NativeWadIdentification::ValidType(Type)) { IdentifiedItemTypes.Add(Type); } }
    }
    StoreItemIdentification();
}

void ANativeWadPlayer::RestoreItemIdentification(const TSet<int32>& Types, bool bRecorded)
{
    if (!bRecorded)
    {
        // The old port exposed all names and did not save a discovery history. Retain carried names;
        // do not grant historical items, starter money, or guessed knowledge of previously sold types.
        InitializeItemIdentification(true);
        return;
    }
    TSet<int32> Valid;
    for (int32 Type : Types) { if (NativeWadIdentification::ValidType(Type)) { Valid.Add(Type); } }
    IdentifiedItemTypes = MoveTemp(Valid);
    bItemIdentificationInitialized = true;
    StoreItemIdentification();
}

bool ANativeWadPlayer::IdentifyItemType(int32 Type)
{
    if (!NativeWadIdentification::ValidType(Type)) { return false; }
    if (!bItemIdentificationInitialized) { InitializeItemIdentification(true); }
    const bool bNew = !IdentifiedItemTypes.Contains(Type);
    IdentifiedItemTypes.Add(Type);
    StoreItemIdentification();
    return bNew;
}

void ANativeWadPlayer::IdentifyEquippedItems()
{
    if (!bItemIdentificationInitialized) { InitializeItemIdentification(true); }
    // EQUIP.H includes the mouse hand, ready hand, six armor locations, necklace, and four rings.
    // The current selected-slot UI can represent a held item while open; persistence of a mouse-hand
    // location through closing forms remains separate inventory work.
    const auto IdentifySlot = [&](int32 Slot)
    {
        if (CarriedItems.IsValidIndex(Slot) && NativeWadIdentification::ValidType(CarriedItems[Slot]))
        { IdentifiedItemTypes.Add(CarriedItems[Slot]); }
    };
    IdentifySlot(SelectedInventorySlot);
    IdentifySlot(ReadySlot);
    for (int32 Slot : WornSlots) { IdentifySlot(Slot); }
    for (int32 Slot : JewelrySlots) { IdentifySlot(Slot); }
    StoreItemIdentification();
}

void ANativeWadPlayer::IdentifyAllCarriedItems()
{
    bItemIdentificationInitialized = true;
    for (int32 Type : CarriedItems)
    { if (NativeWadIdentification::ValidType(Type)) { IdentifiedItemTypes.Add(Type); } }
    StoreItemIdentification();
}

void ANativeWadPlayer::RecordItemUseIdentification(int32 Type)
{
    const FWadInventoryDetails* Rules = GetItemRules(Type);
    if (Rules && Rules->bIdentifyOnUse) { IdentifyItemType(Type); }
}

bool ANativeWadPlayer::UseIdentificationItem(int32 Slot)
{
    if (!CarriedItems.IsValidIndex(Slot)) { return false; }
    const int32 Type = CarriedItems[Slot];
    const FWadInventoryDetails* Rules = GetItemRules(Type);
    if (!Rules || !Rules->bUsable || Rules->IdentificationScope < 1 || Rules->IdentificationScope > 2) { return false; }
    if (Rules->IdentificationScope == 1) { IdentifyEquippedItems(); }
    else { IdentifyAllCarriedItems(); }
    RecordItemUseIdentification(Type);
    if (Rules->bConsumed) { RemoveInventorySlot(Slot); }
    ShowInteractionMessage(Rules->IdentificationScope == 1
        ? TEXT("You understand your equipment better!") : TEXT("You understand your inventory better!"));
    return true;
}
