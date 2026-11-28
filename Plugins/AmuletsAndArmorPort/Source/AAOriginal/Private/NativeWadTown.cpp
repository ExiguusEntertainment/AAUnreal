#include "NativeWadTown.h"

#include "CanvasItem.h"
#include "Misc/ScopeExit.h"
#include "Engine/Canvas.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/PackageName.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadHudData.h"
#include "NativeWadInventoryView.h"
#include "NativeWadStockLayout.h"
#include "NativeWadIdentification.h"
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
    constexpr FTownButton RoomButtons[] = {
        {TEXT("ROOMBTN1"), 45, 24}, {TEXT("ROOMBTN2"), 45, 52},
        {TEXT("ROOMBTN3"), 45, 80}, {TEXT("ROOMBTN4"), 45, 108}
    };
    constexpr int32 RoomPrices[] = {0, 200, 1000, 2000};
    constexpr const TCHAR* RoomPriceLabels[] = {TEXT("Free"), TEXT("2 gold"), TEXT("1 platinum"), TEXT("2 platinum")};
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
    constexpr FTownButton ServiceClose{TEXT("CLOSEWIN"), 196, 5};
    constexpr FTownButton BannerClose{TEXT("CLOSEWIN"), 304, 4};
    constexpr FTownButton StockPrevious{TEXT("LTARROW"), 64, 126};
    constexpr FTownButton StockNext{TEXT("RTARROW"), 74, 126};
    constexpr FTownButton BagPrevious{TEXT("LTARROW"), 267, 129};
    constexpr FTownButton BagNext{TEXT("RTARROW"), 277, 129};
    constexpr FTownButton EquipmentButton{TEXT("2EQBUTN"), 214, 140};
    constexpr FTownButton BagButton{TEXT("2INVBUTN"), 214, 140};
    const FBox2D InventoryBannerButton({5, 155}, {21, 168});
    constexpr int32 Coins[] = {1000, 100, 10, 1};
    constexpr int32 ShopColumns = 6;
    constexpr int32 SlotWidth = 17;
    constexpr int32 SlotHeight = 14;

    int32 StockRows(bool bBank) { return bBank ? 3 : 7; }
    int32 StockTop(bool bBank) { return bBank ? 82 : 26; }

    FTownButton CoinButton(int32 Row, bool bWithdraw)
    {
        return {bWithdraw ? TEXT("RTARROW") : TEXT("LTARROW"), bWithdraw ? 131 : 119, 32 + Row * 11};
    }

    const TCHAR* StockCategory(int32 Category)
    {
        static const TCHAR* Names[]={TEXT("Weapons"),TEXT("Armor"),TEXT("Amulets"),TEXT("Rings"),TEXT("Potions"),TEXT("Wands"),
            TEXT("Scrolls"),TEXT("Food Items"),TEXT(""),TEXT("Assorted Items"),TEXT(""),TEXT("Bolts"),TEXT("Arrows")};
        return Category>=0&&Category<UE_ARRAY_COUNT(Names)?Names[Category]:TEXT("");
    }

    int64 CoinCount(int64 Total, int32 Row)
    {
        return NativeWadRules::CoinDenominationCount(Total, 106 - 2 * Row);
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

    FWadStockLayout StockLayout(const UNativeWadTownData& Content,int32 Tier,bool bBank)
    {
        TArray<FWadStockItem> Items;const auto* Catalog=UNativeWadHudData::LoadCatalog();
        if(Catalog)
        {
            for(int32 Type:AvailableStock(Content,Tier,bBank))
            {
                const auto* Rules=Catalog->ItemDetails.Find(Type);
                Items.Add({Type,Rules?Rules->SourceCategory:13,Catalog->GetItemBitmapSize(Type)});
            }
        }
        FWadStockLayout Layout;
        if(!Layout.Build(Items,StockRows(bBank))){UE_LOG(LogTemp,Error,TEXT("Original merchant stock does not fit its source grid height."));}
        return Layout;
    }

    FString ShopItemName(const ANativeWadPlayer& Player, int32 Type)
    {
        // CONTROL.C's merchant preview reveals stock temporarily without teaching the type to the character.
        const FString Description = Player.GetInventoryItemDescription(Type, true);
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

        void Item(const UTexture2D* Image, FIntPoint SourceSize, FVector2D Position, FVector2D Bounds) const
        {
            if (Image && Image->GetResource() && SourceSize.X>0 && SourceSize.Y>0)
            {
                const FVector2D NativeSize(SourceSize);
                const FVector2D Size = NativeSize * FMath::Min3(1.0, Bounds.X / NativeSize.X, Bounds.Y / NativeSize.Y);
                const FVector2D Corner = Position + (Bounds - Size) * 0.5;
                Tile(*Image, int32(Corner.X), int32(Corner.Y), Size, FLinearColor::White);
            }
        }

        void Text(const FString& Value, const FWadTownFont& Font, int32 X, int32 Y,
            int32 Width, int32 Rows = 1, int32 Scroll = 0, bool bCenter = false) const
        {
            DrawWadBitmapText(Canvas, Font, Data.Palette, Value, Layout.At(X, Y), Layout.Scale, Width, Rows, Scroll, bCenter, TextBrown);
        }

        void FormField(const FString& Value, const FWadTownFont& Font, int32 X, int32 Y, int32 Right, int32 Bottom) const
        {
            if (Data.Palette.Num() != 256) { return; }
            const auto Fill = [&](int32 Left, int32 Top, int32 Width, int32 Height, uint8 Ink)
            {
                FCanvasTileItem TileItem(Layout.At(Left, Top), FVector2D(Width, Height) * Layout.Scale,
                    FLinearColor::FromSRGBColor(Data.Palette[Ink]));
                Canvas.DrawItem(TileItem);
            };
            // The original forms paint over the example values printed into the backdrop.
            const int32 Width = Right - X;
            const int32 Height = Bottom - Y;
            Fill(X, Y, Width + 1, Height + 1, 68);
            Fill(X, Y, Width, 1, 66);
            Fill(X, Y, 1, Height, 66);
            Fill(X + 1, Bottom, Width, 1, 70);
            Fill(Right, Y + 1, 1, Height, 70);
            DrawWadBitmapText(Canvas, Font, Data.Palette, Value, Layout.At(X + 1, Y + 1),
                Layout.Scale, Width, 1, 0, true, 210);
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
void ANativeWadPlayer::TownCharacter() { if (auto* HUD = TownHUD(*this)) { HUD->OpenCharacterPicker(); } }
void ANativeWadPlayer::TownConfirm() { if (auto* HUD = TownHUD(*this)) { HUD->ConfirmAction(); } }
void ANativeWadPlayer::TownCancel() { if (auto* HUD = TownHUD(*this)) { HUD->CancelAction(); } }
void ANativeWadPlayer::TownScrollUp() { if (auto* HUD = TownHUD(*this)) { HUD->Scroll(-1); } }
void ANativeWadPlayer::TownScrollDown() { if (auto* HUD = TownHUD(*this)) { HUD->Scroll(1); } }
void ANativeWadPlayer::TownInn() { if (auto* HUD = TownHUD(*this)) { HUD->OpenService(TEXT("Inn")); } }
void ANativeWadPlayer::TownInnCommons() { if (auto* HUD = TownHUD(*this)) { HUD->RentRoom(0); } }
void ANativeWadPlayer::TownInnLarge() { if (auto* HUD = TownHUD(*this)) { HUD->RentRoom(2); } }
void ANativeWadPlayer::TownInnSuite() { if (auto* HUD = TownHUD(*this)) { HUD->RentRoom(3); } }
void ANativeWadPlayer::TownStore()
{
    if (auto* HUD = TownHUD(*this); HUD && !HUD->RentRoom(1))
    {
        HUD->OpenService(TEXT("Store"));
    }
}
void ANativeWadPlayer::TownBank() { if (auto* HUD = TownHUD(*this)) { HUD->OpenService(TEXT("Bank")); } }

bool ANativeWadPlayer::RentWadRoom(int32 Room)
{
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    if (IsDefeated() || !IsInWadTown() || !Travel || Room < 0 || Room >= UE_ARRAY_COUNT(RoomPrices))
    {
        return false;
    }
    const int64 Wallet = Travel->Quest.GetWalletCopper();
    if (Wallet < RoomPrices[Room])
    {
        ShowInteractionMessage(TEXT("You cannot afford that room."));
        return false;
    }
    Travel->Quest.SetWalletCopper(Wallet - RoomPrices[Room]);
    if (Room > 0)
    {
        constexpr int32 RecoveryDivisors[] = {1, 4, 3, 1};
        Health = FMath::Min(MaxHealth, Health + MaxHealth / RecoveryDivisors[Room]);
        Mana = FMath::Min(MaxMana, Mana + MaxMana / RecoveryDivisors[Room]);
        Food = FMath::Min(NativeWadRules::MaxFood, Food + (Room == 1 ? 1000 : 2000));
        Water = FMath::Min(NativeWadRules::MaxFood, Water + (Room == 1 ? 1000 : 2000));
        PoisonLevel = Room == 3 ? 0 : Room == 2 ? PoisonLevel / 2 : PoisonLevel;
    }
    ShowInteractionMessage(SaveWadCheckpoint() ? TEXT("Rested at the inn. Checkpoint saved.") : TEXT("Rested at the inn."));
    return true;
}

bool ANativeWadPlayer::CanChooseWadClass() const
{
    const UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    return !IsDefeated() && IsInWadTown() && !bWadCharacterCreated && Experience == 0 && CharacterLevel == 1 && Travel
        && Travel->Quest.Active == INDEX_NONE && Travel->VisitedLevels.IsEmpty();
}

bool ANativeWadPlayer::ChooseWadClass(int32 Index)
{
    using namespace NativeWadRules;
    if (Index < 0 || Index >= UE_ARRAY_COUNT(CharacterClasses) || !CanChooseWadClass())
    {
        return false;
    }
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    if (!Catalog || !Catalog->StartingEquipment.IsValidIndex(Index))
    {
        ShowInteractionMessage(TEXT("Starting equipment is unavailable."));
        return false;
    }
    if(!EnsureCarriedObjectData()){return false;}
    TArray<int32> StarterItems=Catalog->StartingEquipment[Index].Items;
    StarterItems.Add(StartingWaterGourd);
    for(int32 FoodIndex=0;FoodIndex<5;++FoodIndex)
    {StarterItems.Add(StartingFoods[FMath::RandHelper(UE_ARRAY_COUNT(StartingFoods))]);}
    StarterItems.Add(NativeWadIdentification::AllScroll);
    FWadCarriedObjectState StarterData=CarriedObjectData;
    for(int32 Type:StarterItems){if(!StarterData.Append(Type)){return false;}}
    const FWadCharacterClass& Class = CharacterClasses[Index];
    bDefeated = false;
    CharacterClass = Index;
    FMemory::Memcpy(Attributes, Class.Base, sizeof(Attributes));
    MaxMana = Mana = Class.Base[MagicAttribute] * 100;
    MaxHealth = Health = (10 + Class.Base[ConstitutionAttribute] / 2) * 100;
    const FWadStartingEquipment& Equipment = Catalog->StartingEquipment[Index];
    CarriedItems.Append(StarterItems);
    CarriedObjectData=MoveTemp(StarterData);
    if (auto* Travel = TownTravel(*GetWorld()))
    {
        Travel->Quest.SetWalletCopper(FMath::Min(FWadQuestProgress::WalletLimit,
            Travel->Quest.GetWalletCopper() + NativeWadIdentification::CommonStarterCopper));
    }
    ReadySlot = SelectedInventorySlot = INDEX_NONE;
    for (int32& Slot : WornSlots)
    {
        Slot = INDEX_NONE;
    }
    for (int32& Slot : JewelrySlots) { Slot = INDEX_NONE; }
    SuppressedFlightEquipment.Reset();
    for (int32 Ammo = 0; Ammo < UE_ARRAY_COUNT(AmmoReserve); ++Ammo)
    {
        AmmoReserve[Ammo] = Equipment.Ammo.IsValidIndex(Ammo) ? Equipment.Ammo[Ammo] : 0;
    }
    InitializeCreationBagLayout();
    for (int32 Slot = 0; Slot < CarriedItems.Num(); ++Slot)
    {
        const FWadInventoryDetails* Item = GetItemRules(CarriedItems[Slot]);
        if (!Item || !CanUseWadItem(CarriedItems[Slot]))
        {
            continue;
        }
        if (Item->ArmorSlot >= 0 && Item->ArmorSlot < EquipmentSlotCount)
        {
            EquipArmor(Slot, *Item);
        }
        else if (Item->JewelrySlot != INDEX_NONE) { EquipJewelry(Slot, *Item); }
        else if (ReadySlot == INDEX_NONE && (Item->WeaponDamage > 0 || CarriedItems[Slot] == Crossbow))
        {
            ReadySlot = Slot;
        }
    }
    bWadCharacterCreated = true;
    InitializeItemIdentification(true);
    ShowInteractionMessage(FString::Printf(TEXT("Starting as a %s."), Class.Name));
    return true;
}

bool ANativeWadPlayer::TradeWadTownItem(int32 Selection, bool bSelling, bool bBank)
{
    UNativeWadTravelState* Travel = TownTravel(*GetWorld());
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    if (IsDefeated() || !IsInWadTown() || !Travel || !Content || Travel->Quest.bShowResult || Selection == INDEX_NONE
        || (bSelling && !CarriedItems.IsValidIndex(Selection)))
    {
        return false;
    }
    FWadQuestProgress& Progress = Travel->Quest;
    const int32 Type = bSelling ? CarriedItems[Selection] : Selection;
    const FWadInventoryDetails* Rules = GetItemRules(Type);
    if (!Rules)
    {
        return false;
    }
    if (bSelling)
    {
        const int32 Price = ResaleValue(*Rules, Type, bBank);
        if (Price <= 0 || Progress.GetWalletCopper() > FWadQuestProgress::WalletLimit - Price || !RemoveInventorySlot(Selection))
        {
            ShowInteractionMessage(TEXT("The shop cannot buy that item."));
            return false;
        }
        Progress.SetWalletCopper(Progress.GetWalletCopper() + Price);
        ShowInteractionMessage(FString::Printf(TEXT("Sold for %s"), *FormatMoney(Price)));
        return true;
    }
    if (GetMouseHandItem() != 0) { return false; }
    const int32* Tier = (bBank ? Content->BankStock : Content->StoreStock).Find(Type);
    if (!Tier || Progress.ShopTier < *Tier || Rules->ValueCopper < 0)
    {
        return false;
    }
    if (Progress.GetWalletCopper() < Rules->ValueCopper)
    {
        ShowInteractionMessage(TEXT("You need more coins in your purse."));
        return false;
    }
    EnsureBagLayout();
    if(!EnsureCarriedObjectData()||!BagLayout.AddSlot()){return false;}
    const int32 Slot=CarriedItems.Num();
    if(!AppendCarriedObject(Type)){BagLayout.RemoveAt(Slot);return false;}
    SelectedInventorySlot = Slot;
    Progress.SetWalletCopper(Progress.GetWalletCopper() - Rules->ValueCopper);
    // STORE.C identifies the exact purchased type after the transaction succeeds.
    IdentifyItemType(Type);
    ShowInteractionMessage(FString::Printf(TEXT("Bought for %s"), *FormatMoney(Rules->ValueCopper)));
    return true;
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
    if (bCharacterPicker)
    {
        constexpr int32 Count = UE_ARRAY_COUNT(NativeWadRules::CharacterClasses);
        ChosenClass = (ChosenClass + Direction + Count) % Count;
        return;
    }
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
    if (Player->CanChooseWadClass() && !Player->ChooseWadClass(Player->GetCharacterClass()))
    {
        return;
    }
    Travel->Quest.Active = Travel->Quest.Selected;
    bDialogue = false;
    UE_LOG(LogTemp, Display, TEXT("Starting WAD quest %d at L%d from town."), Travel->Quest.Active, Map);
    Player->TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(Package + TEXT(".") + FPackageName::GetShortName(Package))));
}

void ANativeWadTownHUD::ConfirmAction()
{
    if (bCharacterPicker)
    {
        if (auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn()); Player && Player->ChooseWadClass(ChosenClass))
        {
            bCharacterPicker = false;
        }
        return;
    }
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
    else if (bDialogue)
    {
        StartQuest();
    }
}

