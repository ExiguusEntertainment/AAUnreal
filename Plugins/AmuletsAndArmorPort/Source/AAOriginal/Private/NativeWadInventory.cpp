#include "NativeWadPlayer.h"

#include "Algo/Find.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "NativeWadInventoryView.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "NativeWadHudData.h"
#include "NativeWadLevel.h"

const UNativeWadHudData* UNativeWadHudData::LoadCatalog()
{
    static TWeakObjectPtr<UNativeWadHudData> Catalog;
    if (!Catalog.IsValid())
    {
        Catalog = LoadObject<UNativeWadHudData>(nullptr, ObjectPath, nullptr, LOAD_NoWarn);
    }
    return Catalog.Get();
}

namespace
{
    using namespace NativeWadRules;

    FString FallbackItemName(int32 Type)
    {
        switch (Type)
        {
        case IronDagger: return TEXT("Iron dagger");
        case Berry: return TEXT("Berry");
        case FeatherfallPotion: return TEXT("Potion of Featherfalling");
        case JugurthaJournal: return TEXT("Jugurtha's journal");
        case ChurchKey: return TEXT("Church key");
        case AndrewNecklace: return TEXT("Necklace of Andrew");
        case OrbOfDefense: return TEXT("Orb of Defense");
        case OrbOfTheUndead: return TEXT("Orb of the Undead");
        case ElymasNotes: return TEXT("Elymas's notes");
        case WandOfDisintegration: return TEXT("Wand of Disintegration");
        case OrbOfDestruction: return TEXT("Orb of Destruction");
        case Key1: return TEXT("Key 1");
        case Key4: return TEXT("Key 4");
        case Key9: return TEXT("Key 9");
        case ElmoreKey3: return TEXT("Key 3");
        case ElmoreKey5: return TEXT("Key 5");
        case ElmoreKey6: return TEXT("Key 6");
        case ElmoreKey8: return TEXT("Key 8");
        default: return Type > 0 ? FString::Printf(TEXT("Item %d"), Type) : FString(TEXT("Empty"));
        }
    }

    FBox2D InventoryBox(const APlayerController* Controller, int32 Slot)
    {
        int32 Width = 0;
        int32 Height = 0;
        if (Controller)
        {
            Controller->GetViewportSize(Width, Height);
        }
        const double Left = FMath::Max(16.0, Width * 0.5 - 220.0);
        const double Top = FMath::Max(70.0, Height * 0.5 - 130.0);
        const FVector2D Position(Left + (Slot == INDEX_NONE ? 232.0 : 0.0),
            Top + (Slot == INDEX_NONE ? 0.0 : Slot * 32.0));
        return FBox2D(Position, Position + FVector2D(206.0, 28.0));
    }
}

int32 ANativeWadPlayer::GetReadyItem() const
{
    return CarriedItems.IsValidIndex(ReadySlot) ? CarriedItems[ReadySlot] : 0;
}

FString ANativeWadPlayer::GetInventoryItemDescription(int32 Type, bool bMerchantPreview) const
{
    if (const FWadInventoryDetails* Details = GetItemRules(Type); Details && !Details->Description.IsEmpty())
    {
        return bMerchantPreview || HasIdentifiedItemType(Type) ? Details->Description
            : Details->UnidentifiedDescription.IsEmpty() ? FString(TEXT("Unidentified item")) : Details->UnidentifiedDescription;
    }
    return FallbackItemName(Type);
}

const FWadInventoryDetails* ANativeWadPlayer::GetItemRules(int32 Type) const
{
    const UNativeWadHudData* Catalog = Type > 0 ? UNativeWadHudData::LoadCatalog() : nullptr;
    return Catalog ? Catalog->ItemDetails.Find(Type) : nullptr;
}

bool ANativeWadPlayer::CanUseWadItem(int32 Type) const
{
    const FWadInventoryDetails* Rules = GetItemRules(Type);
    return !Rules || (Rules->UsableClasses & ClassFlag(CharacterClass)) != 0;
}

int32 ANativeWadPlayer::GetStatusPower(EItemStatus Status) const
{
    if (Status < 0 || Status >= ItemStatusCount) { return 0; }
    const auto PassivePower = [&](int32 Slot)
    {
        const int32 Type = CarriedItems.IsValidIndex(Slot) ? CarriedItems[Slot] : 0;
        // DES00954 pays its READY health cost before adding its own protection.
        if (bApplyingDefenseOrbReady && Slot == ReadySlot && Type == OrbOfDefense) { return 0; }
        const FWadInventoryDetails* Rules = GetItemRules(Type);
        return Rules && CanUseWadItem(Type) && Rules->Status == Status && Rules->StatusTicks == 0 ? Rules->StatusPower : 0;
    };
    int32 Power = ItemStatusSeconds[Status] > 0.0 ? ItemStatusPower[Status] : 0;
    TArray<int32, TInlineAllocator<1 + EquipmentSlotCount + JewelrySlotCount>> Owners;
    const auto AddOwner = [&](int32 Slot)
    {
        if (!CarriedItems.IsValidIndex(Slot) || Owners.Contains(Slot)) { return; }
        Owners.Add(Slot);
        Power += PassivePower(Slot);
    };
    // EFFECT.C sums effects from distinct object owners. Each worn object is counted
    // once, even when several rings of the same type occupy the original four slots.
    // InventoryTransferToReadyHand explicitly skips armor READY effects.
    const FWadInventoryDetails* Ready = GetItemRules(GetReadyItem());
    if (!IsWornInventorySlot(ReadySlot) && (!Ready || Ready->ArmorSlot == INDEX_NONE)) { AddOwner(ReadySlot); }
    for (const int32 Slot : JewelrySlots) { AddOwner(Slot); }
    for (int32 Location = 0; Location < EquipmentSlotCount; ++Location)
    {
        const int32 Slot = WornSlots[Location];
        const FWadInventoryDetails* Rules = CarriedItems.IsValidIndex(Slot) ? GetItemRules(CarriedItems[Slot]) : nullptr;
        if (Rules && (Rules->ArmorSlot == Location || (Location == RightArmSlot && Rules->ArmorSlot == LeftArmSlot)))
        { AddOwner(Slot); }
    }
    return Power;
}

