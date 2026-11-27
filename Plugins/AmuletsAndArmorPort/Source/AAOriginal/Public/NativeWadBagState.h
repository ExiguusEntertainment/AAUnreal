#pragma once
#include "NativeWadBagLayout.h"
#include "NativeWadRules.h"
struct FURL;

namespace NativeWadBagState
{
    struct FItemRule
    {
        FWadBagItem Item;
        int32 ArmorSlot=INDEX_NONE,JewelrySlot=INDEX_NONE;
        uint16 Classes=NativeWadRules::AllClasses;
    };
    // Pure validation: ownership and metadata are supplied; no player/world state is mutated.
    AAORIGINAL_API bool ValidateOptions(const FURL& URL,TFunctionRef<FItemRule(int32)> Resolve,FWadBagLayout* Out=nullptr);
    // Shared runtime metadata bridge used by both the player and checkpoint preflight.
    AAORIGINAL_API FItemRule CatalogRule(int32 Type);
}
