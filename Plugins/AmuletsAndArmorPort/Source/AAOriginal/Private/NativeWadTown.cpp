#include "NativeWadTown.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/PackageName.h"
#include "NativeWadCheckpoint.h"
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

    TArray<FTownTextLine> WrapTownText(const FString& Text, const FWadTownFont& Font, int32 MaxWidth)
    {
        // MESSAGE.C maps the form's ^nnn markers to entries in GAME.PAL.
        constexpr uint8 Colors[] = {0,31,16,160,164,144,148,211,82,128,132,177,182,34,36,38,40,42,
            226,227,228,229,230,231,232,233,234,235,236,237,238,99,240,241,242,243,247,248,208,99};
        TArray<FTownTextLine> Lines;
        FTownTextLine* Line = &Lines.AddDefaulted_GetRef();
        TArray<FTownGlyph> Word;
        int32 WordWidth = 0;
        uint8 Color = TextBrown;
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
                Color = Marker < UE_ARRAY_COUNT(Colors) ? Colors[Marker] : TextBrown;
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

        void Text(const FString& Value, const FWadTownFont& Font, int32 X, int32 Y,
            int32 Width, int32 Rows = 1, int32 Scroll = 0, bool bCenter = false) const
        {
            if (!Font.Atlas || Font.Widths.Num() != 256 || Font.Height <= 0 || Data.Palette.Num() != 256)
            {
                return;
            }
            const TArray<FTownTextLine> Lines = WrapTownText(Value, Font, Width);
            const int32 First = FMath::Clamp(Scroll, 0, FMath::Max(0, Lines.Num() - Rows));
            for (int32 LineIndex = First; LineIndex < FMath::Min(Lines.Num(), First + Rows); ++LineIndex)
            {
                const FTownTextLine& Line = Lines[LineIndex];
                int32 PenX = bCenter ? X + (Width - Line.Width) / 2 : X;
                for (const FTownGlyph& Ch : Line.Glyphs)
                {
                    const int32 DrawWidth = FMath::Min(int32(Font.Widths[Ch.Glyph]), GlyphWidth);
                    if (Ch.Glyph != ' ' && DrawWidth > 0)
                    {
                        const FVector2D UV(double(Ch.Glyph % AtlasColumns) / AtlasColumns, double(Ch.Glyph / AtlasColumns) / AtlasColumns);
                        const FVector2D UVEnd = UV + FVector2D(double(DrawWidth) / (AtlasColumns * GlyphWidth), 1.0 / AtlasColumns);
                        Tile(*Font.Atlas, PenX, Y + (LineIndex - First) * Font.Height, FVector2D(DrawWidth, Font.Height),
                            FLinearColor::FromSRGBColor(Data.Palette[Ch.PaletteIndex]), UV, UVEnd);
                    }
                    PenX += Font.Widths[Ch.Glyph];
                }
            }
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
    if (Content && !Content->Quests.IsEmpty() && Travel && !Travel->Quest.bShowResult && !bDialogue)
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
    if (!Content || !Travel || !Content->Quests.IsValidIndex(Travel->Quest.Selected) || Travel->Quest.bShowResult || bDialogue)
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
    else if (bDialogue)
    {
        StartQuest();
    }
}

void ANativeWadTownHUD::CancelAction()
{
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
        Player->ShowInteractionMessage(FString::Printf(TEXT("The %s form is pending."), *Name.ToString()));
    }
}

void ANativeWadTownHUD::ClickAction(const FVector2D& Position)
{
    const UNativeWadTownData* Content = NativeWadTown::FindContent(*GetWorld());
    const UNativeWadTravelState* Travel = TownTravel(*GetWorld());
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
