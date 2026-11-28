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
        static const TCHAR* Categories[]={TEXT("WEAPON"),TEXT("ARMOR"),TEXT("AMULET"),TEXT("RING"),TEXT("POTION"),TEXT("WAND"),
            TEXT("SCROLL"),TEXT("FOODSTUFF"),TEXT("POWERUP"),TEXT("THING"),TEXT("COIN"),TEXT("BOLT"),TEXT("QUIVER")};
        Details.SourceCategory=13;
        for(int32 Category=0;Category<UE_ARRAY_COUNT(Categories);++Category)
        {if(Type==Categories[Category]){Details.SourceCategory=Category;break;}}
        Details.UsableClasses = AllClasses;
        uint16 Denied = 0;
        // INVENTOR.C applies these proficiency checks in addition to each item's useable list.
        if (Type == TEXT("WEAPON"))
        {
            if (Subtype == TEXT("DAGGER")) { Denied = ClassFlag(PriestClass); }
            else if (Subtype == TEXT("AXE"))
            {
                Denied = ClassFlag(MageClass) | ClassFlag(PriestClass) | ClassFlag(MagicianClass) | ClassFlag(RogueClass);
            }
            else if (Subtype == TEXT("LONGSWORD"))
            {
                Denied = ClassFlag(RogueClass) | ClassFlag(MagicianClass) | ClassFlag(MageClass) | ClassFlag(PriestClass);
            }
            else if (Subtype == TEXT("SHORTSWORD")) { Denied = ClassFlag(MageClass) | ClassFlag(PriestClass); }
            else if (Subtype == TEXT("MACE"))
            {
                Denied = ClassFlag(MageClass) | ClassFlag(MercenaryClass) | ClassFlag(MagicianClass);
            }
            else if (Subtype == TEXT("CROSSBOW"))
            {
                Denied = ClassFlag(MageClass) | ClassFlag(WarlockClass) | ClassFlag(PriestClass)
                    | ClassFlag(PaladinClass) | ClassFlag(MagicianClass);
            }
        }
        else if (Type == TEXT("ARMOR"))
        {
            if (Subtype.EndsWith(TEXT("PLATE")))
            {
                Denied = ClassFlag(WarlockClass) | ClassFlag(PriestClass) | ClassFlag(ArcherClass)
                    | ClassFlag(SailorClass) | ClassFlag(MageClass) | ClassFlag(RogueClass) | ClassFlag(MagicianClass);
            }
            else if (Subtype.EndsWith(TEXT("CHAIN")))
            {
                Denied = ClassFlag(MageClass) | ClassFlag(RogueClass) | ClassFlag(MagicianClass);
            }
        }
        Details.UsableClasses &= uint16(~Denied);
        const int32 UseableList = Lines.Find(TEXT("USEABLE LIST"));
        if (UseableList != INDEX_NONE)
        {
            uint16 Listed = 0;
            for (int32 Line = UseableList + 1; Line < End && Lines[Line] != TEXT("END USEABLE LIST"); ++Line)
            {
                for (int32 Class = 0; Class < CharacterClassCount; ++Class)
                {
                    if (Lines[Line].Equals(CharacterClasses[Class].Name, ESearchCase::IgnoreCase))
                    {
                        Listed |= ClassFlag(Class);
                    }
                }
            }
            Details.UsableClasses &= Listed;
        }
        Details.bStoreBuys = Type == TEXT("WEAPON") || Type == TEXT("ARMOR") || Type == TEXT("POTION")
            || Type == TEXT("WAND") || Type == TEXT("SCROLL") || Type == TEXT("THING") || Type == TEXT("BOLT") || Type == TEXT("QUIVER");
        Details.bBankBuys = Type == TEXT("RING") || Type == TEXT("AMULET");
        Details.JewelrySlot = Type == TEXT("AMULET") ? NeckSlot : Type == TEXT("RING") ? Ring1Slot : INDEX_NONE;
        Details.StackLimit = FMath::Max(1, FCString::Atoi(*Field(TEXT("NUMSTACKABLE"), 0, End)));
        Details.bConsumed = Type == TEXT("FOODSTUFF") || Field(TEXT("DESTROY ON"), 0, End) == TEXT("USE");
        if (Type == TEXT("ARMOR"))
        {
            Details.ArmorSlot = Subtype.StartsWith(TEXT("HELMET")) ? HeadSlot : Subtype.StartsWith(TEXT("BRACING")) ? LeftArmSlot
                : Subtype.StartsWith(TEXT("LEGGINGS")) ? LegsSlot : Subtype.StartsWith(TEXT("BREASTPLATE")) ? ChestSlot
                : Subtype == TEXT("SHIELD") ? ShieldSlot : INDEX_NONE;
            Details.ArmorClass = Subtype.EndsWith(TEXT("CHAIN")) ? 1 : Subtype.EndsWith(TEXT("PLATE")) ? 2 : 0;
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
            if (Trigger == TEXT("USE") && (Type == TEXT("POTION") || Type == TEXT("SCROLL") || Type == TEXT("FOODSTUFF"))
                && Name != TEXT("DISPLAY CANNED MESSAGE") && Name != TEXT("PLAY LOCAL SOUND")
                && Name != TEXT("PLAY AREA SOUND") && Name != TEXT("COLOR FLASH"))
            { Details.bIdentifyOnUse = true; }
            if (Type == TEXT("WAND") && Trigger == TEXT("USE") && Name == TEXT("FIRE WAND"))
            {
                TArray<FString> Values;
                Field(TEXT("EFFECT DATA"), Index + 2, EndEffect).ParseIntoArray(Values, TEXT(","), false);
                int32 Projectile;
                int32 ManaCost;
                if (Values.Num() == 3 && LexTryParseString(Projectile, *Values[0])
                    && LexTryParseString(ManaCost, *Values[2]) && ManaCost > 0)
                {
                    Details.WandProjectile = Projectile;
                    Details.WandManaCost = ManaCost;
                }
            }
            else if (Name == TEXT("ADD PLAYER EFFECT") && (Trigger == TEXT("USE") || Trigger == TEXT("READY")))
            {
                const FString SubEffect = Field(TEXT("EFFECT SUBTYPE"), Index + 2, EndEffect);
                const int32 Status = SubEffect == TEXT("MAGIC MAP") ? MagicMapStatus
                    : SubEffect == TEXT("INVISIBLE") ? InvisibleStatus
                    : SubEffect == TEXT("STICKY FEET") ? StickyFeetStatus
                    : SubEffect == TEXT("NIGHT VISION") ? NightVisionStatus
                    : SubEffect == TEXT("INVULNERABLE") ? InvulnerableStatus
                    : SubEffect == TEXT("TRANSLUCENT") ? TranslucentStatus
                    : SubEffect == TEXT("STRENGTH MOD") ? StrengthStatus
                    : SubEffect == TEXT("SPEED MOD") ? SpeedStatus
                    : SubEffect == TEXT("ACCURACY MOD") ? AccuracyStatus
                    : SubEffect == TEXT("STEALTH MOD") ? StealthStatus
                    : SubEffect == TEXT("MAGIC MOD") ? MagicStatus
                    : SubEffect == TEXT("MOD JUMP POWER") ? JumpStatus
                    : SubEffect == TEXT("HEALTH REGEN MOD") ? HealthRegenStatus
                    : SubEffect == TEXT("MANA REGEN MOD") ? ManaRegenStatus
                    : SubEffect == TEXT("FOOD CONSERVATION") ? FoodConservationStatus
                    : SubEffect == TEXT("WATER CONSERVATION") ? WaterConservationStatus
                    : SubEffect == TEXT("ETERNAL NOURISHMENT") ? EternalNourishmentStatus
                    : SubEffect == TEXT("SHIELD") ? SpellShieldStatus
                    : SubEffect == TEXT("LAVA WALK") ? LavaWalkStatus
                    : SubEffect == TEXT("FIRE ATTACK") ? FireAttackStatus
                    : SubEffect == TEXT("ELECTRICITY ATTACK") ? ElectricityAttackStatus
                    : SubEffect == TEXT("ACID ATTACK") ? AcidAttackStatus
                    : SubEffect == TEXT("POISON ATTACK") ? PoisonAttackStatus
                    : SubEffect == TEXT("PIERCING ATTACK") ? PiercingAttackStatus
                    : SubEffect == TEXT("MANA DRAIN ATTACK") ? ManaDrainAttackStatus
                    : SubEffect == TEXT("RESIST FIRE") ? FireResistStatus
                    : SubEffect == TEXT("RESIST ACID") ? AcidResistStatus
                    : SubEffect == TEXT("RESIST POISON") ? PoisonResistStatus
                    : SubEffect == TEXT("RESIST ELECTRICITY") ? ElectricityResistStatus
                    : SubEffect == TEXT("RESIST PIERCING") ? PiercingResistStatus
                    : SubEffect == TEXT("RESIST MANA DRAIN") ? ManaDrainResistStatus : INDEX_NONE;
                const int32 Ticks = FCString::Atoi(*Field(TEXT("EFFECT DURATION"), Index + 2, EndEffect));
                if (Status != INDEX_NONE && ((Trigger == TEXT("READY") && Ticks == 0) || (Trigger == TEXT("USE") && Ticks > 0)))
                {
                    Details.Status = Status;
                    Details.StatusTicks = Ticks;
                    Details.StatusPower = Status == StrengthStatus || Status == SpeedStatus || Status == SpellShieldStatus
                        || Status == AccuracyStatus || Status == StealthStatus || Status == MagicStatus
                        || Status == HealthRegenStatus || Status == ManaRegenStatus || Status == JumpStatus || Status == MagicMapStatus || Status == NightVisionStatus ? Power : 1;
                }
                else if (Trigger == TEXT("USE"))
                {
                    bUnknownUse = true;
                }
            }
            else if (Trigger == TEXT("USE"))
            {
                if (Name == TEXT("IDENTIFY READIED")) { Details.IdentificationScope = 1; }
                else if (Name == TEXT("IDENTIFY ALL")) { Details.IdentificationScope = 2; }
                else if (Name == TEXT("MOD PLAYER HEALTH")) { Details.Health += Power; }
                else if (Name == TEXT("MOD PLAYER MANA")) { Details.Mana += Power; }
                else if (Name == TEXT("MOD PLAYER FOOD")) { Details.Food += Power; }
                else if (Name == TEXT("MOD PLAYER WATER")) { Details.Water += Power; }
                else if (Name == TEXT("HALVE PLAYER POISON LEVEL")) { Details.PoisonDivisor = 2; }
                // DES00807 uses SET PLAYER POISON LEVEL with the default power of zero.
                // Reuse the existing clear representation; unsupported nonzero setters stay unusable.
                else if (Name == TEXT("REMOVE PLAYER POISON") || (Name == TEXT("SET PLAYER POISON LEVEL") && Power == 0))
                { Details.PoisonDivisor = MAX_int32; }
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
        Details.bUsable = !bUnknownUse && (Details.Health || Details.Mana || Details.Food || Details.Water
            || Details.PoisonDivisor > 1 || Details.IdentificationScope > 0 || (Details.Status != INDEX_NONE && Details.StatusTicks > 0));

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
        int32 Number;
        if (!ParseWadObjectInitialPicture(Values, Number)) { return false; }
        if (!FFileHelper::LoadFileToStringArray(Lines, *(Root / FPaths::ChangeExtension(Resource, TEXT("FL")))))
        {
            return false;
        }
        const FString Picture = ReadResourceList(Lines, TEXT("CPC")).FindRef(FMath::Abs(Number));
        if (Picture.IsEmpty())
        {
            return false;
        }
        Item.Picture = FPaths::GetPath(Resource) / Picture;
        Item.Radius = Values[1];
        Item.Details.WorldAttributes = uint16(Values[2]);
        Item.Weight = Values[3];
        Item.Details.ValueCopper = Values[4];
        Item.bPassable = (Values[2] & (0x0001 | 0x2000)) != 0 || Item.Radius == 0;
        Item.bHanging = (Values[7] & 0x0200) != 0;
        Item.bRaised = (Values[7] & 0x0400) != 0;
        return true;
    }
}