int32 ANativeWadPlayer::GetJewelryInventorySlot(int32 Location) const
{
    return Location >= 0 && Location < JewelrySlotCount ? JewelrySlots[Location] : INDEX_NONE;
}

bool ANativeWadPlayer::HasEquippedItem(int32 Type) const
{
    if (Type <= 0 || !CanUseWadItem(Type)) { return false; }
    if (GetReadyItem() == Type) { return true; }
    for (const int32 Slot : JewelrySlots)
    {
        if (CarriedItems.IsValidIndex(Slot) && CarriedItems[Slot] == Type) { return true; }
    }
    return false;
}

int32 ANativeWadPlayer::GetEffectiveAttribute(int32 Attribute) const
{
    if (Attribute < 0 || Attribute >= AttributeCount) { return 0; }
    const EItemStatus Effects[] = {StrengthStatus, SpeedStatus, MagicStatus, AccuracyStatus, StealthStatus};
    const int32 Modifier = Attribute < UE_ARRAY_COUNT(Effects) ? GetStatusPower(Effects[Attribute]) : 0;
    return EffectiveAttribute(Attributes[Attribute], Modifier);
}

int32 ANativeWadPlayer::GetHealthRegeneration() const
{
    const int32 Rate = Attributes[ConstitutionAttribute] * (CharacterLevel + 5) + GetStatusPower(HealthRegenStatus);
    return FMath::Max(0, Rate >> 3) * Food / MaxFood;
}

int32 ANativeWadPlayer::GetManaRegeneration() const
{
    // Source MAGIC MOD affects spell rolls, not the permanent mana-regeneration base.
    const int32 Base = Attributes[MagicAttribute] * (CharacterLevel + 5) * CharacterClasses[CharacterClass].ManaRecoveryPercent / 100;
    return FMath::Max(0, (Base + GetStatusPower(ManaRegenStatus)) >> 2) * Water / MaxFood;
}

bool ANativeWadPlayer::ConsumeItem(int32 Type)
{
    return RemoveInventorySlot(CarriedItems.Find(Type));
}

bool ANativeWadPlayer::RemoveInventorySlot(int32 Slot)
{
    if (!CarriedItems.IsValidIndex(Slot))
    {
        return false;
    }
    CarriedItems.RemoveAt(Slot);
    if (ReadySlot == Slot)
    {
        ReadySlot = INDEX_NONE;
    }
    else if (ReadySlot > Slot)
    {
        --ReadySlot;
    }
    if (SelectedInventorySlot == Slot)
    {
        SelectedInventorySlot = INDEX_NONE;
    }
    else if (SelectedInventorySlot > Slot)
    {
        --SelectedInventorySlot;
    }
    for (int32& Worn : WornSlots)
    {
        if (Worn == Slot) { Worn = INDEX_NONE; }
        else if (Worn > Slot) { --Worn; }
    }
    for (int32& Worn : JewelrySlots)
    {
        if (Worn == Slot) { Worn = INDEX_NONE; }
        else if (Worn > Slot) { --Worn; }
    }
    if (!HasEquippedItem(FlyingAmulet)) { bReadiedFlightSuppressed = false; }
    return true;
}

int32 ANativeWadPlayer::GetEquippedWeapon() const
{
    const int32 Type = GetReadyItem();
    const FWadInventoryDetails* Details = GetItemRules(Type);
    return CanUseWadItem(Type) && (Type == IronDagger || Type == Crossbow
        || (Details && Details->WeaponDamage > 0 && Details->HitFrame > 0)) ? Type : 0;
}

bool ANativeWadPlayer::IsWornInventorySlot(int32 Slot) const
{
    return Slot >= 0 && (Algo::Find(WornSlots, Slot) != nullptr || Algo::Find(JewelrySlots, Slot) != nullptr);
}

uint8 ANativeWadPlayer::GetMeleeDamageMask() const
{
    uint8 Mask = NormalDamage;
    constexpr EItemStatus Statuses[] = {PoisonAttackStatus, PiercingAttackStatus, FireAttackStatus,
        ElectricityAttackStatus, ManaDrainAttackStatus, AcidAttackStatus};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Statuses); ++Index)
    {
        if (GetStatusPower(Statuses[Index]) > 0)
        {
            Mask |= DamageFlag(EProjectileDamage(Index + 1));
        }
    }
    return Mask;
}

int32 ANativeWadPlayer::GetArmorClass(int32 Location) const
{
    if (Location < 0 || Location >= EquipmentSlotCount) { return 0; }
    const int32 Slot = WornSlots[Location];
    const FWadInventoryDetails* Rules = CarriedItems.IsValidIndex(Slot) ? GetItemRules(CarriedItems[Slot]) : nullptr;
    return Rules && CanUseWadItem(CarriedItems[Slot]) ? Rules->ArmorClass : 0;
}

uint8 ANativeWadPlayer::GetResistanceMask() const
{
    uint8 Mask = 0;
    constexpr EItemStatus Statuses[] = {PoisonResistStatus, PiercingResistStatus, FireResistStatus,
        ElectricityResistStatus, ManaDrainResistStatus, AcidResistStatus};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Statuses); ++Index)
    {
        if (GetStatusPower(Statuses[Index]) > 0) { Mask |= DamageFlag(EProjectileDamage(Index + 1)); }
    }
    return Mask;
}

int32 ANativeWadPlayer::GetArmorRating() const
{
    const auto Rating = [&](int32 Location)
    {
        const int32 Slot = WornSlots[Location];
        const FWadInventoryDetails* Rules = CarriedItems.IsValidIndex(Slot) ? GetItemRules(CarriedItems[Slot]) : nullptr;
        return Rules && CanUseWadItem(CarriedItems[Slot]) ? Rules->ArmorValue : 0;
    };
    int32 Shield = GetStatusPower(SpellShieldStatus);
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(ShieldSeconds); ++Index)
    {
        if (ShieldSeconds[Index] > 0.0)
        {
            Shield += TimedEffects.Power(EWadTimedEffectKind::ShieldPotion, Index);
        }
    }
    // STATS.C caps only magical shielding at ninety before comparing each body
    // piece. Its shield-hand contribution to the weighted average is disabled.
    Shield = FMath::Clamp(Shield, 0, 90);
    return (2 * (FMath::Max(Shield, Rating(HeadSlot)) + FMath::Max(Shield, Rating(RightArmSlot)) + FMath::Max(Shield, Rating(LeftArmSlot)))
        + 4 * (FMath::Max(Shield, Rating(LegsSlot)) + FMath::Max(Shield, Rating(ChestSlot)))) / 14;
}

