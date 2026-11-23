#include "NativeWadItemSource.h"

#if WITH_EDITOR

#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"
#include "NativeWadRules.h"

namespace
{
    constexpr int32 BasicTypeMask = 0x0fff;

    void ReadInventoryRules(const FString& Source, const TArray<FString>& Input, FWadInventoryDetails& Details)
    {
        using namespace NativeWadRules;

        TArray<FString> Lines;
        for (const FString& Raw : Input)
        {
            FString Line = Raw.TrimStartAndEnd().ToUpper();
            if (!Line.IsEmpty() && !Line.StartsWith(TEXT("#")))
            {
                Lines.Add(MoveTemp(Line));
            }
        }
        const auto Field = [&Lines](const TCHAR* Name, int32 Begin, int32 End)
        {
            for (int32 Index = Begin; Index + 1 < End; ++Index)
            {
                if (Lines[Index] == Name)
                {
                    return Lines[Index + 1];
                }
            }
            return FString();
        };
        const int32 End = Lines.Num();
        const FString Type = Field(TEXT("TYPE"), 0, End);
        const FString Subtype = Field(TEXT("SUBTYPE"), 0, End);
        Details.bStoreBuys = Type == TEXT("WEAPON") || Type == TEXT("ARMOR") || Type == TEXT("POTION")
            || Type == TEXT("WAND") || Type == TEXT("SCROLL") || Type == TEXT("THING") || Type == TEXT("BOLT") || Type == TEXT("QUIVER");
        Details.bBankBuys = Type == TEXT("RING") || Type == TEXT("AMULET");
        Details.StackLimit = FMath::Max(1, FCString::Atoi(*Field(TEXT("NUMSTACKABLE"), 0, End)));
        Details.bConsumed = Type == TEXT("FOODSTUFF") || Field(TEXT("DESTROY ON"), 0, End) == TEXT("USE");
        if (Type == TEXT("ARMOR"))
        {
            Details.ArmorSlot = Subtype.StartsWith(TEXT("HELMET")) ? HeadSlot : Subtype.StartsWith(TEXT("BRACING")) ? LeftArmSlot
                : Subtype.StartsWith(TEXT("LEGGINGS")) ? LegsSlot : Subtype.StartsWith(TEXT("BREASTPLATE")) ? ChestSlot
                : Subtype == TEXT("SHIELD") ? ShieldSlot : INDEX_NONE;
            Details.ArmorValue = FMath::Clamp(FCString::Atoi(*Field(TEXT("ARMOR VALUE"), 0, End)), 0, 100);
        }

        bool bUnknownUse = false;
        for (int32 Index = 0; Index + 1 < End; ++Index)
        {
            if (Lines[Index] != TEXT("EFFECT"))
            {
                continue;
            }
            const FString& Name = Lines[Index + 1];
            int32 EndEffect = Index + 2;
            while (EndEffect < End && Lines[EndEffect] != TEXT("END EFFECT"))
            {
                ++EndEffect;
            }
            const FString Trigger = Field(TEXT("EFFECT TRIGGER"), Index + 2, EndEffect);
            const int32 Power = FCString::Atoi(*Field(TEXT("EFFECT POWER"), Index + 2, EndEffect));
            if (Trigger == TEXT("USE"))
            {
                if (Name == TEXT("MOD PLAYER HEALTH")) { Details.Health += Power; }
                else if (Name == TEXT("MOD PLAYER MANA")) { Details.Mana += Power; }
                else if (Name == TEXT("MOD PLAYER FOOD")) { Details.Food += Power; }
                else if (Name == TEXT("MOD PLAYER WATER")) { Details.Water += Power; }
                else if (Name == TEXT("HALVE PLAYER POISON LEVEL")) { Details.PoisonDivisor = 2; }
                else if (Name == TEXT("REMOVE PLAYER POISON")) { Details.PoisonDivisor = MAX_int32; }
                else if (Name != TEXT("DISPLAY CANNED MESSAGE") && Name != TEXT("PLAY LOCAL SOUND")) { bUnknownUse = true; }
            }
            else if (Type == TEXT("WEAPON") && Trigger == TEXT("READY") && Name == TEXT("READY WEAPON"))
            {
                TArray<FString> Values;
                Field(TEXT("EFFECT DATA"), Index + 2, EndEffect).ParseIntoArray(Values, TEXT(","), false);
                if (Values.Num() == 3)
                {
                    Details.WeaponDamage = FCString::Atoi(*Values[1]);
                    Details.WeaponSpeed = FCString::Atoi(*Values[2]);
                }
            }
            Index = EndEffect;
        }
        Details.bUsable = !bUnknownUse && (Details.Health || Details.Mana || Details.Food || Details.Water || Details.PoisonDivisor > 1);

        if (Details.WeaponDamage <= 0)
        {
            return;
        }
        const FString Animation = Subtype == TEXT("LONGSWORD") || Subtype == TEXT("SHORTSWORD") ? TEXT("SWORD")
            : Subtype == TEXT("DAGGER") || Subtype == TEXT("AXE") || Subtype == TEXT("MACE") || Subtype == TEXT("STAFF")
            ? Subtype : FString();
        TArray<uint8> Frames;
        if (Animation.IsEmpty() || !FFileHelper::LoadFileToArray(Frames, *(Source / TEXT("WEAPONS") / Animation / (Animation + TEXT(".ANI")))))
        {
            return;
        }
        constexpr int32 Header = 2 + 32 * 16;
        constexpr int32 BytesPerFrame = 6;
        constexpr int32 MaxFramesPerLayer = 140;
        if (Frames.Num() < Header + MaxFramesPerLayer * BytesPerFrame)
        {
            return;
        }
        Details.SwingFrames = FMath::Clamp(int32(Frames[0]) | (int32(Frames[1]) << 8), 0, MaxFramesPerLayer);
        for (int32 Frame = 0; Frame < Details.SwingFrames; ++Frame)
        {
            if (Frames[Header + Frame * BytesPerFrame + 5] == 1)
            {
                Details.HitFrame = Frame + 1;
                break;
            }
        }
    }

