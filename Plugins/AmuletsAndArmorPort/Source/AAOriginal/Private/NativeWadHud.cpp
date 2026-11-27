#include "NativeWadPlayer.h"

#include "Engine/Canvas.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "NativeWadHudData.h"
#include "NativeWadInventoryView.h"
#include "NativeWadNotes.h"
#include "NativeWadOptions.h"
#include "NativeWadLook.h"
#include "Materials/MaterialInterface.h"
#include "TextureResource.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    using NativeWadInventoryView::Ready;
    using NativeWadInventoryView::ArmorBoxes;
    using NativeWadInventoryView::JewelryBoxes;
    using NativeWadInventoryView::SlotBox;
    using NativeWadInventoryView::SlotsPerPage;
    using FInventoryView = NativeWadInventoryView::FView;
    using FInventoryCell = NativeWadInventoryView::FCell;

    constexpr int32 Width = 320;
    constexpr int32 Height = 200;
    constexpr uint8 White = 31;
    const FBox2D ViewOpening({4, 3}, {316, 151});
    const FBox2D Use({93, 186}, {131, 196});
    const FBox2D CastButton({240, 181}, {259, 196});
    const FBox2D InventoryButton({5, 155}, {21, 168});
    const FBox2D EquipmentButton({22, 155}, {38, 168});
    const FBox2D StatisticsButton({39, 155}, {55, 168});
    const FBox2D OptionsButton({5, 169}, {21, 182});
    const FBox2D FinancesButton({39, 169}, {55, 182});
    const FBox2D AmmoButton({5, 183}, {21, 196});
    const FBox2D NotesButton({22, 183}, {38, 196});
    const FBox2D JournalButton({39, 183}, {55, 196});
    const FBox2D NotesField({214, 16}, {306, 136});
    const FBox2D NotesUp({308, 16}, {315, 25});
    const FBox2D NotesDown({308, 128}, {315, 137});
    const FBox2D Close({304, 4}, {315, 15});
    const FBox2D PreviousPage({267, 129}, {277, 137});
    const FBox2D NextPage({277, 129}, {287, 137});
    const FBox2D BagEquipmentButton({214, 140}, {315, 149});
    const FBox2D FinancesPanel({214, 18}, {315, 147});
    const TCHAR* const CoinButtons[][2] = {
        {TEXT("PGET1"), TEXT("PGET5")}, {TEXT("GGET1"), TEXT("GGET5")},
        {TEXT("SGET1"), TEXT("SGET5")}, {TEXT("CGET1"), TEXT("CGET5")},
    };
    constexpr int32 RuneCount = 9;

    FVector2D CoinButtonPosition(int32 Row, int32 Column)
    {
        return {215.0 + 12.0 * Column, 32.0 + 12.0 * Row};
    }

    FBox2D RuneButton(int32 Index)
    {
        const FVector2D Corner(265 + 17 * (Index % 3), 183 - 14 * (Index / 3));
        return {Corner, Corner + FVector2D(16, 13)};
    }

    FName RunePicture(int32 Index, NativeWadRules::ESpellSchool School, bool bPressed)
    {
        static const TArray<FName> Pictures = []
        {
            TArray<FName> Names;
            for (const TCHAR* Prefix : {TEXT("RUNBUT"), TEXT("RUDBUT")})
            {
                for (int32 Art = 0; Art < 2 * RuneCount; ++Art)
                {
                    Names.Emplace(*FString::Printf(TEXT("%s%02d"), Prefix, Art));
                }
            }
            return Names;
        }();
        using NativeWadRules::ESpellSchool;
        const int32 Offset = School == ESpellSchool::Cleric ? RuneCount : School == ESpellSchool::Arcane && Index >= 4 ? 5 : 0;
        return Pictures[Index + Offset + (bPressed ? 2 * RuneCount : 0)];
    }

    struct FJournalButton
    {
        const TCHAR* Picture;
        int32 X;
        int32 Pages;
    };
    constexpr FJournalButton JournalButtons[] = {
        {TEXT("RWND"), 214, -5}, {TEXT("LTARROW"), 224, -1}, {TEXT("RTARROW"), 296, 1}, {TEXT("FFWD"), 306, 5}
    };

    struct FHudLayout
    {
        FHudLayout(double ViewWidth, double ViewHeight)
            : Scale(FMath::Min(ViewWidth / Width, ViewHeight / Height))
            , Origin((ViewWidth - Width * Scale) * 0.5, (ViewHeight - Height * Scale) * 0.5)
        {
        }

        FVector2D At(FVector2D Point) const { return Origin + Point * Scale; }

        double Scale;
        FVector2D Origin;
    };

    FHudLayout InputLayout(const APlayerController* Controller)
    {
        int32 X = 0;
        int32 Y = 0;
        if (Controller)
        {
            Controller->GetViewportSize(X, Y);
        }
        return FHudLayout(X, Y);
    }

    struct FHudCanvas
    {
        UCanvas& Canvas;
        const UNativeWadHudData& Data;
        FHudLayout Layout;

        FLinearColor Color(uint8 Index) const
        {
            return FLinearColor::FromSRGBColor(Data.Palette[Index]);
        }

        void Rect(FBox2D Box, FLinearColor Ink) const
        {
            FCanvasTileItem Tile(Layout.At(Box.Min), Box.GetSize() * Layout.Scale, Ink);
            Tile.BlendMode = SE_BLEND_Translucent;
            Canvas.DrawItem(Tile);
        }

        void Line(FVector2D From, FVector2D To, FLinearColor Ink) const
        {
            FCanvasLineItem Item(Layout.At(From), Layout.At(To));
            Item.SetColor(Ink);
            Item.LineThickness = Layout.Scale;
            Canvas.DrawItem(Item);
        }

        void Texture(const UTexture2D& Image, FBox2D Box, FVector2D UV = {0, 0}, FVector2D UVEnd = {1, 1}) const
        {
            if (const FTexture* Resource = Image.GetResource())
            {
                FCanvasTileItem Tile(Layout.At(Box.Min), Resource, Box.GetSize() * Layout.Scale, UV, UVEnd, FLinearColor::White);
                Tile.BlendMode = SE_BLEND_Translucent;
                Canvas.DrawItem(Tile);
            }
        }

        void Picture(FName Name, FVector2D At) const
        {
            if (const UTexture2D* Image = Data.Pictures.FindRef(Name))
            {
                Texture(*Image, {At, At + FVector2D(Image->GetSizeX(), Image->GetSizeY())});
            }
        }

        void Frame() const
        {
            const UTexture2D* Background = Data.Pictures.FindRef(TEXT("MAINBACK"));
            if (!Background)
            {
                return;
            }
            // Draw around the original view opening so the native camera remains visible through it.
            for (const FBox2D& Border : {
                FBox2D({0, 0}, {Width, ViewOpening.Min.Y}), FBox2D({0, ViewOpening.Max.Y}, {Width, Height}),
                FBox2D({0, ViewOpening.Min.Y}, {ViewOpening.Min.X, ViewOpening.Max.Y}),
                FBox2D({ViewOpening.Max.X, ViewOpening.Min.Y}, {Width, ViewOpening.Max.Y})})
            {
                Texture(*Background, Border, Border.Min / FVector2D(Width, Height), Border.Max / FVector2D(Width, Height));
            }
        }

        void Text(const FString& Value, int32 X, int32 Y, int32 AvailableWidth, int32 Rows = 1, bool bCenter = false) const
        {
            DrawWadBitmapText(Canvas, Data.Tiny, Data.Palette, Value, Layout.At({double(X), double(Y)}),
                Layout.Scale, AvailableWidth, Rows, 0, bCenter, White);
        }

        void Item(int32 Type, FBox2D Box) const
        {
            if (const UTexture2D* Image = Data.Items.FindRef(Type))
            {
                const FVector2D NaturalSize(Image->GetSizeX(), Image->GetSizeY());
                const FVector2D Room = Box.GetSize() - FVector2D(2, 2);
                const FVector2D Size = NaturalSize * FMath::Min3(1.0, Room.X / NaturalSize.X, Room.Y / NaturalSize.Y);
                const FVector2D Start = Box.GetCenter() - Size * 0.5;
                Texture(*Image, {Start, Start + Size});
            }
            else if (Type > 0)
            {
                Text(TEXT("?"), int32(Box.Min.X), int32(Box.GetCenter().Y) - 3, int32(Box.GetSize().X), 1, true);
            }
        }

        void Gauges(const ANativeWadPlayer& Player, double Time) const
        {
            const double Fractions[] = {
                double(Player.GetHealth()) / Player.GetMaxHealth(),
                double(Player.GetMana()) / Player.GetMaxMana(),
                double(Player.GetFood()) / NativeWadRules::MaxFood,
                double(Player.GetWater()) / NativeWadRules::MaxFood
            };
            const int32 Frame = FMath::FloorToInt(Time * (70.0 / 9.0)) % 8;
            Picture(FName(*FString::Printf(TEXT("POTION%d"), Frame)), {61, 155});
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fractions); ++Index)
            {
                const double Depth = FMath::FloorToDouble((1.0 - FMath::Clamp(Fractions[Index], 0.0, 1.0)) * 35.0);
                const double X = 65.0 + Index * 7.0;
                Rect({{X, 157}, {X + 2, 158 + Depth}}, Color(77));
                Rect({{X - 1, 157}, {X + 3, 158}}, Color(77));
                if (Depth > 32.0)
                {
                    Rect({{X - 1, 189}, {X + 3, 190 + (Depth - 32)}}, Color(77));
                }
            }

            // BANNER.C sets the mana display's palette entry to a dark red once the startup fade settles.
            Rect({{177, 156}, {258, 179}}, FLinearColor::FromSRGBColor(FColor(80, 0, 0)));
            const double Mana = FMath::Clamp(Fractions[1], 0.0, 1.0);
            for (int32 Trail = 3; Trail >= 0; --Trail)
            {
                const auto Wave = [&](int32 X)
                {
                    return FVector2D(177 + X, 167 + FMath::RoundToInt(FMath::Sin(X * 0.29 - Time * 10.0 + Trail * 0.22) * Mana * 9));
                };
                const FLinearColor Ink = Color(Trail == 0 && Mana > 0.9 ? 32 : 38 + Trail);
                for (int32 X = 0; X < 80; X += 5)
                {
                    Line(Wave(X), Wave(X + 5), Ink);
                }
            }
        }
    };
}

