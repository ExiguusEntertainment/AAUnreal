#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadLook.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCreatureLook, "AAOriginal.WAD.Interface.CreatureLook",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalCreatureLook::RunTest(const FString& Parameters)
{
    FWadCreatureDescription Info;
    TestTrue(TEXT("The source Look parser accepts CRLF descriptions"),
        NativeWadLook::ParseDescription(TEXT("Creature name\r\nFirst description line.\r\nNot shown by LOOK.C\r\n"), Info));
    TestEqual(TEXT("Name is the first field without newline controls"), Info.Name, FString(TEXT("Creature name")));
    TestEqual(TEXT("Only the second source line supplies the description"), Info.Description, FString(TEXT("First description line.")));
    TestFalse(TEXT("A malformed record without field separation is rejected"), NativeWadLook::ParseDescription(TEXT("Missing description"), Info));
    TestEqual(TEXT("A rejected record does not replace a previously valid description"), Info.Name, FString(TEXT("Creature name")));
    TestTrue(TEXT("The parser accepts the final description without a trailing newline"),
        NativeWadLook::ParseDescription(TEXT("Other creature\nDescription"), Info));
    TestEqual(TEXT("The complete final field is retained"), Info.Description, FString(TEXT("Description")));

    FString Original;
    const FString Path = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/CREDESC/DES01018.TXT");
    if (!TestTrue(TEXT("The original Elf Guardian description is available for the source-data regression"),
        FFileHelper::LoadFileToString(Original, *Path))) { return false; }
    if (!TestTrue(TEXT("The original file parses"), NativeWadLook::ParseDescription(Original, Info))) { return false; }
    TestEqual(TEXT("The original creature name is preserved"), Info.Name, FString(TEXT("Elf Guardian")));
    TestEqual(TEXT("The original second line is preserved"), Info.Description, FString(TEXT("Elf Guardian of the Groves.")));
    TestFalse(TEXT("The original third line is not folded into the classic Look field"), Info.Description.Contains(TEXT("Allies")));

    const FBox2D Wide = NativeWadLook::PortraitBox({200, 100});
    TestEqual(TEXT("A wide source portrait fits the original ninety-nine-pixel width"), Wide.GetSize().X, 99.0);
    TestEqual(TEXT("A wide portrait retains its proportions with source integer rounding"), Wide.GetSize().Y, 49.0);
    const FBox2D Tall = NativeWadLook::PortraitBox({40, 120});
    TestEqual(TEXT("A tall source portrait fits the original sixty-nine-pixel height"), Tall.GetSize().Y, 69.0);
    TestEqual(TEXT("A tall portrait retains its proportions"), Tall.GetSize().X, 23.0);
    const FBox2D Small = NativeWadLook::PortraitBox({20, 30});
    TestEqual(TEXT("Small portraits retain natural width instead of being enlarged"), Small.GetSize().X, 20.0);
    TestEqual(TEXT("Small portraits retain natural height"), Small.GetSize().Y, 30.0);
    const FBox2D Frame({215, 16}, {314, 85});
    for (const FBox2D& Box : {Wide, Tall, Small})
    {
        TestTrue(TEXT("Portrait scaling and centering stay inside the original form opening"),
            Box.Min.X >= Frame.Min.X && Box.Min.Y >= Frame.Min.Y && Box.Max.X <= Frame.Max.X && Box.Max.Y <= Frame.Max.Y);
        TestTrue(TEXT("The portrait is centered to the source's integer pixel tolerance"),
            FMath::Abs(Box.GetCenter().X - Frame.GetCenter().X) <= 0.5
            && FMath::Abs(Box.GetCenter().Y - Frame.GetCenter().Y) <= 0.5);
    }
    return true;
}