void ANativeWadTownHUD::CancelAction()
{
    if (bCharacterPicker)
    {
        bCharacterPicker = false;
        return;
    }
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
        const auto Layout=StockLayout(*Content,Progress.ShopTier,IsBankOpen());
        StockPage=FMath::Clamp(StockPage+Direction,0,Layout.PageCount()-1);
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
        Player && Travel && !Travel->Quest.bShowResult && !bDialogue && !bCharacterPicker)
    {
        const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
        const TCHAR* Backdrop = Name == TEXT("Bank") ? TEXT("BANKBACK") : Name == TEXT("Store") ? TEXT("STORBACK")
            : Name == TEXT("Inn") ? TEXT("INNBACK") : nullptr;
        if (Content && Backdrop && Content->Pictures.Contains(Backdrop))
        {
            Service = Name;
            StockPage = BagPage = 0;
            ServiceBanner = EServiceBanner::Bag;
            Player->ShowInteractionMessage(Name == TEXT("Bank") ? TEXT("Welcome to the bank!")
                : Name == TEXT("Inn") ? TEXT("Welcome to the inn!") : TEXT("Welcome to the store!"));
            return;
        }
        Player->ShowInteractionMessage(FString::Printf(TEXT("The %s form is pending."), *Name.ToString()));
    }
}

