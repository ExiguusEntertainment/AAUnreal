#include "NativeWadPlayer.h"

#include "NativeWadFlightOwners.h"

bool ANativeWadPlayer::IsFlightEquipmentOwner(int32 Slot) const
{
    return CarriedItems.IsValidIndex(Slot) && CarriedItems[Slot] == NativeWadRules::FlyingAmulet
        && (Slot == ReadySlot || IsWornInventorySlot(Slot));
}

bool ANativeWadPlayer::HasActiveFlightEquipment() const
{
    const auto Active = [&](int32 Slot)
    { return IsFlightEquipmentOwner(Slot) && !SuppressedFlightEquipment.Contains(Slot); };
    if (Active(ReadySlot)) { return true; }
    for (const int32 Slot : JewelrySlots) { if (Active(Slot)) { return true; } }
    for (const int32 Slot : WornSlots) { if (Active(Slot)) { return true; } }
    return false;
}

void ANativeWadPlayer::PruneFlightEquipmentOwners()
{
    SuppressedFlightEquipment.RemoveAll([&](int32 Slot) { return !IsFlightEquipmentOwner(Slot); });
}

void ANativeWadPlayer::ActivateFlightEquipmentOwner(int32 Slot)
{
    PruneFlightEquipmentOwners();
    // A new owner's READY does not send READY to older worn objects.
    SuppressedFlightEquipment.Remove(Slot);
}

void ANativeWadPlayer::SuppressFlightEquipmentOwners()
{
    SuppressedFlightEquipment.Reset();
    const auto Add = [&](int32 Slot)
    { if (IsFlightEquipmentOwner(Slot)) { SuppressedFlightEquipment.AddUnique(Slot); } };
    Add(ReadySlot);
    for (const int32 Slot : JewelrySlots) { Add(Slot); }
    for (const int32 Slot : WornSlots) { Add(Slot); }
}

FString ANativeWadPlayer::EncodeFlightEquipmentOwners() const
{
    TArray<int32> Owners;
    for (const int32 Slot : SuppressedFlightEquipment)
    { if (IsFlightEquipmentOwner(Slot)) { Owners.AddUnique(Slot); } }
    Owners.Sort();
    return Owners.IsEmpty() ? TEXT("1") : TEXT("1~") + FString::JoinBy(Owners, TEXT(","), [](int32 Slot) { return LexToString(Slot); });
}

void ANativeWadPlayer::RestoreFlightEquipmentOwners(const FURL& URL)
{
    SuppressedFlightEquipment.Reset();
    if (const TCHAR* Payload = URL.GetOption(TEXT("WadFlightOwners="), nullptr))
    {
        TArray<int32> Owners;
        bool bValid = NativeWadFlightOwners::Decode(Payload, Owners);
        for (const int32 Slot : Owners) { bValid &= IsFlightEquipmentOwner(Slot); }
        if (bValid) { SuppressedFlightEquipment = MoveTemp(Owners); }
        else
        {
            // Invalid modern data must not silently reactivate Earthbound equipment.
            // Actual checkpoint loading rejects malformed codec data before mutation.
            SuppressFlightEquipmentOwners();
            UE_LOG(LogTemp, Warning, TEXT("Invalid saved flight-owner state; equipped flight remains suppressed."));
        }
        return; // An explicit modern empty payload overrides an old global flag.
    }
    if (FCString::Atoi(URL.GetOption(TEXT("WadFlightSuppressed="), TEXT("0"))) != 0)
    {
        // Old saves contain no individual history: conservatively migrate their
        // original global restriction to every currently equipped flight owner.
        SuppressFlightEquipmentOwners();
    }
}
