#pragma once

#include "CoreMinimal.h"

namespace NativeWadRules
{
    inline constexpr float TicksPerSecond = 70.0f;
    inline constexpr int32 IronDagger = 200;
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
    enum EEquipmentSlot : int32 { HeadSlot, RightArmSlot, LeftArmSlot, LegsSlot, ChestSlot, ShieldSlot, EquipmentSlotCount };
    inline constexpr int32 PriestAttributes[AttributeCount] = {Strength, Speed, Magic, 15, 20, Constitution};
    inline constexpr int32 PriestAdvancement[AttributeCount] = {2, 1, 3, 2, 2, 2};
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
    inline constexpr int32 ShieldPotionFirst = 808;
    inline constexpr int32 ShieldPotionLast = 811;
    inline constexpr double ShieldPotionSeconds = 10000.0 / TicksPerSecond;
    inline constexpr double FeatherfallSpeed = 15000.0 * TicksPerSecond / 65536.0;

    constexpr int32 CoinValueCopper(int32 Type)
    {
        constexpr int32 Values[] = {1, 5, 10, 50, 100, 500, 1000, 5000};
        return Type >= 100 && Type <= 107 ? Values[Type - 100] : 0;
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