void ANativeWadTownHUD::OpenCharacterPicker()
{
    if (const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
        Player && Player->CanChooseWadClass() && Service.IsNone() && !bDialogue)
    {
        ChosenClass = Player->GetCharacterClass();
        bCharacterPicker = true;
    }
}

bool ANativeWadTownHUD::RentRoom(int32 Room)
{
    if (Service != TEXT("Inn"))
    {
        return false;
    }
    if (auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn()))
    {
        Player->RentWadRoom(Room);
    }
    return true;
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
    if (bCharacterPicker)
    {
        if (Clicked(AcceptButton)) { ConfirmAction(); }
        else if (Clicked(CancelButton)) { CancelAction(); }
        else if (Clicked(LastButton)) { SelectQuest(-1); }
        else if (Clicked(NextButton)) { SelectQuest(1); }
        return;
    }
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

FVector2D ANativeWadTownHUD::ServiceBagItemCenter(int32 Slot) const
{
    using namespace NativeWadInventoryView;
    const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    if (!Player || !Catalog || ServiceBanner != EServiceBanner::Bag)
    {
        return FVector2D(-1, -1);
    }
    const FView View(*Player, *Catalog, true, Player->GetMouseHandSlot());
    const int32 Cell = View.CellForSlot(Slot);
    return View.Cells.IsValidIndex(Cell)&&View.Cells[Cell].Page==Player->GetBagPage()?View.Box(Cell).GetCenter():FVector2D(-1,-1);
}

