#pragma once

#include "CoreMinimal.h"

namespace NativeWadIdentification
{
    inline constexpr int32 EquippedScroll = 350;
    inline constexpr int32 AllScroll = 351;
    inline constexpr int32 CommonStarterCopper = 400; // INVENTOR.C gives four gold before the class-specific equipment.
    inline bool ValidType(int32 Type) { return Type > 0 && Type <= MAX_uint16; }
}
