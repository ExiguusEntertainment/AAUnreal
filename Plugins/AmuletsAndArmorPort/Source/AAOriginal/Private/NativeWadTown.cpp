#include "NativeWadTown.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/PackageName.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadHudData.h"
#include "TextureResource.h"

namespace
{
    constexpr int32 ScreenWidth = 320;
    constexpr int32 ScreenHeight = 200;
    constexpr int32 AtlasColumns = 16;
    constexpr int32 GlyphWidth = 8;
    constexpr int32 TextBrown = 211;
    constexpr int32 NotesWidth = 192;
    constexpr int32 NotesRows = 9;
    constexpr int32 ResultWidth = 245;
    constexpr int32 ResultRows = 9;
    constexpr int32 DialogueWidth = 244;
    constexpr int32 DialogueRows = 17;

    UNativeWadTravelState* TownTravel(const UWorld& World)
    {
        UGameInstance* Instance = World.GetGameInstance();
        return Instance ? Instance->GetSubsystem<UNativeWadTravelState>() : nullptr;
    }

    ANativeWadTownHUD* TownHUD(ANativeWadPlayer& Player)
    {
        APlayerController* Controller = Player.GetController<APlayerController>();
        return Controller ? Controller->GetHUD<ANativeWadTownHUD>() : nullptr;
    }

    int32 QuestDestination(const FWadTownQuest& Quest, const FWadQuestProgress& Progress)
    {
        const int32 Resume = Progress.ResumeMaps.FindRef(Progress.Selected);
        return Quest.Maps.Contains(Resume) ? Resume : Quest.Maps[0];
    }

    struct FTownButton
    {
        const TCHAR* Picture;
        int32 X;
        int32 Y;
    };

    constexpr FTownButton QuestButton{TEXT("QUEST"), 6, 17};
    constexpr FTownButton InnButton{TEXT("INN"), 57, 17};
    constexpr FTownButton StoreButton{TEXT("STORE"), 107, 17};
    constexpr FTownButton BankButton{TEXT("BANK"), 157, 17};
    constexpr FTownButton LastButton{TEXT("LAST"), 41, 35};
    constexpr FTownButton NextButton{TEXT("NEXT"), 188, 35};
    constexpr FTownButton UpButton{TEXT("UPARROW"), 200, 64};
    constexpr FTownButton DownButton{TEXT("DNARROW"), 200, 129};
    constexpr FTownButton AcceptButton{TEXT("ACCEPT"), 88, 162};
    constexpr FTownButton CancelButton{TEXT("CANCEL"), 172, 162};
    constexpr FTownButton OkButton{TEXT("OK"), 131, 129};
    constexpr FTownButton DialogueUp{TEXT("BULLUP"), 279, 21};
    constexpr FTownButton DialogueDown{TEXT("BULLDN"), 279, 149};
    constexpr FTownButton ResultUp{TEXT("BULLUP"), 280, 54};
    constexpr FTownButton ResultDown{TEXT("BULLDN"), 280, 116};
    constexpr int32 Coins[] = {1000, 100, 10, 1};
    constexpr int32 BagRows = 8;
    constexpr int32 ShopColumns = 6;
    constexpr int32 SlotWidth = 17;
    constexpr int32 SlotHeight = 14;

    int32 StockRows(bool bBank) { return bBank ? 3 : 7; }
    int32 StockTop(bool bBank) { return bBank ? 82 : 26; }

    int64 CoinCount(int64 Total, int32 Row)
    {
        return Row == 0 ? Total / Coins[Row] : Total / Coins[Row] % 10;
    }

    FString FormatMoney(int64 Amount)
    {
        FString Result;
        const TCHAR Currency[] = TEXT("pgsc");
        for (int32 Row = 0; Row < UE_ARRAY_COUNT(Coins); ++Row)
        {
            if (const int64 Count = CoinCount(Amount, Row); Count > 0)
            {
                Result += FString::Printf(TEXT("%lld%c "), Count, Currency[Row]);
            }
        }
        return Result.IsEmpty() ? FString(TEXT("0c")) : Result.TrimEnd();
    }

    TArray<int32> AvailableStock(const UNativeWadTownData& Content, int32 Tier, bool bBank)
    {
        TArray<int32> Types;
        for (const auto& [Type, RequiredTier] : bBank ? Content.BankStock : Content.StoreStock)
        {
            if (Tier >= RequiredTier)
            {
                Types.Add(Type);
            }
        }
        Types.Sort();
        return Types;
    }

    FString ShopItemName(const ANativeWadPlayer& Player, int32 Type)
    {
        const FString Description = Player.GetInventoryItemDescription(Type);
        FString Name;
        return Description.Split(TEXT(" ["), &Name, nullptr) ? Name : Description;
    }

    int32 ResaleValue(const FWadInventoryDetails& Item, int32 Type, bool bBank)
    {
        if ((bBank && !Item.bBankBuys) || (!bBank && !Item.bStoreBuys) || (Type >= 300 && Type <= 317))
        {
            return 0;
        }
        if (Type == 60 || Type == 117) { return 1000; }
        const bool bTreasure = (Type >= 110 && Type <= 113) || Type == 56 || Type == 412 || Type == 413
            || Type == 414 || Type == 425 || Type == 433;
        return bTreasure ? Item.ValueCopper : Item.ValueCopper / 3;
    }