void ANativeWadPlayer::EquipArmor(int32 Slot, const FWadInventoryDetails& Details)
{
    int32 Location = Details.ArmorSlot;
    for (int32& Worn : WornSlots)
    {
        if (Worn == Slot)
        {
            Worn = INDEX_NONE;
            ShowInteractionMessage(FString::Printf(TEXT("Removed %s"), *GetInventoryItemDescription(CarriedItems[Slot])));
            return;
        }
    }
    if (!CanUseWadItem(CarriedItems[Slot]))
    {
        ShowInteractionMessage(TEXT("Your class can't equip this item."));
        return;
    }
    if (Location == LeftArmSlot && WornSlots[LeftArmSlot] != INDEX_NONE && WornSlots[RightArmSlot] == INDEX_NONE)
    {
        Location = RightArmSlot;
    }
    WornSlots[Location] = Slot;
    if (ReadySlot == Slot)
    {
        ReadySlot = INDEX_NONE;
    }
    ShowInteractionMessage(FString::Printf(TEXT("Equipped %s"), *GetInventoryItemDescription(CarriedItems[Slot])));
}

bool ANativeWadPlayer::EquipJewelry(int32 Slot, const FWadInventoryDetails& Details, int32 Location)
{
    if (!CarriedItems.IsValidIndex(Slot) || (Details.JewelrySlot != NeckSlot && Details.JewelrySlot != Ring1Slot)
        || !CanUseWadItem(CarriedItems[Slot])) { return false; }
    int32 Existing = INDEX_NONE;
    for (int32 Index = 0; Index < JewelrySlotCount; ++Index)
    {
        if (JewelrySlots[Index] == Slot) { Existing = Index; break; }
    }
    if (Existing != INDEX_NONE && (Location == INDEX_NONE || Location == Existing))
    {
        JewelrySlots[Existing] = INDEX_NONE;
        if (!HasEquippedItem(FlyingAmulet)) { bReadiedFlightSuppressed = false; }
        ShowInteractionMessage(FString::Printf(TEXT("Removed %s"), *GetInventoryItemDescription(CarriedItems[Slot])));
        return true;
    }
    if (Location == INDEX_NONE)
    {
        const int32 Begin = Details.JewelrySlot;
        const int32 End = Details.JewelrySlot == NeckSlot ? Ring1Slot : JewelrySlotCount;
        for (int32 Index = Begin; Index < End; ++Index)
        {
            if (JewelrySlots[Index] == INDEX_NONE) { Location = Index; break; }
        }
        if (Location == INDEX_NONE)
        {
            ShowInteractionMessage(Details.JewelrySlot == NeckSlot ? TEXT("You are already wearing an amulet.") : TEXT("All four ring slots are occupied."));
            return false;
        }
    }
    if (Location < 0 || Location >= JewelrySlotCount || (Location == NeckSlot) != (Details.JewelrySlot == NeckSlot)) { return false; }
    if (Existing != INDEX_NONE) { JewelrySlots[Existing] = INDEX_NONE; }
    JewelrySlots[Location] = Slot;
    if (ReadySlot == Slot) { ReadySlot = INDEX_NONE; }
    // Equipping is a new READY trigger. Changing a weapon alone does not restart a
    // worn flight amulet that Earthbind suppressed.
    if (CarriedItems[Slot] == FlyingAmulet) { bReadiedFlightSuppressed = false; }
    ShowInteractionMessage(FString::Printf(TEXT("Equipped %s"), *GetInventoryItemDescription(CarriedItems[Slot])));
    return true;
}

bool ANativeWadPlayer::TakeInventoryItem(int32 Slot)
{
    if (IsDefeated() || !CarriedItems.IsValidIndex(Slot) || IsRuneItem(CarriedItems[Slot])) { return false; }
    if (SelectedInventorySlot == Slot) { return true; }
    if (CarriedItems.IsValidIndex(SelectedInventorySlot) || Slot == ReadySlot || IsWornInventorySlot(Slot)) { return false; }
    SelectedInventorySlot = Slot;
    return true;
}

void ANativeWadPlayer::StoreMouseHandInInventory()
{
    if (IsDefeated()) { return; }
    // CarriedItems owns every object. A free bag placement only releases its location pointer;
    // display stacks regroup individual objects without destroying or duplicating one.
    SelectedInventorySlot = INDEX_NONE;
}

bool ANativeWadPlayer::CreateWadItemInMouseHand(int32 Type)
{
    // EFFECT.C CREATE_OBJECT checks the hand after SPELLS.C has charged mana.
    // It adds an ordinary inventory object without a GET/identify/use trigger.
    if (GetMouseHandItem() != 0)
    {
        ShowInteractionMessage(TEXT("Can't create while item is in hand."));
        return false;
    }
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    if (IsDefeated() || Type <= 0 || Type > MAX_uint16 || !Catalog
        || !Catalog->ItemDetails.Contains(Type) || !Catalog->Items.FindRef(Type))
    {
        ShowInteractionMessage(TEXT("The created item is unavailable."));
        return false;
    }
    SelectedInventorySlot = CarriedItems.Add(Type);
    return true;
}

bool ANativeWadPlayer::SwapMouseAndReady()
{
    if (IsDefeated()) { return false; }
    if (CarriedItems.IsValidIndex(SelectedInventorySlot))
    {
        const int32 Type = CarriedItems[SelectedInventorySlot];
        if (IsRuneItem(Type) || !CanUseWadItem(Type))
        { ShowInteractionMessage(TEXT("Your class can't use this item.")); return false; }
        if (const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog())
        {
            if (const UTexture2D* Image = Catalog->Items.FindRef(Type);
                Image && (Image->GetSizeX() >= 35 || Image->GetSizeY() >= 27))
            { ShowInteractionMessage(TEXT("This item won't fit in your ready hand.")); return false; }
        }
    }
    Swap(SelectedInventorySlot, ReadySlot);
    if (GetReadyItem() == FlyingAmulet || !HasEquippedItem(FlyingAmulet)) { bReadiedFlightSuppressed = false; }
    ApplyReadiedDefenseOrbEffect();
    if (IsDefeated()) { return true; }
    ShowInteractionMessage(FString::Printf(TEXT("Ready hand: %s"), *GetInventoryItemDescription(GetReadyItem())));
    return true;
}