ANativeWadHUD::ANativeWadHUD()
{
    static ConstructorHelpers::FObjectFinderOptional<UNativeWadHudData> Art(UNativeWadHudData::ObjectPath);
    ClassicData = Art.Get();
}

void ANativeWadHUD::BeginPlay()
{
    Super::BeginPlay();
    if (!ClassicData)
    {
        // The editor can have loaded this class before regenerating the original art.
        ClassicData = LoadObject<UNativeWadHudData>(nullptr, UNativeWadHudData::ObjectPath);
    }
    if (ClassicData && GetOwningPlayerController())
    {
        EnableInput(GetOwningPlayerController());
        InputComponent->BindKey(FInputChord(EKeys::NumPadTwo, EModifierKey::Alt), IE_Pressed, this, &ANativeWadHUD::ToggleNotes);
        InputComponent->BindKey(FInputChord(EKeys::NumPadThree, EModifierKey::Alt), IE_Pressed, this, &ANativeWadHUD::ToggleJournal);
        InputComponent->BindKey(FInputChord(EKeys::NumPadFour, EModifierKey::Alt), IE_Pressed, this, &ANativeWadHUD::ToggleOptions);
        InputComponent->BindKey(EKeys::F1, IE_Pressed, this, &ANativeWadHUD::ToggleControls);
    }
}

void ANativeWadHUD::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    SyncNotesInput();
    if (DraggedOptionSlider != INDEX_NONE)
    {
        auto* Controller = GetOwningPlayerController();
        if (!IsOptionsOpen() || !Controller || !Controller->IsInputKeyDown(EKeys::LeftMouseButton))
        { DraggedOptionSlider = INDEX_NONE; }
        else
        {
            FVector2D Cursor;
            if (Controller->GetMousePosition(Cursor.X, Cursor.Y)) { UpdateOptionsSlider(Cursor); }
        }
    }
}

void ANativeWadHUD::EndPlay(const EEndPlayReason::Type Reason)
{
    ReleaseNotesInput(false);
    Super::EndPlay(Reason);
}

bool ANativeWadHUD::IsNotesOpen() const
{
    const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    return Player && Player->IsInventoryOpen() && Form == EForm::Notes;
}

bool ANativeWadHUD::IsOptionsOpen() const
{
    const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    return Player && Player->IsInventoryOpen() && Form == EForm::Options;
}

bool ANativeWadHUD::IsControlsOpen() const
{
    const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    return Player && Player->IsInventoryOpen() && Form == EForm::Controls;
}

bool ANativeWadHUD::IsLookOpen() const
{
    const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    return Player && Player->IsInventoryOpen() && Form == EForm::Look;
}