int32 ANativeWadTownHUD::ServiceStockItemPage(int32 Type) const
{
    const auto* Content=NativeWadTown::FindContent(*GetWorld());const auto* Travel=TownTravel(*GetWorld());
    if(!Content||!Travel){return INDEX_NONE;}const auto Layout=StockLayout(*Content,Travel->Quest.ShopTier,IsBankOpen());
    for(const auto& Cell:Layout.GetCells()){if(Cell.Item.Type==Type){return Cell.Page;}}return INDEX_NONE;
}
FVector2D ANativeWadTownHUD::ServiceStockItemCenter(int32 Type) const
{
    const auto* Content=NativeWadTown::FindContent(*GetWorld());const auto* Travel=TownTravel(*GetWorld());
    if(!Content||!Travel){return FVector2D(-1,-1);}const auto Layout=StockLayout(*Content,Travel->Quest.ShopTier,IsBankOpen());
    for(const auto& Cell:Layout.GetCells())
    {if(Cell.Item.Type==Type&&Cell.Page==StockPage){return {10+(Cell.X+Cell.Footprint.X*0.5)*17,StockTop(IsBankOpen())+(Cell.Y+Cell.Footprint.Y*0.5)*14};}}
    return FVector2D(-1,-1);
}

void ANativeWadTownHUD::ClickService(const UNativeWadTownData& Content, FWadQuestProgress& Progress, const FVector2D& Point)
{
    using namespace NativeWadInventoryView;
    auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
    if (!Player) { return; }
    int32& HeldSlot = Player->SelectedInventorySlot;
    BagPage=Player->GetBagPage();

    const auto Clicked = [&](const FTownButton& Button) { return ContainsButton(Content, Point, Button); };
    if (Clicked(ServiceClose)) { CancelAction(); return; }
    if (Service == TEXT("Inn"))
    {
        for (int32 Room = 0; Room < UE_ARRAY_COUNT(RoomButtons); ++Room)
        {
            if (Clicked(RoomButtons[Room]))
            {
                RentRoom(Room);
                break;
            }
        }
        return;
    }
    if (Clicked(StockPrevious)) { Scroll(-1); return; }
    if (Clicked(StockNext)) { Scroll(1); return; }
    if (Clicked(BannerClose) && ServiceBanner != EServiceBanner::Closed)
    {
        ServiceBanner = EServiceBanner::Closed;
        return;
    }
    if (InventoryBannerButton.IsInside(Point)
        || (ServiceBanner == EServiceBanner::Equipment && Clicked(BagButton)))
    {
        ServiceBanner = EServiceBanner::Bag;
        return;
    }
    if (ServiceBanner == EServiceBanner::Bag && Clicked(EquipmentButton))
    {
        ServiceBanner = EServiceBanner::Equipment;
        return;
    }

    if (Ready.IsInside(Point)) { Player->SwapMouseAndReady(); return; }
    if (ServiceBanner == EServiceBanner::Equipment)
    {
        // INVENTOR.C infers an armor's body location anywhere in the equipment window;
        // the cursor chooses the side only for a pair of bracings.
        const auto* HeldRules = Player->GetItemRules(Player->GetMouseHandItem());
        if (FBox2D({213, 16}, {315, 128}).IsInside(Point) && HeldRules && HeldRules->ArmorSlot != INDEX_NONE)
        {
            const int32 Location = HeldRules->ArmorSlot == NativeWadRules::LeftArmSlot && Point.X > 248
                ? NativeWadRules::RightArmSlot : HeldRules->ArmorSlot;
            Player->ClickArmorSlot(Location);
            return;
        }
        for (int32 Location = 0; Location < UE_ARRAY_COUNT(ArmorBoxes); ++Location)
        { if (ArmorBoxes[Location].IsInside(Point)) { Player->ClickArmorSlot(Location); return; } }
        for (int32 Location = 0; Location < UE_ARRAY_COUNT(JewelryBoxes); ++Location)
        { if (JewelryBoxes[Location].IsInside(Point)) { Player->ClickJewelrySlot(Location); return; } }
    }

    const bool bBank = IsBankOpen();
    if (bBank)
    {
        for (int32 Row = 0; Row < UE_ARRAY_COUNT(Coins); ++Row)
        {
            const int64 Wallet = Progress.GetWalletCopper();
            if (Clicked(CoinButton(Row, false)))
            {
                const int64 Deposit = CoinCount(Wallet, Row) * Coins[Row];
                if (Progress.BankCopper <= FWadQuestProgress::WalletLimit - Deposit)
                {
                    Progress.BankCopper += Deposit;
                    Progress.SetWalletCopper(Wallet - Deposit);
                }
                return;
            }
            if (Clicked(CoinButton(Row, true)))
            {
                if (Progress.BankCopper >= Coins[Row] && Wallet <= FWadQuestProgress::WalletLimit - Coins[Row])
                {
                    Progress.BankCopper -= Coins[Row];
                    Progress.SetWalletCopper(Wallet + Coins[Row]);
                }
                return;
            }
        }
    }

    const int32 Top = StockTop(bBank);
    const int32 Rows = StockRows(bBank);
    if (Point.X >= 10 && Point.X < 10 + ShopColumns * SlotWidth && Point.Y >= Top && Point.Y < Top + Rows * SlotHeight)
    {
        if (Player->GetCarriedItems().IsValidIndex(HeldSlot))
        {
            Player->TradeWadTownItem(HeldSlot, true, bBank);
            return;
        }
        const auto Layout=StockLayout(Content,Progress.ShopTier,bBank);
        const int32 Type=Layout.At(StockPage,{int32(Point.X-10)/17,int32(Point.Y-Top)/14});
        if(Type>0&&Player->TradeWadTownItem(Type,false,bBank))
        {
            // The successful transaction places its newly owned object in the shared mouse hand.
        }
        return;
    }

    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    if (!Catalog || ServiceBanner != EServiceBanner::Bag)
    {
        return;
    }
    if(Clicked(BagPrevious)){Player->SetBagPage(Player->GetBagPage()-1);BagPage=Player->GetBagPage();return;}
    if(Clicked(BagNext)){Player->SetBagPage(Player->GetBagPage()+1);BagPage=Player->GetBagPage();return;}
    if(!InBag(Point)){return;}
    if(Player->GetMouseHandItem()){Player->TryPlaceMouseHandInBag(BagPage,Point);}
    else if(const int32 Slot=Player->BagItemAt(BagPage,Point);Slot!=INDEX_NONE){Player->ClickInventorySlot(Slot);}

}

