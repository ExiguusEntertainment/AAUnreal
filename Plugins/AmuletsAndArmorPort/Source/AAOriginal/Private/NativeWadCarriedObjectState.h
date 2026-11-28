#pragma once
#include "CoreMinimal.h"
struct FURL;
namespace NativeWadCarriedObjectState
{
    // Shared save/load preflight. No world/player mutation occurs here.
    AAORIGINAL_API bool ValidateOptions(const FURL& URL);
}
