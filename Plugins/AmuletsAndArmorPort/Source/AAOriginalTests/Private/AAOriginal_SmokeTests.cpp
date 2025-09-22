#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "AAOriginalAPI.h"

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