void ANativeWadTownHUD::DrawService(const UNativeWadTownData& Content, const FWadQuestProgress& Progress, const ANativeWadPlayer& Player)
{
    using namespace NativeWadInventoryView;
    const int32 HeldSlot = Player.GetMouseHandSlot();
    const FTownCanvas Screen{*Canvas, Content, FTownLayout(Canvas->ClipX, Canvas->ClipY)};
    if (Service == TEXT("Inn"))
    {
        Screen.Picture(TEXT("INNBACK"), 4, 3);
        Screen.Button(ServiceClose);
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(RoomButtons); ++Index)
        {
            const FTownButton& Button = RoomButtons[Index];
            Screen.Button(Button);
            Screen.FormField(RoomPriceLabels[Index], Content.Medium, 45, Button.Y + 13, 110, Button.Y + 22);
            const int32 Y = 32 + Index * 11;
            Screen.FormField(LexToString(CoinCount(Progress.GetWalletCopper(), Index)), Content.Medium, 164, Y, 204, Y + 9);
        }
        Screen.Text(Player.GetInteractionMessage(), Content.Tiny, 215, 28, 96, 4);
        return;
    }
    const bool bBank = IsBankOpen();
    Screen.Picture(bBank ? TEXT("BANKBACK") : TEXT("STORBACK"), 4, 3);
    Screen.Button(ServiceClose);
    const int64 Wallet = Progress.GetWalletCopper();
    for (int32 Row = 0; Row < UE_ARRAY_COUNT(Coins); ++Row)
    {
        const int32 Y = (bBank ? 31 : 32) + Row * 11;
        Screen.FormField(LexToString(CoinCount(Wallet, Row)), Content.Medium, bBank ? 148 : 164, Y, bBank ? 200 : 204, Y + 9);
        if (bBank)
        {
            Screen.FormField(LexToString(CoinCount(Progress.BankCopper, Row)), Content.Medium, 57, Y, 109, Y + 9);
            Screen.Button(CoinButton(Row, false));
            Screen.Button(CoinButton(Row, true));
        }
    }

    const int32 Top=StockTop(bBank);const auto Stock=StockLayout(Content,Progress.ShopTier,bBank);
    const UNativeWadHudData* Catalog=UNativeWadHudData::LoadCatalog();if(!Catalog){return;}
    StockPage=FMath::Clamp(StockPage,0,Stock.PageCount()-1);
    Screen.Button(StockPrevious);Screen.Button(StockNext);
    Screen.FormField(StockCategory(Stock.CategoryAt(StockPage)),Content.Tiny,10,126,62,133);
    Screen.FormField(FString::Printf(TEXT("pg:%d/%d"),StockPage+1,Stock.PageCount()),Content.Tiny,84,126,112,133);
    FVector2D Cursor(-1,-1);
    if(GetOwningPlayerController()->GetMousePosition(Cursor.X,Cursor.Y)){Cursor=(Cursor-Screen.Layout.Origin)/Screen.Layout.Scale;}
    int32 InspectedType=INDEX_NONE;
    const auto DrawItem=[&](int32 Type,const FBox2D& Box)
    {Screen.Item(Catalog->Items.FindRef(Type),Catalog->GetItemBitmapSize(Type),Box.Min+FVector2D(1,1),Box.GetSize()-FVector2D(2,2));};
    NativeWadInventoryView::DrawGrid(*Canvas,*Catalog,Screen.Layout.Origin,Screen.Layout.Scale,{10.0,double(Top)},6,StockRows(bBank));
    for(const auto& Cell:Stock.GetCells())
    {
        if(Cell.Page!=StockPage){continue;}
        const FVector2D Corner(10+Cell.X*17,Top+Cell.Y*14);
        NativeWadInventoryView::DrawFootprintItem(*Canvas,*Catalog,Screen.Layout.Origin,Screen.Layout.Scale,
            Cell.Item.Type,Corner,Cell.Footprint,Cell.Item.Bitmap);
        if(FBox2D(Corner,Corner+FVector2D(Cell.Footprint.X*17,Cell.Footprint.Y*14)).IsInside(Cursor)){InspectedType=Cell.Item.Type;}
    }

    const TArray<int32>& Items = Player.GetCarriedItems();
    if (ServiceBanner == EServiceBanner::Bag)
    {
        if (const UTexture2D* BagArt = Catalog->Pictures.FindRef(TEXT("INVBAR3")))
        {
            Screen.Tile(*BagArt, 209, 0, FVector2D(BagArt->GetSizeX(), BagArt->GetSizeY()), FLinearColor::White);
        }
        Screen.Button(BannerClose);
        Screen.Button(EquipmentButton);
        Screen.Button(BagPrevious);
        Screen.Button(BagNext);
        const FView View(Player,*Catalog,true,HeldSlot);const int32 Pages=View.PageCount();BagPage=Player.GetBagPage();
        NativeWadInventoryView::DrawBagPage(*Canvas,Player,*Catalog,Screen.Layout.Origin,Screen.Layout.Scale,BagPage);
        for(const auto& Cell:View.Cells)
        {if(Cell.Page==BagPage&&Cell.Box().IsInside(Cursor)){InspectedType=Items[Cell.FirstSlot];}}
        int32 Weight = 0;
        for (int32 Type : Items) { Weight += Catalog->ItemWeights.FindRef(Type); }
        Screen.FormField(FString::Printf(TEXT("LOAD:%3.1f kg"), Weight / 10.0), Content.Tiny, 213, 129, 265, 136);
        Screen.FormField(FString::Printf(TEXT("PG:%d/%d"), BagPage + 1, Pages), Content.Tiny, 287, 129, 314, 136);
    }
    else if (ServiceBanner == EServiceBanner::Equipment)
    {
        Screen.Picture(TEXT("EQPBAR"), 209, 0);
        Screen.Button(BannerClose);
        Screen.Button(BagButton);
        static_assert(UE_ARRAY_COUNT(ArmorBoxes) == NativeWadRules::EquipmentSlotCount);
        for (int32 Location = 0; Location < NativeWadRules::EquipmentSlotCount; ++Location)
        {
            const int32 Slot = Player.WornSlots[Location];
            if (Items.IsValidIndex(Slot)) { DrawItem(Items[Slot], ArmorBoxes[Location]); }
        }
        static_assert(UE_ARRAY_COUNT(JewelryBoxes) == NativeWadRules::JewelrySlotCount);
        for (int32 Location = 0; Location < NativeWadRules::JewelrySlotCount; ++Location)
        {
            const int32 Slot = Player.GetJewelryInventorySlot(Location);
            if (Items.IsValidIndex(Slot)) { DrawItem(Items[Slot], JewelryBoxes[Location]); }
        }
    }
    else
    {
        Screen.Picture(TEXT("CLOSEDBA"), 209, 0);
    }
    DrawItem(Player.GetReadyItem(), Ready);
    FString Message = Player.GetInteractionMessage();
    if (Items.IsValidIndex(HeldSlot))
    {
        const int32 Type = Items[HeldSlot];
        if (const FWadInventoryDetails* Rules = Catalog->ItemDetails.Find(Type))
        {
            const int32 Price = ResaleValue(*Rules, Type, bBank);
            if (Message.IsEmpty())
            {
                Message = ShopItemName(Player, Type);
                if (Price > 0) { Message += TEXT("  ") + FormatMoney(Price); }
            }
        }
    }
    else if (Message.IsEmpty())
    {
        if (const FWadInventoryDetails* Rules = Catalog->ItemDetails.Find(InspectedType))
        {
            Message = ShopItemName(Player, InspectedType) + TEXT("  ") + FormatMoney(Rules->ValueCopper);
        }
    }
    Screen.FormField(Message, Content.Medium, 6, 139, 206, 148);
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

