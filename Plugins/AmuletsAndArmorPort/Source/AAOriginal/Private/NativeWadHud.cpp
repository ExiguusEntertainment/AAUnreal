#include "NativeWadPlayer.h"

#include "Engine/Canvas.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadHudData.h"
#include "TextureResource.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    constexpr int32 Width = 320;
    constexpr int32 Height = 200;
    constexpr int32 Columns = 6;
    constexpr int32 SlotsPerPage = Columns * 8;
    constexpr uint8 White = 31;
    const FBox2D ViewOpening({4, 3}, {316, 151});
    const FBox2D Ready({94, 156}, {130, 184});
    const FBox2D Use({93, 186}, {131, 196});
    const FBox2D CastButton({240, 181}, {259, 196});
    const FBox2D InventoryButton({5, 155}, {21, 168});
    const FBox2D StatisticsButton({39, 155}, {55, 168});
    const FBox2D JournalButton({22, 183}, {38, 196});
    const FBox2D Close({304, 4}, {315, 15});
    const FBox2D PreviousPage({267, 129}, {277, 137});
    const FBox2D NextPage({277, 129}, {287, 137});
    const FBox2D RuneFour({265, 169}, {281, 182});
    const FBox2D RuneFive({282, 169}, {298, 182});

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

    FBox2D SlotBox(int32 Slot)
    {
        if (Slot == INDEX_NONE)
        {
            return Ready;
        }
        const int32 Cell = Slot % SlotsPerPage;
        const FVector2D Corner(213 + (Cell % Columns) * 17, 16 + (Cell / Columns) * 14);
        return {Corner, Corner + FVector2D(17, 14)};
    }

    struct FInventoryCell
    {
        int32 FirstSlot;
        int32 Count = 1;
    };

    // Keep the saved inventory as individual objects. Only the displayed cells combine stackable items.
    struct FInventoryView
    {
        FInventoryView(const ANativeWadPlayer& Player, const UNativeWadHudData& Data)
        {
            const TArray<int32>& Items = Player.GetCarriedItems();
            CellBySlot.Reserve(Items.Num());
            TMap<int32, int32> LastStackByType;
            for (int32 Slot = 0; Slot < Items.Num(); ++Slot)
            {
                const int32 Type = Items[Slot];
                const FWadInventoryDetails* Details = Data.ItemDetails.Find(Type);
                const int32 Limit = Details ? Details->StackLimit : 1;
                const int32* Previous = LastStackByType.Find(Type);
                if (Previous && Cells[*Previous].Count < Limit)
                {
                    ++Cells[*Previous].Count;
                    CellBySlot.Add(*Previous);
                }
                else
                {
                    const int32 Cell = Cells.Add({Slot});
                    CellBySlot.Add(Cell);
                    if (Limit > 1)
                    {
                        LastStackByType.Add(Type, Cell);
                    }
                }
            }
        }

        int32 PageCount() const { return FMath::Max(1, FMath::DivideAndRoundUp(Cells.Num(), SlotsPerPage)); }
        int32 CellForSlot(int32 Slot) const { return CellBySlot.IsValidIndex(Slot) ? CellBySlot[Slot] : INDEX_NONE; }

        TArray<FInventoryCell> Cells;
        TArray<int32> CellBySlot;
    };

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
    const ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (ClassicData && Player && Player->IsInventoryOpen() && Form == EForm::Inventory)
    {
        InventoryPage = FMath::Clamp(InventoryPage + Offset, 0, FInventoryView(*Player, *ClassicData).PageCount() - 1);
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
    if (InventoryButton.IsInside(Point) || StatisticsButton.IsInside(Point) || (bHasJournal && JournalButton.IsInside(Point)))
    {
        const EForm Requested = InventoryButton.IsInside(Point) ? EForm::Inventory
            : StatisticsButton.IsInside(Point) ? EForm::Statistics : EForm::Journal;
        if (!Player->IsInventoryOpen() || Form == Requested)
        {
            Player->ToggleInventory();
        }
        Form = Player->IsInventoryOpen() ? Requested : EForm::Inventory;
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
        else if (RuneFour.IsInside(Point))
        {
            Player->EnterRuneFour();
        }
        else if (RuneFive.IsInside(Point))
        {
            Player->EnterRuneFive();
        }
        return;
    }
    if (Close.IsInside(Point))
    {
        Player->ToggleInventory();
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
    Screen.Picture(TEXT("RUNBUT12"), RuneFour.Min);
    Screen.Picture(TEXT("RUNBUT13"), RuneFive.Min);
    const FString Runes = Player->GetRunes();
    for (int32 Index = 0; Index < Runes.Len(); ++Index)
    {
        if (Runes[Index] == '4' || Runes[Index] == '5')
        {
            Screen.Picture(Runes[Index] == '4' ? TEXT("RUDBUT12") : TEXT("RUDBUT13"), {177.0 + Index * 15.0, 182.0});
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
        if (Index == 2 || (Index == 7 && bHasJournal))
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
    if (Form == EForm::Journal && bHasJournal)
    {
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
        Screen.Picture(TEXT("JNLBAR"), {209, 0});
        Screen.Picture(TEXT("CLOSEWIN"), Close.Min);
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
        Screen.Picture(TEXT("SMPORT04"), {215, 27});
        Screen.Text(FString::FromInt(Player->GetCharacterLevel()), 299, 27, 14, 1, true);
        const auto Attributes = Player->GetAttributes();
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(StatRows); ++Index)
        {
            Screen.Text(FString::FromInt(Attributes[StatRows[Index]]), 299, 37 + Index * 10, 14, 1, true);
        }
        Screen.Text(TEXT("Priest"), 240, 103, 73, 1, true);
        Screen.Text(PriestTitles[FMath::Clamp(Player->GetCharacterLevel() - 1, 0, UE_ARRAY_COUNT(PriestTitles) - 1)], 240, 113, 73, 1, true);
        Screen.Text(FString::FromInt(Player->GetExperience()), 263, 129, 50, 1, true);
        Screen.Text(FString::FromInt(Player->GetExperienceNeeded()), 263, 139, 50, 1, true);
        return;
    }

    Screen.Picture(TEXT("INVBAR3"), {209, 0});
    Screen.Rect({{214, 140}, {315, 149}}, FLinearColor::FromSRGBColor(FColor(39, 22, 15)));
    Screen.Text(TEXT("READY ITEM     G DROP"), 214, 141, 101, 1, true);
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
