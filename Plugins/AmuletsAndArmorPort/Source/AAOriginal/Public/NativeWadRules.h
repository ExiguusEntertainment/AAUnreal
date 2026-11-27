#pragma once

#include "CoreMinimal.h"

namespace NativeWadRules
{
    inline constexpr float TicksPerSecond = 70.0f;
    inline constexpr int32 IronDagger = 200;
    inline constexpr int32 IronMace = 201;
    inline constexpr int32 Berry = 243;
    inline constexpr int32 BerryFood = 200; // INVDESC/DES00243
    inline constexpr int32 DaggerWeaponDamage = 6; // INVDESC/DES00200 ready effect
    inline constexpr int32 DaggerWeaponSpeed = 10;
    // Level-one Priest from STATS.C.
    inline constexpr int32 Strength = 15;
    inline constexpr int32 Speed = 20;
    inline constexpr int32 Constitution = 25;
    inline constexpr int32 Magic = 35;
    inline constexpr int32 MaxHealth = (10 + Constitution / 2) * 100;
    inline constexpr int32 MaxMana = Magic * 100;
    inline constexpr int32 MaxFood = 2000;
    inline constexpr int32 FirstLevelThreshold = 2000;
    // SPL00070 is Rejuvenate. The source requires mana strictly above the cost.
    inline constexpr int32 RejuvenateCost = 1000;
    inline constexpr int32 RejuvenatePower = 500;
    inline constexpr int32 RejuvenateHardness = 50;
    inline constexpr int32 GuardianHealth = 700;

