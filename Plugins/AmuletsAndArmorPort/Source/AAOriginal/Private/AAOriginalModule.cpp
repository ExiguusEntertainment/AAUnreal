#include "Modules/ModuleManager.h"
#include "CoreMinimal.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "DoomWadLoader.h"

class FAAOriginalModule : public IModuleInterface, public FSelfRegisteringExec
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}

    virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override
    {
        // Copy so we can mutate pointer for FParse helpers.
        const TCHAR* Original = Cmd;
        if (FParse::Command(&Cmd, TEXT("AA.LOADWAD")))
        {
            FString WadPath = FParse::Token(Cmd, false);
            FString MapName = FParse::Token(Cmd, false);
            if (WadPath.IsEmpty() || MapName.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.LoadWad <WadPath> <MapName>"));
                return true;
            }

            // Allow relative path to project directory.
            if (!FPaths::FileExists(WadPath))
            {
                FString Alt = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / WadPath);
                if (FPaths::FileExists(Alt))
                {
                    WadPath = Alt;
                }
            }

            if (!FPaths::FileExists(WadPath))
            {
                Ar.Log(*FString::Printf(TEXT("WAD file not found: %s"), *WadPath));
                return true; // handled
            }

            if (UDoomWadLoader::LoadDoomWadMap(WadPath, MapName))
            {
                Ar.Log(*FString::Printf(TEXT("AA.LoadWad succeeded: %s (%s)"), *MapName, *WadPath));
            }
            else
            {
                Ar.Log(*FString::Printf(TEXT("AA.LoadWad failed: %s (%s)"), *MapName, *WadPath));
            }
            return true;
        }
        return false; // not our command
    }
};

IMPLEMENT_MODULE(FAAOriginalModule, AAOriginal)
