#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DefaultValueHelper.h"
#include "../../AAOriginal/Private/NativeWadItemSource.h"

namespace
{
    FString Objects() { return FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/OBJS"); }
    bool ReadNumbers(const FString& Filename,TArray<int32>& Values,int32* LabeledFirst=nullptr)
    {
        TArray<FString> Lines;if (!FFileHelper::LoadFileToStringArray(Lines,*Filename)) { return false; }
        bool bNextPicture=false,bFound=false;
        for (const auto& Line:Lines)
        {
            if (Line.Contains(TEXT("List of angle identifiers for STANCE 0: FRAME 0:"),ESearchCase::IgnoreCase)) { bNextPicture=true; }
            const int32 Comment=Line.Find(TEXT(";"));const FString Number=(Comment==INDEX_NONE?Line:Line.Left(Comment)).TrimStartAndEnd();
            if (Number.IsEmpty()) { continue; }int32 Value;if (!FDefaultValueHelper::ParseInt(Number,Value)) { return false; }
            Values.Add(Value);if (bNextPicture && !bFound) { if (LabeledFirst) { *LabeledFirst=Value; }bFound=true; }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalItemInfSource,"AAOriginal.WAD.Import.ItemInitialFrameSource",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalItemInfSource::RunTest(const FString& Parameters)
{
    TArray<FString> Lines;if (!FFileHelper::LoadFileToStringArray(Lines,*(Objects()/TEXT("OBJS.FL")))) { return false; }
    TMap<int32,FString> Resources;
    for (const auto& Line:Lines)
    {
        TArray<FString> Words;Line.ParseIntoArrayWS(Words);int32 Type;
        if (Words.Num()>=2 && FPaths::GetExtension(Words[0]).Equals(TEXT("RES"),ESearchCase::IgnoreCase) && LexTryParseString(Type,*Words[1]))
        { FPaths::NormalizeFilename(Words[0]);Resources.Add(Type,Words[0]); }
    }
    int32 Parsed=0,Multi=0,Labeled=0;
    for (const auto& [Type,Resource]:Resources)
    {
        TArray<int32> Values;int32 Expected=MAX_int32;
        if (!ReadNumbers(FPaths::ChangeExtension(Objects()/Resource,TEXT("INF")),Values,&Expected)) { return false; }
        int32 First=MAX_int32;
        if (!TestTrue(FString::Printf(TEXT("Validate complete real object layout%d %s"),Type,*Resource),ParseWadObjectInitialPicture(Values,First))) { return false; }
        ++Parsed;Multi+=Values[0]>1;
        if (Expected!=MAX_int32) { ++Labeled;TestEqual(TEXT("Actual source-labeled stance0/frame0/angle0 agrees"),First,Expected); }
    }
    TestEqual(TEXT("Every original mapped INF validates, including signed metadata"),Parsed,542);
    TestEqual(TEXT("Real multistance objects exercise the corrected offset"),Multi,70);
    TestEqual(TEXT("Independent source labels cover537 definitions; five original numeric-only records still validate"),Labeled,537);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalItemInfBounds,"AAOriginal.WAD.Import.ItemInitialFrameBounds",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalItemInfBounds::RunTest(const FString& Parameters)
{
    TArray<int32> Shield;if (!ReadNumbers(Objects()/TEXT("SHIELD02/SHIELD02.INF"),Shield)) { return false; }
    int32 Picture=99;TestTrue(TEXT("Actual five-stance shield validates"),ParseWadObjectInitialPicture(Shield,Picture));
    TestEqual(TEXT("Correct shield selects ID0, never stance2 framecount1"),Picture,0);
    for (int32 Count=0;Count<Shield.Num();++Count)
    {
        Picture=99;
        if (ParseWadObjectInitialPicture(MakeArrayView(Shield).Slice(0,Count),Picture) || Picture!=99)
        { AddError(FString::Printf(TEXT("Truncated real shield accepted or changed output at%d"),Count));return false; }
    }
    const auto Reject=[&](int32 At,int32 Value)
    { auto Bad=Shield;Bad[At]=Value;int32 Out=99;return !ParseWadObjectInitialPicture(Bad,Out)&&Out==99; };
    for (int32 Count:{-1,0,MAX_int32}) { TestTrue(TEXT("Invalid stance counts reject without overflow"),Reject(0,Count));TestTrue(TEXT("Invalid frame counts reject without overflow"),Reject(8,Count)); }
    TestTrue(TEXT("Unknown angle count rejects"),Reject(28,2));TestTrue(TEXT("Out-of-range source picture rejects before abs"),Reject(32,MIN_int32));
    TestTrue(TEXT("Picture exceeding signed16 rejects"),Reject(32,32768));TestTrue(TEXT("Invalid stance animation rejects"),Reject(10,3));
    TestTrue(TEXT("Invalid next stance rejects"),Reject(11,5));TestTrue(TEXT("Out-of-word metadata rejects"),Reject(29,65536));
    auto Extra=Shield;Extra.Add(0);TestFalse(TEXT("Trailing numeric garbage rejects"),ParseWadObjectInitialPicture(Extra,Picture));
    auto Reverse=Shield;Reverse[32]=-32768;TestTrue(TEXT("Minimum signed16 picture remains bounded and retains orientation sign"),ParseWadObjectInitialPicture(Reverse,Picture));
    TestEqual(TEXT("Parser preserves source signed picture identity"),Picture,-32768);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalItemInfAuthoredItems,"AAOriginal.WAD.Import.ItemInitialFrameAuthoredItems",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalItemInfAuthoredItems::RunTest(const FString& Parameters)
{
    const TSet<int32> Shields{712,4808,8904,13000,21192,25288,29384,33480,37576,41672,45768};
    TSet<int32> Types=Shields;Types.Add(200);Types.Add(600);
    TMap<int32,FWadAuthoredItem> Items;
    if (!TestTrue(TEXT("Actual source item loader resolves original variants and unrelated controls"),ReadWadAuthoredItems(Types,Items,*GLog))) { return false; }
    TestEqual(TEXT("All exact requested source identities retained"),Items.Num(),Types.Num());
    for (int32 Type:Shields)
    {
        const auto& Item=Items.FindChecked(Type);
        TestEqual(TEXT("All eleven source variants select exact original first picture"),Item.Picture,FString(TEXT("SHIELD02/PLH01.CPC")));
        TestEqual(TEXT("Asset requests encode the corrected picture"),Item.AssetName(),FString(TEXT("SHIELD02_PLH01")));
        TestEqual(TEXT("Source shield radius unchanged"),Item.Radius,20);TestEqual(TEXT("Source weight unchanged"),Item.Weight,350);
        FString Description;
        if (!TestTrue(TEXT("Read this exact variant's original armor descriptor"),FFileHelper::LoadFileToString(Description,
            *(FPaths::GetPath(Objects())/TEXT("INVDESC")/FString::Printf(TEXT("DES%05d.TXT"),Type))))) { return false; }
        TArray<FString> Lines;Description.ParseIntoArrayLines(Lines,false);
        int32 ExpectedArmor=INDEX_NONE;
        for(int32 Line=0;Line+1<Lines.Num();++Line)
        { if(Lines[Line].TrimStartAndEnd()==TEXT("ARMOR VALUE")){ExpectedArmor=FCString::Atoi(*Lines[Line+1]);break;} }
        TestEqual(TEXT("Armor retains this full variant's original descriptor value"),Item.Details.ArmorValue,ExpectedArmor);
        TestEqual(TEXT("Class permissions unchanged"),Item.Details.UsableClasses,NativeWadRules::AllClasses);
        TestEqual(TEXT("Source shield slot unchanged"),Item.Details.ArmorSlot,int32(NativeWadRules::ShieldSlot));
        TestTrue(TEXT("Original passability unchanged"),Item.bPassable);TestFalse(TEXT("No new hanging flag"),Item.bHanging);TestFalse(TEXT("No new raised flag"),Item.bRaised);
    }
    TestEqual(TEXT("Unrelated one-stance dagger art unchanged"),Items.FindChecked(200).Picture,FString(TEXT("DAGGER/DAGGER.CPC")));
    TArray<uint8> Cpc;if (!FFileHelper::LoadFileToArray(Cpc,*(Objects()/Items.FindChecked(712).Picture)) || Cpc.Num()<4) { return false; }
    TestEqual(TEXT("Original padded shield width remains64, with no inferred crop"),int32(Cpc[2])|int32(Cpc[3])<<8,64);
    TestEqual(TEXT("Original padded shield height remains64"),int32(Cpc[0])|int32(Cpc[1])<<8,64);
    // DISOAF's signed value field is deliberately unchanged by this art-only fix.
    TArray<int32> Amulet;if (!ReadNumbers(Objects()/TEXT("AMULET00/AMULET00.INF"),Amulet)) { return false; }
    TestEqual(TEXT("The original signed metadata is preserved, not silently repriced"),Items.FindChecked(600).Details.ValueCopper,Amulet[4]);
    return true;
}
