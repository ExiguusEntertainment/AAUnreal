#include "Modules/ModuleManager.h"
#include "CoreMinimal.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "DoomWadLoader.h"
#include "LegacyMapLoader.h"

class FAAOriginalModule : public IModuleInterface, public FSelfRegisteringExec
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}

    static FString ResolvePathOrProjectRelative(FString InPath)
    {
        if (InPath.IsEmpty()) return InPath;
        if (FPaths::FileExists(InPath) || FPaths::DirectoryExists(InPath))
        {
            return FPaths::ConvertRelativePathToFull(InPath);
        }
        const FString ProjectRel = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / InPath);
        if (FPaths::FileExists(ProjectRel) || FPaths::DirectoryExists(ProjectRel))
        {
            return ProjectRel;
        }
        return InPath; // unchanged; will fail later with a clear message
    }

    static bool TryAutoPickWadFromDirectory(const FString& DirPath, FString& OutWad)
    {
        if (!FPaths::DirectoryExists(DirPath)) return false;
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *DirPath, TEXT("*.wad"), true, false);
        IFileManager::Get().FindFilesRecursive(Files, *DirPath, TEXT("*.WAD"), true, false);
        if (Files.Num() > 0)
        {
            // Prefer common names if present
            int32 BestIdx = 0;
            for (int32 i = 0; i < Files.Num(); ++i)
            {
                const FString Name = FPaths::GetCleanFilename(Files[i]).ToLower();
                if (Name == TEXT("doom.wad") || Name == TEXT("doom2.wad") || Name == TEXT("amulets.wad"))
                {
                    BestIdx = i; break;
                }
            }
            OutWad = Files[BestIdx];
            return true;
        }
        return false;
    }

    static FString SanitizeMapMarker(const FString& In)
    {
        FString M = In;
        // Strip extension if user typed e.g. L30.MAP
        if (M.EndsWith(TEXT(".map"), ESearchCase::IgnoreCase))
        {
            M = FPaths::GetBaseFilename(M);
        }
        return M;
    }

    // New: allow .map files that are actually WADs
    static bool LooksLikeWadFile(const FString& Path)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 4)
        {
            return false;
        }
        const char* Sig = reinterpret_cast<const char*>(Bytes.GetData());
        return (FMemory::Memcmp(Sig, "IWAD", 4) == 0) || (FMemory::Memcmp(Sig, "PWAD", 4) == 0);
    }

    virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override
    {
        if (FParse::Command(&Cmd, TEXT("AA.LOADWAD")))
        {
            FString WadPath = FParse::Token(Cmd, false);
            FString MapName = FParse::Token(Cmd, false);
            if (WadPath.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.LoadWad <WadPathOrDir> <MapName>"));
                Ar.Log(TEXT("  Hint: If a directory is provided, the first .wad found will be used."));
                return true;
            }

            WadPath = ResolvePathOrProjectRelative(WadPath);

            // Allow passing a directory containing a WAD
            if (FPaths::DirectoryExists(WadPath))
            {
                FString Picked;
                if (!TryAutoPickWadFromDirectory(WadPath, Picked))
                {
                    Ar.Log(*FString::Printf(TEXT("No .wad files found under directory: %s"), *WadPath));
                    return true;
                }
                WadPath = Picked;
            }

            // Accept .map files if they are actual IWAD/PWAD containers
            if (FPaths::GetExtension(WadPath, true).Equals(TEXT(".map"), ESearchCase::IgnoreCase))
            {
                if (!LooksLikeWadFile(WadPath))
                {
                    Ar.Log(TEXT("AA.LoadWad expects a WAD (IWAD/PWAD). The provided .map is not a WAD container."));
                    Ar.Log(TEXT("Use: AA.LoadLegacyMap <PathToAAMap> for native A&A .MAP parsing."));
                    return true;
                }
                else
                {
                    Ar.Log(TEXT("Treating .map as WAD (IWAD/PWAD signature detected)."));
                }
            }

            if (!FPaths::FileExists(WadPath))
            {
                Ar.Log(*FString::Printf(TEXT("WAD file not found: %s"), *WadPath));
                return true; // handled
            }

            if (MapName.IsEmpty())
            {
                Ar.Log(TEXT("Missing <MapName>. Example: AA.LoadWad Content/TestData/doom2.wad MAP01"));
                return true;
            }

            MapName = SanitizeMapMarker(MapName);

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
        else if (FParse::Command(&Cmd, TEXT("AA.LOADLEGACYMAP")))
        {
            FString MapPath = FParse::Token(Cmd, false);
            if (MapPath.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.LoadLegacyMap <PathToAAMap>"));
                return true;
            }
            MapPath = ResolvePathOrProjectRelative(MapPath);
            if (!FPaths::FileExists(MapPath))
            {
                Ar.Log(*FString::Printf(TEXT("Legacy map file not found: %s"), *MapPath));
                return true;
            }
            if (ULegacyMapLoader::LoadLegacyMapFromPath(MapPath))
            {
                const auto Stats = ULegacyMapLoader::GetLastLegacyStats();
                Ar.Log(*FString::Printf(TEXT("AA.LoadLegacyMap succeeded: V=%d L=%d Sides=%d Sec=%d (%s)"), Stats.NumVertices, Stats.NumLines, Stats.NumSides, Stats.NumSectors, *MapPath));
            }
            else
            {
                Ar.Log(*FString::Printf(TEXT("AA.LoadLegacyMap failed: %s"), *MapPath));
            }
            return true;
        }
        return false; // not our command
    }
};

IMPLEMENT_MODULE(FAAOriginalModule, AAOriginal)