void ANativeWadPlayer::ApplyReadiedDefenseOrbEffect()
{
    if (GetReadyItem() != OrbOfDefense) { return; }
    TGuardValue<bool> Applying(bApplyingDefenseOrbReady, true);
    ReceiveEncounterHit(300);
    // Normal in-level death drops the orb. If a drop could not be placed,
    // leave it owned in the bag rather than activate a postmortem READY effect.
    if (IsDefeated() && GetReadyItem() == OrbOfDefense) { ReadySlot = INDEX_NONE; }
}

void ANativeWadPlayer::ClickInventorySlot(int32 Slot)
{
    if (IsDefeated()) { return; }
    if (Slot == INDEX_NONE) { SwapMouseAndReady(); return; }
    if (!CarriedItems.IsValidIndex(Slot)) { return; }
    if (CarriedItems.IsValidIndex(SelectedInventorySlot) && Slot != SelectedInventorySlot)
    {
        // INVENTOR.C permits placement on an existing stack only for the exact same type with room.
        const int32 Type = CarriedItems[SelectedInventorySlot];
        const FWadInventoryDetails* Details = GetItemRules(Type);
        if (CarriedItems[Slot] == Type && Slot != ReadySlot && !IsWornInventorySlot(Slot) && Details)
        {
            const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
            if (Catalog)
            {
                const NativeWadInventoryView::FView View(*this, *Catalog, true, SelectedInventorySlot);
                const int32 Cell = View.CellForSlot(Slot);
                if (View.Cells.IsValidIndex(Cell) && View.Cells[Cell].Count < Details->StackLimit)
                { StoreMouseHandInInventory(); }
            }
        }
        return;
    }
    TakeInventoryItem(Slot);
}

void ANativeWadPlayer::ClickArmorSlot(int32 Location)
{
    if (IsDefeated() || Location < 0 || Location >= EquipmentSlotCount) { return; }
    const int32 Previous = WornSlots[Location];
    if (!CarriedItems.IsValidIndex(SelectedInventorySlot))
    {
        SelectedInventorySlot = Previous;
        WornSlots[Location] = INDEX_NONE;
        return;
    }
    const int32 Type = CarriedItems[SelectedInventorySlot];
    const FWadInventoryDetails* Details = GetItemRules(Type);
    if (!Details) { return; }
    if (Details->WeaponDamage > 0 || Type == Crossbow)
    {
        if (ReadySlot == INDEX_NONE) { SwapMouseAndReady(); }
        else { ShowInteractionMessage(TEXT("There is already something in your hand.")); }
        return;
    }
    if (Details->ArmorSlot != Location && !(Location == RightArmSlot && Details->ArmorSlot == LeftArmSlot)) { return; }
    if (!CanUseWadItem(Type)) { ShowInteractionMessage(TEXT("Your class can't equip this item.")); return; }
    WornSlots[Location] = SelectedInventorySlot;
    SelectedInventorySlot = Previous;
    ShowInteractionMessage(FString::Printf(TEXT("Equipped %s"), *GetInventoryItemDescription(Type)));
}

void ANativeWadPlayer::ClickJewelrySlot(int32 Location)
{
    if (IsDefeated() || Location < 0 || Location >= JewelrySlotCount) { return; }
    const int32 Previous = JewelrySlots[Location];
    const bool bPlacing = CarriedItems.IsValidIndex(SelectedInventorySlot);
    const int32 Slot = bPlacing ? SelectedInventorySlot : Previous;
    if (!CarriedItems.IsValidIndex(Slot)) { return; }
    const FWadInventoryDetails* Rules = GetItemRules(CarriedItems[Slot]);
    if (Rules && EquipJewelry(Slot, *Rules, bPlacing ? Location : INDEX_NONE))
    {
        SelectedInventorySlot = bPlacing ? Previous : Slot;
    }
}

void ANativeWadPlayer::UseReadyHand()
{
    if (IsDefeated() || bInventoryOpen)
    {
        return;
    }
    const int32 Type = GetReadyItem();
    if (!CanUseWadItem(Type))
    {
        ShowInteractionMessage(TEXT("Your class can't use this item."));
        return;
    }
    if (UseIdentificationItem(ReadySlot)) { return; }
    if (Type == Crossbow)
    {
        FireCrossbow();
        return;
    }
    if (const FWadInventoryDetails* Rules = GetItemRules(Type); Rules && Rules->WandManaCost > 0)
    {
        if (const FWadWandProjectile* Projectile = FindWandProjectile(Rules->WandProjectile))
        {
            FireWand(*Projectile, Rules->WandManaCost);
            return;
        }
    }
    if (Type == WandOfDisintegration)
    {
        FireWand(*FindWandProjectile(28), DisintegrationManaCost);
        return;
    }
    if (Type == 0 || GetEquippedWeapon() != 0)
    {
        AttackPressed();
        return;
    }
    if (Type == FlyingPotion || Type == FlyingScroll)
    {
        if (RemoveInventorySlot(ReadySlot))
        {
            RecordItemUseIdentification(Type);
            const double Duration = Type == FlyingPotion ? FlyingPotionSeconds : FlyingScrollSeconds;
            ApplyTimedEffect(FWadPlayerEffects::ConsumableOwner(Type), EWadTimedEffectKind::Flight, INDEX_NONE, 1, Duration);
            ShowInteractionMessage(TEXT("You feel as light as the air around you!"));
            UE_LOG(LogTemp, Display, TEXT("Used WAD flight item %d for %.2f seconds."), Type, Duration);
        }
        return;
    }
    if (Type == WaterWalkPotion || Type == FeatherfallPotion)
    {
        if (RemoveInventorySlot(ReadySlot))
        {
            RecordItemUseIdentification(Type);
            if (Type == WaterWalkPotion)
            {
                ApplyTimedEffect(FWadPlayerEffects::ConsumableOwner(Type), EWadTimedEffectKind::WaterWalk, INDEX_NONE, 1, WaterWalkPotionSeconds);
                Water = FMath::Clamp(Water + 200, 0, MaxFood);
            }
            else
            {
                ApplyTimedEffect(FWadPlayerEffects::ConsumableOwner(Type), EWadTimedEffectKind::FeatherFall, INDEX_NONE, 1, FeatherfallPotionSeconds);
            }
            ShowInteractionMessage(FString::Printf(TEXT("Used %s"), *GetInventoryItemDescription(Type)));
        }
        return;
    }
    if (Type >= ShieldPotionFirst && Type <= ShieldPotionLast)
    {
        if (RemoveInventorySlot(ReadySlot))
        {
            RecordItemUseIdentification(Type);
            ApplyTimedEffect(FWadPlayerEffects::ConsumableOwner(Type), EWadTimedEffectKind::ShieldPotion,
                Type - ShieldPotionFirst, 10 + 5 * (Type - ShieldPotionFirst), ShieldPotionSeconds);
            ShowInteractionMessage(TEXT("You feel protected!"));
        }
        return;
    }
    if (const FWadInventoryDetails* Rules = GetItemRules(Type); Rules && Rules->bUsable)
    {
        UseConsumable(*Rules);
    }
    else if (Type == Berry)
    {
        FWadInventoryDetails BerryRules;
        BerryRules.Food = BerryFood;
        BerryRules.bConsumed = true;
        UseConsumable(BerryRules);
    }
}