bool ANativeWadHUD::ExamineAt(const FVector2D& ViewportPoint)
{
    auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    auto* Controller = GetOwningPlayerController();
    const auto Layout = InputLayout(Controller);
    if (!Player || !Controller || !ClassicData || Player->IsMouseLook() || Layout.Scale <= 0.0) { return false; }
    const FVector2D Point = (ViewportPoint - Layout.Origin) / Layout.Scale;
    const FBox2D VisibleView(ViewOpening.Min, {Player->IsInventoryOpen() ? 205.0 : ViewOpening.Max.X, ViewOpening.Max.Y});
    if (!VisibleView.IsInside(Point)) { return false; }
    FVector Origin, Direction;
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(WadCreatureLook), true, Player);
    const bool bHit = Controller->DeprojectScreenPositionToWorld(ViewportPoint.X, ViewportPoint.Y, Origin, Direction)
        && GetWorld()->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 20000.0, ECC_Visibility, Query);
    const auto* Creature = bHit ? Cast<ANativeWadGuardian>(Hit.GetActor()) : nullptr;
    if (!Creature || Creature->IsHidden())
    {
        Player->ShowInteractionMessage(TEXT("I don't see anything special there."));
        return false;
    }
    LookSourceType = Creature->GetLookSourceType();
    const auto* Description = ClassicData->CreatureDescriptions.Find(LookSourceType);
    LookName = Description ? Description->Name : TEXT("Unknown Creature");
    LookDescription = Description ? Description->Description : TEXT("No other information available.");
    LookPortrait = Creature->GetPortraitMaterial();
    LookPortraitSize = Creature->GetPortraitSize();
    LookScroll = 0;
    if (!IsLookOpen()) { ToggleForm(EForm::Look); }
    return true;
}

int32 ANativeWadHUD::GetNotesCursor() const { return NotesEditor ? NotesEditor->GetCursor() : 0; }
int32 ANativeWadHUD::GetNotesScroll() const { return NotesEditor ? NotesEditor->GetScroll() : 0; }

void ANativeWadHUD::StoreNotes()
{
    if (NotesEditor && GetGameInstance())
    {
        if (auto* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>())
        { Travel->PlayerNotes = NotesEditor->GetText(); }
    }
}

void ANativeWadHUD::ReleaseNotesInput(bool bRestoreInput)
{
    StoreNotes();
    if (NotesInputWidget)
    {
        if (GetGameInstance())
        {
            if (auto* Viewport = GetGameInstance()->GetGameViewportClient())
            { Viewport->RemoveViewportWidgetContent(NotesInputWidget.ToSharedRef()); }
        }
        NotesInputWidget.Reset();
        if (bRestoreInput)
        {
            if (auto* Controller = GetOwningPlayerController())
            {
                Controller->FlushPressedKeys();
                const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
                if (Player && Player->IsMouseLook())
                {
                    FInputModeGameOnly Input;
                    Input.SetConsumeCaptureMouseDown(false);
                    Controller->SetInputMode(Input);
                }
                else
                {
                    FInputModeGameAndUI Input;
                    Input.SetHideCursorDuringCapture(false);
                    Input.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
                    Controller->SetInputMode(Input);
                }
            }
            if (FSlateApplication::IsInitialized()) { FSlateApplication::Get().SetAllUserFocusToGameViewport(); }
        }
    }
    NotesEditor.Reset();
}

void ANativeWadHUD::SyncNotesInput()
{
    if (!IsNotesOpen())
    {
        if (NotesEditor || NotesInputWidget) { ReleaseNotesInput(true); }
        return;
    }
    if (!NotesEditor && ClassicData)
    {
        const auto* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
        NotesEditor = MakeShared<FNativeWadNotesEditor>(ClassicData->Tiny, Travel ? Travel->PlayerNotes : FString());
        StoreNotes();
    }
    if (NotesEditor && !NotesInputWidget && FSlateApplication::IsInitialized())
    {
        if (auto* Viewport = GetGameInstance()->GetGameViewportClient())
        {
            NotesInputWidget = MakeNativeWadNotesInput(*this);
            Viewport->AddViewportWidgetContent(NotesInputWidget.ToSharedRef(), 20);
            if (auto* Controller = GetOwningPlayerController())
            {
                Controller->FlushPressedKeys();
                FInputModeGameAndUI Input;
                Input.SetWidgetToFocus(NotesInputWidget);
                Input.SetHideCursorDuringCapture(false);
                Input.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
                Controller->SetInputMode(Input);
            }
            FSlateApplication::Get().SetKeyboardFocus(NotesInputWidget);
        }
    }
}

void ANativeWadHUD::HandleNotesCharacter(TCHAR Character)
{
    if (IsNotesOpen() && NotesEditor && NotesEditor->Insert(Character)) { StoreNotes(); }
}

void ANativeWadHUD::HandleNotesKey(const FKey& Key, bool bControl, bool bAlt)
{
    if (!IsNotesOpen() || !NotesEditor) { return; }
    if (Key == EKeys::Escape) { ToggleForm(EForm::Notes); return; }
    if (Key == EKeys::F1) { ToggleControls(); return; }
    if (bAlt && Key == EKeys::NumPadTwo) { ToggleNotes(); return; }
    if (bAlt && Key == EKeys::NumPadThree) { ToggleJournal(); return; }
    if (bAlt && Key == EKeys::NumPadFour) { ToggleOptions(); return; }
    if (NotesEditor->Key(Key, bControl)) { StoreNotes(); }
}

void ANativeWadHUD::ToggleNotes() { ToggleForm(EForm::Notes); }
void ANativeWadHUD::ToggleJournal() { ToggleForm(EForm::Journal); }
void ANativeWadHUD::ToggleOptions() { ToggleForm(EForm::Options); }
void ANativeWadHUD::ToggleControls() { ToggleForm(EForm::Controls); }

void ANativeWadHUD::ScrollClassicText(int32 Delta)
{
    if (!ClassicData || (!IsOptionsOpen() && !IsControlsOpen() && !IsLookOpen())) { return; }
    const bool bOptions = IsOptionsOpen();
    const bool bLook = IsLookOpen();
    const FString& Text = bLook ? LookDescription : bOptions ? NativeWadOptions::Credits() : NativeWadOptions::Controls();
    const int32 Rows = (bLook ? 45 : bOptions ? 75 : 132) / FMath::Max(1, ClassicData->Tiny.Height);
    int32& Scroll = bLook ? LookScroll : bOptions ? OptionsScroll : ControlsScroll;
    Scroll = FMath::Clamp(Scroll + Delta, 0, FMath::Max(0, NativeWadOptions::TextLineCount(Text, ClassicData->Tiny, 92) - Rows));
}

