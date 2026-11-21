#include "NativeWadPlayer.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
    using namespace NativeWadRules;

    const TCHAR* ItemName(int32 Type)
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
        default: return Type > 0 ? TEXT("Item") : TEXT("Empty");
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

bool ANativeWadPlayer::ConsumeItem(int32 Type)
{
    const int32 Slot = CarriedItems.Find(Type);
    if (Slot == INDEX_NONE)
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
    return true;
}

int32 ANativeWadPlayer::GetEquippedWeapon() const
{
    return GetReadyItem() == IronDagger ? IronDagger : 0;
}

void ANativeWadPlayer::ClickInventorySlot(int32 Slot)
{
    if (!bInventoryOpen)
    {
        return;
    }
    if (Slot == INDEX_NONE)
    {
        ReadySlot = SelectedInventorySlot;
        SelectedInventorySlot = INDEX_NONE;
        ShowInteractionMessage(FString::Printf(TEXT("Ready hand: %s"), ItemName(GetReadyItem())));
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
    switch (GetReadyItem())
    {
    case Berry:
        ConsumeBerry();
        break;
    case 0:
    case IronDagger:
        AttackPressed();
        break;
    }
}

void ANativeWadPlayer::ConsumeBerry()
{
    CarriedItems.RemoveAt(ReadySlot);
    ReadySlot = INDEX_NONE;
    const int32 Before = Food;
    Food = FMath::Min(MaxFood, Food + BerryFood);
    ShowInteractionMessage(TEXT("You get berry juice all over your face."));
    UE_LOG(LogTemp, Display, TEXT("Consumed WAD berry. Food %d -> %d."), Before, Food);
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
        AddControllerPitchInput(Axis);
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
    DrawText(FString::Printf(TEXT("Ready hand: %s   Runes: %s"), ItemName(Player->GetReadyItem()), *Player->GetRunes()),
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
        DrawSlot(Slot, FString(ItemName(Player->GetCarriedItems()[Slot])) + (Slot == Player->GetReadySlot() ? TEXT(" (ready)") : TEXT("")));
    }
    DrawSlot(INDEX_NONE, FString::Printf(TEXT("Ready hand: %s"), ItemName(Player->GetReadyItem())));
}
