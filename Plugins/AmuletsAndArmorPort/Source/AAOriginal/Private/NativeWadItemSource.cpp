#include "NativeWadItemSource.h"

#if WITH_EDITOR

#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    constexpr int32 BasicTypeMask = 0x0fff;

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
        Item.bPassable = (Values[2] & (0x0001 | 0x2000)) != 0 || Item.Radius == 0;
        Item.bHanging = (Values[7] & 0x0200) != 0;
        Item.bRaised = (Values[7] & 0x0400) != 0;
        return true;
    }
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
        for (int32 Index = 0; Index + 1 < Lines.Num(); ++Index)
        {
            if (Lines[Index].TrimStartAndEnd().Equals(TEXT("NUMSTACKABLE"), ESearchCase::IgnoreCase))
            {
                Item.Details.StackLimit = FMath::Max(1, FCString::Atoi(*Lines[Index + 1]));
                break;
            }
        }
        Items.Add(Type, MoveTemp(Item));
    }
    return true;
}

#endif
