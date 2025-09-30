#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "AAOriginalAPI.h"
#include "DoomWadLoader.h"
#include "LegacyMapLoader.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalModuleLoads, "AAOriginal.Smoke.ModuleLoads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalModuleLoads::RunTest(const FString& Parameters)
{
    IModuleInterface* Module = FModuleManager::Get().LoadModule("AAOriginal");
    TestNotNull(TEXT("AAOriginal module should load"), Module);
    return true;
}

// CRC32 known vectors
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_CRC32_KnownVector, "AAOriginal.Unit.CRC32.KnownVector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_CRC32_KnownVector::RunTest(const FString& Parameters)
{
    const ANSICHAR* TestData = "123456789"; // length 9
    const int32 Length = 9;
    const uint32 Expected = 0xCBF43926u; // standard CRC32 of "123456789"

    const uint32 Actual = AA_ComputeCRC32(reinterpret_cast<const uint8*>(TestData), Length);
    TestEqual(TEXT("CRC32(123456789)"), Actual, Expected);
    return true;
}

// Additional TDD tests
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_CRC32_Empty, "AAOriginal.Unit.CRC32.Empty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_CRC32_Empty::RunTest(const FString& Parameters)
{
    const uint32 Actual1 = AA_ComputeCRC32(nullptr, 0);
    const uint32 Actual2 = AA_ComputeCRC32(reinterpret_cast<const uint8*>(""), 0);
    TestEqual(TEXT("CRC32(nullptr,0) == 0"), Actual1, 0u);
    TestEqual(TEXT("CRC32(\"\",0) == 0"), Actual2, 0u);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_CRC32_QuickBrownFox, "AAOriginal.Unit.CRC32.QuickBrownFox",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_CRC32_QuickBrownFox::RunTest(const FString& Parameters)
{
    const char* Str = "The quick brown fox jumps over the lazy dog";
    const uint32 Expected = 0x414FA339u;
    const uint32 Actual = AA_ComputeCRC32(reinterpret_cast<const uint8*>(Str), static_cast<int32>(FCStringAnsi::Strlen(Str)));
    TestEqual(TEXT("CRC32(quick brown fox)"), Actual, Expected);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_Version_Sanity, "AAOriginal.Unit.Version.Sanity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_Version_Sanity::RunTest(const FString& Parameters)
{
    const TCHAR* Ver = AA_GetVersion();
    TestNotNull(TEXT("Version pointer not null"), Ver);
    const FString VerStr(Ver);
    TestTrue(TEXT("Version not empty"), !VerStr.IsEmpty());
    TestTrue(TEXT("Version looks like x.y.z"), VerStr.Contains(TEXT(".")));
    return true;
}

// WAD (MAP) loader test: expects a test .map placed under either
//  1) Project/Content/TestData/*.map
//  2) Plugin Content (AmuletsAndArmorPort)/TestData/*.map
// The custom loader currently treats .map as a WAD-compatible binary for this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_Basic, "AAOriginal.WAD.LoadBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static void GatherCandidateRoots(TArray<FString>& OutRoots)
{
    OutRoots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("TestData")));
	IPluginManager& PManager = IPluginManager::Get();
    if (const TSharedPtr<IPlugin> Plugin = PManager.FindPlugin(TEXT("AmuletsAndArmorPort")))
    {
        OutRoots.Add(FPaths::ConvertRelativePathToFull(Plugin->GetContentDir() / TEXT("TestData")));
    }
}

static FString FindTestMapFile(FString& OutSearched)
{
    TArray<FString> Roots; 
    TArray<FString> Found;

    GatherCandidateRoots(Roots);

    for (const FString& Root : Roots)
    {
        if (!FPaths::DirectoryExists(Root)) continue;
        TArray<FString> Local;
        IFileManager::Get().FindFilesRecursive(Local, *Root, TEXT("*.map"), true, false);
        IFileManager::Get().FindFilesRecursive(Local, *Root, TEXT("*.MAP"), true, false);
        if (Local.Num())
        {
            OutSearched = FString::Join(Roots, TEXT(";"));
            return Local[0];
        }
    }
    OutSearched = FString::Join(Roots, TEXT(";"));
    return FString();
}

bool FAAOriginal_WadLoader_Basic::RunTest(const FString& Parameters)
{
    FString RootsStr;
    const FString MapFile = FindTestMapFile(RootsStr);
    if (MapFile.IsEmpty())
    {
        AddWarning(FString::Printf(TEXT("No .map test file found. Searched roots: %s"), *RootsStr));
        return true; // skip without failing
    }

    // Allow external parameter to specify map marker, else default
    FString MapMarker = Parameters.IsEmpty() ? TEXT("L30") : Parameters;

    // Use loader (treat .map as WAD). If loader requires .wad extension adapt here.
    const bool bLoaded = UDoomWadLoader::LoadDoomWadMap(MapFile, MapMarker);
    TestTrue(FString::Printf(TEXT("Map load for marker '%s' from '%s'"), *MapMarker, *MapFile), bLoaded);

    // Optionally inspect stats if available
    if (bLoaded)
    {
        const FDoomWadLoadStats Stats = UDoomWadLoader::GetLastLoadStats();
        TestTrue(TEXT("Has vertices"), Stats.NumVertices > 0);
        TestTrue(TEXT("Has lines"), Stats.NumLines > 0);
        TestTrue(TEXT("Has sectors"), Stats.NumSectors > 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_LegacyMap_Load, "AAOriginal.LegacyMap.LoadBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static FString FindAnyModernLegacyMap(FString& OutTried)
{
    TArray<FString> Roots;
    Roots.Add(FPaths::ProjectDir());
    Roots.Add(FPaths::ProjectContentDir() / TEXT("TestData"));
    TArray<FString> Found;
    for (const FString& Root : Roots)
    {
        if (!FPaths::DirectoryExists(Root)) continue;
        TArray<FString> Local;
        IFileManager::Get().FindFilesRecursive(Local, *Root, TEXT("l*.map"), true, false);
        if (Local.Num()) { OutTried = FString::Join(Roots, TEXT(";")); return Local[0]; }
    }
    OutTried = FString::Join(Roots, TEXT(";"));
    return FString();
}

bool FAAOriginal_LegacyMap_Load::RunTest(const FString& Parameters)
{
    FString Tried; const FString Path = FindAnyModernLegacyMap(Tried);
    if (Path.IsEmpty())
    {
        AddWarning(FString::Printf(TEXT("No test l*.map file found (searched: %s). Place a modern AAM1 format file to exercise loader."), *Tried));
        return true;
    }
    const bool bLoaded = ULegacyMapLoader::LoadLegacyMapFromPath(Path);
    TestTrue(TEXT("Legacy map loaded (modern placeholder format)"), bLoaded);
    if (bLoaded)
    {
        const FLegacyMapLoadStats Stats = ULegacyMapLoader::GetLastLegacyStats();
        TestTrue(TEXT("Lines > 0"), Stats.NumLines > 0);
        TestTrue(TEXT("Sectors > 0"), Stats.NumSectors > 0);
    }
    return true;
}