    enum EAttribute : int32 { StrengthAttribute, SpeedAttribute, MagicAttribute, AccuracyAttribute,
        StealthAttribute, ConstitutionAttribute, AttributeCount };
    enum ECharacterClass : int32 { CitizenClass, KnightClass, MageClass, WarlockClass, PriestClass,
        RogueClass, ArcherClass, SailorClass, PaladinClass, MercenaryClass, MagicianClass, CharacterClassCount };
    constexpr uint16 ClassFlag(int32 Index)
    {
        return Index >= 0 && Index < CharacterClassCount ? uint16(1u << Index) : 0;
    }
    inline constexpr uint16 AllClasses = uint16((1u << CharacterClassCount) - 1);
    // InventorySetDefaultInventoryForClass gives one water gourd and five random foods.
    inline constexpr int32 StartingWaterGourd = 246;
    inline constexpr int32 StartingFoods[] = {19, 243, 244, 245, 247, 248, 249, 251};
    enum EEquipmentSlot : int32 { HeadSlot, RightArmSlot, LeftArmSlot, LegsSlot, ChestSlot, ShieldSlot, EquipmentSlotCount };
    enum EItemStatus : int32 { InvisibleStatus, TranslucentStatus, StrengthStatus, LavaWalkStatus, SpellShieldStatus,
        FireAttackStatus, ElectricityAttackStatus, AcidAttackStatus, PoisonAttackStatus, PiercingAttackStatus, ManaDrainAttackStatus,
        FireResistStatus, AcidResistStatus, PoisonResistStatus, ElectricityResistStatus, PiercingResistStatus, ManaDrainResistStatus, SpeedStatus, ItemStatusCount };
    enum class ESpellSchool : uint8 { Arcane, Mage, Cleric };
    enum class ESpellEffect : uint8 { WaterWalk, FeatherFall, Shield, Projectile };
    struct FWadSpell
    {
        const TCHAR* Runes;
        const TCHAR* Name;
        ESpellSchool School;
        ESpellEffect Effect;
        int32 Mana;
        int32 Hardness;
        int32 DurationTicks;
        int32 Power;
        int32 ProjectileIndex = INDEX_NONE;
    };
    // Original SPELDESC metadata; projectile records name their individual source files.
    inline constexpr FWadSpell SupportedSpells[] = {
        {TEXT("14"), TEXT("Deflect"), ESpellSchool::Mage, ESpellEffect::Shield, 500, 50, 8400, 25},
        {TEXT("41"), TEXT("Feather Fall"), ESpellSchool::Mage, ESpellEffect::FeatherFall, 250, 50, 2100, 1},
        {TEXT("21"), TEXT("Water Walk"), ESpellSchool::Cleric, ESpellEffect::WaterWalk, 500, 50, 8400, 1},
        {TEXT("34"), TEXT("Magic Dart"), ESpellSchool::Mage, ESpellEffect::Projectile, 150, 50, 0, 1, 0 }, // SPL00001
        {TEXT("347"), TEXT("Magic Missile"), ESpellSchool::Mage, ESpellEffect::Projectile, 200, 40, 0, 1, 1 }, // SPL00003
        {TEXT("13"), TEXT("Confuse"), ESpellSchool::Mage, ESpellEffect::Projectile, 100, 40, 0, 1, 17 }, // SPL00004
        {TEXT("124"), TEXT("Pull"), ESpellSchool::Mage, ESpellEffect::Projectile, 200, 30, 0, 1, 14 }, // SPL00005
        {TEXT("452"), TEXT("Acid Cloud"), ESpellSchool::Mage, ESpellEffect::Projectile, 700, 30, 0, 1, 2 }, // SPL00007
        {TEXT("24"), TEXT("Push"), ESpellSchool::Mage, ESpellEffect::Projectile, 300, 20, 0, 1, 13 }, // SPL00009
        {TEXT("54"), TEXT("Poison Arrow"), ESpellSchool::Mage, ESpellEffect::Projectile, 350, 10, 0, 1, 16 }, // SPL00014
        {TEXT("456"), TEXT("Fireball"), ESpellSchool::Mage, ESpellEffect::Projectile, 1000, 0, 0, 1, 4 }, // SPL00017
        {TEXT("4527"), TEXT("Elemental Acid"), ESpellSchool::Mage, ESpellEffect::Projectile, 1000, -5, 0, 1, 3 }, // SPL00022
        {TEXT("564"), TEXT("Elemental Fire"), ESpellSchool::Mage, ESpellEffect::Projectile, 1500, -15, 0, 1, 5 }, // SPL00028
        {TEXT("549"), TEXT("Lightning Bolt"), ESpellSchool::Mage, ESpellEffect::Projectile, 1000, -25, 0, 1, 8 }, // SPL00033
        {TEXT("964"), TEXT("Fire Blast"), ESpellSchool::Mage, ESpellEffect::Projectile, 1500, -30, 0, 1, 28 }, // SPL00037
        {TEXT("5897"), TEXT("Death Cloud"), ESpellSchool::Mage, ESpellEffect::Projectile, 2000, -35, 0, 1, 6 }, // SPL00039
        {TEXT("37"), TEXT("Confuse"), ESpellSchool::Arcane, ESpellEffect::Projectile, 100, 40, 0, 1, 17 }, // SPL00046
        {TEXT("78"), TEXT("Pull"), ESpellSchool::Arcane, ESpellEffect::Projectile, 200, 30, 0, 1, 14 }, // SPL00049
        {TEXT("48"), TEXT("Push"), ESpellSchool::Arcane, ESpellEffect::Projectile, 300, 20, 0, 1, 13 }, // SPL00051
        {TEXT("12"), TEXT("Earth Smite"), ESpellSchool::Cleric, ESpellEffect::Projectile, 1000, 40, 0, 1, 7 }, // SPL00074
        {TEXT("25"), TEXT("Confuse"), ESpellSchool::Cleric, ESpellEffect::Projectile, 100, 40, 0, 1, 17 }, // SPL00075
        {TEXT("23"), TEXT("Pull"), ESpellSchool::Cleric, ESpellEffect::Projectile, 200, 30, 0, 1, 14 }, // SPL00078
        {TEXT("43"), TEXT("Push"), ESpellSchool::Cleric, ESpellEffect::Projectile, 300, 20, 0, 1, 13 }, // SPL00079
        {TEXT("697"), TEXT("Gods Wrath"), ESpellSchool::Cleric, ESpellEffect::Projectile, 1000, -40, 0, 1, 8 }, // SPL00101
        {TEXT("46"), TEXT("Wizard Lock"), ESpellSchool::Mage, ESpellEffect::Projectile, 500, 30, 0, 1, 9}, // SPL00006
        {TEXT("146"), TEXT("Knock"), ESpellSchool::Mage, ESpellEffect::Projectile, 1000, 10, 0, 1, 10}, // SPL00011
        {TEXT("467"), TEXT("Lock"), ESpellSchool::Arcane, ESpellEffect::Projectile, 1000, 30, 0, 1, 9}, // SPL00048
        {TEXT("1467"), TEXT("Knock"), ESpellSchool::Arcane, ESpellEffect::Projectile, 1000, 10, 0, 1, 10}, // SPL00053
        {TEXT("42"), TEXT("Knock"), ESpellSchool::Cleric, ESpellEffect::Projectile, 500, 40, 0, 1, 10}, // SPL00076
        {TEXT("134"), TEXT("Angry Word"), ESpellSchool::Mage, ESpellEffect::Projectile, 300, 20, 0, 1, 18}, // SPL00010
        {TEXT("134"), TEXT("Angry Word"), ESpellSchool::Arcane, ESpellEffect::Projectile, 300, 20, 0, 1, 18}, // SPL00052
        {TEXT("184"), TEXT("Dispel Magic"), ESpellSchool::Mage, ESpellEffect::Projectile, 800, 10, 0, 1, 11}, // SPL00015
        {TEXT("138"), TEXT("Dispel Magic"), ESpellSchool::Arcane, ESpellEffect::Projectile, 800, 10, 0, 1, 11}, // SPL00064
        {TEXT("68"), TEXT("Dispel Magic"), ESpellSchool::Cleric, ESpellEffect::Projectile, 800, 10, 0, 1, 11}, // SPL00082
        {TEXT("127"), TEXT("Slow"), ESpellSchool::Mage, ESpellEffect::Projectile, 1000, -15, 0, 1, 19}, // SPL00029
        {TEXT("157"), TEXT("Earthbind"), ESpellSchool::Mage, ESpellEffect::Projectile, 500, 0, 0, 1, 15}, // SPL00102
        {TEXT("17"), TEXT("Earthbind"), ESpellSchool::Arcane, ESpellEffect::Projectile, 500, 0, 0, 1, 15}, // SPL00103
        {TEXT("637"), TEXT("Earthbind"), ESpellSchool::Cleric, ESpellEffect::Projectile, 500, 0, 0, 1, 15}, // SPL00104
    };
    constexpr ESpellSchool SchoolForClass(int32 Index)
    {
        return Index == PriestClass || Index == PaladinClass ? ESpellSchool::Cleric
            : Index == MageClass || Index == WarlockClass || Index == MagicianClass ? ESpellSchool::Mage : ESpellSchool::Arcane;
    }
    constexpr bool IsRuneItem(int32 Type) { return Type >= 300 && Type <= 317; }
    constexpr int32 RuneItemForClass(int32 Class, int32 Rune)
    {
        if (Rune < 1 || Rune > 9) { return INDEX_NONE; }
        const ESpellSchool School = SchoolForClass(Class);
        return School == ESpellSchool::Mage ? 299 + Rune : School == ESpellSchool::Cleric ? 308 + Rune
            : Rune <= 4 ? 299 + Rune : 304 + Rune;
    }
    inline constexpr int32 PriestAttributes[AttributeCount] = {Strength, Speed, Magic, 15, 20, Constitution};
    inline constexpr int32 PriestAdvancement[AttributeCount] = {2, 1, 3, 2, 2, 2};
    struct FWadCharacterClass
    {
        const TCHAR* Name;
        int32 Base[AttributeCount];
        int32 Advancement[AttributeCount];
        int32 ManaRecoveryPercent = 100;
    };
    inline constexpr FWadCharacterClass CharacterClasses[] = {
        {TEXT("Citizen"), {25, 25, 25, 25, 25, 25}, {2, 2, 2, 2, 2, 2}},
        {TEXT("Knight"), {35, 15, 10, 20, 15, 35}, {3, 2, 1, 2, 1, 3}},
        {TEXT("Mage"), {10, 25, 35, 25, 20, 15}, {1, 2, 3, 3, 2, 1}, 300},
        {TEXT("Warlock"), {25, 20, 30, 20, 15, 20}, {2, 2, 2, 2, 2, 2}, 150},
        {TEXT("Priest"), {15, 20, 35, 15, 20, 25}, {2, 1, 3, 2, 2, 2}, 300},
        {TEXT("Rogue"), {15, 30, 15, 25, 30, 15}, {2, 2, 1, 2, 3, 2}},
        {TEXT("Archer"), {20, 20, 15, 35, 20, 20}, {2, 2, 2, 3, 1, 2}},
        {TEXT("Sailor"), {25, 25, 10, 20, 15, 35}, {2, 2, 1, 2, 2, 3}},
        {TEXT("Paladin"), {30, 20, 25, 15, 10, 30}, {3, 2, 2, 2, 1, 2}},
        {TEXT("Mercenary"), {30, 20, 10, 25, 20, 25}, {2, 2, 2, 2, 2, 2}},
        {TEXT("Magician"), {15, 30, 30, 20, 20, 15}, {1, 3, 3, 2, 1, 2}, 150},
    };
    static_assert(UE_ARRAY_COUNT(CharacterClasses) == CharacterClassCount);
    inline constexpr int32 FeatherfallPotion = 817;
    inline constexpr int32 WaterWalkRing = 400;
    inline constexpr int32 WaterWalkPotion = 812;
    inline constexpr double WaterWalkPotionSeconds = 5000.0 / TicksPerSecond;
    inline constexpr double FeatherfallPotionSeconds = 5000.0 / TicksPerSecond;
    inline constexpr int32 Crossbow = 28;
    inline constexpr int32 NormalAmmoBolt = 206;
    inline constexpr int32 NormalQuiver = 144;
    inline constexpr int32 NormalQuiverBolts = 12;
    inline constexpr int32 CrossbowDamage = 300; // CREDATA.H, Normal Effect Bolt (886)
    inline constexpr int32 CrossbowFrames = 79; // WEAPONS/XBOW/XBOW.ANI
    enum class EProjectileDamage : uint8 { Normal, Poison, Piercing, Fire, Electricity, ManaDrain, Acid };
    constexpr uint8 DamageFlag(EProjectileDamage Effect) { return uint8(1 << uint8(Effect)); }
    struct FWadAmmo
    {
        int32 Bolt;
        int32 Quiver;
        int32 Missile;
        int32 Damage;
        EProjectileDamage Effect;
        const TCHAR* Name;
        const TCHAR* Art;
    };
    // Banner order and the effect-to-object mapping in CLIENT.C. The two elemental labels
    // retain their original mapping to 889 and 890, whose damage types come from CREDATA.H.
    inline constexpr FWadAmmo CrossbowAmmo[] = {
        {206, 144, 886, 300, EProjectileDamage::Normal, TEXT("Normal"), TEXT("ARROW1/BOLT0.CPC")},
        {222, 146, 887, 300, EProjectileDamage::Poison, TEXT("Poison"), TEXT("ARROW2/1.CPC")},
        {229, 147, 888, 300, EProjectileDamage::Piercing, TEXT("Piercing"), TEXT("ARROW7/1.CPC")},
        {218, 145, 889, 500, EProjectileDamage::Electricity, TEXT("Fire"), TEXT("ARROW5/1.CPC")},
        {241, 149, 890, 600, EProjectileDamage::Fire, TEXT("Electricity"), TEXT("ARROW4/1.CPC")},
        {242, 150, 891, 600, EProjectileDamage::ManaDrain, TEXT("Mana Drain"), TEXT("ARROW6/1.CPC")},
        {240, 148, 892, 300, EProjectileDamage::Acid, TEXT("Acid"), TEXT("ARROW1/BOLT0.CPC")},
    };
    inline constexpr int32 ShieldPotionFirst = 808;
    inline constexpr int32 ShieldPotionLast = 811;
    inline constexpr double ShieldPotionSeconds = 10000.0 / TicksPerSecond;
    inline constexpr double FeatherfallSpeed = 15000.0 * TicksPerSecond / 65536.0;

