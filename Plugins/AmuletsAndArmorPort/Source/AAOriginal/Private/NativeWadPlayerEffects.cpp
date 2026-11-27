#include "NativeWadPlayerEffects.h"

#include "NativeWadPlayer.h"
#include "NativeWadRules.h"

namespace
{
    bool IsDecimalDigit(TCHAR Character) { return Character >= TEXT('0') && Character <= TEXT('9'); }

    bool ParseEffectInteger(const FString& Token, int32& Value)
    {
        if (Token.IsEmpty()) { return false; }
        int32 Index = 0;
        const bool bNegative = Token[0] == TEXT('-');
        if (bNegative || Token[0] == TEXT('+')) { ++Index; }
        if (Index == Token.Len()) { return false; }
        const uint32 Limit = bNegative ? uint32(MAX_int32) + 1u : uint32(MAX_int32);
        uint32 Magnitude = 0;
        for (; Index < Token.Len(); ++Index)
        {
            if (!IsDecimalDigit(Token[Index])) { return false; }
            const uint32 Digit = uint32(Token[Index] - TEXT('0'));
            if (Magnitude > (Limit - Digit) / 10u) { return false; }
            Magnitude = Magnitude * 10u + Digit;
        }
        Value = bNegative ? (Magnitude == uint32(MAX_int32) + 1u ? MIN_int32 : -int32(Magnitude)) : int32(Magnitude);
        return true;
    }

    bool ParseEffectDuration(const FString& Token, double& Value)
    {
        // LexTryParseString accepts numeric prefixes. Validate a complete decimal
        // token, including the scientific notation emitted by %.17g, first.
        if (Token.IsEmpty()) { return false; }
        int32 Index = Token[0] == TEXT('+') || Token[0] == TEXT('-') ? 1 : 0;
        const int32 WholeStart = Index;
        while (Index < Token.Len() && IsDecimalDigit(Token[Index])) { ++Index; }
        if (Index == WholeStart) { return false; }
        if (Index < Token.Len() && Token[Index] == TEXT('.'))
        {
            const int32 FractionStart = ++Index;
            while (Index < Token.Len() && IsDecimalDigit(Token[Index])) { ++Index; }
            if (Index == FractionStart) { return false; }
        }
        if (Index < Token.Len() && (Token[Index] == TEXT('e') || Token[Index] == TEXT('E')))
        {
            ++Index;
            if (Index < Token.Len() && (Token[Index] == TEXT('+') || Token[Index] == TEXT('-'))) { ++Index; }
            const int32 ExponentStart = Index;
            while (Index < Token.Len() && IsDecimalDigit(Token[Index])) { ++Index; }
            if (Index == ExponentStart) { return false; }
        }
        return Index == Token.Len() && LexTryParseString(Value, *Token)
            && FMath::IsFinite(Value) && Value > 0.0 && Value <= 1200.0;
    }
}

FString FWadPlayerEffects::ConsumableOwner(int32 Type) { return FString::Printf(TEXT("item:%d"), Type); }
FString FWadPlayerEffects::SpellOwner(int32 Definition) { return FString::Printf(TEXT("spell:%d"), Definition); }
FString FWadPlayerEffects::LegacyOwner(EWadTimedEffectKind Kind, int32 Status)
{ return FString::Printf(TEXT("legacy:%d:%d"), int32(Kind), Status); }

bool FWadPlayerEffects::IsValid(const FWadTimedPlayerEffect& Effect)
{
    using namespace NativeWadRules;
    if (Effect.Owner.IsEmpty() || Effect.Owner.Len() > 64 || !FMath::IsFinite(Effect.Seconds)
        || Effect.Seconds <= 0.0 || Effect.Seconds > 1200.0 || Effect.Power < -28000 || Effect.Power > 28000) { return false; }
    for (const TCHAR Character : Effect.Owner)
    {
        if (!FChar::IsAlnum(Character) && Character != TEXT(':') && Character != TEXT('-') && Character != TEXT('_')) { return false; }
    }
    switch (Effect.Kind)
    {
    case EWadTimedEffectKind::Status: return Effect.Status >= 0 && Effect.Status < ItemStatusCount;
    case EWadTimedEffectKind::ShieldPotion:
        return Effect.Status >= 0 && Effect.Status <= ShieldPotionLast - ShieldPotionFirst && Effect.Power == 10 + 5 * Effect.Status;
    case EWadTimedEffectKind::Flight: case EWadTimedEffectKind::WaterWalk:
    case EWadTimedEffectKind::FeatherFall: case EWadTimedEffectKind::Paralysis:
        return Effect.Status == INDEX_NONE && Effect.Power == 1;
    default: return false;
    }
}