    TMap<int32, FString> ReadResourceList(const TArray<FString>& Lines, const FString& Extension)
    {
        TMap<int32, FString> Resources;
        for (const FString& Line : Lines)
        {
            TArray<FString> Words;
            Line.ParseIntoArrayWS(Words);
            int32 Number;
            if (Words.Num() >= 2 && FPaths::GetExtension(Words[0]).Equals(Extension, ESearchCase::IgnoreCase)
                && LexTryParseString(Number, *Words[1]))
            {
                FPaths::NormalizeFilename(Words[0]);
                Resources.Add(Number, Words[0].ToUpper());
            }
        }
        return Resources;
    }

    bool ReadObjectArt(const FString& Root, const FString& Resource, FWadAuthoredItem& Item)
    {
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *(Root / FPaths::ChangeExtension(Resource, TEXT("INF")))))
        {
            return false;
        }
        TArray<int32> Values;
        for (const FString& Line : Lines)
        {
            FString Number;
            FString Comment;
            if (!Line.Split(TEXT(";"), &Number, &Comment))
            {
                Number = Line;
            }
            Number.TrimStartAndEndInline();
            if (!Number.IsEmpty())
            {
                int32 Value;
                if (!FDefaultValueHelper::ParseInt(Number, Value))
                {
                    return false;
                }
                Values.Add(Value);
            }
        }
        // Object header, first stance, and its frame headers precede the first picture number.
        constexpr int32 Stance = 8;
        constexpr int32 Frame = 12;
        if (Values.Num() <= Frame || Values[0] < 1 || Values[Stance] < 1 || Values[Stance] > (Values.Num() - Frame - 1) / 4)
        {
            return false;
        }
        if (!FFileHelper::LoadFileToStringArray(Lines, *(Root / FPaths::ChangeExtension(Resource, TEXT("FL")))))
        {
            return false;
        }
        const int32 Number = Values[Frame + Values[Stance] * 4];
        const FString Picture = ReadResourceList(Lines, TEXT("CPC")).FindRef(FMath::Abs(Number));
        if (Picture.IsEmpty())
        {
            return false;
        }
        Item.Picture = FPaths::GetPath(Resource) / Picture;
        Item.Radius = Values[1];
        Item.Weight = Values[3];
        Item.Details.ValueCopper = Values[4];
        Item.bPassable = (Values[2] & (0x0001 | 0x2000)) != 0 || Item.Radius == 0;
        Item.bHanging = (Values[7] & 0x0200) != 0;
        Item.bRaised = (Values[7] & 0x0400) != 0;
        return true;
    }
}