    struct FTownLayout
    {
        FTownLayout(double Width, double Height)
            : Scale(FMath::Min(Width / ScreenWidth, Height / ScreenHeight))
            , Origin((Width - ScreenWidth * Scale) * 0.5, (Height - ScreenHeight * Scale) * 0.5)
        {
        }

        FVector2D At(double X, double Y) const { return Origin + FVector2D(X, Y) * Scale; }

        double Scale;
        FVector2D Origin;
    };

    struct FTownGlyph
    {
        uint8 Glyph;
        uint8 PaletteIndex;
    };

    struct FTownTextLine
    {
        TArray<FTownGlyph> Glyphs;
        int32 Width = 0;
    };

    TArray<FTownTextLine> WrapTownText(const FString& Text, const FWadTownFont& Font, int32 MaxWidth, uint8 Ink = TextBrown)
    {
        // MESSAGE.C maps the form's ^nnn markers to entries in GAME.PAL.
        constexpr uint8 Colors[] = {0,31,16,160,164,144,148,211,82,128,132,177,182,34,36,38,40,42,
            226,227,228,229,230,231,232,233,234,235,236,237,238,99,240,241,242,243,247,248,208,99};
        TArray<FTownTextLine> Lines;
        FTownTextLine* Line = &Lines.AddDefaulted_GetRef();
        TArray<FTownGlyph> Word;
        int32 WordWidth = 0;
        uint8 Color = Ink;
        const auto FinishWord = [&]
        {
            if (Line->Width && Line->Width + WordWidth > MaxWidth)
            {
                Line = &Lines.AddDefaulted_GetRef();
            }
            Line->Glyphs.Append(Word);
            Line->Width += WordWidth;
            Word.Reset();
            WordWidth = 0;
        };
        for (int32 Index = 0; Index < Text.Len(); ++Index)
        {
            const TCHAR Char = Text[Index];
            if (Char == '^' && Index + 3 < Text.Len() && FChar::IsDigit(Text[Index + 1])
                && FChar::IsDigit(Text[Index + 2]) && FChar::IsDigit(Text[Index + 3]))
            {
                const int32 Marker = (Text[Index + 1] - '0') * 100 + (Text[Index + 2] - '0') * 10 + Text[Index + 3] - '0';
                Color = Marker < UE_ARRAY_COUNT(Colors) ? Colors[Marker] : Ink;
                Index += 3;
            }
            else if (Char == '\n')
            {
                FinishWord();
                Line = &Lines.AddDefaulted_GetRef();
            }
            else if (Char == ' ' || Char == '\t')
            {
                FinishWord();
                const int32 Spaces = Char == '\t' ? 3 : 1;
                for (int32 Space = 0; Space < Spaces; ++Space)
                {
                    if (Line->Width + Font.Widths[' '] <= MaxWidth)
                    {
                        Line->Glyphs.Add({' ', Color});
                        Line->Width += Font.Widths[' '];
                    }
                }
            }
            else
            {
                const uint8 Glyph = Char <= 255 ? uint8(Char) : uint8('?');
                if (WordWidth + Font.Widths[Glyph] > MaxWidth)
                {
                    FinishWord();
                }
                Word.Add({Glyph, Color});
                WordWidth += Font.Widths[Glyph];
            }
        }
        FinishWord();
        return Lines;
    }

    struct FTownCanvas
    {
        UCanvas& Canvas;
        const UNativeWadTownData& Data;
        FTownLayout Layout;

        void Tile(const UTexture2D& Image, int32 X, int32 Y, FVector2D Size, const FLinearColor& Color,
            FVector2D UV = FVector2D::ZeroVector, FVector2D UVEnd = FVector2D(1, 1)) const
        {
            FCanvasTileItem Item(Layout.At(X, Y), Image.GetResource(), Size * Layout.Scale, UV, UVEnd, Color);
            Item.BlendMode = SE_BLEND_Translucent;
            Canvas.DrawItem(Item);
        }

        void Picture(FName Name, int32 X, int32 Y, bool bCentered = false) const
        {
            if (const UTexture2D* Image = Data.Pictures.FindRef(Name))
            {
                const FVector2D Size(Image->GetSizeX(), Image->GetSizeY());
                Tile(*Image, bCentered ? X - int32(Size.X) / 2 : X, bCentered ? Y - int32(Size.Y) / 2 : Y,
                    Size, FLinearColor::White);
            }
        }

        void Button(const FTownButton& Button) const { Picture(Button.Picture, Button.X, Button.Y); }

        void Panel(int32 X, int32 Y, int32 W, int32 H, const FLinearColor& Color) const
        {
            FCanvasTileItem Item(Layout.At(X, Y), FVector2D(W, H) * Layout.Scale, Color);
            Item.BlendMode = SE_BLEND_Translucent;
            Canvas.DrawItem(Item);
        }