bool FWadPlayerEffects::Apply(const FWadTimedPlayerEffect& Effect)
{
    if (!IsValid(Effect)) { return false; }
    FWadTimedPlayerEffect Updated = Effect; // The caller may be refreshing a record from GetEffects().
    const int32 Existing = Effects.IndexOfByPredicate([&](const auto& Entry) { return Entry.Owner == Updated.Owner; });
    if (Existing != INDEX_NONE) { Effects.RemoveAt(Existing); }
    else if (Effects.Num() >= 128) { return false; }
    Effects.Add(MoveTemp(Updated));
    return true;
}

bool FWadPlayerEffects::RemoveOwner(const FString& Owner)
{
    const FString Key = Owner;
    return Effects.RemoveAll([&](const auto& Entry) { return Entry.Owner == Key; }) > 0;
}

void FWadPlayerEffects::RemoveKind(EWadTimedEffectKind Kind)
{ Effects.RemoveAll([&](const auto& Entry) { return Entry.Kind == Kind; }); }

bool FWadPlayerEffects::Dispel(int32 Selection)
{
    if (!Effects.IsValidIndex(Selection)) { return false; }
    const FString Owner = Effects[Selection].Owner;
    return RemoveOwner(Owner);
}

void FWadPlayerEffects::Tick(double DeltaSeconds)
{
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0) { return; }
    for (auto& Effect : Effects) { Effect.Seconds = FMath::Max(0.0, Effect.Seconds - DeltaSeconds); }
    Effects.RemoveAll([](const auto& Entry) { return Entry.Seconds == 0.0; });
}

double FWadPlayerEffects::Remaining(const FString& Owner) const
{
    const auto* Effect = Effects.FindByPredicate([&](const auto& Entry) { return Entry.Owner == Owner; });
    return Effect ? Effect->Seconds : 0.0;
}

int32 FWadPlayerEffects::Power(EWadTimedEffectKind Kind, int32 Status) const
{
    int32 Total = 0;
    for (const auto& Entry : Effects) { if (Entry.Kind == Kind && Entry.Status == Status) { Total += Entry.Power; } }
    return Total;
}

double FWadPlayerEffects::Duration(EWadTimedEffectKind Kind, int32 Status) const
{
    double Seconds = 0.0;
    for (const auto& Entry : Effects) { if (Entry.Kind == Kind && Entry.Status == Status) { Seconds = FMath::Max(Seconds, Entry.Seconds); } }
    return Seconds;
}

FString FWadPlayerEffects::Encode() const
{
    FString Payload(TEXT("1"));
    for (const auto& Entry : Effects)
    {
        Payload += FString::Printf(TEXT("~%s,%d,%d,%d,%.17g"), *Entry.Owner, int32(Entry.Kind), Entry.Status, Entry.Power, Entry.Seconds);
    }
    return Payload;
}

bool FWadPlayerEffects::Decode(const FString& Payload)
{
    if (Payload.Len() > 32768) { return false; }
    TArray<FString> Rows;
    Payload.ParseIntoArray(Rows, TEXT("~"), false);
    if (Rows.IsEmpty() || Rows[0] != TEXT("1") || Rows.Num() > 129) { return false; }
    FWadPlayerEffects Parsed;
    for (int32 Index = 1; Index < Rows.Num(); ++Index)
    {
        TArray<FString> Fields;
        Rows[Index].ParseIntoArray(Fields, TEXT(","), false);
        FWadTimedPlayerEffect Entry;
        int32 Kind = INDEX_NONE;
        if (Fields.Num() != 5 || !ParseEffectInteger(Fields[1], Kind) || Kind < 0 || Kind > int32(EWadTimedEffectKind::Paralysis)
            || !ParseEffectInteger(Fields[2], Entry.Status) || !ParseEffectInteger(Fields[3], Entry.Power)
            || !ParseEffectDuration(Fields[4], Entry.Seconds)) { return false; }
        Entry.Owner = Fields[0];
        Entry.Kind = EWadTimedEffectKind(Kind);
        if (Parsed.Remaining(Entry.Owner) > 0.0 || !Parsed.Apply(Entry)) { return false; }
    }
    Effects = MoveTemp(Parsed.Effects);
    return true;
}