void ANativeWadPlayer::FireCrossbow()
{
    const double Now = GetWorld()->GetTimeSeconds();
    if (IsParalyzed() || Now < NextMeleeTime)
    {
        return;
    }
    if (GetAmmoCount(SelectedAmmo) == 0)
    {
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(CrossbowAmmo); ++Index)
        {
            if (GetAmmoCount(Index) > 0)
            {
                SelectedAmmo = Index;
                break;
            }
        }
    }
    const FWadAmmo& Ammo = CrossbowAmmo[SelectedAmmo];
    const bool bFromReserve = AmmoReserve[SelectedAmmo] > 0;
    const int32 AmmoSlot = CarriedItems.Find(Ammo.Bolt);
    const int32 QuiverSlot = AmmoSlot == INDEX_NONE ? CarriedItems.Find(Ammo.Quiver) : INDEX_NONE;
    if (!bFromReserve && AmmoSlot == INDEX_NONE && QuiverSlot == INDEX_NONE)
    {
        ShowInteractionMessage(TEXT("No bolts."));
        return;
    }

    const FVector Direction = GetControlRotation().Vector();
    const FVector Start = GetPawnViewLocation();
    FActorSpawnParameters Parameters;
    Parameters.Owner = this;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ANativeWadBolt* Bolt = GetWorld()->SpawnActor<ANativeWadBolt>(Start, Direction.Rotation(), Parameters);
    if (!Bolt)
    {
        return;
    }
    if (bFromReserve)
    {
        --AmmoReserve[SelectedAmmo];
    }
    else if (QuiverSlot != INDEX_NONE)
    {
        RemoveInventorySlot(QuiverSlot);
        for (int32 Index = 1; Index < NormalQuiverBolts; ++Index)
        {
            CarriedItems.Add(Ammo.Bolt);
        }
    }
    else
    {
        RemoveInventorySlot(AmmoSlot);
    }
    UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    UMaterialInterface* Image = Catalog ? Catalog->AmmoProjectiles.FindRef(Ammo.Missile).Get() : nullptr;
    if (!Image)
    {
        Image = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Maps/L30_Assets/Things/M_GuardianBolt.M_GuardianBolt"));
    }
    Bolt->Launch(Plane, Image, Direction * (30.0 * TicksPerSecond), Ammo.Damage);
    Bolt->SetDamageType(Ammo.Effect);
    const int32 FrameSpeed = 40000 + FMath::Clamp(GetEffectiveAttribute(SpeedAttribute), 0, 100) * 1100;
    NextMeleeTime = Now + FMath::DivideAndRoundUp(CrossbowFrames * 65536, FrameSpeed) / TicksPerSecond;
    ShowInteractionMessage(TEXT("Fired a crossbow bolt."));
    UE_LOG(LogTemp, Display, TEXT("Fired WAD crossbow missile %d. %s bolts remaining: %d."),
        Ammo.Missile, Ammo.Name, GetAmmoCount(SelectedAmmo));
}

int32 ANativeWadPlayer::GetAmmoCount(int32 Index) const
{
    if (Index < 0 || Index >= UE_ARRAY_COUNT(CrossbowAmmo))
    {
        return 0;
    }
    const FWadAmmo& Ammo = CrossbowAmmo[Index];
    return AmmoReserve[Index] + GetItemCount(Ammo.Bolt) + NormalQuiverBolts * GetItemCount(Ammo.Quiver);
}

void ANativeWadPlayer::SelectAmmo(int32 Index)
{
    if (Index >= 0 && Index < UE_ARRAY_COUNT(CrossbowAmmo) && GetAmmoCount(Index) > 0)
    {
        SelectedAmmo = Index;
    }
}

bool ANativeWadPlayer::CollectAmmo(int32 Index, int32 Quantity)
{
    if (Index < 0 || Index >= UE_ARRAY_COUNT(AmmoReserve) || Quantity <= 0 || Quantity > MAX_uint16 - AmmoReserve[Index])
    {
        return false;
    }
    AmmoReserve[Index] += Quantity;
    ShowInteractionMessage(FString::Printf(TEXT("Picked up %d %s bolts."), Quantity, CrossbowAmmo[Index].Name));
    return true;
}

void ANativeWadPlayer::RetrieveAmmo(int32 Index)
{
    if (IsDefeated() || GetMouseHandItem() != 0 || Index < 0 || Index >= UE_ARRAY_COUNT(AmmoReserve) || AmmoReserve[Index] <= 0)
    {
        return;
    }
    const bool bQuiver = AmmoReserve[Index] >= NormalQuiverBolts;
    if (CollectItem(bQuiver ? CrossbowAmmo[Index].Quiver : CrossbowAmmo[Index].Bolt))
    {
        AmmoReserve[Index] -= bQuiver ? NormalQuiverBolts : 1;
        SelectedInventorySlot = CarriedItems.Num() - 1;
    }
}

