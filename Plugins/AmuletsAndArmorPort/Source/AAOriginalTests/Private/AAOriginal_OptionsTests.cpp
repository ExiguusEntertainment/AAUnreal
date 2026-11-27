#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Components/AudioComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadOptions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalClassicAudioOptions, "AAOriginal.WAD.Interface.ClassicAudioOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalClassicAudioOptions::RunTest(const FString& Parameters)
{
    using namespace NativeWadOptions;
    const FAudioPreferences Original = GetAudioPreferences();
    ON_SCOPE_EXIT { SetAudioPreferences(Original); };
    UAudioComponent* Music = NewObject<UAudioComponent>();
    UAudioComponent* NearbyEffect = NewObject<UAudioComponent>();
    UAudioComponent* DistantEffect = NewObject<UAudioComponent>();
    UAudioComponent* LaterEffect = NewObject<UAudioComponent>();
    // Inactive components test mixer inputs without opening an audio device or speakers.
    TrackAudio(Music, EAudioChannel::Music, 0.45f);
    TrackAudio(NearbyEffect, EAudioChannel::Effects, 0.8f);
    TrackAudio(DistantEffect, EAudioChannel::Effects, 0.2f);
    FAudioPreferences Value;
    Value.MusicVolume = 0.25f;
    Value.EffectsVolume = 0.5f;
    SetAudioPreferences(Value);
    TestTrue(TEXT("Music applies the user gain to its existing nominal level"), FMath::IsNearlyEqual(Music->VolumeMultiplier, 0.1125f));
    TestTrue(TEXT("Effects apply gain to an authored near sound"), FMath::IsNearlyEqual(NearbyEffect->VolumeMultiplier, 0.4f));
    TestTrue(TEXT("Effects retain authored positional volume ratios"), FMath::IsNearlyEqual(DistantEffect->VolumeMultiplier, 0.1f));
    Value.bEffects = false;
    SetAudioPreferences(Value);
    TestEqual(TEXT("Turning effects off updates a live tracked component"), NearbyEffect->VolumeMultiplier, 0.0f);
    TestTrue(TEXT("Turning effects off leaves music at its own gain"), FMath::IsNearlyEqual(Music->VolumeMultiplier, 0.1125f));
    TrackAudio(LaterEffect, EAudioChannel::Effects, 0.6f);
    TestEqual(TEXT("New sounds also respect the disabled effects channel"), LaterEffect->VolumeMultiplier, 0.0f);
    Value.bEffects = true;
    SetAudioPreferences(Value);
    TestTrue(TEXT("Re-enabling effects restores a prior sound from its nominal volume"), FMath::IsNearlyEqual(NearbyEffect->VolumeMultiplier, 0.4f));
    TestTrue(TEXT("Re-enabling effects restores a newer sound from its own nominal volume"), FMath::IsNearlyEqual(LaterEffect->VolumeMultiplier, 0.3f));
    TrackAudio(DistantEffect, EAudioChannel::Effects, 0.1f);
    TestTrue(TEXT("Ambient updates replace nominal falloff without multiplying the gain twice"), FMath::IsNearlyEqual(DistantEffect->VolumeMultiplier, 0.05f));
    Value.bMusic = false;
    SetAudioPreferences(Value);
    TestEqual(TEXT("Turning music off updates the existing track"), Music->VolumeMultiplier, 0.0f);
    TestTrue(TEXT("Music toggle does not change effects"), FMath::IsNearlyEqual(NearbyEffect->VolumeMultiplier, 0.4f));
    bool bSavedMusic = true, bSavedEffects = false;
    float SavedMusicVolume = -1.0f, SavedEffectsVolume = -1.0f;
    const FString SettingsPath = AudioSettingsPath();
    TestTrue(TEXT("Options writes a real user configuration file to disk"), IFileManager::Get().FileExists(*SettingsPath));
    if (IFileManager::Get().FileExists(*SettingsPath))
    {
        // Read through a new FConfigFile, not the process's GConfig cache that masked the old write failure.
        FConfigFile Disk;
        Disk.Read(SettingsPath);
        Disk.GetBool(TEXT("AmuletsAndArmor.Audio"), TEXT("MusicEnabled"), bSavedMusic);
        Disk.GetBool(TEXT("AmuletsAndArmor.Audio"), TEXT("EffectsEnabled"), bSavedEffects);
        Disk.GetFloat(TEXT("AmuletsAndArmor.Audio"), TEXT("MusicVolume"), SavedMusicVolume);
        Disk.GetFloat(TEXT("AmuletsAndArmor.Audio"), TEXT("EffectsVolume"), SavedEffectsVolume);
        TestFalse(TEXT("The music toggle is saved independently of a character checkpoint"), bSavedMusic);
        TestTrue(TEXT("The effects toggle is saved independently"), bSavedEffects);
        TestTrue(TEXT("The music slider is saved"), FMath::IsNearlyEqual(SavedMusicVolume, 0.25f));
        TestTrue(TEXT("The effects slider is saved"), FMath::IsNearlyEqual(SavedEffectsVolume, 0.5f));
    }
    Value.MusicVolume = 2.0f;
    Value.EffectsVolume = -1.0f;
    SetAudioPreferences(Value);
    TestEqual(TEXT("Music volume clamps to the source slider maximum"), GetAudioPreferences().MusicVolume, 1.0f);
    TestEqual(TEXT("Effects volume clamps to the source slider minimum"), GetAudioPreferences().EffectsVolume, 0.0f);

    FWadTownFont Font;
    Font.Widths.Init(4, 256);
    Font.Height = 6;
    TestEqual(TEXT("Classic scrolling counts color markers as zero-width and keeps explicit lines"),
        TextLineCount(TEXT("^009ab cd\nx\tz"), Font, 20), 2);
    TestEqual(TEXT("Classic scrolling includes the wrapped word's row"), TextLineCount(TEXT("ab cde"), Font, 20), 2);
    return true;
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAudioOptionsWriteRetry, "AAOriginal.WAD.Interface.AudioOptionsWriteRetry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalAudioOptionsWriteRetry::RunTest(const FString& Parameters)
{
    using namespace NativeWadOptions;
    const FAudioPreferences Original = GetAudioPreferences();
    ON_SCOPE_EXIT { SetAudioPreferences(Original); };
    FAudioPreferences Value = Original;
    Value.MusicVolume = 0.2f;
    SetAudioPreferences(Value);
    const FString Path = AudioSettingsPath();
    // A normal read handle without write/delete sharing models another process briefly using the
    // existing settings file. No permissions or attributes are changed, and the old file stays intact.
    TUniquePtr<IFileHandle> Reading(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path, false));
    if (!TestNotNull(TEXT("Hold the existing configuration open during a transient conflict"), Reading.Get())) { return false; }
    Value.MusicVolume = 0.8f;
    SetAudioPreferences(Value);
    TestTrue(TEXT("The pending preference still applies live when persistence fails"),
        FMath::IsNearlyEqual(GetAudioPreferences().MusicVolume, 0.8f));
    FConfigFile BeforeRetry;
    BeforeRetry.Read(Path);
    float Saved = -1.0f;
    BeforeRetry.GetFloat(TEXT("AmuletsAndArmor.Audio"), TEXT("MusicVolume"), Saved);
    TestTrue(TEXT("A transient sharing conflict leaves the previous disk value intact"), FMath::IsNearlyEqual(Saved, 0.2f));
    Reading.Reset();
    // The cached value is identical and the old file exists: neither condition can suppress retry.
    SetAudioPreferences(Value);
    FConfigFile AfterRetry;
    AfterRetry.Read(Path);
    Saved = -1.0f;
    AfterRetry.GetFloat(TEXT("AmuletsAndArmor.Audio"), TEXT("MusicVolume"), Saved);
    TestTrue(TEXT("An identical request retries and saves after the conflict ends"), FMath::IsNearlyEqual(Saved, 0.8f));
    return true;
}
#endif