bool ParseWadObjectInitialPicture(TConstArrayView<int32> Values, int32& SignedPicture)
{
    // DISOAF INF lists all stance headers first, then each stance's complete
    // frame-header array followed by its angle-picture lists. OBJTYPE.C selects
    // stance0/frame0 and quantizes viewing angle0 to its first signed picture.
    constexpr int32 Header = 8, StanceFields = 4, FrameFields = 4;
    if (Values.Num() < Header || Values[0] < 1 || Values[0] > MAX_uint16 || Values[0] > (Values.Num()-Header)/StanceFields) { return false; }
    for (int32 Field : {1,2,3,4,6,7})
    { if (Values[Field] < MIN_int16 || Values[Field] > MAX_uint16) { return false; } }
    const int32 Stances = Values[0];
    int32 Cursor = Header + Stances*StanceFields;
    int32 First = 0;
    // DISOAF prints some unsigned16 fields through signed16 (e.g. amulet values).
    // Preserve those original bit patterns and existing price/rule interpretation.
    const auto Word = [](int32 Value) { return Value >= MIN_int16 && Value <= MAX_uint16; };
    for (int32 Stance = 0; Stance < Stances; ++Stance)
    {
        const int32 At = Header + Stance*StanceFields;
        const int32 Frames = Values[At];
        if (Frames < 1 || Frames > MAX_uint16 || Frames > (Values.Num()-Cursor)/FrameFields
            || !Word(Values[At+1]) || Values[At+2] < 0 || Values[At+2] > 2
            || Values[At+3] < -1 || Values[At+3] >= Stances) { return false; }
        const int32 FrameStart = Cursor;
        Cursor += Frames*FrameFields;
        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            const int32 Fields = FrameStart + Frame*FrameFields;
            const int32 Angles = Values[Fields];
            if ((Angles != 1 && Angles != 4 && Angles != 8) || Angles > Values.Num()-Cursor
                || !Word(Values[Fields+1]) || !Word(Values[Fields+2]) || !Word(Values[Fields+3])) { return false; }
            for (int32 Angle = 0; Angle < Angles; ++Angle)
            {
                const int32 Picture = Values[Cursor++];
                // The original picture number is signed16. Bound before abs()
                // and preserve its sign for callers that support reversed views.
                if (Picture < MIN_int16 || Picture > MAX_int16) { return false; }
                if (Stance == 0 && Frame == 0 && Angle == 0) { First = Picture; }
            }
        }
    }
    if (Cursor != Values.Num()) { return false; }
    SignedPicture = First;
    return true;
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
        if (!FFileHelper::LoadFileToString(Item.Details.UnidentifiedDescription, *(Source / TEXT("OBJDESC") / File)))
        {
            Item.Details.UnidentifiedDescription = TEXT("Unidentified item");
            Output.Logf(TEXT("Missing original unidentified description for item %d."), Type);
        }
        Item.Details.UnidentifiedDescription.TrimStartAndEndInline();
        constexpr int32 ValuePercent[] = {100, 5, 25, 50, 100, 150, 200, 300, 400, 600, 800, 1200, 100, 100, 100, 100};
        Item.Details.ValueCopper = int32(int64(Item.Details.ValueCopper) * ValuePercent[(Type >> 12) & 15] / 100);
        ReadInventoryRules(Source, Lines, Item.Details);
        Items.Add(Type, MoveTemp(Item));
    }
    return true;
}