void ANativeWadPlayer::RetrieveCoin(int32 Type)
{
    auto* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    if (IsDefeated() || CarriedItems.IsValidIndex(SelectedInventorySlot) || !Travel || CoinValueCopper(Type) == 0)
    {
        return;
    }
    const int64 Wallet = Travel->Quest.GetWalletCopper();
    const int32 Count = Type % 2 == 0 ? 1 : 5;
    if (CoinDenominationCount(Wallet, Type & ~1) >= Count)
    {
        Travel->Quest.SetWalletCopper(Wallet - CoinValueCopper(Type));
        SelectedInventorySlot = CarriedItems.Add(Type);
    }
}

void ANativeWadPlayer::ReturnSelectedCoin()
{
    if (CarriedItems.IsValidIndex(SelectedInventorySlot) && AddCoinsToPurse(CarriedItems[SelectedInventorySlot]))
    {
        RemoveInventorySlot(SelectedInventorySlot);
    }
}

void ANativeWadPlayer::FireWand(const FWadWandProjectile& Projectile, int32 ManaCost)
{
    const double Now = GetWorld()->GetTimeSeconds();
    if (IsParalyzed() || Now < NextMeleeTime)
    {
        return;
    }
    constexpr double WandDelay = 0.2;
    const int32 ManaBefore = Mana;
    if (Mana <= ManaCost)
    {
        Mana = FMath::Max(0, Mana - FMath::DivideAndRoundUp(ManaCost, 2));
        NextMeleeTime = Now + WandDelay;
        ShowInteractionMessage(TEXT("Nothing happens when you activate the wand."));
        UE_LOG(LogTemp, Display, TEXT("WAD wand failed with low mana. Mana %d -> %d."), ManaBefore, Mana);
        return;
    }

    if (!LaunchWadProjectile(Projectile)) { return; }
    Mana -= ManaCost;
    NextMeleeTime = Now + WandDelay;
    const FString Name = GetInventoryItemDescription(GetReadyItem());
    ShowInteractionMessage(FString::Printf(TEXT("Fired %s."), *Name));
    UE_LOG(LogTemp, Display, TEXT("Fired WAD missile %d with %s. Mana %d -> %d."), Projectile.Type, *Name, ManaBefore, Mana);
}

bool ANativeWadPlayer::LaunchWadProjectile(const FWadWandProjectile& Projectile)
{
    const FVector Start = GetPawnViewLocation();
    const FVector Direction = GetControlRotation().Vector();
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    UMaterialInterface* Material = nullptr;
    if (Catalog)
    {
        Material = Projectile.Type == 4017 ? Catalog->PlasmaProjectile.Get()
            : Catalog->WandProjectileMaterials.FindRef(Projectile.Type).Get();
    }

    FActorSpawnParameters Parameters;
    Parameters.Owner = this;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ANativeWadBolt* Bolt = GetWorld()->SpawnActor<ANativeWadBolt>(Start, Direction.Rotation(), Parameters);
    if (!Bolt)
    {
        return false;
    }
    UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    Bolt->Launch(Plane, Material, Direction * (50.0 * TicksPerSecond), Projectile.Damage);
    if (Projectile.bHoming)
    {
        constexpr double TargetRange = 4000.0;
        FHitResult Hit;
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(WadWandAim), true, this);
        GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Direction * TargetRange, ECC_Visibility, Query);
        Bolt->SetHomingTarget(Cast<ANativeWadGuardian>(Hit.GetActor()));
    }
    Bolt->SetBlastRadius(Projectile.Radius);
    Bolt->SetDamageMask(Projectile.DamageMask);
    return true;
}

void ANativeWadPlayer::UseConsumable(const FWadInventoryDetails& Details)
{
    const int32 Type = GetReadyItem();
    RecordItemUseIdentification(Type);
    const int32 BeforeHealth = Health;
    const int32 BeforeFood = Food;
    const int32 BeforePoison = PoisonLevel;
    Mana = FMath::Clamp(Mana + Details.Mana, 0, MaxMana);
    Food = FMath::Clamp(Food + Details.Food, 0, MaxFood);
    Water = FMath::Clamp(Water + Details.Water, 0, MaxFood);
    // EFFECT.C removes floor(poison / 2), leaving the upper half for an odd level.
    // In particular, the weaker cure cannot clear the last point of poison.
    if (Details.PoisonDivisor == 2) { PoisonLevel -= PoisonLevel >> 1; }
    else { PoisonLevel /= FMath::Max(1, Details.PoisonDivisor); }
    if (Details.Status >= 0 && Details.Status < ItemStatusCount && Details.StatusTicks > 0)
    {
        ApplyTimedEffect(FWadPlayerEffects::ConsumableOwner(Type), EWadTimedEffectKind::Status,
            Details.Status, Details.StatusPower, Details.StatusTicks / double(TicksPerSecond));
    }
    if (Details.bConsumed)
    {
        RemoveInventorySlot(ReadySlot);
    }
    if (Details.Health < 0)
    {
        ReceiveEncounterHit(-Details.Health);
    }
    else
    {
        Health = FMath::Min(MaxHealth, Health + Details.Health);
    }
    if (!IsDefeated())
    {
        ShowInteractionMessage(BeforePoison > 0 && Details.PoisonDivisor > 1
            ? PoisonLevel == 0 ? FString(TEXT("Your body is cleansed of all poison.")) : FString(TEXT("The poison in your body is less."))
            : Type == Berry ? FString(TEXT("You get berry juice all over your face."))
            : FString::Printf(TEXT("Used %s"), *GetInventoryItemDescription(Type)));
    }
    UE_LOG(LogTemp, Display, TEXT("Used WAD item %d. Health %d -> %d, food %d -> %d."), Type, BeforeHealth, Health, BeforeFood, Food);
}

