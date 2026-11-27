#pragma once

#include "CoreMinimal.h"

struct FWadCreatureDescription;

namespace NativeWadLook
{
    // LOOK.C presents only the first two newline-delimited CREDESC fields.
    // Import this data into ClassicHud; packaged play must not depend on External source files.
    AAORIGINAL_API bool ParseDescription(const FString& Source, FWadCreatureDescription& Out);
    AAORIGINAL_API FBox2D PortraitBox(FVector2D NaturalSize);
}
