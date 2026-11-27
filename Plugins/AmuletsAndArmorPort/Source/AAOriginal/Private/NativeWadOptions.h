#pragma once

#include "CoreMinimal.h"

class UAudioComponent;
struct FWadTownFont;

namespace NativeWadOptions
{
    enum class EAudioChannel : uint8 { Music, Effects };
    struct FAudioPreferences
    {
        bool bMusic = true;
        bool bEffects = true;
        float MusicVolume = 1.0f;
        float EffectsVolume = 1.0f;
    };

    // These are user preferences, separate from any character or campaign checkpoint.
    AAORIGINAL_API const FAudioPreferences& GetAudioPreferences();
    AAORIGINAL_API void SetAudioPreferences(FAudioPreferences Preferences);
    AAORIGINAL_API float EffectiveVolume(EAudioChannel Channel, float AuthoredVolume);
    AAORIGINAL_API void TrackAudio(UAudioComponent* Audio, EAudioChannel Channel, float AuthoredVolume);

    const FString& Credits();
    const FString& Controls();
    AAORIGINAL_API int32 TextLineCount(const FString& Text, const FWadTownFont& Font, int32 Width);
}