void ANativeWadHUD::UpdateOptionsSlider(FVector2D ViewportPoint)
{
    if (!IsOptionsOpen() || DraggedOptionSlider == INDEX_NONE) { return; }
    const auto Layout = InputLayout(GetOwningPlayerController());
    if (Layout.Scale <= 0.0) { return; }
    const double X = ((ViewportPoint - Layout.Origin) / Layout.Scale).X;
    // SLIDR.C positions the nine-pixel knob from x216 to300, with the mouse at its center.
    const float Volume = float(FMath::Clamp((X - 220.0) / 84.0, 0.0, 1.0));
    auto Preferences = NativeWadOptions::GetAudioPreferences();
    if (DraggedOptionSlider == 0) { Preferences.MusicVolume = Volume; }
    else { Preferences.EffectsVolume = Volume; }
    NativeWadOptions::SetAudioPreferences(Preferences);
}

void ANativeWadHUD::ScrollNotes(int32 Delta)
{
    if (IsNotesOpen() && NotesEditor) { NotesEditor->ScrollLines(Delta); }
}

FVector2D ANativeWadHUD::InventorySlotCenter(int32 Slot) const
{
    const ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (!ClassicData || !Player)
    {
        return FallbackInventorySlotCenter(Slot);
    }
    const int32 Cell = Slot == INDEX_NONE ? INDEX_NONE : FInventoryView(*Player, *ClassicData).CellForSlot(Slot);
    return InputLayout(GetOwningPlayerController()).At(SlotBox(Cell).GetCenter());
}

void ANativeWadHUD::ChangeInventoryPage(int32 Offset)
{
    if (IsNotesOpen()) { ScrollNotes(Offset * (NotesEditor ? NotesEditor->GetVisibleRows() : 1)); return; }
    if (IsOptionsOpen() || IsControlsOpen() || IsLookOpen())
    {
        const int32 Rows = (IsLookOpen() ? 45 : IsOptionsOpen() ? 75 : 132) / FMath::Max(1, ClassicData->Tiny.Height);
        ScrollClassicText(Offset * Rows);
        return;
    }
    const ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (ClassicData && Player && Player->IsInventoryOpen() && Form == EForm::Inventory)
    {
        InventoryPage = FMath::Clamp(InventoryPage + Offset, 0, FInventoryView(*Player, *ClassicData).PageCount() - 1);
    }
}

void ANativeWadHUD::ToggleForm(EForm Requested)
{
    if (ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn()))
    {
        if (!Player->IsInventoryOpen() || Form == Requested)
        {
            Player->ToggleInventory();
        }
        Form = Player->IsInventoryOpen() ? Requested : EForm::Inventory;
        DraggedOptionSlider = INDEX_NONE;
        if (Form == EForm::Options) { OptionsScroll = 0; }
        if (Form == EForm::Controls) { ControlsScroll = 0; }
        SyncNotesInput();
    }
}