        void Item(const UTexture2D* Image, FVector2D Position, FVector2D Bounds) const
        {
            if (Image && Image->GetResource() && Image->GetSizeX() && Image->GetSizeY())
            {
                const FVector2D NativeSize(Image->GetSizeX(), Image->GetSizeY());
                const FVector2D Size = NativeSize * FMath::Min(Bounds.X / NativeSize.X, Bounds.Y / NativeSize.Y);
                const FVector2D Corner = Position + (Bounds - Size) * 0.5;
                Tile(*Image, int32(Corner.X), int32(Corner.Y), Size, FLinearColor::White);
            }
        }

        void Text(const FString& Value, const FWadTownFont& Font, int32 X, int32 Y,
            int32 Width, int32 Rows = 1, int32 Scroll = 0, bool bCenter = false) const
        {
            DrawWadBitmapText(Canvas, Font, Data.Palette, Value, Layout.At(X, Y), Layout.Scale, Width, Rows, Scroll, bCenter, TextBrown);
        }
    };

    bool ContainsButton(const UNativeWadTownData& Data, const FVector2D& Point, const FTownButton& Button)
    {
        const UTexture2D* Texture = Data.Pictures.FindRef(Button.Picture);
        const FVector2D Corner(Button.X, Button.Y);
        return Texture && FBox2D(Corner, Corner + FVector2D(Texture->GetSizeX(), Texture->GetSizeY())).IsInside(Point);
    }

    FString ResultText(const FWadTownQuest& Quest, bool bSucceeded, int32 Page)
    {
        if (!bSucceeded)
        {
            return Page == 0 ? Quest.Failure : TEXT("^005You have failed your quest.");
        }
        switch (Page)
        {
        case 0: return Quest.Success;
        case 1: return TEXT("^036Your quest has been completed!");
        case 2: return !Quest.bKeepItem ? Quest.TurnInMessage : FString();
        case 3: return Quest.Gold > 0 ? Quest.GoldMessage : FString();
        case 4: return Quest.Experience > 0 ? FString::Printf(TEXT("^009You have gained ^018%d ^009experience!"), Quest.Experience) : FString();
        default: return FString();
        }
    }

    FString QuestNotes(const FWadTownQuest& Quest, int32 Destination)
    {
        const int32 Completed = Quest.Maps.IndexOfByKey(Destination);
        FString Notes = Quest.Description + FString::Printf(TEXT("^007     Number of maps :\t^009%d\n"), Quest.Maps.Num());
        if (Completed <= 0)
        {
            Notes += TEXT("^007     Quest is currently :\t^009Available\n");
        }
        else
        {
            Notes += FString::Printf(TEXT("^007     Quest is currently :\t^005In progress\n^007     Maps Completed:\t^009%d of %d\n"),
                Completed, Quest.Maps.Num());
        }
        Notes += TEXT("\n") + Quest.NotesHeading;
        for (int32 Index = Completed - 1; Index >= 0; --Index)
        {
            Notes += FString::Printf(TEXT("^011Notes for map %d:\n%s\n\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n^007"),
                Index + 1, *Quest.MapNotes[Index]);
        }
        return Notes;
    }
}

int32 NativeWadTown::MapNumber(const FString& LevelPackage)
{
    const FString Name = FPackageName::GetShortName(LevelPackage);
    int32 Number = 0;
    return Name.StartsWith(TEXT("L")) && LexTryParseString(Number, *Name.Mid(1)) && Number > 0 ? Number : 0;
}

FString NativeWadTown::MapPackage(int32 Number)
{
    return FString::Printf(TEXT("/Game/Maps/L%d"), Number);
}

const UNativeWadTownData* NativeWadTown::FindContent(const UWorld& World)
{
    const TActorIterator<ANativeWadTownContent> Content(&World);
    return Content ? Content->Data.Get() : nullptr;
}

ANativeWadTownGameMode::ANativeWadTownGameMode()
{
    HUDClass = ANativeWadTownHUD::StaticClass();
}

bool ANativeWadPlayer::IsInWadTown() const
{
    return GetWorld()->GetAuthGameMode<ANativeWadTownGameMode>() != nullptr;
}

void ANativeWadPlayer::CompleteTownArrival()
{
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (!Travel || Travel->ArrivingFromMap == 0)
    {
        return;
    }
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    if (!Content)
    {
        return;
    }
    const int32 From = Travel->ArrivingFromMap;
    Travel->ArrivingFromMap = 0;
    FWadQuestProgress& Progress = Travel->Quest;
    int32 Index = Progress.Active;
    Progress.Active = INDEX_NONE;
    if (Index == INDEX_NONE)
    {
        // Older checkpoints and standalone map starts do not have a selected adventure.
        Index = Content->Quests.IndexOfByPredicate([From](const FWadTownQuest& Quest) { return Quest.Maps.Last() == From; });
    }
    if (!Content->Quests.IsValidIndex(Index) || !Content->Quests[Index].Maps.Contains(From))
    {
        return;
    }
    const FWadTownQuest& Quest = Content->Quests[Index];
    Progress.Selected = Index;
    Progress.ResumeMaps.Remove(Index);
    Progress.ResultQuest = Index;
    Progress.bSucceeded = Quest.QuestItem == 0 || GetItemCount(Quest.QuestItem) > 0;
    Progress.bShowResult = true;
    if (Progress.bSucceeded)
    {
        if (!Quest.bKeepItem && Quest.QuestItem != 0)
        {
            ConsumeItem(Quest.QuestItem);
        }
        Progress.Gold = int32(FMath::Min(int64(Progress.Gold) + Quest.Gold, int64(MAX_int32)));
        Progress.ShopTier = FMath::Max(Progress.ShopTier, Index + 1);
        if (Quest.Experience > 0)
        {
            AwardExperience(Quest.Experience);
        }
    }
    UE_LOG(LogTemp, Display, TEXT("WAD quest %d concluded at L%d. Success=%d, gold=%d."),
        Index, From, Progress.bSucceeded, Progress.Gold);
}

