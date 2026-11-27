#pragma once

#include "CoreMinimal.h"

namespace NativeWadFlightOwners
{
    // Version1, canonical ascending carried-object indices. Explicit "1" means empty.
    // This is distinct from temporary spell/item owners in WadOwnedEffects.
    inline bool Decode(const FString& Payload, TArray<int32>& Owners)
    {
        Owners.Reset();
        if (Payload == TEXT("1")) { return true; }
        if (!Payload.StartsWith(TEXT("1~")) || Payload.Len() > 256) { return false; }
        TArray<FString> Tokens;
        Payload.Mid(2).ParseIntoArray(Tokens, TEXT(","), false);
        if (Tokens.IsEmpty() || Tokens.Num() > 12) { return false; }
        int32 Previous = INDEX_NONE;
        for (const FString& Token : Tokens)
        {
            int32 Slot = INDEX_NONE;
            // The roundtrip rejects overflow, whitespace, prefixes, signs and leading zeroes.
            if (!LexTryParseString(Slot, *Token) || Slot < 0 || Slot <= Previous || LexToString(Slot) != Token)
            { Owners.Reset(); return false; }
            Owners.Add(Slot);
            Previous = Slot;
        }
        return true;
    }
}