bool ReadWadStartingEquipment(TArray<FWadStartingEquipment>& Equipment, FOutputDevice& Output)
{
    using namespace NativeWadRules;
    const FString File = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Source/INVENTOR.C");
    FString Source;
    if (!FFileHelper::LoadFileToString(Source, *File))
    {
        Output.Log(TEXT("Could not read the original starting equipment."));
        return false;
    }
    const int32 Begin = Source.Find(TEXT("InventorySetDefaultInventoryForClass(T_void)"));
    const int32 End = Source.Find(TEXT("InventoryReorder"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Begin);
    if (Begin == INDEX_NONE || End <= Begin)
    {
        return false;
    }
    Source = Source.Mid(Begin, End - Begin);
    Equipment.SetNum(CharacterClassCount);
    for (FWadStartingEquipment& Entry : Equipment)
    {
        Entry.Items.Reset();
        Entry.Ammo.Init(0, UE_ARRAY_COUNT(CrossbowAmmo));
    }
    const FRegexPattern Tokens(TEXT("(?s)/\\*.*?\\*/|//[^\\r\\n]*|#ifdef\\s+COMPILE_OPTION_SHAREWARE_DEMO.*?#endif|case\\s+CLASS_([A-Z_]+)\\s*:|InventoryAddObjectToInventory\\s*\\(\\s*INVENTORY_PLAYER\\s*,\\s*([0-9]+)\\s*,\\s*([0-9]+)\\s*\\)|Effect\\s*\\(\\s*EFFECT_TAKE_AMMO\\s*,\\s*EFFECT_TRIGGER_NONE\\s*,\\s*([0-9]+)\\s*,\\s*([0-9]+)\\s*,"));
    FRegexMatcher Match(Tokens, Source);
    int32 Class = INDEX_NONE;
    while (Match.FindNext())
    {
        const FString ClassName = Match.GetCaptureGroup(1);
        if (!ClassName.IsEmpty())
        {
            Class = INDEX_NONE;
            for (int32 Index = 0; Index < CharacterClassCount; ++Index)
            {
                if (ClassName.Equals(CharacterClasses[Index].Name, ESearchCase::IgnoreCase))
                {
                    Class = Index;
                    break;
                }
            }
        }
        if (!Equipment.IsValidIndex(Class))
        {
            continue;
        }
        const FString Type = Match.GetCaptureGroup(2);
        if (!Type.IsEmpty())
        {
            const int32 Count = FCString::Atoi(*Match.GetCaptureGroup(3));
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Equipment[Class].Items.Add(FCString::Atoi(*Type));
            }
        }
        const FString Ammo = Match.GetCaptureGroup(4);
        if (!Ammo.IsEmpty())
        {
            const int32 Index = FCString::Atoi(*Ammo);
            if (!Equipment[Class].Ammo.IsValidIndex(Index))
            {
                return false;
            }
            Equipment[Class].Ammo[Index] += FCString::Atoi(*Match.GetCaptureGroup(5));
        }
    }
    return Equipment.Num() == CharacterClassCount
        && !Equipment.ContainsByPredicate([](const FWadStartingEquipment& Entry) { return Entry.Items.IsEmpty(); });
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