void ANativeWadHUD::ClickHUD(const FVector2D& Position)
{
    if (!ClassicData)
    {
        ClickFallbackInventory(Position);
        return;
    }
    ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    const FHudLayout Layout = InputLayout(GetOwningPlayerController());
    if (!Player || Layout.Scale <= 0.0)
    {
        return;
    }
    const FVector2D Point = (Position - Layout.Origin) / Layout.Scale;
    const auto* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    const bool bHasJournal = Travel && !Travel->JournalPages.IsEmpty();
    if (InventoryButton.IsInside(Point) || EquipmentButton.IsInside(Point) || StatisticsButton.IsInside(Point)
        || OptionsButton.IsInside(Point) || FinancesButton.IsInside(Point) || AmmoButton.IsInside(Point) || NotesButton.IsInside(Point)
        || JournalButton.IsInside(Point))
    {
        const EForm Requested = InventoryButton.IsInside(Point) ? EForm::Inventory : EquipmentButton.IsInside(Point) ? EForm::Equipment
            : StatisticsButton.IsInside(Point) ? EForm::Statistics : OptionsButton.IsInside(Point) ? EForm::Options : FinancesButton.IsInside(Point) ? EForm::Finances
            : AmmoButton.IsInside(Point) ? EForm::Ammo : NotesButton.IsInside(Point) ? EForm::Notes : EForm::Journal;
        ToggleForm(Requested);
        return;
    }
    if (!Player->IsInventoryOpen())
    {
        if (Use.IsInside(Point))
        {
            Player->UseReadyHand();
        }
        else if (CastButton.IsInside(Point))
        {
            Player->CastRejuvenate();
        }
        else
        {
            for (int32 Index = 0; Index < RuneCount; ++Index)
            {
                if (RuneButton(Index).IsInside(Point))
                {
                    Player->EnterRune(Index + 1);
                    break;
                }
            }
        }
        return;
    }
    if (Close.IsInside(Point))
    {
        Player->ToggleInventory();
        SyncNotesInput();
        return;
    }
    if ((Form == EForm::Notes || Form == EForm::Journal) && BagEquipmentButton.IsInside(Point))
    {
        Form = Form == EForm::Notes ? EForm::Journal : EForm::Notes;
        SyncNotesInput();
        return;
    }
    if (Form == EForm::Notes)
    {
        if (NotesUp.IsInside(Point)) { ScrollNotes(-1); }
        else if (NotesDown.IsInside(Point)) { ScrollNotes(1); }
        else if (NotesField.IsInside(Point) && NotesEditor) { NotesEditor->PlaceCursor(Point - NotesField.Min); }
        return;
    }
    if (Form == EForm::Options)
    {
        auto Preferences = NativeWadOptions::GetAudioPreferences();
        if (FBox2D({217, 19}, {227, 27}).IsInside(Point))
        { Preferences.bMusic = !Preferences.bMusic; NativeWadOptions::SetAudioPreferences(Preferences); }
        else if (FBox2D({217, 44}, {227, 52}).IsInside(Point))
        { Preferences.bEffects = !Preferences.bEffects; NativeWadOptions::SetAudioPreferences(Preferences); }
        else if (FBox2D({216, 31}, {309, 42}).IsInside(Point))
        { DraggedOptionSlider = 0; UpdateOptionsSlider(Position); }
        else if (FBox2D({216, 56}, {309, 67}).IsInside(Point))
        { DraggedOptionSlider = 1; UpdateOptionsSlider(Position); }
        else if (FBox2D({308, 73}, {315, 82}).IsInside(Point)) { ScrollClassicText(-1); }
        else if (FBox2D({308, 140}, {315, 149}).IsInside(Point)) { ScrollClassicText(1); }
        return;
    }
    if (Form == EForm::Controls)
    {
        if (FBox2D({308, 16}, {315, 25}).IsInside(Point)) { ScrollClassicText(-1); }
        else if (FBox2D({308, 140}, {315, 149}).IsInside(Point)) { ScrollClassicText(1); }
        return;
    }
    if (Form == EForm::Look)
    {
        if (FBox2D({308, 102}, {315, 111}).IsInside(Point)) { ScrollClassicText(-1); }
        else if (FBox2D({308, 139}, {315, 148}).IsInside(Point)) { ScrollClassicText(1); }
        return;
    }
    if ((Form == EForm::Inventory || Form == EForm::Equipment) && BagEquipmentButton.IsInside(Point))
    {
        Form = Form == EForm::Inventory ? EForm::Equipment : EForm::Inventory;
        return;
    }
    if (Form == EForm::Equipment)
    {
        for (int32 Location = 0; Location < UE_ARRAY_COUNT(ArmorBoxes); ++Location)
        {
            const int32 Slot = Player->WornSlots[Location];
            if (ArmorBoxes[Location].IsInside(Point) && Player->GetCarriedItems().IsValidIndex(Slot))
            {
                if (const FWadInventoryDetails* Item = Player->GetItemRules(Player->GetCarriedItems()[Slot]))
                {
                    Player->EquipArmor(Slot, *Item);
                    break;
                }
            }
        }
        for (int32 Location = 0; Location < UE_ARRAY_COUNT(JewelryBoxes); ++Location)
        {
            if (!JewelryBoxes[Location].IsInside(Point)) { continue; }
            Player->ClickJewelrySlot(Location);
            break;
        }
        return;
    }
    if (Form == EForm::Journal)
    {
        for (const FJournalButton& Button : JournalButtons)
        {
            if (bHasJournal && FBox2D({double(Button.X), 129}, {double(Button.X + 9), 137}).IsInside(Point))
            {
                JournalPage = FMath::Clamp(JournalPage + Button.Pages, 0, Travel->JournalPages.Num() - 1);
                break;
            }
        }
        return;
    }
    if (Form == EForm::Statistics)
    {
        return;
    }
    if (Form == EForm::Finances)
    {
        for (int32 Row = 0; Row < UE_ARRAY_COUNT(CoinButtons); ++Row)
        {
            for (int32 Column = 0; Column < UE_ARRAY_COUNT(CoinButtons[Row]); ++Column)
            {
                const FVector2D Start = CoinButtonPosition(Row, Column);
                if (FBox2D(Start, Start + FVector2D(11, 10)).IsInside(Point))
                {
                    Player->RetrieveCoin(106 - 2 * Row + Column);
                    return;
                }
            }
        }
        if (FinancesPanel.IsInside(Point))
        {
            Player->ReturnSelectedCoin();
        }
        return;
    }
    if (Form == EForm::Ammo)
    {
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(NativeWadRules::CrossbowAmmo); ++Index)
        {
            const double Y = 18 + Index * 19;
            if (FBox2D({214, Y + 8}, {268, Y + 17}).IsInside(Point))
            {
                Player->RetrieveAmmo(Index);
                break;
            }
            if (FBox2D({296, Y}, {315, Y + 17}).IsInside(Point))
            {
                Player->SelectAmmo(Index);
                break;
            }
        }
        return;
    }
    if (Ready.IsInside(Point))
    {
        Player->ClickInventorySlot(INDEX_NONE);
        return;
    }
    const FInventoryView View(*Player, *ClassicData);
    InventoryPage = FMath::Min(InventoryPage, View.PageCount() - 1);
    if (PreviousPage.IsInside(Point) || NextPage.IsInside(Point))
    {
        ChangeInventoryPage(PreviousPage.IsInside(Point) ? -1 : 1);
        return;
    }
    const int32 Start = InventoryPage * SlotsPerPage;
    const int32 End = FMath::Min(Start + SlotsPerPage, View.Cells.Num());
    for (int32 Cell = Start; Cell < End; ++Cell)
    {
        if (SlotBox(Cell).IsInside(Point))
        {
            Player->ClickInventorySlot(View.Cells[Cell].FirstSlot);
            return;
        }
    }
}