void ANativeWadPlayer::DropInventoryItem()
{
    const int32 Slot = GetMouseHandItem() != 0 ? SelectedInventorySlot : ReadySlot;
    ANativeWadLevel* Level = NativeLevel.Get();
    if (IsDefeated() || IsParalyzed() || !CarriedItems.IsValidIndex(Slot) || !Level || !GetCharacterMovement()->IsMovingOnGround())
    {
        return;
    }
    const int32 Type = CarriedItems[Slot];
    const FWadInventoryDetails* Details = GetItemRules(Type);
    const FVector Ahead = GetActorLocation() + FRotator(0.0, GetControlRotation().Yaw, 0.0).Vector() * 36.0;
    const int32 Sector = Level->FindWadSector(FVector2D(Ahead - Level->GetActorLocation()));
    FVector2D Heights;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(WadDropClearance), true, this);
    if (!Details || !Level->GetSectorSurfaceHeights(Sector, Heights) || Heights.Y - Heights.X <= Details->SpriteSize.Y
        || GetWorld()->LineTraceTestByChannel(GetActorLocation(), Ahead, ECC_Visibility, Query))
    {
        ShowInteractionMessage(TEXT("Cannot drop that item here"));
        return;
    }

    FVector Position = Ahead;
    Position.Z = Level->GetActorLocation().Z + Heights.X + Details->SpriteSize.Y * 0.5 + 1.0;
    if (!ANativeWadPickup::SpawnDropped(*Level, Type, Position))
    {
        ShowInteractionMessage(TEXT("Cannot drop that item here"));
        return;
    }
    RemoveInventorySlot(Slot);
    ShowInteractionMessage(FString::Printf(TEXT("Dropped %s"), *GetInventoryItemDescription(Type)));
}

void ANativeWadPlayer::DropDefeatedItems()
{
    ANativeWadLevel* Level = NativeLevel.Get();
    if (!Level)
    {
        return;
    }
    const auto Drop = [&](int32 Type, int32 Index)
    {
        const FVector Origin = GetActorLocation();
        const FVector Candidate = Origin + FRotator(0.0, Index * 137.5, 0.0).Vector() * (18.0 + Index % 4 * 7.0);
        for (const FVector& Location : {Candidate, Origin})
        {
            FVector2D Heights;
            const int32 Sector = Level->FindWadSector(FVector2D(Location - Level->GetActorLocation()));
            if (Level->GetSectorSurfaceHeights(Sector, Heights))
            {
                const FWadInventoryDetails* Item = GetItemRules(Type);
                const double Offset = Item ? Item->SpriteSize.Y * 0.5 + 1.0 : 12.0;
                if (ANativeWadPickup* Pickup = ANativeWadPickup::SpawnDropped(*Level, Type,
                    FVector(Location.X, Location.Y, Level->GetActorLocation().Z + Heights.X + Offset)))
                {
                    return Pickup;
                }
            }
        }
        return static_cast<ANativeWadPickup*>(nullptr);
    };
    for (int32 Slot = CarriedItems.Num() - 1; Slot >= 0; --Slot)
    {
        const int32 Type = CarriedItems[Slot];
        if ((Type < 300 || Type > 317) && Drop(Type, Slot))
        {
            RemoveInventorySlot(Slot);
        }
    }
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(AmmoReserve); ++Index)
    {
        if (AmmoReserve[Index] > 0)
        {
            if (ANativeWadPickup* Quiver = Drop(CrossbowAmmo[Index].Quiver, Index))
            {
                Quiver->SetAuthoredAmmo(Index, AmmoReserve[Index]);
                AmmoReserve[Index] = 0;
            }
        }
    }
    if (auto* Travel = UGameInstance::GetSubsystem<UNativeWadTravelState>(GetGameInstance()))
    {
        const int64 Wallet = Travel->Quest.GetWalletCopper();
        int64 Remaining = Wallet;
        int64 Unplaced = 0;
        // CLIENT.C drops one five-coin pile per denomination and clears loose change.
        for (int32 Type = 107; Type >= 101; Type -= 2)
        {
            const int32 Unit = CoinValueCopper(Type - 1);
            const int64 Count = Remaining / Unit;
            Remaining %= Unit;
            if (Count >= 5 && !Drop(Type, Type))
            {
                Unplaced += Count * Unit;
            }
        }
        Travel->Quest.SetWalletCopper(Unplaced);
    }
}

void ANativeWadPlayer::ClickPressed()
{
    if (!bMouseLook)
    {
        if (APlayerController* LocalPlayer = Cast<APlayerController>(GetController()))
        {
            FVector2D Cursor;
            if (ANativeWadHUD* HUD = Cast<ANativeWadHUD>(LocalPlayer->GetHUD()); HUD && LocalPlayer->GetMousePosition(Cursor.X, Cursor.Y))
            {
                HUD->ClickHUD(Cursor);
            }
        }
    }
    else
    {
        UseReadyHand();
    }
}

void ANativeWadPlayer::ToggleInventory()
{
    bInventoryOpen = !bInventoryOpen;
    if (const APlayerController* LocalPlayer = Cast<APlayerController>(GetController()))
    {
        if (ANativeWadHUD* HUD = LocalPlayer->GetHUD<ANativeWadHUD>())
        {
            HUD->Form = ANativeWadHUD::EForm::Inventory;
            if (bInventoryOpen)
            {
                HUD->InventoryPage = 0;
            }
        }
    }
    if (bInventoryOpen)
    {
        bRestoreLookAfterInventory = bMouseLook;
        GetCharacterMovement()->StopMovementImmediately();
        SetMouseLook(false);
    }
    else
    {
        SetMouseLook(bRestoreLookAfterInventory);
    }
}

void ANativeWadPlayer::ToggleFinances()
{
    if (const APlayerController* LocalPlayer = GetController<APlayerController>())
    {
        if (ANativeWadHUD* HUD = LocalPlayer->GetHUD<ANativeWadHUD>(); HUD && HUD->HasClassicHud())
        {
            HUD->ToggleForm(ANativeWadHUD::EForm::Finances);
        }
    }
}

void ANativeWadPlayer::InventoryPageUp()
{
    if (const APlayerController* LocalPlayer = GetController<APlayerController>())
    {
        if (ANativeWadHUD* HUD = LocalPlayer->GetHUD<ANativeWadHUD>())
        {
            HUD->ChangeInventoryPage(-1);
        }
    }
}

