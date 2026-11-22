#include "NativeWadPlayer.h"

#include "Algo/Find.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
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

FString ANativeWadPlayer::GetInventoryItemDescription(int32 Type) const
{
    if (const FWadInventoryDetails* Details = GetItemRules(Type); Details && !Details->Description.IsEmpty())
    {
        return Details->Description;
    }
    return FallbackItemName(Type);
}

const FWadInventoryDetails* ANativeWadPlayer::GetItemRules(int32 Type) const
{
    const UNativeWadHudData* Catalog = Type > 0 ? UNativeWadHudData::LoadCatalog() : nullptr;
    return Catalog ? Catalog->ItemDetails.Find(Type) : nullptr;
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
    return true;
}

int32 ANativeWadPlayer::GetEquippedWeapon() const
{
    const int32 Type = GetReadyItem();
    const FWadInventoryDetails* Details = GetItemRules(Type);
    return Type == IronDagger || (Details && Details->WeaponDamage > 0 && Details->HitFrame > 0) ? Type : 0;
}

bool ANativeWadPlayer::IsWornInventorySlot(int32 Slot) const
{
    return Slot >= 0 && Algo::Find(WornSlots, Slot) != nullptr;
}

int32 ANativeWadPlayer::GetArmorRating() const
{
    const auto Rating = [&](int32 Location)
    {
        const int32 Slot = WornSlots[Location];
        const FWadInventoryDetails* Rules = CarriedItems.IsValidIndex(Slot) ? GetItemRules(CarriedItems[Slot]) : nullptr;
        return Rules ? Rules->ArmorValue : 0;
    };
    const int32 Shield = Rating(ShieldSlot);
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

void ANativeWadPlayer::ClickInventorySlot(int32 Slot)
{
    if (!bInventoryOpen)
    {
        return;
    }
    if (Slot == INDEX_NONE)
    {
        const int32 Selected = SelectedInventorySlot;
        SelectedInventorySlot = INDEX_NONE;
        const FWadInventoryDetails* Rules = CarriedItems.IsValidIndex(Selected) ? GetItemRules(CarriedItems[Selected]) : nullptr;
        if (Rules && Rules->ArmorSlot >= 0 && Rules->ArmorSlot < EquipmentSlotCount)
        {
            EquipArmor(Selected, *Rules);
            return;
        }
        ReadySlot = Selected;
        ShowInteractionMessage(FString::Printf(TEXT("Ready hand: %s"), *GetInventoryItemDescription(GetReadyItem())));
    }
    else if (CarriedItems.IsValidIndex(Slot))
    {
        SelectedInventorySlot = Slot == SelectedInventorySlot ? INDEX_NONE : Slot;
    }
}

void ANativeWadPlayer::UseReadyHand()
{
    if (Health <= 0 || bInventoryOpen)
    {
        return;
    }
    const int32 Type = GetReadyItem();
    if (Type == 0 || GetEquippedWeapon() != 0)
    {
        AttackPressed();
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

void ANativeWadPlayer::UseConsumable(const FWadInventoryDetails& Details)
{
    const int32 Type = GetReadyItem();
    const int32 BeforeHealth = Health;
    const int32 BeforeFood = Food;
    Health = FMath::Clamp(Health + Details.Health, 0, MaxHealth);
    Mana = FMath::Clamp(Mana + Details.Mana, 0, MaxMana);
    Food = FMath::Clamp(Food + Details.Food, 0, MaxFood);
    Water = FMath::Clamp(Water + Details.Water, 0, MaxFood);
    PoisonLevel /= FMath::Max(1, Details.PoisonDivisor);
    if (Details.bConsumed)
    {
        RemoveInventorySlot(ReadySlot);
    }
    if (Health == 0)
    {
        ReceiveEncounterHit(0);
    }
    else
    {
        ShowInteractionMessage(Type == Berry ? FString(TEXT("You get berry juice all over your face."))
            : FString::Printf(TEXT("Used %s"), *GetInventoryItemDescription(Type)));
    }
    UE_LOG(LogTemp, Display, TEXT("Used WAD item %d. Health %d -> %d, food %d -> %d."), Type, BeforeHealth, Health, BeforeFood, Food);
}

void ANativeWadPlayer::DropInventoryItem()
{
    const int32 Slot = bInventoryOpen && SelectedInventorySlot != INDEX_NONE ? SelectedInventorySlot : ReadySlot;
    ANativeWadLevel* Level = NativeLevel.Get();
    if (Health <= 0 || IsParalyzed() || !CarriedItems.IsValidIndex(Slot) || !Level || !GetCharacterMovement()->IsMovingOnGround())
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
    SelectedInventorySlot = INDEX_NONE;
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

void ANativeWadPlayer::EnterRuneFour()
{
    if (!bInventoryOpen && Runes.Len() < 4)
    {
        Runes.AppendChar(TEXT('4'));
    }
}

void ANativeWadPlayer::EnterRuneFive()
{
    if (!bInventoryOpen && Runes.Len() < 4)
    {
        Runes.AppendChar(TEXT('5'));
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