FString FWadAuthoredItem::AssetName() const
{
    return FPaths::GetCleanFilename(FPaths::GetPath(Picture)) + TEXT("_") + FPaths::GetBaseFilename(Picture);
}

FString FWadAuthoredItem::WorldMaterialPackage() const
{
    return TEXT("/Game/WadInterface/Inventory/Things/M_") + AssetName();
}

bool ReadWadAuthoredItems(const TSet<int32>& ThingTypes, TMap<int32, FWadAuthoredItem>& Items, FOutputDevice& Output)
{
    const FString Source = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
    const FString Objects = Source / TEXT("OBJS");
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *(Objects / TEXT("OBJS.FL"))))
    {
        Output.Log(TEXT("Could not read the original WAD object list."));
        return false;
    }
    const TMap<int32, FString> Resources = ReadResourceList(Lines, TEXT("RES"));
    TMap<int32, FWadAuthoredItem> ArtByBaseType;
    for (const int32 Type : ThingTypes)
    {
        const FString File = FString::Printf(TEXT("DES%05d.TXT"), Type);
        const FString InventoryPath = Source / TEXT("INVDESC") / File;
        if (!FPaths::FileExists(InventoryPath))
        {
            continue;
        }
        const int32 BasicType = Type & BasicTypeMask;
        if (!ArtByBaseType.Contains(BasicType))
        {
            const FString* Resource = Resources.Find(BasicType);
            if (!Resource || !ReadObjectArt(Objects, *Resource, ArtByBaseType.Add(BasicType)))
            {
                Output.Logf(TEXT("Could not resolve the original picture for item %d."), Type);
                return false;
            }
        }
        FWadAuthoredItem Item = ArtByBaseType.FindChecked(BasicType);
        if (!FFileHelper::LoadFileToStringArray(Lines, *InventoryPath)
            || !FFileHelper::LoadFileToString(Item.Details.Description, *(Source / TEXT("OBJDESC2") / File)))
        {
            Output.Logf(TEXT("Could not load the original description for item %d."), Type);
            return false;
        }
        Item.Details.Description.TrimStartAndEndInline();
        constexpr int32 ValuePercent[] = {100, 5, 25, 50, 100, 150, 200, 300, 400, 600, 800, 1200, 100, 100, 100, 100};
        Item.Details.ValueCopper = int32(int64(Item.Details.ValueCopper) * ValuePercent[(Type >> 12) & 15] / 100);
        ReadInventoryRules(Source, Lines, Item.Details);
        Items.Add(Type, MoveTemp(Item));
    }
    return true;
}

bool ReadWadShopStock(bool bBank, TMap<int32, int32>& Tiers, FOutputDevice& Output)
{
    const FString Path = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Source")
        / (bBank ? TEXT("BANKUI.C") : TEXT("STORE.C"));
    FString Source;
    if (!FFileHelper::LoadFileToString(Source, *Path))
    {
        Output.Logf(TEXT("Could not read authored shop stock from %s."), *Path);
        return false;
    }
    const FRegexPattern Tokens(TEXT("(?s)/\\*.*?\\*/|//[^\\r\\n]*|switch\\s*\\(adv\\)|case\\s+ADVENTURE_(NONE|UNKNOWN|[1-7])\\s*:|StoreAddItem\\s*\\(\\s*([0-9]+)\\s*,"));
    FRegexMatcher Match(Tokens, Source);
    bool bInStock = false;
    int32 Tier = INDEX_NONE;
    Tiers.Reset();
    while (Match.FindNext())
    {
        const FString Token = Match.GetCaptureGroup(0);
        if (Token.StartsWith(TEXT("switch")))
        {
            bInStock = true;
        }
        if (!bInStock)
        {
            continue;
        }
        const FString Adventure = Match.GetCaptureGroup(1);
        if (Adventure == TEXT("UNKNOWN"))
        {
            break;
        }
        if (!Adventure.IsEmpty())
        {
            Tier = FCString::Atoi(*Adventure);
        }
        const FString Item = Match.GetCaptureGroup(2);
        if (Tier != INDEX_NONE && !Item.IsEmpty())
        {
            Tiers.Add(FCString::Atoi(*Item), Tier);
        }
    }
    return !Tiers.IsEmpty();
}

#endif
