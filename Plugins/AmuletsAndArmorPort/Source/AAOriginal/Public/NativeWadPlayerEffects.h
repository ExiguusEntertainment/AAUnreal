#pragma once

#include "CoreMinimal.h"

enum class EWadTimedEffectKind : uint8 { Status, Flight, WaterWalk, FeatherFall, ShieldPotion, Paralysis };

struct FWadTimedPlayerEffect
{
    FString Owner;
    EWadTimedEffectKind Kind = EWadTimedEffectKind::Status;
    int32 Status = INDEX_NONE;
    int32 Power = 0;
    double Seconds = 0.0;
};

// Only temporary effects live here. Worn/readied object effects remain derived
// from inventory, so Dispel cannot choose them or restore stale equipment data.
class AAORIGINAL_API FWadPlayerEffects
{
public:
    static FString ConsumableOwner(int32 Type);
    static FString SpellOwner(int32 Definition);
    static FString LegacyOwner(EWadTimedEffectKind Kind, int32 Status);
    static constexpr const TCHAR* IncomingControlOwner = TEXT("control:incoming");

    bool Apply(const FWadTimedPlayerEffect& Effect);
    bool RemoveOwner(const FString& Owner);
    void RemoveKind(EWadTimedEffectKind Kind);
    bool Dispel(int32 Selection);
    void Tick(double DeltaSeconds);
    void Reset() { Effects.Reset(); }
    int32 Num() const { return Effects.Num(); }
    double Remaining(const FString& Owner) const;
    int32 Power(EWadTimedEffectKind Kind, int32 Status = INDEX_NONE) const;
    double Duration(EWadTimedEffectKind Kind, int32 Status = INDEX_NONE) const;
    const TArray<FWadTimedPlayerEffect>& GetEffects() const { return Effects; }

    // A modern empty payload is authoritative. Only a missing payload migrates
    // old aggregate fields, whose unknown original owners are retained as Legacy.
    FString Encode() const;
    bool Decode(const FString& Payload);
    bool Restore(const TCHAR* Payload, TConstArrayView<FWadTimedPlayerEffect> Legacy);

private:
    static bool IsValid(const FWadTimedPlayerEffect& Effect);
    TArray<FWadTimedPlayerEffect> Effects;
};