void ANativeWadPlayer::TownSelectPrevious() { if (auto* HUD = TownHUD(*this)) { HUD->SelectQuest(-1); } }
void ANativeWadPlayer::TownSelectNext() { if (auto* HUD = TownHUD(*this)) { HUD->SelectQuest(1); } }
void ANativeWadPlayer::TownQuest() { if (auto* HUD = TownHUD(*this)) { HUD->OpenQuest(); } }
void ANativeWadPlayer::TownConfirm() { if (auto* HUD = TownHUD(*this)) { HUD->ConfirmAction(); } }
void ANativeWadPlayer::TownCancel() { if (auto* HUD = TownHUD(*this)) { HUD->CancelAction(); } }
void ANativeWadPlayer::TownScrollUp() { if (auto* HUD = TownHUD(*this)) { HUD->Scroll(-1); } }
void ANativeWadPlayer::TownScrollDown() { if (auto* HUD = TownHUD(*this)) { HUD->Scroll(1); } }
void ANativeWadPlayer::TownInn() { if (auto* HUD = TownHUD(*this)) { HUD->OpenService(TEXT("Inn")); } }
void ANativeWadPlayer::TownStore() { if (auto* HUD = TownHUD(*this)) { HUD->OpenService(TEXT("Store")); } }
void ANativeWadPlayer::TownBank() { if (auto* HUD = TownHUD(*this)) { HUD->OpenService(TEXT("Bank")); } }

void ANativeWadPlayer::TradeWadTownItem(int32 Selection, bool bSelling, bool bBank)
{
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    if (!IsInWadTown() || !Travel || !Content || Travel->Quest.bShowResult || Selection == INDEX_NONE)
    {
        return;
    }
    FWadQuestProgress& Progress = Travel->Quest;
    const int32 Type = bSelling && CarriedItems.IsValidIndex(Selection) ? CarriedItems[Selection] : Selection;
    const FWadInventoryDetails* Rules = GetItemRules(Type);
    if (!Rules)
    {
        return;
    }
    if (bSelling)
    {
        const int32 Price = ResaleValue(*Rules, Type, bBank);
        if (Price <= 0 || Progress.GetWalletCopper() > FWadQuestProgress::WalletLimit - Price || !RemoveInventorySlot(Selection))
        {
            ShowInteractionMessage(TEXT("The shop cannot buy that item."));
            return;
        }
        Progress.SetWalletCopper(Progress.GetWalletCopper() + Price);
        ShowInteractionMessage(FString::Printf(TEXT("Sold for %s"), *FormatMoney(Price)));
        return;
    }
    const int32* Tier = (bBank ? Content->BankStock : Content->StoreStock).Find(Type);
    if (!Tier || Progress.ShopTier < *Tier || Rules->ValueCopper < 0)
    {
        return;
    }
    if (Progress.GetWalletCopper() < Rules->ValueCopper)
    {
        ShowInteractionMessage(TEXT("You need more coins in your purse."));
        return;
    }
    CarriedItems.Add(Type);
    Progress.SetWalletCopper(Progress.GetWalletCopper() - Rules->ValueCopper);
    ShowInteractionMessage(FString::Printf(TEXT("Bought for %s"), *FormatMoney(Rules->ValueCopper)));
}

void ANativeWadPlayer::TownClick()
{
    const APlayerController* LocalPlayer = GetController<APlayerController>();
    float X, Y;
    if (auto* HUD = TownHUD(*this); HUD && LocalPlayer->GetMousePosition(X, Y))
    {
        HUD->ClickAction(FVector2D(X, Y));
    }
}

void ANativeWadTownHUD::SelectQuest(int32 Direction)
{
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (Content && !Content->Quests.IsEmpty() && Travel && !Travel->Quest.bShowResult && !bDialogue && Service.IsNone())
    {
        const int32 Count = Content->Quests.Num();
        const int32 Selected = FMath::Clamp(Travel->Quest.Selected, 0, Count - 1);
        Travel->Quest.Selected = (Selected + Direction + Count) % Count;
        ScrollLine = 0;
    }
}

void ANativeWadTownHUD::OpenQuest()
{
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    const UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (!Content || !Travel || !Content->Quests.IsValidIndex(Travel->Quest.Selected) || Travel->Quest.bShowResult || bDialogue || !Service.IsNone())
    {
        return;
    }
    const FWadTownQuest& Quest = Content->Quests[Travel->Quest.Selected];
    if (QuestDestination(Quest, Travel->Quest) == Quest.Maps[0])
    {
        bDialogue = true;
        ScrollLine = 0;
    }
    else
    {
        StartQuest();
    }
}