void ANativeWadTownHUD::EndPlay(const EEndPlayReason::Type Reason)
{
    if (auto* Controller = GetOwningPlayerController()) { Controller->CurrentMouseCursor = EMouseCursor::Default; }
    Super::EndPlay(Reason);
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
    ON_SCOPE_EXIT
    {
        const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn());
        const auto* Catalog = UNativeWadHudData::LoadCatalog();
        if (auto* Controller = GetOwningPlayerController(); Player && Catalog && Controller)
        { NativeWadInventoryView::DrawMouseHand(*Canvas, *Player, *Controller, *Catalog, Screen.Layout.Origin, Screen.Layout.Scale); }
    };
    const FWadQuestProgress& Progress = Travel->Quest;
    const FWadTownQuest& Quest = Content->Quests[Progress.Selected];
    Screen.Picture(TEXT("MAINBACK"), 0, 0);
    if (!Service.IsNone())
    {
        if (const auto* Player = Cast<ANativeWadPlayer>(GetOwningPawn())) { DrawService(*Content, Progress, *Player); }
        return;
    }
    Screen.Picture(TEXT("TWNBACK2"), 4, 3);
    if (bCharacterPicker)
    {
        const auto& Class = NativeWadRules::CharacterClasses[ChosenClass];
        Screen.Picture(TEXT("DLGBACK"), 26, 16);
        Screen.Text(Class.Name, Content->Medium, 72, 35, 110, 1, 0, true);
        const TCHAR* Names[] = {TEXT("Strength"), TEXT("Speed"), TEXT("Magic"), TEXT("Accuracy"), TEXT("Stealth"), TEXT("Constitution")};
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
        {
            const int32 Y = 55 + Index * 12;
            Screen.Text(Names[Index], Content->Tiny, 80, Y, 80, 1);
            Screen.Text(FString::FromInt(Class.Base[Index]), Content->Tiny, 170, Y, 32, 1);
        }
        for (const FTownButton& Button : {LastButton, NextButton, AcceptButton, CancelButton})
        {
            Screen.Button(Button);
        }
        return;
    }
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
        if (Player->CanChooseWadClass())
        {
            Screen.Text(TEXT("Alt+N Choose character"), Content->Tiny, 215, 18, 96, 1);
        }
        Screen.Text(Player->GetInteractionMessage(), Content->Tiny, 215, 28, 96, 4);
    }
}
