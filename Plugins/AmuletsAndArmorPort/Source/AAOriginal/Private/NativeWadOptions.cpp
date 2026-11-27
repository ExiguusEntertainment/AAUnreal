#include "NativeWadOptions.h"

#include "Components/AudioComponent.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "NativeWadHudData.h"

namespace NativeWadOptions
{
    namespace
    {
        constexpr const TCHAR* Section = TEXT("AmuletsAndArmor.Audio");
        FAudioPreferences Preferences;
        FString LoadedIni;
        bool bPreferencesNeedWrite = false;
        struct FTrackedAudio { EAudioChannel Channel; float AuthoredVolume; };
        TMap<TWeakObjectPtr<UAudioComponent>, FTrackedAudio> Playing;

        float Normalized(float Value) { return FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0f, 1.0f) : 1.0f; }

        bool WritePreferences()
        {
            const FString Path = AudioSettingsPath();
            FConfigFile File;
            File.bCanSaveAllSections = true;
            File.SetBool(Section, TEXT("MusicEnabled"), Preferences.bMusic);
            File.SetBool(Section, TEXT("EffectsEnabled"), Preferences.bEffects);
            File.SetFloat(Section, TEXT("MusicVolume"), Preferences.MusicVolume);
            File.SetFloat(Section, TEXT("EffectsVolume"), Preferences.EffectsVolume);
            const bool bSaved = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true) && File.Write(Path, false);
            bPreferencesNeedWrite = !bSaved;
            UE_LOG(LogTemp, Display, TEXT("WAD audio preferences write: %s, success=%d."), *Path, bSaved);
            return bSaved;
        }

        void ApplyLive()
        {
            for (auto It = Playing.CreateIterator(); It; ++It)
            {
                if (UAudioComponent* Audio = It.Key().Get())
                { Audio->SetVolumeMultiplier(EffectiveVolume(It.Value().Channel, It.Value().AuthoredVolume)); }
                else { It.RemoveCurrent(); }
            }
        }
    }

    FString AudioSettingsPath()
    {
        // GeneratedConfigDir honors -UserDir, and remains shared by that user's character slots.
        return FPaths::ConvertRelativePathToFull(FPaths::GeneratedConfigDir() / TEXT("NativeWadOptions.ini"));
    }

    const FAudioPreferences& GetAudioPreferences()
    {
        const FString Path = AudioSettingsPath();
        if (LoadedIni != Path)
        {
            LoadedIni = Path;
            Preferences = FAudioPreferences();
            bPreferencesNeedWrite = false;
            bool bMigrate = false;
            if (IFileManager::Get().FileExists(*Path))
            {
                FConfigFile File;
                File.Read(Path);
                File.GetBool(Section, TEXT("MusicEnabled"), Preferences.bMusic);
                File.GetBool(Section, TEXT("EffectsEnabled"), Preferences.bEffects);
                File.GetFloat(Section, TEXT("MusicVolume"), Preferences.MusicVolume);
                File.GetFloat(Section, TEXT("EffectsVolume"), Preferences.EffectsVolume);
                UE_LOG(LogTemp, Display, TEXT("Read WAD audio preferences from %s."), *Path);
            }
            else if (GConfig && !GGameUserSettingsIni.IsEmpty())
            {
                bMigrate |= GConfig->GetBool(Section, TEXT("MusicEnabled"), Preferences.bMusic, GGameUserSettingsIni);
                bMigrate |= GConfig->GetBool(Section, TEXT("EffectsEnabled"), Preferences.bEffects, GGameUserSettingsIni);
                bMigrate |= GConfig->GetFloat(Section, TEXT("MusicVolume"), Preferences.MusicVolume, GGameUserSettingsIni);
                bMigrate |= GConfig->GetFloat(Section, TEXT("EffectsVolume"), Preferences.EffectsVolume, GGameUserSettingsIni);
            }
            Preferences.MusicVolume = Normalized(Preferences.MusicVolume);
            Preferences.EffectsVolume = Normalized(Preferences.EffectsVolume);
            if (bMigrate) { WritePreferences(); }
        }
        return Preferences;
    }

    void SetAudioPreferences(FAudioPreferences Value)
    {
        GetAudioPreferences();
        Value.MusicVolume = Normalized(Value.MusicVolume);
        Value.EffectsVolume = Normalized(Value.EffectsVolume);
        if (Preferences.bMusic == Value.bMusic && Preferences.bEffects == Value.bEffects
            && Preferences.MusicVolume == Value.MusicVolume && Preferences.EffectsVolume == Value.EffectsVolume
            && !bPreferencesNeedWrite
            && IFileManager::Get().FileExists(*AudioSettingsPath()))
        { return; }
        Preferences = Value;
        WritePreferences();
        ApplyLive();
    }

    float EffectiveVolume(EAudioChannel Channel, float AuthoredVolume)
    {
        const auto& Value = GetAudioPreferences();
        const float Gain = Channel == EAudioChannel::Music
            ? (Value.bMusic ? Value.MusicVolume : 0.0f) : (Value.bEffects ? Value.EffectsVolume : 0.0f);
        return FMath::Max(0.0f, AuthoredVolume) * Gain;
    }

    void TrackAudio(UAudioComponent* Audio, EAudioChannel Channel, float AuthoredVolume)
    {
        if (!IsValid(Audio)) { return; }
        for (auto It = Playing.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
        Playing.Add(Audio, {Channel, FMath::Max(0.0f, AuthoredVolume)});
        Audio->SetVolumeMultiplier(EffectiveVolume(Channel, AuthoredVolume));
    }

    const FString& Credits()
    {
        // The original BANNER.C credits; its MESSAGE.C color markers use the imported game palette.
        static const FString Text(
            TEXT("\n^036    Amulets and Armor\n\n")
            TEXT("^009           EXIGUUS    \n^009       ENTERTAINMENT  \n^010    ----------------- \n")
            TEXT("^013      DAVID WEBSTER   \n^014      LYSLE SHIELDS   \n\n")
            TEXT("^009         PRODUCERS    \n^010    ----------------- \n^013      DAVID WEBSTER   \n^014       ERIC WEBSTER   \n\n")
            TEXT("^009       PROGRAMMING    \n^010    ----------------- \n^013      LYSLE SHIELDS   \n^014      JANUS ANDERSON  \n\n")
            TEXT("^009             ART      \n^010    ----------------- \n^013       JOEL THOMAS    \n^014       JOHN HILEMAN   \n^015        DEAN BEERS    \n\n")
            TEXT("^009     SOUND AND MUSIC  \n^010    ----------------- \n^013      JANUS ANDERSON  \n^014         BILLY FOX    \n\n")
            TEXT("^009        MAP DESIGN    \n^010    ----------------- \n^013      EDWARD SINGER   \n^014      TYCE TOOTHAKER  \n^015      DAVID WEBSTER   \n\n")
            TEXT("^009         STORY AND    \n^009       GAME CONCEPT   \n^010    ----------------- \n^013      DAVID WEBSTER   \n\n")
            TEXT("^009      GAME MECHANICS  \n^010    ----------------- \n^013      LYSLE SHIELDS   \n^014      JANUS ANDERSON  \n\n")
            TEXT("^009      USER INTERFACE  \n^010    ----------------- \n^013      JANUS ANDERSON  \n\n")
            TEXT("^009      ADDITIONAL ART  \n^010    ----------------- \n^013     TIFFANY WEBSTER  \n^013      DAVID WEBSTER   \n^014     TENLEY THURSTON  \n^015      JANUS ANDERSON  \n^016    MICHAEL KIRKBRIDE \n^017       LYSLE SHIELDS  \n\n")
            TEXT("^009      DOCUMENTATION   \n^010    ----------------- \n^013      JANUS ANDERSON  \n\n")
            TEXT("^009        ASSISTANCE    \n^010    ----------------- \n^014         GREG HAAS    \n^015      MIKE TOOTHAKER  \n^016      AARON TRICKEY   \n\n")
            TEXT("^009        PLAYTESTERS   \n^010    ----------------- \n^013       ERIC SHAMLIN   \n^014    CHARLES E. MARTIN \n^015         TERRY LUNA   \n^016       JASON HOLMES   \n^017         BILL IRWIN   \n\n")
            TEXT("^011   A Webster Brothers \n^011         Production   \n\n^011      (C)1996 EXIGUUS \n^011       ENTERTAINMENT  \n^011   ALL RIGHTS RESERVED\n\n")
            TEXT("^009          WEB SITE   \n^010    ----------------- \n^036  amuletsandarmor.com\n\n"));
        return Text;
    }

    const FString& Controls()
    {
        // This form describes the port's actual bound inputs, as the original generated CONTROL.TXT did.
        static const FString Text(
            TEXT("^009AMULETS & ARMOR\n^007CONTROLS\n\n")
            TEXT("^009W A S D\n^007Move\n\n^009Mouse / Tab\n^007Look / toggle mouse look\n\n")
            TEXT("^009Ctrl\n^007Attack or use ready hand\n^009Left mouse\n^007Attack in mouse look; click interface with cursor\n\n")
            TEXT("^009E\n^007Open, activate, or pick up\n\n^009Space\n^007Jump; revive after defeat\n\n")
            TEXT("^009Keypad 1-9\n^007Enter owned spell runes\n^009Right mouse / Enter\n^007Cast entered spell\n^009Backspace\n^007Remove last rune\n\n")
            TEXT("^009Right mouse (cursor)\n^007Examine a creature when no spell runes are entered\n\n")
            TEXT("^009I / Alt+keypad 7\n^007Inventory\n^009G\n^007Drop selected item\n^009Wheel / PgUp / PgDn\n^007Change inventory page or scroll open form\n\n")
            TEXT("^009Alt+keypad 6\n^007Finances\n^009Alt+keypad 2\n^007Player-written Notes\n^009Alt+keypad 3\n^007Authored Journal pages\n\n")
            TEXT("^009Alt+keypad 4\n^007Music and effects options\n^009F1\n^007Open / close Controls\n^009F5 / F9\n^007Save / load checkpoint\n\n")
            TEXT("^009NOTES\n^007Type to write. Arrows move the cursor. Home/End move within a line; Ctrl+Home/End move to the beginning/end. Enter inserts a line. Tab inserts a tab. Backspace/Delete edit. Escape closes Notes.\n"));
        return Text;
    }

    int32 TextLineCount(const FString& Text, const FWadTownFont& Font, int32 Width)
    {
        if (Font.Widths.Num() != 256 || Width <= 0) { return 1; }
        // Match DrawWadBitmapText's wrapping, including zero-width ^nnn color markers.
        int32 Rows = 1, LineWidth = 0, WordWidth = 0;
        const auto FinishWord = [&]
        {
            if (LineWidth && LineWidth + WordWidth > Width) { ++Rows; LineWidth = 0; }
            LineWidth += WordWidth; WordWidth = 0;
        };
        for (int32 Index = 0; Index < Text.Len(); ++Index)
        {
            const TCHAR Ch = Text[Index];
            if (Ch == '^' && Index + 3 < Text.Len() && FChar::IsDigit(Text[Index + 1])
                && FChar::IsDigit(Text[Index + 2]) && FChar::IsDigit(Text[Index + 3]))
            { Index += 3; }
            else if (Ch == '\n') { FinishWord(); ++Rows; LineWidth = 0; }
            else if (Ch == ' ' || Ch == '\t')
            {
                FinishWord();
                for (int32 Space = 0; Space < (Ch == '\t' ? 3 : 1); ++Space)
                { if (LineWidth + Font.Widths[' '] <= Width) { LineWidth += Font.Widths[' ']; } }
            }
            else
            {
                const uint8 Glyph = Ch <= 255 ? uint8(Ch) : uint8('?');
                if (WordWidth + Font.Widths[Glyph] > Width) { FinishWord(); }
                WordWidth += Font.Widths[Glyph];
            }
        }
        FinishWord();
        return Rows;
    }
}