void ANativeWadTownHUD::StartQuest()
{
    ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    if (!Player || !Travel || !Content || !Content->Quests.IsValidIndex(Travel->Quest.Selected))
    {
        return;
    }
    const FWadTownQuest& Quest = Content->Quests[Travel->Quest.Selected];
    const int32 Map = QuestDestination(Quest, Travel->Quest);
    const FString Package = NativeWadTown::MapPackage(Map);
    if (!FPackageName::DoesPackageExist(Package))
    {
        bDialogue = false;
        ScrollLine = 0;
        Player->ShowInteractionMessage(FString::Printf(TEXT("L%d still needs a native map."), Map));
        return;
    }
    if (Map == Quest.Maps[0])
    {
        for (const int32 QuestMap : Quest.Maps)
        {
            Travel->VisitedLevels.Remove(NativeWadTown::MapPackage(QuestMap));
        }
    }
    Travel->Quest.Active = Travel->Quest.Selected;
    bDialogue = false;
    UE_LOG(LogTemp, Display, TEXT("Starting WAD quest %d at L%d from town."), Travel->Quest.Active, Map);
    Player->TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(Package + TEXT(".") + FPackageName::GetShortName(Package))));
}

void ANativeWadTownHUD::ConfirmAction()
{
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (!Content || !Travel)
    {
        return;
    }
    FWadQuestProgress& Quest = Travel->Quest;
    if (Quest.bShowResult && Content->Quests.IsValidIndex(Quest.ResultQuest))
    {
        const int32 LastPage = Quest.bSucceeded ? 4 : 1;
        do
        {
            ++ResultPage;
        }
        while (ResultPage <= LastPage && ResultText(Content->Quests[Quest.ResultQuest], Quest.bSucceeded, ResultPage).IsEmpty());
        if (ResultPage > LastPage)
        {
            Quest.bShowResult = false;
            ResultPage = 0;
        }
        ScrollLine = 0;
    }
    else if (!Service.IsNone())
    {
        if (ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn()))
        {
            Player->TradeWadTownItem(ShopSelection, bSelling, IsBankOpen());
            if (bSelling) { ShopSelection = INDEX_NONE; }
        }
    }
    else if (bDialogue)
    {
        StartQuest();
    }
}

void ANativeWadTownHUD::CancelAction()
{
    Service = NAME_None;
    if (bDialogue)
    {
        bDialogue = false;
        ScrollLine = 0;
    }
}

void ANativeWadTownHUD::Scroll(int32 Direction)
{
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    const UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (!Content || !Travel || !Content->Quests.IsValidIndex(Travel->Quest.Selected) || Content->Medium.Widths.Num() != 256)
    {
        return;
    }
    const FWadQuestProgress& Progress = Travel->Quest;
    if (!Service.IsNone())
    {
        const int32 PageSize = ShopColumns * StockRows(IsBankOpen());
        const int32 Count = AvailableStock(*Content, Progress.ShopTier, IsBankOpen()).Num();
        StockPage = FMath::Clamp(StockPage + Direction, 0, FMath::Max(0, (Count - 1) / PageSize));
        return;
    }
    const FWadTownQuest& Quest = Content->Quests[Progress.Selected];
    const bool bResult = Progress.bShowResult && Content->Quests.IsValidIndex(Progress.ResultQuest);
    const FString Text = bResult ? ResultText(Content->Quests[Progress.ResultQuest], Progress.bSucceeded, ResultPage)
        : bDialogue ? Quest.Dialogue : QuestNotes(Quest, QuestDestination(Quest, Progress));
    const int32 Width = bResult ? ResultWidth : bDialogue ? DialogueWidth : NotesWidth;
    const int32 Rows = bResult ? ResultRows : bDialogue ? DialogueRows : NotesRows;
    const int32 Lines = WrapTownText(Text, Content->Medium, Width).Num();
    ScrollLine = FMath::Clamp(ScrollLine + Direction, 0, FMath::Max(0, Lines - Rows));
}

void ANativeWadTownHUD::OpenService(FName Name)
{
    const UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
        Player && Travel && !Travel->Quest.bShowResult && !bDialogue)
    {
        const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
        if (Content && (Name == TEXT("Bank") || Name == TEXT("Store"))
            && Content->Pictures.Contains(Name == TEXT("Bank") ? TEXT("BANKBACK") : TEXT("STORBACK")))
        {
            Service = Name;
            StockPage = BagPage = 0;
            ShopSelection = INDEX_NONE;
            bSelling = false;
            Player->ShowInteractionMessage(FString());
            return;
        }
        Player->ShowInteractionMessage(FString::Printf(TEXT("The %s form is pending."), *Name.ToString()));
    }
}