bool FWadPlayerEffects::Restore(const TCHAR* Payload, TConstArrayView<FWadTimedPlayerEffect> Legacy)
{
    if (Payload) { return Decode(Payload); }
    FWadPlayerEffects Parsed;
    for (const auto& Entry : Legacy) { if (Entry.Seconds > 0.0 && !Parsed.Apply(Entry)) { return false; } }
    Effects = MoveTemp(Parsed.Effects);
    return true;
}

void ANativeWadPlayer::RefreshTimedEffectCache()
{
    FlightSeconds = TimedEffects.Duration(EWadTimedEffectKind::Flight);
    WaterWalkSeconds = TimedEffects.Duration(EWadTimedEffectKind::WaterWalk);
    FeatherfallSeconds = TimedEffects.Duration(EWadTimedEffectKind::FeatherFall);
    ParalysisSeconds = TimedEffects.Duration(EWadTimedEffectKind::Paralysis);
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(ShieldSeconds); ++Index)
    { ShieldSeconds[Index] = TimedEffects.Duration(EWadTimedEffectKind::ShieldPotion, Index); }
    for (int32 Index = 0; Index < NativeWadRules::ItemStatusCount; ++Index)
    {
        ItemStatusSeconds[Index] = TimedEffects.Duration(EWadTimedEffectKind::Status, Index);
        ItemStatusPower[Index] = TimedEffects.Power(EWadTimedEffectKind::Status, Index);
    }
    SynchronizeMagicMapOwners();
}

void ANativeWadPlayer::ApplyTimedEffect(const FString& EffectOwner, EWadTimedEffectKind Kind, int32 Status, int32 Power, double Seconds)
{
    const bool bApplied = TimedEffects.Apply({EffectOwner, Kind, Status, Power, Seconds});
    if (!bApplied)
    { UE_LOG(LogTemp, Error, TEXT("Rejected invalid WAD temporary effect for %s."), *EffectOwner); }
    if (bApplied && Kind == EWadTimedEffectKind::Status && Status == NativeWadRules::MagicMapStatus)
    { MagicMap.StartOwner(EffectOwner, Power); }
    RefreshTimedEffectCache();
}

void ANativeWadPlayer::RestoreTimedEffects(const TCHAR* Payload)
{
    TArray<FWadTimedPlayerEffect> Legacy;
    const auto Add = [&](EWadTimedEffectKind Kind, int32 Status, int32 Power, double Seconds)
    {
        if (Seconds > 0.0) { Legacy.Add({FWadPlayerEffects::LegacyOwner(Kind, Status), Kind, Status, Power, Seconds}); }
    };
    Add(EWadTimedEffectKind::Flight, INDEX_NONE, 1, FlightSeconds);
    Add(EWadTimedEffectKind::WaterWalk, INDEX_NONE, 1, WaterWalkSeconds);
    Add(EWadTimedEffectKind::FeatherFall, INDEX_NONE, 1, FeatherfallSeconds);
    Add(EWadTimedEffectKind::Paralysis, INDEX_NONE, 1, ParalysisSeconds);
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(ShieldSeconds); ++Index)
    { Add(EWadTimedEffectKind::ShieldPotion, Index, 10 + 5 * Index, ShieldSeconds[Index]); }
    for (int32 Index = 0; Index < NativeWadRules::ItemStatusCount; ++Index)
    { Add(EWadTimedEffectKind::Status, Index, ItemStatusPower[Index], ItemStatusSeconds[Index]); }
    TimedEffects.Reset();
    if (!TimedEffects.Restore(Payload, Legacy))
    { UE_LOG(LogTemp, Error, TEXT("Rejected malformed WAD owned-effect state; legacy fields were not substituted.")); }
    RefreshTimedEffectCache();
}
