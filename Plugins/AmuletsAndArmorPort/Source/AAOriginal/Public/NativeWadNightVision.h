#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;
class UMaterialInterface;

namespace NativeWadNightVision
{
    inline constexpr int32 SkyStencil = 254;
    inline constexpr const TCHAR* MaterialPath = TEXT("/Game/WadInterface/Effects/M_WadNightVision.M_WadNightVision");
    inline constexpr const TCHAR* SwatchMaterialPath = TEXT("/Game/WadInterface/Effects/M_WadNightVisionSwatch.M_WadNightVisionSwatch");
    inline constexpr const TCHAR* PowerParameter = TEXT("NightVisionPower");
    AAORIGINAL_API void MarkSky(UPrimitiveComponent& Component);
    // Startup/first-use priming requests background editor compilation and an
    // asynchronous actual-render-resource proof. Cooked builds only validate.
    AAORIGINAL_API void PrimeRendering(UMaterialInterface& Parent);
    AAORIGINAL_API bool IsRenderingReady(UMaterialInterface& Parent);

    // Native policy: strongest surviving owner, never summed. The source directly
    // assigned the last-started power and reset it when ANY owner stopped.
    inline int32 BoundPower(int32 Power) { return FMath::Clamp(Power, 0, 127); }
    inline bool IsVisibleSky(int32 Stencil, float CustomDepth, float SceneDepth)
    {
        // UE bIsSky uses SkyPass without scene-depth writes. Clear/far scene depth
        // is sky too; only opaque geometry in FRONT of sky depth cancels the mask.
        return Stencil == SkyStencil && CustomDepth > 0.0f && SceneDepth > 0.0f
            && SceneDepth >= CustomDepth - FMath::Max(1.0f, CustomDepth * 0.0001f);
    }
    // SDR approximation after tonemapping: neutral luminance-weighted shadow gain,
    // not the original indexed-palette shade table. Black texels remain black.
    inline FLinearColor Lift(FLinearColor Color, int32 Power, bool bVisibleSky)
    {
        if (bVisibleSky || Power <= 0) { return Color; }
        const float Luminance = FMath::Clamp(0.2126f * Color.R + 0.7152f * Color.G + 0.0722f * Color.B, 0.0f, 1.0f);
        const float Gain = 1.0f + 2.0f * BoundPower(Power) / 127.0f * FMath::Square(1.0f - Luminance);
        return FLinearColor(FMath::Clamp(Color.R * Gain, 0.0f, 1.0f), FMath::Clamp(Color.G * Gain, 0.0f, 1.0f),
            FMath::Clamp(Color.B * Gain, 0.0f, 1.0f), Color.A);
    }
}