void ANativeWadTownHUD::ClickAction(const FVector2D& Position)
{
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (!Content || !Travel)
    {
        return;
    }
    int32 Width = 0, Height = 0;
    GetOwningPlayerController()->GetViewportSize(Width, Height);
    const FTownLayout Layout(Width, Height);
    if (Layout.Scale <= 0)
    {
        return;
    }
    const FVector2D Point = (Position - Layout.Origin) / Layout.Scale;
    const auto Clicked = [&](const FTownButton& Button) { return ContainsButton(*Content, Point, Button); };
    if (!Service.IsNone())
    {
        ClickService(*Content, Travel->Quest, Point);
        return;
    }
    if (Travel->Quest.bShowResult)
    {
        if (Clicked(OkButton)) { ConfirmAction(); }
        else if (Clicked(ResultUp)) { Scroll(-1); }
        else if (Clicked(ResultDown)) { Scroll(1); }
        return;
    }
    if (bDialogue)
    {
        if (Clicked(AcceptButton)) { ConfirmAction(); }
        else if (Clicked(CancelButton)) { CancelAction(); }
        else if (Clicked(DialogueUp)) { Scroll(-1); }
        else if (Clicked(DialogueDown)) { Scroll(1); }
        return;
    }
    if (Clicked(QuestButton)) { OpenQuest(); }
    else if (Clicked(LastButton)) { SelectQuest(-1); }
    else if (Clicked(NextButton)) { SelectQuest(1); }
    else if (Clicked(UpButton)) { Scroll(-1); }
    else if (Clicked(DownButton)) { Scroll(1); }
    else if (Clicked(InnButton)) { OpenService(TEXT("Inn")); }
    else if (Clicked(StoreButton)) { OpenService(TEXT("Store")); }
    else if (Clicked(BankButton)) { OpenService(TEXT("Bank")); }
}

void ANativeWadTownHUD::ClickService(const UNativeWadTownData& Content, FWadQuestProgress& Progress, const FVector2D& Point)
{
    const auto In = [&](double X, double Y, double X2, double Y2) { return FBox2D({X, Y}, {X2, Y2}).IsInside(Point); };
    const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (!Player) { return; }
    const bool bBank = IsBankOpen();
    const int32 Top = StockTop(bBank);
    const int32 Rows = StockRows(bBank);
    if (In(156, 157, 210, 173) || In(194, 3, 208, 17)) { CancelAction(); }
    else if (In(9, 157, 77, 173) && !bSelling) { ConfirmAction(); }
    else if (In(81, 157, 149, 173) && bSelling) { ConfirmAction(); }
    else if (In(9, 126, 24, 145)) { Scroll(-1); }
    else if (In(95, 126, 112, 145)) { Scroll(1); }
    else if (In(216, 135, 234, 154)) { BagPage = FMath::Max(0, BagPage - 1); }
    else if (In(297, 135, 314, 154))
    {
        BagPage = FMath::Min(BagPage + 1, FMath::Max(0, (Player->GetCarriedItems().Num() - 1) / BagRows));
    }
    else if (bBank && In(118, 31, 144, 76))
    {
        const int32 Row = FMath::Clamp(int32(Point.Y - 31) / 11, 0, UE_ARRAY_COUNT(Coins) - 1);
        const int64 Wallet = Progress.GetWalletCopper();
        if (Point.X < 130)
        {
            const int64 Deposit = CoinCount(Wallet, Row) * Coins[Row];
            if (Progress.BankCopper <= FWadQuestProgress::WalletLimit - Deposit)
            {
                Progress.BankCopper += Deposit;
                Progress.SetWalletCopper(Wallet - Deposit);
            }
        }
        else if (Progress.BankCopper >= Coins[Row] && Wallet <= FWadQuestProgress::WalletLimit - Coins[Row])
        {
            Progress.BankCopper -= Coins[Row];
            Progress.SetWalletCopper(Wallet + Coins[Row]);
        }
    }
    else if (In(10, Top, 10 + ShopColumns * SlotWidth, Top + Rows * SlotHeight))
    {
        const int32 Cell = int32(Point.X - 10) / SlotWidth + int32(Point.Y - Top) / SlotHeight * ShopColumns;
        const TArray<int32> Types = AvailableStock(Content, Progress.ShopTier, bBank);
        const int32 Index = StockPage * ShopColumns * Rows + Cell;
        ShopSelection = Types.IsValidIndex(Index) ? Types[Index] : INDEX_NONE;
        bSelling = false;
    }
    else if (In(216, 38, 314, 38 + BagRows * 12))
    {
        const int32 Slot = BagPage * BagRows + int32(Point.Y - 38) / 12;
        ShopSelection = Player->GetCarriedItems().IsValidIndex(Slot) ? Slot : INDEX_NONE;
        bSelling = true;
    }
}