void ANativeWadPlayer::InventoryPageDown()
{
    if (const APlayerController* LocalPlayer = GetController<APlayerController>())
    {
        if (ANativeWadHUD* HUD = LocalPlayer->GetHUD<ANativeWadHUD>())
        {
            HUD->ChangeInventoryPage(1);
        }
    }
}

void ANativeWadPlayer::ToggleMouseLook()
{
    if (!bInventoryOpen)
    {
        SetMouseLook(!bMouseLook);
    }
}

void ANativeWadPlayer::SetMouseLook(bool bEnabled)
{
    bMouseLook = bEnabled;
    if (APlayerController* LocalPlayer = Cast<APlayerController>(GetController()))
    {
        LocalPlayer->bShowMouseCursor = !bEnabled;
        if (bEnabled)
        {
            FInputModeGameOnly Mode;
            Mode.SetConsumeCaptureMouseDown(false);
            LocalPlayer->SetInputMode(Mode);
        }
        else
        {
            FInputModeGameAndUI Mode;
            Mode.SetHideCursorDuringCapture(false);
            Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
            LocalPlayer->SetInputMode(Mode);
        }
    }
}

void ANativeWadPlayer::LookYaw(float Axis)
{
    if (bMouseLook)
    {
        AddControllerYawInput(Axis);
    }
}

void ANativeWadPlayer::LookPitch(float Axis)
{
    if (bMouseLook)
    {
        AddControllerPitchInput(-Axis);
    }
}

bool ANativeWadPlayer::HasRune(int32 Rune) const
{
    const int32 Type = RuneItemForClass(CharacterClass, Rune);
    // Direct Editor map previews can exist before the Town Hall character has been created.
    return Type != INDEX_NONE && (!bWadCharacterCreated || GetItemCount(Type) > 0);
}

void ANativeWadPlayer::EnterRune(int32 Rune)
{
    if (!IsDefeated() && !bInventoryOpen && Runes.Len() < 4 && HasRune(Rune))
    {
        Runes.AppendChar(TCHAR(TEXT('0') + Rune));
    }
}

void ANativeWadPlayer::RuneBackspace()
{
    if (!Runes.IsEmpty())
    {
        Runes.LeftChopInline(1);
    }
}

FVector2D ANativeWadHUD::FallbackInventorySlotCenter(int32 Slot) const
{
    return InventoryBox(GetOwningPlayerController(), Slot).GetCenter();
}

void ANativeWadHUD::ClickFallbackInventory(const FVector2D& Position)
{
    ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (!Player || !Player->IsInventoryOpen())
    {
        return;
    }
    const APlayerController* Controller = GetOwningPlayerController();
    if (InventoryBox(Controller, INDEX_NONE).IsInside(Position))
    {
        Player->ClickInventorySlot(INDEX_NONE);
        return;
    }
    for (int32 Slot = 0; Slot < Player->GetCarriedItems().Num(); ++Slot)
    {
        if (InventoryBox(Controller, Slot).IsInside(Position))
        {
            Player->ClickInventorySlot(Slot);
            return;
        }
    }
}

void ANativeWadHUD::DrawFallbackHUD(const ANativeWadPlayer& FallbackPlayer)
{
    const ANativeWadPlayer* Player = &FallbackPlayer;
    const FLinearColor Ink(0.94, 0.91, 0.83);
    const FLinearColor Panel(0.045, 0.052, 0.065, 0.95);
    const FLinearColor Selected(0.20, 0.28, 0.38, 0.98);
    UFont* Font = GEngine->GetMediumFont();
    if (Player->IsMouseLook())
    {
        const float X = Canvas->ClipX * 0.5f;
        const float Y = Canvas->ClipY * 0.5f;
        DrawLine(X - 4.0f, Y, X + 4.0f, Y, Ink);
        DrawLine(X, Y - 4.0f, X, Y + 4.0f, Ink);
    }
    DrawText(FString::Printf(TEXT("Level %d   Health %d/%d   Mana %d/%d   Food %d   XP %d/%d"),
        Player->GetCharacterLevel(), Player->GetHealth(), Player->GetMaxHealth(), Player->GetMana(), Player->GetMaxMana(),
        Player->GetFood(), Player->GetExperience(), Player->GetExperienceNeeded()), Ink, 20.0f, Canvas->ClipY - 52.0f, Font);
    DrawText(FString::Printf(TEXT("Ready hand: %s   Runes: %s"), *Player->GetInventoryItemDescription(Player->GetReadyItem()), *Player->GetRunes()),
        Ink, 20.0f, Canvas->ClipY - 28.0f, Font);
    const FString Message = Player->GetInteractionMessage();
    if (!Message.IsEmpty())
    {
        DrawText(Message, Ink, 20.0f, Canvas->ClipY - 76.0f, Font);
    }
    if (!Player->IsInventoryOpen())
    {
        return;
    }
    const APlayerController* Controller = GetOwningPlayerController();
    const auto DrawSlot = [&](int32 Slot, const FString& Label)
    {
        const FBox2D Box = InventoryBox(Controller, Slot);
        const FVector2D Size = Box.GetSize();
        DrawRect(Slot != INDEX_NONE && Slot == Player->GetSelectedInventorySlot() ? Selected : Panel,
            Box.Min.X, Box.Min.Y, Size.X, Size.Y);
        DrawText(Label, Ink, Box.Min.X + 8.0, Box.Min.Y + 4.0, Font);
    };
    const FBox2D FirstSlot = InventoryBox(Controller, 0);
    DrawText(TEXT("Inventory"), Ink, FirstSlot.Min.X, FirstSlot.Min.Y - 24.0, Font);
    if (Player->GetCarriedItems().IsEmpty())
    {
        DrawSlot(0, TEXT("Empty"));
    }
    for (int32 Slot = 0; Slot < Player->GetCarriedItems().Num(); ++Slot)
    {
        DrawSlot(Slot, Player->GetInventoryItemDescription(Player->GetCarriedItems()[Slot])
            + (Slot == Player->GetReadySlot() ? TEXT(" (ready)") : TEXT("")));
    }
    DrawSlot(INDEX_NONE, FString::Printf(TEXT("Ready hand: %s"), *Player->GetInventoryItemDescription(Player->GetReadyItem())));
}