void ANativeWadHUD::DrawHUD()
{
    Super::DrawHUD();
    const ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (!Canvas || !Player)
    {
        return;
    }
    if (!ClassicData || ClassicData->Palette.Num() != 256)
    {
        DrawFallbackHUD(*Player);
        return;
    }
    const FHudCanvas Screen{*Canvas, *ClassicData, FHudLayout(Canvas->ClipX, Canvas->ClipY)};
    if (Screen.Layout.Origin.X > 0.0)
    {
        const float Side = Screen.Layout.Origin.X;
        DrawRect(FLinearColor::Black, 0, 0, Side, Canvas->ClipY);
        DrawRect(FLinearColor::Black, Canvas->ClipX - Side, 0, Side, Canvas->ClipY);
    }
    if (Screen.Layout.Origin.Y > 0.0)
    {
        const float Border = Screen.Layout.Origin.Y;
        DrawRect(FLinearColor::Black, 0, 0, Canvas->ClipX, Border);
        DrawRect(FLinearColor::Black, 0, Canvas->ClipY - Border, Canvas->ClipX, Border);
    }
    Screen.Frame();
    if (Player->IsMouseLook())
    {
        const float X = Canvas->ClipX * 0.5f;
        const float Y = Canvas->ClipY * 0.5f;
        DrawLine(X - 4.0f, Y, X + 4.0f, Y, FLinearColor::White);
        DrawLine(X, Y - 4.0f, X, Y + 4.0f, FLinearColor::White);
    }
    const double Time = GetWorld()->GetTimeSeconds();
    Screen.Gauges(*Player, Time);
    Screen.Item(Player->GetReadyItem(), Ready);
    const auto School = NativeWadRules::SchoolForClass(Player->GetCharacterClass());
    for (int32 Index = 0; Index < RuneCount; ++Index)
    {
        if (Player->HasRune(Index + 1)) { Screen.Picture(RunePicture(Index, School, false), RuneButton(Index).Min); }
        else { Screen.Rect(RuneButton(Index), FLinearColor::Black); }
    }
    const FString Runes = Player->GetRunes();
    for (int32 Index = 0; Index < Runes.Len(); ++Index)
    {
        const int32 Rune = Runes[Index] - '1';
        if (Rune >= 0 && Rune < RuneCount)
        {
            Screen.Picture(RunePicture(Rune, School, true), {177.0 + Index * 15.0, 182.0});
        }
    }

    int32 Weight = 0;
    bool bKnownWeight = true;
    for (const int32 Type : Player->GetCarriedItems())
    {
        if (const int32* ItemWeight = ClassicData->ItemWeights.Find(Type))
        {
            Weight += *ItemWeight;
        }
        else
        {
            bKnownWeight = false;
        }
    }
    const auto Status = [&](const FString& Text, int32 Y) { Screen.Text(Text, 140, Y, 28, 1, true); };
    Status(FString::Printf(TEXT("%d/%d"), (Player->GetHealth() + 99) / 100, (Player->GetMaxHealth() + 99) / 100), 156);
    Status(FString::Printf(TEXT("%d/%d"), (Player->GetMana() + 99) / 100, (Player->GetMaxMana() + 99) / 100), 164);
    Status(FString::Printf(TEXT("%d%%"), (Player->GetFood() + 19) / 20), 172);
    Status(FString::Printf(TEXT("%d%%"), (Player->GetWater() + 19) / 20), 180);
    const FString Load = bKnownWeight ? FString::Printf(TEXT("%.1f KG"), Weight / 10.0) : TEXT("--");
    Status(Load, 188);
    Screen.Text(Player->GetInteractionMessage(), 6, 5, Player->IsInventoryOpen() ? 198 : 306, 4);

    // Keep the original labels visible, with the forms beyond the current port greyed out.
    const FLinearColor Disabled(0, 0, 0, 0.55);
    const auto* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    const bool bHasJournal = Travel && !Travel->JournalPages.IsEmpty();
    for (int32 Index = 1; Index < 9; ++Index)
    {
        if (Index == 1 || Index == 2 || Index == 3 || Index == 5 || Index == 6 || Index == 7 || Index == 8)
        {
            continue;
        }
        const FVector2D Corner(5 + Index % 3 * 17, 155 + Index / 3 * 14);
        Screen.Rect({Corner, Corner + FVector2D(16, 13)}, Disabled);
    }
    if (!Player->IsInventoryOpen())
    {
        return;
    }
    if (Form == EForm::Look)
    {
        Screen.Picture(TEXT("VIEWBAR"), {209, 0});
        Screen.Picture(TEXT("VIEWSB"), {308, 112});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        Screen.Picture(TEXT("UPARROW"), {308, 102});
        Screen.Picture(TEXT("DNARROW"), {308, 139});
        if (LookPortrait)
        {
            const FBox2D Box = NativeWadLook::PortraitBox(LookPortraitSize);
            FCanvasTileItem Tile(Screen.Layout.At(Box.Min), LookPortrait->GetRenderProxy(), Box.GetSize() * Screen.Layout.Scale);
            Tile.BlendMode = SE_BLEND_Translucent;
            Canvas->DrawItem(Tile);
        }
        DrawWadBitmapText(*Canvas, ClassicData->Medium, ClassicData->Palette, LookName,
            Screen.Layout.At({214, 91}), Screen.Layout.Scale, 100, 1, 0, true, White);
        const int32 Rows = 45 / FMath::Max(1, ClassicData->Tiny.Height);
        DrawWadBitmapText(*Canvas, ClassicData->Tiny, ClassicData->Palette, LookDescription,
            Screen.Layout.At({214, 102}), Screen.Layout.Scale, 92, Rows, LookScroll, false, White);
        const int32 TotalRows = NativeWadOptions::TextLineCount(LookDescription, ClassicData->Tiny, 92);
        if (TotalRows > Rows)
        {
            const double Y = 112 + double(LookScroll) / (TotalRows - Rows) * 24;
            Screen.Rect({{309, Y}, {314, Y + 3}}, Screen.Color(White));
        }
        return;
    }
    if (Form == EForm::Options || Form == EForm::Controls)
    {
        const bool bOptions = Form == EForm::Options;
        Screen.Picture(bOptions ? TEXT("OPTBAR") : TEXT("CNTRLBAR"), {209, 0});
        Screen.Picture(bOptions ? TEXT("OPTSB") : TEXT("CNTRLSB"), {308, bOptions ? 83.0 : 26.0});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        Screen.Picture(TEXT("UPARROW"), {308, bOptions ? 73.0 : 16.0});
        Screen.Picture(TEXT("DNARROW"), {308, 140});
        if (bOptions)
        {
            const auto& Preferences = NativeWadOptions::GetAudioPreferences();
            Screen.Picture(Preferences.bMusic ? TEXT("CRC_TOG1") : TEXT("CRC_TOG2"), {217, 19});
            Screen.Picture(Preferences.bEffects ? TEXT("CRC_TOG1") : TEXT("CRC_TOG2"), {217, 44});
            for (int32 Slider = 0; Slider < 2; ++Slider)
            {
                const double Y = Slider == 0 ? 35 : 60;
                const float Volume = Slider == 0 ? Preferences.MusicVolume : Preferences.EffectsVolume;
                Screen.Rect({{216, Y - 4}, {309, Y + 7}}, Screen.Color(76));
                Screen.Line({216, Y}, {308, Y}, Screen.Color(213));
                Screen.Line({216, Y + 1}, {308, Y + 1}, Screen.Color(218));
                Screen.Line({217, Y + 2}, {309, Y + 2}, Screen.Color(0));
                Screen.Picture(TEXT("SLIDER"), {216.0 + FMath::RoundToInt(Volume * 84), Y - 4});
            }
        }
        const FString& Text = bOptions ? NativeWadOptions::Credits() : NativeWadOptions::Controls();
        const int32 Scroll = bOptions ? OptionsScroll : ControlsScroll;
        const int32 Rows = (bOptions ? 75 : 132) / FMath::Max(1, ClassicData->Tiny.Height);
        DrawWadBitmapText(*Canvas, ClassicData->Tiny, ClassicData->Palette, Text,
            Screen.Layout.At({214, bOptions ? 73.0 : 16.0}), Screen.Layout.Scale, 92, Rows, Scroll, false, White);
        const int32 TotalRows = NativeWadOptions::TextLineCount(Text, ClassicData->Tiny, 92);
        if (TotalRows > Rows)
        {
            const double Top = bOptions ? 83 : 26;
            const double Y = Top + double(Scroll) / (TotalRows - Rows) * (137 - Top);
            Screen.Rect({{309, Y}, {314, Y + 3}}, Screen.Color(White));
        }
        return;
    }
    if (Form == EForm::Notes)
    {
        Screen.Picture(TEXT("NTSBAR"), {209, 0});
        Screen.Picture(TEXT("NOTESSB"), {308, 26});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        Screen.Picture(TEXT("2JOURNBT"), BagEquipmentButton.Min);
        Screen.Picture(TEXT("UPARROW"), NotesUp.Min);
        Screen.Picture(TEXT("DNARROW"), NotesDown.Min);
        if (NotesEditor)
        {
            const auto& Lines = NotesEditor->GetLines();
            const FString& Text = NotesEditor->GetText();
            const int32 First = NotesEditor->GetScroll();
            const int32 End = FMath::Min(Lines.Num(), First + NotesEditor->GetVisibleRows());
            for (int32 Row = First; Row < End; ++Row)
            {
                int32 X = 0;
                const int32 Y = 16 + (Row - First) * NotesEditor->GetLineHeight();
                for (int32 Index = Lines[Row].Start; Index < Lines[Row].End; ++Index)
                {
                    const TCHAR Ch = Text[Index];
                    // Draw one literal glyph at a time, so player-entered ^123 is not interpreted as a color escape.
                    if (Ch != '\t') { Screen.Text(FString::Chr(Ch), 214 + X, Y, 8); }
                    X += NotesEditor->Advance(Ch, X);
                }
            }
            const int32 CursorRow = NotesEditor->GetCursorLine();
            if (CursorRow >= First && CursorRow < End && FMath::Fmod(GetWorld()->GetRealTimeSeconds(), 1.0) < 0.6)
            {
                const double X = 214 + NotesEditor->CursorX(CursorRow, NotesEditor->GetCursor());
                const double Y = 16 + (CursorRow - First) * NotesEditor->GetLineHeight();
                Screen.Rect({{X, Y}, {X + 1, Y + NotesEditor->GetLineHeight()}}, Screen.Color(White));
            }
            if (Lines.Num() > NotesEditor->GetVisibleRows())
            {
                const double Progress = double(First) / (Lines.Num() - NotesEditor->GetVisibleRows());
                const double Y = 26 + Progress * 98;
                Screen.Rect({{309, Y}, {314, Y + 3}}, Screen.Color(White));
            }
        }
        return;
    }
    if (Form == EForm::Equipment)
    {
        using namespace NativeWadRules;
        static_assert(UE_ARRAY_COUNT(ArmorBoxes) == EquipmentSlotCount);
        Screen.Picture(TEXT("EQPBAR"), {209, 0});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        Screen.Picture(TEXT("2INVBUTN"), BagEquipmentButton.Min);
        const TArray<int32>& Items = Player->GetCarriedItems();
        int32 Armor[EquipmentSlotCount] = {};
        for (int32 Location = 0; Location < EquipmentSlotCount; ++Location)
        {
            const int32 Slot = Player->WornSlots[Location];
            if (Items.IsValidIndex(Slot))
            {
                Screen.Item(Items[Slot], ArmorBoxes[Location]);
                if (const FWadInventoryDetails* Item = Player->GetItemRules(Items[Slot]))
                {
                    Armor[Location] = Item->ArmorValue;
                }
            }
        }
        static_assert(UE_ARRAY_COUNT(JewelryBoxes) == JewelrySlotCount);
        for (int32 Location = 0; Location < JewelrySlotCount; ++Location)
        {
            const int32 Slot = Player->GetJewelryInventorySlot(Location);
            if (Items.IsValidIndex(Slot)) { Screen.Item(Items[Slot], JewelryBoxes[Location]); }
        }
        // BAN00002.FRM lists these fields in two columns.
        const int32 Values[] = {Armor[HeadSlot], Armor[ChestSlot], Armor[LeftArmSlot], Armor[RightArmSlot],
            Armor[LegsSlot], Armor[LegsSlot], Armor[ShieldSlot], Player->GetArmorRating()};
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Values); ++Index)
        {
            Screen.Text(FString::Printf(TEXT("%d%%"), Values[Index]), Index < 4 ? 241 : 294, 99 + 9 * (Index % 4), 17, 1, true);
        }
        FVector2D Cursor;
        const int32 Selected = Player->GetSelectedInventorySlot();
        if (Items.IsValidIndex(Selected) && GetOwningPlayerController()->GetMousePosition(Cursor.X, Cursor.Y))
        {
            Cursor = (Cursor - Screen.Layout.Origin) / Screen.Layout.Scale;
            Screen.Item(Items[Selected], {Cursor + FVector2D(1, 1), Cursor + FVector2D(19, 19)});
        }
        return;
    }
    if (Form == EForm::Journal)
    {
        Screen.Picture(TEXT("JNLBAR"), {209, 0});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        Screen.Picture(TEXT("2NOTESBT"), BagEquipmentButton.Min);
        if (!bHasJournal)
        {
            Screen.Text(TEXT("NO PAGES"), 234, 129, 60, 1, true);
            return;
        }
        TArray<int32, TInlineAllocator<8>> Pages;
        for (const auto& [Id, Image] : Travel->JournalPages)
        {
            Pages.Add(Id);
        }
        Pages.Sort();
        JournalPage = FMath::Clamp(JournalPage, 0, Pages.Num() - 1);
        const auto& Page = Travel->JournalPages.FindChecked(Pages[JournalPage]);
        if (Page.Get() != JournalImage || !JournalImage)
        {
            JournalImage = Page.LoadSynchronous();
        }
        if (JournalImage)
        {
            const FVector2D Start(214, 16);
            Screen.Texture(*JournalImage, {Start, Start + FVector2D(JournalImage->GetSizeX(), JournalImage->GetSizeY())});
        }
        for (const FJournalButton& Button : JournalButtons)
        {
            Screen.Picture(Button.Picture, {double(Button.X), 129});
        }
        Screen.Text(FString::Printf(TEXT("PG %d/%d"), JournalPage + 1, Pages.Num()), 252, 129, 43, 1, true);
        return;
    }
    if (Form == EForm::Statistics)
    {
        using namespace NativeWadRules;
        constexpr EAttribute StatRows[] = {StrengthAttribute, ConstitutionAttribute, AccuracyAttribute,
            StealthAttribute, MagicAttribute, SpeedAttribute};
        constexpr const TCHAR* PriestTitles[] = {
            TEXT("Flock Tender"), TEXT("Student"), TEXT("Scribe"), TEXT("Teacher"), TEXT("Evangelist"),
            TEXT("Cleric"), TEXT("Elder"), TEXT("The Annointed"), TEXT("Parable Master"), TEXT("Master of Canon"),
            TEXT("Bishop"), TEXT("Minor Prophet"), TEXT("Cross Bearer"), TEXT("Disciple"), TEXT("Sacred Branch"),
            TEXT("Witness"), TEXT("Apostle"), TEXT("Saint"), TEXT("Herald of Glory"), TEXT("Illuminated One")
        };
        Screen.Picture(TEXT("STABAR"), {209, 0});
        const FName Portrait(*FString::Printf(TEXT("SMPORT%02d"), Player->GetCharacterClass()));
        Screen.Picture(Portrait, {215, 27});
        Screen.Text(FString::FromInt(Player->GetCharacterLevel()), 299, 27, 14, 1, true);
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(StatRows); ++Index)
        {
            Screen.Text(FString::FromInt(Player->GetEffectiveAttribute(StatRows[Index])), 299, 37 + Index * 10, 14, 1, true);
        }
        Screen.Text(CharacterClasses[Player->GetCharacterClass()].Name, 240, 103, 73, 1, true);
        if (Player->GetCharacterClass() == PriestClass)
        {
            Screen.Text(PriestTitles[FMath::Clamp(Player->GetCharacterLevel() - 1, 0, UE_ARRAY_COUNT(PriestTitles) - 1)],
                240, 113, 73, 1, true);
        }
        Screen.Text(FString::FromInt(Player->GetExperience()), 263, 129, 50, 1, true);
        Screen.Text(FString::FromInt(Player->GetExperienceNeeded()), 263, 139, 50, 1, true);
        return;
    }
    if (Form == EForm::Finances && Travel)
    {
        Screen.Picture(TEXT("MONBAR2"), {209, 0});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        const auto DrawCount = [&](int64 Coins, int32 Type, FVector2D Start, int32 FieldWidth)
        {
            const FString Count = LexToString(NativeWadRules::CoinDenominationCount(Coins, Type));
            DrawWadBitmapText(*Canvas, ClassicData->Medium, ClassicData->Palette, Count, Screen.Layout.At(Start),
                Screen.Layout.Scale, FieldWidth, 1, 0, true, White);
        };
        for (int32 Row = 0; Row < UE_ARRAY_COUNT(CoinButtons); ++Row)
        {
            const int32 Type = 106 - 2 * Row;
            DrawCount(Travel->Quest.GetWalletCopper(), Type, {284, 32.0 + 12.0 * Row}, 28);
            DrawCount(Travel->Quest.BankCopper, Type, {260, 101.0 + 12.0 * Row}, 52);
            for (int32 Column = 0; Column < UE_ARRAY_COUNT(CoinButtons[Row]); ++Column)
            {
                Screen.Picture(CoinButtons[Row][Column], CoinButtonPosition(Row, Column));
            }
        }
        FVector2D Cursor;
        const int32 Selected = Player->GetSelectedInventorySlot();
        if (Player->GetCarriedItems().IsValidIndex(Selected) && GetOwningPlayerController()->GetMousePosition(Cursor.X, Cursor.Y))
        {
            Cursor = (Cursor - Screen.Layout.Origin) / Screen.Layout.Scale;
            Screen.Item(Player->GetCarriedItems()[Selected], {Cursor + FVector2D(1, 1), Cursor + FVector2D(19, 19)});
        }
        return;
    }
    if (Form == EForm::Ammo)
    {
        Screen.Picture(TEXT("AMMBAR"), {209, 0});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(NativeWadRules::CrossbowAmmo); ++Index)
        {
            const int32 Y = 18 + Index * 19;
            const auto& Ammo = NativeWadRules::CrossbowAmmo[Index];
            Screen.Text(Ammo.Name, 214, Y, 78, 1, true);
            Screen.Text(FString::FromInt(Player->GetAmmoCount(Index)), 270, Y + 8, 22, 1, true);
            if (Player->GetAmmoReserve(Index) > 0)
            {
                Screen.Picture(TEXT("AMMGET"), {214, double(Y + 8)});
            }
            Screen.Picture(Index == Player->GetSelectedAmmo() ? TEXT("AMMSELDN") : TEXT("AMMSELUP"), {296, double(Y)});
        }
        return;
    }

    Screen.Picture(TEXT("INVBAR3"), {209, 0});
    Screen.Picture(TEXT("2EQBUTN"), BagEquipmentButton.Min);
    const FInventoryView View(*Player, *ClassicData);
    const int32 Pages = View.PageCount();
    InventoryPage = FMath::Min(InventoryPage, Pages - 1);
    const int32 Start = InventoryPage * SlotsPerPage;
    const int32 End = FMath::Min(Start + SlotsPerPage, View.Cells.Num());
    const int32 Selected = View.CellForSlot(Player->GetSelectedInventorySlot());
    const int32 ReadyCell = View.CellForSlot(Player->GetReadySlot());
    FVector2D Cursor(-1, -1);
    if (const APlayerController* Controller = GetOwningPlayerController();
        Controller && Controller->GetMousePosition(Cursor.X, Cursor.Y))
    {
        Cursor = (Cursor - Screen.Layout.Origin) / Screen.Layout.Scale;
    }
    int32 InspectedSlot = Player->GetSelectedInventorySlot();
    for (int32 Cell = Start; Cell < End; ++Cell)
    {
        const FInventoryCell& Item = View.Cells[Cell];
        const FBox2D Box = SlotBox(Cell);
        if (Cell == Selected || Cell == ReadyCell || Player->IsWornInventorySlot(Item.FirstSlot))
        {
            Screen.Rect({Box.Min + FVector2D(1, 1), Box.Max}, Screen.Color(Cell == Selected ? 82 : 78));
        }
        Screen.Item(Player->GetCarriedItems()[Item.FirstSlot], Box);
        if (Item.Count > 1)
        {
            const FVector2D Corner = Box.Max - FVector2D(9, 7);
            Screen.Rect({Corner, Box.Max}, FLinearColor(0, 0, 0, 0.8));
            Screen.Text(FString::FromInt(Item.Count), int32(Corner.X), int32(Corner.Y), 9, 1, true);
        }
        if (Box.IsInside(Cursor))
        {
            InspectedSlot = Item.FirstSlot;
        }
    }
    if (Ready.IsInside(Cursor))
    {
        InspectedSlot = Player->GetReadySlot();
    }
    if (Player->GetCarriedItems().IsValidIndex(InspectedSlot))
    {
        Screen.Rect({{6, 129}, {205, 149}}, FLinearColor(0.01, 0.01, 0.01, 0.92));
        FString Description = Player->GetInventoryItemDescription(Player->GetCarriedItems()[InspectedSlot]);
        if (Player->IsWornInventorySlot(InspectedSlot))
        {
            Description += TEXT(" (WORN)");
        }
        Screen.Text(Description, 9, 132, 193, 2);
    }
    if (View.Cells.IsEmpty())
    {
        Screen.Text(TEXT("EMPTY"), 214, 69, 101, 1, true);
    }
    Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
    Screen.Picture(TEXT("LTARROW"), PreviousPage.Min);
    Screen.Picture(TEXT("RTARROW"), NextPage.Min);
    Screen.Text(Load, 213, 129, 52, 1, true);
    Screen.Text(FString::Printf(TEXT("PG:%d/%d"), InventoryPage + 1, Pages), 287, 129, 27, 1, true);
}