void ANativeWadTownHUD::DrawService(const UNativeWadTownData& Content, const FWadQuestProgress& Progress, const ANativeWadPlayer& Player)
{
    const FTownCanvas Screen{*Canvas, Content, FTownLayout(Canvas->ClipX, Canvas->ClipY)};
    const bool bBank = IsBankOpen();
    Screen.Picture(bBank ? TEXT("BANKBACK") : TEXT("STORBACK"), 4, 3);
    const int64 Wallet = Progress.GetWalletCopper();
    for (int32 Row = 0; Row < UE_ARRAY_COUNT(Coins); ++Row)
    {
        const int32 Y = 32 + Row * 11;
        const FLinearColor MoneyField = FLinearColor::FromSRGBColor(FColor(93, 0, 0));
        const int32 WalletX = bBank ? 148 : 164;
        const int32 WalletWidth = bBank ? 52 : 37;
        Screen.Panel(WalletX, Y, WalletWidth, 8, MoneyField);
        Screen.Text(LexToString(CoinCount(Wallet, Row)), Content.Medium, WalletX, Y, WalletWidth, 1, 0, true);
        if (bBank)
        {
            Screen.Panel(57, Y, 52, 8, MoneyField);
            Screen.Text(LexToString(CoinCount(Progress.BankCopper, Row)), Content.Medium, 57, Y, 52, 1, 0, true);
            Screen.Picture(TEXT("LTARROW"), 119, Y);
            Screen.Picture(TEXT("RTARROW"), 131, Y);
        }
    }

    const FLinearColor PanelColor(0.10f, 0.061f, 0.034f);
    const FLinearColor SelectionColor(0.26f, 0.17f, 0.072f);
    const int32 Rows = StockRows(bBank);
    const int32 Top = StockTop(bBank);
    const TArray<int32> Types = AvailableStock(Content, Progress.ShopTier, bBank);
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    const int32 PageSize = ShopColumns * Rows;
    for (int32 Cell = 0; Catalog && Cell < PageSize; ++Cell)
    {
        const int32 Index = StockPage * PageSize + Cell;
        if (!Types.IsValidIndex(Index)) { break; }
        const int32 Type = Types[Index];
        const int32 X = 10 + Cell % ShopColumns * SlotWidth;
        const int32 Y = Top + Cell / ShopColumns * SlotHeight;
        if (!bSelling && ShopSelection == Type) { Screen.Panel(X, Y, SlotWidth, SlotHeight, SelectionColor); }
        Screen.Item(Catalog->Items.FindRef(Type), FVector2D(X + 1, Y + 1), FVector2D(SlotWidth - 2, SlotHeight - 2));
    }
    Screen.Panel(213, 17, 103, 138, PanelColor);
    Screen.Text(TEXT("^038Purse"), Content.Medium, 216, 20, 98, 1, 0, true);
    Screen.Text(TEXT("^038") + FormatMoney(Wallet), Content.Tiny, 216, 31, 98, 1, 0, true);
    const TArray<int32>& Items = Player.GetCarriedItems();
    for (int32 Row = 0; Row < BagRows; ++Row)
    {
        const int32 Slot = BagPage * BagRows + Row;
        if (!Items.IsValidIndex(Slot)) { break; }
        const int32 Y = 38 + Row * 12;
        if (bSelling && ShopSelection == Slot) { Screen.Panel(215, Y, 99, 12, SelectionColor); }
        if (Catalog) { Screen.Item(Catalog->Items.FindRef(Items[Slot]), FVector2D(217, Y), FVector2D(11, 11)); }
        Screen.Text(TEXT("^038") + ShopItemName(Player, Items[Slot]).Left(22), Content.Tiny, 230, Y + 2, 83);
    }
    Screen.Picture(TEXT("LTARROW"), 217, 136);
    Screen.Picture(TEXT("RTARROW"), 299, 136);
    Screen.Panel(8, 126, 106, 11, PanelColor);
    Screen.Picture(TEXT("LTARROW"), 10, 128);
    Screen.Picture(TEXT("RTARROW"), 97, 128);
    Screen.Text(FString::Printf(TEXT("^038Stock %d/%d"), StockPage + 1, FMath::Max(1, FMath::DivideAndRoundUp(Types.Num(), PageSize))),
        Content.Tiny, 24, 129, 68, 1, 0, true);
    Screen.Panel(8, 143, 202, 31, PanelColor);
    const int32 Type = bSelling ? (Items.IsValidIndex(ShopSelection) ? Items[ShopSelection] : 0) : ShopSelection;
    if (const FWadInventoryDetails* Rules = Catalog ? Catalog->ItemDetails.Find(Type) : nullptr)
    {
        const int32 Price = bSelling ? ResaleValue(*Rules, Type, bBank) : Rules->ValueCopper;
        const FString Offer = bSelling && Price <= 0 ? FString(TEXT("No offer")) : FormatMoney(Price);
        Screen.Text(TEXT("^038") + ShopItemName(Player, Type) + TEXT("  ") + Offer, Content.Tiny, 11, 147, 195);
    }
    Screen.Text(TEXT("^038Buy"), Content.Medium, 9, 160, 68, 1, 0, true);
    Screen.Text(TEXT("^038Sell"), Content.Medium, 81, 160, 68, 1, 0, true);
    Screen.Text(TEXT("^038Town"), Content.Medium, 156, 160, 54, 1, 0, true);
    Screen.Panel(213, 155, 103, 20, PanelColor);
    Screen.Text(TEXT("^038") + Player.GetInteractionMessage(), Content.Tiny, 216, 158, 99, 2);
}