    constexpr int32 CoinValueCopper(int32 Type)
    {
        constexpr int32 Values[] = {1, 5, 10, 50, 100, 500, 1000, 5000};
        return Type >= 100 && Type <= 107 ? Values[Type - 100] : 0;
    }

    constexpr int64 CoinDenominationCount(int64 Copper, int32 Type)
    {
        const int32 Value = CoinValueCopper(Type);
        return Value == 0 ? 0 : Type == 106 ? Copper / Value : Copper / Value % 10;
    }
    // INVDESC/DES00360, DES00613 and DES00805.
    inline constexpr int32 FlyingScroll = 360;
    inline constexpr int32 FlyingAmulet = 613;
    inline constexpr int32 FlyingPotion = 805;
    inline constexpr double FlyingScrollSeconds = 5000.0 / TicksPerSecond;
    inline constexpr double FlyingPotionSeconds = 4000.0 / TicksPerSecond;
    inline constexpr int32 JugurthaJournal = 957;
    inline constexpr int32 ChurchKey = 258;
    inline constexpr int32 AndrewNecklace = 958;
    inline constexpr int32 OrbOfDefense = 954;
    inline constexpr int32 OrbOfTheUndead = 955;
    inline constexpr int32 ElymasNotes = 380;
    inline constexpr int32 WandOfDisintegration = 918;
    inline constexpr int32 DisintegrationManaCost = 1500; // INVDESC/DES00918
    inline constexpr int32 PlasmaDamage = 2000; // CREDATA.H, Plasma Ball (4017)
    inline constexpr float PlasmaBlastRadius = 250.0f;
    struct FWadWandProjectile
    {
        int32 SourceIndex;
        int32 Type;
        int32 Damage;
        float Radius;
        uint8 DamageMask;
        bool bHoming;
        const TCHAR* Art;
    };
    inline constexpr int32 AbsoluteDoorLock = 65535;
    constexpr int32 AdjustDoorLockStrength(int32 Current, bool bLock)
    {
        return Current == AbsoluteDoorLock ? Current : bLock ? FMath::Min(255, Current + 10) : FMath::Max(0, Current - 10);
    }
    inline constexpr double MaxCreatureControlSeconds = (65535.0 + 2100.0) / TicksPerSecond;
    inline constexpr uint8 SpecialDamage = 128;
    enum class ESpecialDamage : uint8 { Lock, Unlock, Dispel, Push, Pull, Earthbind, Confuse, Berserk, Slow, Paralyze };
    constexpr uint8 SpecialDamageFlag(ESpecialDamage Effect) { return SpecialDamage | uint8(Effect); }
    inline constexpr uint8 NormalDamage = DamageFlag(EProjectileDamage::Normal);
    struct FWadPlayerDamage
    {
        int32 Health = 0;
        int32 Poison = 0;
        int32 Mana = 0;
    };
    // STATS.C: player bonuses are sequential; resistance is capped at 80%, then armor applies.
    inline FWadPlayerDamage ResolvePlayerDamage(int32 Amount, uint8 Mask, uint8 Resistance, int32 ArmorClass, int32 ArmorRating)
    {
        FWadPlayerDamage Result;
        if (Amount <= 0 || (Mask & SpecialDamage)) { return Result; }
        const auto Has = [Mask](EProjectileDamage Element) { return (Mask & DamageFlag(Element)) != 0; };
        const auto Resists = [Resistance](EProjectileDamage Element) { return (Resistance & DamageFlag(Element)) != 0; };
        if (Has(EProjectileDamage::Fire) && !Resists(EProjectileDamage::Fire)) { Amount += Amount >> 2; }
        if (Has(EProjectileDamage::Poison)) { Result.Poison = Amount / (Resists(EProjectileDamage::Poison) ? 40 : 10); }
        if (Has(EProjectileDamage::Electricity) && !Resists(EProjectileDamage::Electricity))
        {
            if (ArmorClass == 1) { Amount += Amount >> 2; }
            else if (ArmorClass == 2) { Amount += Amount >> 1; }
        }
        if (Has(EProjectileDamage::ManaDrain)) { Result.Mana = Amount / (Resists(EProjectileDamage::ManaDrain) ? 4 : 1); }
        const int32 Effects = FMath::CountBits(Mask & 127);
        const int32 Resisted = FMath::CountBits(Mask & Resistance & 127);
        if (Effects > 0)
        {
            Amount = Resisted * 5 > Effects * 4 ? Amount / 5 : int32(int64(Amount) * (Effects - Resisted) / Effects);
        }
        if (!Has(EProjectileDamage::Piercing) || Resists(EProjectileDamage::Piercing))
        {
            Amount = int32(int64(Amount) * (100 - FMath::Clamp(ArmorRating, 0, 100) / 2) / 100);
        }
        Result.Health = Amount;
        return Result;
    }
    inline constexpr uint8 PlasmaElements = NormalDamage | DamageFlag(EProjectileDamage::Fire);
    inline constexpr uint8 DeathBallElements = PlasmaElements | DamageFlag(EProjectileDamage::Poison)
        | DamageFlag(EProjectileDamage::Electricity) | DamageFlag(EProjectileDamage::Piercing) | DamageFlag(EProjectileDamage::ManaDrain);
    // CLIENT.C maps the wand effect's index to an object. Damage and radius come from CREDATA.H.
    inline constexpr FWadWandProjectile WandProjectiles[] = {
        {0, 4011, 100, 100.0f, NormalDamage, false, TEXT("MISSIL12/1.CPC")},
        {1, 4018, 100, 60.0f, NormalDamage, true, TEXT("HMISS12/1.CPC")},
        {2, 4004, 500, 100.0f, DamageFlag(EProjectileDamage::Acid), false, TEXT("MISSILE5/1.CPC")},
        {3, 4019, 500, 60.0f, DamageFlag(EProjectileDamage::Acid), true, TEXT("HMISS5/1.CPC")},
        {4, 4005, 750, 100.0f, DamageFlag(EProjectileDamage::Fire), false, TEXT("MISSILE6/1.CPC")},
        {5, 4020, 1000, 60.0f, DamageFlag(EProjectileDamage::Fire), true, TEXT("HMISS6/1.CPC")},
        {6, 4003, 2000, 100.0f, DeathBallElements, true, TEXT("MISSILE4/1.CPC")},
        {7, 4014, 500, 100.0f, NormalDamage, false, TEXT("MISSIL15/1.CPC")},
        {8, 4006, 1000, 100.0f, DamageFlag(EProjectileDamage::Electricity), false, TEXT("MISSILE7/1.CPC")},
        {12, 4001, 1000, 100.0f, DamageFlag(EProjectileDamage::ManaDrain), false, TEXT("MISSILE2/1.CPC")},
        {16, 4015, 250, 100.0f, DamageFlag(EProjectileDamage::Poison), false, TEXT("MISSIL16/1.CPC")},
        {13, 31, 2500, 20.0f, SpecialDamageFlag(ESpecialDamage::Push), false, TEXT("FIREBALL/BOLT01.CPC")},
        {14, 4002, 2500, 100.0f, SpecialDamageFlag(ESpecialDamage::Pull), false, TEXT("MISSILE3/1.CPC")},
        {9, 4012, 10, 100.0f, SpecialDamageFlag(ESpecialDamage::Lock), false, TEXT("MISSIL13/1.CPC")},
        {10, 4013, 10, 100.0f, SpecialDamageFlag(ESpecialDamage::Unlock), false, TEXT("MISSIL14/1.CPC")},
        {18, 4000, 100, 100.0f, SpecialDamageFlag(ESpecialDamage::Berserk), false, TEXT("MISSILE1/1.CPC")},
        {11, 4007, 100, 100.0f, SpecialDamageFlag(ESpecialDamage::Dispel), false, TEXT("MISSILE8/1.CPC")},
        {15, 4008, 100, 100.0f, SpecialDamageFlag(ESpecialDamage::Earthbind), false, TEXT("MISSILE9/1.CPC")},
        {19, 4009, 100, 100.0f, SpecialDamageFlag(ESpecialDamage::Slow), false, TEXT("MISSIL10/1.CPC")},
        {17, 4010, 100, 100.0f, SpecialDamageFlag(ESpecialDamage::Confuse), false, TEXT("MISSIL11/1.CPC")},
        {28, 4017, PlasmaDamage, PlasmaBlastRadius, PlasmaElements, true, TEXT("PBALL/PBALL001.CPC")},
    };
    constexpr const FWadWandProjectile* FindWandProjectile(int32 SourceIndex)
    {
        for (const auto& Projectile : WandProjectiles)
        {
            if (Projectile.SourceIndex == SourceIndex)
            {
                return &Projectile;
            }
        }
        return nullptr;
    }
    constexpr const FWadWandProjectile* FindHomingMissile(int32 Type)
    {
        for (const auto& Missile : WandProjectiles)
        {
            if (Missile.Type == Type && Missile.bHoming)
            {
                return &Missile;
            }
        }
        return nullptr;
    }
    inline constexpr int32 OrbOfDestruction = 956;
    inline constexpr int32 Key1 = 257;
    inline constexpr int32 Key4 = 260;
    inline constexpr int32 Key9 = 265;
    inline constexpr int32 ElmoreKey3 = 259;
    inline constexpr int32 ElmoreKey5 = 261;
    inline constexpr int32 ElmoreKey6 = 262;
    inline constexpr int32 ElmoreKey8 = 264;
    inline constexpr int32 ChurchMountedKnight = 1008;
    inline constexpr int32 ChurchFootKnight = 1009;
    inline constexpr int32 GrayCrossbowBolt = 886;
    inline constexpr int32 PoisonCrossbowBolt = 887;
    inline constexpr int32 PiercingCrossbowBolt = 888;
    inline constexpr int32 ChurchBoltDamage = 300;
    inline constexpr int32 AcidBall = 4004;
    inline constexpr int32 AcidBallDamage = 500;

    enum class ERejuvenateResult : uint8 { Success, Fizzle, InsufficientMana };

    inline ERejuvenateResult ResolveRejuvenate(int32& Health, int32& Mana, int32 HealthLimit, int32 MagicAttributeValue,
        int32 Level, int32 Roll)
    {
        if (Mana <= RejuvenateCost)
        {
            return ERejuvenateResult::InsufficientMana;
        }
        if (Roll > MagicAttributeValue + RejuvenateHardness + 2 * Level)
        {
            Mana -= RejuvenateCost / 2;
            return ERejuvenateResult::Fizzle;
        }
        Mana -= RejuvenateCost;
        Health = FMath::Min(HealthLimit, Health + RejuvenatePower);
        return ERejuvenateResult::Success;
    }
}