void DrawWadBitmapText(UCanvas& Canvas, const FWadTownFont& Font, TConstArrayView<FColor> Palette,
    const FString& Text, FVector2D Origin, double Scale, int32 Width, int32 Rows, int32 Scroll,
    bool bCenter, uint8 Ink)
{
    if (!Font.Atlas || !Font.Atlas->GetResource() || Font.Widths.Num() != 256 || Font.Height <= 0 || Palette.Num() != 256)
    {
        return;
    }
    const TArray<FTownTextLine> Lines = WrapTownText(Text, Font, Width, Ink);
    const int32 First = FMath::Clamp(Scroll, 0, FMath::Max(0, Lines.Num() - Rows));
    for (int32 LineIndex = First; LineIndex < FMath::Min(Lines.Num(), First + Rows); ++LineIndex)
    {
        const FTownTextLine& Line = Lines[LineIndex];
        int32 PenX = bCenter ? (Width - Line.Width) / 2 : 0;
        for (const FTownGlyph& Ch : Line.Glyphs)
        {
            const int32 DrawWidth = FMath::Min(int32(Font.Widths[Ch.Glyph]), GlyphWidth);
            if (Ch.Glyph != ' ' && DrawWidth > 0)
            {
                const FVector2D UV(double(Ch.Glyph % AtlasColumns) / AtlasColumns, double(Ch.Glyph / AtlasColumns) / AtlasColumns);
                const FVector2D UVEnd = UV + FVector2D(double(DrawWidth) / (AtlasColumns * GlyphWidth), 1.0 / AtlasColumns);
                FCanvasTileItem Item(Origin + FVector2D(PenX, (LineIndex - First) * Font.Height) * Scale,
                    Font.Atlas->GetResource(), FVector2D(DrawWidth, Font.Height) * Scale, UV, UVEnd,
                    FLinearColor::FromSRGBColor(Palette[Ch.PaletteIndex]));
                Item.BlendMode = SE_BLEND_Translucent;
                Canvas.DrawItem(Item);
            }
            PenX += Font.Widths[Ch.Glyph];
        }
    }
}

void ANativeWadTownHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas)
    {
        return;
    }
    DrawRect(FLinearColor::Black, 0, 0, Canvas->ClipX, Canvas->ClipY);
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    const UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (!Content || !Travel || !Content->Quests.IsValidIndex(Travel->Quest.Selected))
    {
        return;
    }
    const FTownCanvas Screen{*Canvas, *Content, FTownLayout(Canvas->ClipX, Canvas->ClipY)};
    const FWadQuestProgress& Progress = Travel->Quest;
    const FWadTownQuest& Quest = Content->Quests[Progress.Selected];
    Screen.Picture(TEXT("MAINBACK"), 0, 0);
    if (!Service.IsNone())
    {
        if (const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn())) { DrawService(*Content, Progress, *Player); }
        return;
    }
    Screen.Picture(TEXT("TWNBACK2"), 4, 3);
    for (const FTownButton& Button : {QuestButton, InnButton, StoreButton, BankButton, LastButton, NextButton, UpButton, DownButton})
    {
        Screen.Button(Button);
    }
    Screen.Picture(TEXT("SB2"), 200, 74);
    Screen.Picture(TEXT("BLOCK"), 6, 35);
    Screen.Picture(FName(*FString::Printf(TEXT("QuestItem%d"), Quest.QuestItem)), 22, 48, true);
    Screen.Text(Quest.Title, Content->Medium, 61, 35, 126, 1, 0, true);
    Screen.Text(Quest.Goal, Content->Tiny, 61, 46, 146, 1, 0, true);
    Screen.Text(FString::FromInt(Quest.Gold), Content->Tiny, 82, 55, 33, 1, 0, true);
    Screen.Text(FString::FromInt(Quest.Experience), Content->Tiny, 156, 55, 51, 1, 0, true);
    Screen.Text(QuestNotes(Quest, QuestDestination(Quest, Progress)), Content->Medium, 6, 64, NotesWidth, NotesRows,
        bDialogue || Progress.bShowResult ? 0 : ScrollLine);

    if (Progress.bShowResult && Content->Quests.IsValidIndex(Progress.ResultQuest))
    {
        Screen.Picture(TEXT("PMPTBULL"), 27, 49);
        Screen.Picture(TEXT("BULLSB"), 280, 63);
        Screen.Button(OkButton);
        Screen.Button(ResultUp);
        Screen.Button(ResultDown);
        Screen.Text(ResultText(Content->Quests[Progress.ResultQuest], Progress.bSucceeded, ResultPage),
            Content->Medium, 31, 52, ResultWidth, ResultRows, ScrollLine);
    }
    else if (bDialogue)
    {
        Screen.Picture(TEXT("DLGBACK"), 26, 16);
        Screen.Picture(TEXT("DLGSB"), 279, 30);
        Screen.Button(AcceptButton);
        Screen.Button(CancelButton);
        Screen.Button(DialogueUp);
        Screen.Button(DialogueDown);
        Screen.Text(Quest.Dialogue, Content->Medium, 30, 20, DialogueWidth, DialogueRows, ScrollLine);
    }
    else if (const ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwningPawn()))
    {
        Screen.Text(Player->GetInteractionMessage(), Content->Tiny, 215, 28, 96, 4);
    }
}
