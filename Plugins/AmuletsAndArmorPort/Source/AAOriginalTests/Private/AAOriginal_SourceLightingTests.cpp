#include "Misc/AutomationTest.h"
#include <limits>
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "NativeWadLightRules.h"
#include "NativeWadLevel.h"
#include "NativeWadArtEditor.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadLightSource.h"

namespace SourceLightingMetadataTests
{
    // Independent packed-lump reader: never asks the importer for expected source counts.
    bool Original(const FString& Name,FDoomWadMapData& M,TArray<uint8>& Bytes)
    {
        if(!FFileHelper::LoadFileToArray(Bytes,*(FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe")/(Name+TEXT(".MAP"))))){return false;}
        if(Bytes.Num()<12||FMemory::Memcmp(Bytes.GetData(),"PWAD",4)){return false;}
        const auto U32=[&](int32 At){return uint32(Bytes[At])|uint32(Bytes[At+1])<<8|uint32(Bytes[At+2])<<16|uint32(Bytes[At+3])<<24;};
        const uint32 Count=U32(4),Directory=U32(8);
        if(uint64(Directory)+uint64(Count)*16>uint64(Bytes.Num())){return false;}
        TMap<FString,TConstArrayView<uint8>> Lumps;
        for(uint32 I=0;I<Count;++I)
        {
            const int32 At=int32(Directory+I*16);const uint32 Offset=U32(At),Size=U32(At+4);
            if(uint64(Offset)+Size>uint64(Bytes.Num())){return false;}
            ANSICHAR Label[9]={};FMemory::Memcpy(Label,Bytes.GetData()+At+8,8);const FString Key=ANSI_TO_TCHAR(Label);
            if(Lumps.Contains(Key)){return false;}Lumps.Add(Key,MakeArrayView(Bytes).Slice(int32(Offset),int32(Size)));
        }
        const auto Read=[&](const TCHAR* Key,auto& Records)
        {
            const auto* Data=Lumps.Find(Key);const int32 Stride=sizeof(Records[0]);
            if(!Data||Data->Num()%Stride){return false;}Records.SetNumUninitialized(Data->Num()/Stride);
            if(Data->Num()){FMemory::Memcpy(Records.GetData(),Data->GetData(),Data->Num());}return true;
        };
        return Read(TEXT("VERTEXES"),M.Vertices)&&Read(TEXT("LINEDEFS"),M.Lines)&&Read(TEXT("SIDEDEFS"),M.Sides)
            &&Read(TEXT("SECTORS"),M.Sectors)&&Read(TEXT("THINGS"),M.Things);
    }
    FWadSourceLightingMetadata Kernel(int32 Count)
    {
        FWadSourceLightingMetadata M;M.Version=NativeWadLightRules::MetadataVersion;M.ThingCount=20;
        M.SourceMapName=TEXT("SyntheticKernel");M.SourceMapSha1=TEXT("0000000000000000000000000000000000000000");
        for(int32 I=0;I<Count;++I){auto& S=M.Sectors.AddDefaulted_GetRef();S.Sector=I;S.AuthoredLight=20;}
        return M;
    }
    void Owner(FWadSourceLightingMetadata& M,int32 Id,int32 Sector,int32 Type,int32 Power)
    {
        auto& O=M.Illuminators.AddDefaulted_GetRef();O.ThingId=Id;O.SourceType=Type;O.CenterSector=O.AuthoredSector=Sector;
        O.AccessoryData=Type==924?uint16(Power):0;O.Illumination=Power;
    }
    bool Run(FWadSourceLightingMetadata M,TArray<int32> Input,TArray<int32>& Output,TArray<int32> Doors={},int32 Outside=200)
    {
        if(Doors.IsEmpty()){Doors.Init(256,M.Sectors.Num());}FString Error;
        return NativeWadLightRules::Recalculate(M,Input,Doors,Outside,Output,Error);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceLightCensus,"AAOriginal.WAD.Lighting.SourceMetadataAll30",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceLightCensus::RunTest(const FString&)
{
    using namespace SourceLightingMetadataTests;
    struct FExpected{const TCHAR* Name;int32 Markers,Props,Glows,Animations,Directives;};
    const FExpected Expected[]={
        {TEXT("L30"),6,6,2,2,18},
        {TEXT("L301"),0,0,0,0,9},
        {TEXT("L531"),0,0,0,0,1},
        {TEXT("L31"),0,5,3,3,16},
        {TEXT("L22"),0,10,1,1,2},
        {TEXT("L25"),4,20,0,0,1},
        {TEXT("L20"),3,4,1,1,3},
        {TEXT("L60"),1,29,2,2,71},
        {TEXT("L64"),35,27,15,16,77},
        {TEXT("L65"),17,37,6,7,42},
        {TEXT("L34"),7,32,5,5,5},
        {TEXT("L151"),2,0,1,0,24},
        {TEXT("L152"),1,1,2,2,8},
        {TEXT("L153"),13,9,2,7,27},
        {TEXT("L231"),3,26,0,11,14},
        {TEXT("L75"),31,21,6,7,7},
        {TEXT("L10"),1,39,0,1,3},
        {TEXT("L12"),14,11,3,5,26},
        {TEXT("L51"),3,11,0,0,17},
        {TEXT("L52"),0,10,1,1,21},
        {TEXT("L41"),21,5,6,15,74},
        {TEXT("L44"),3,6,6,8,8},
        {TEXT("L74"),1,20,0,0,2},
        {TEXT("L73"),0,28,6,6,12},
        {TEXT("L72"),0,10,0,5,14},
        {TEXT("L32"),1,27,3,3,7},
        {TEXT("L71"),1,37,0,6,14},
        {TEXT("L70"),2,11,2,11,11},
        {TEXT("L61"),4,13,2,3,14},
        {TEXT("L40"),26,0,3,23,39}
    };
    int32 Owners=0,Markers=0,Props=0,Glows=0,Animations=0,Negative=0,Zero=0,Self=0,Excluded=0;
    TMap<int32,int32> Rates;
    for(const auto& E:Expected)
    {
        FDoomWadMapData Source;TArray<uint8> Bytes;FWadSourceLightingMetadata M;FString Error;
        if(!TestTrue(FString::Printf(TEXT("%s independent original records"),E.Name),Original(E.Name,Source,Bytes))){return false;}
        if(!TestTrue(FString::Printf(TEXT("%s metadata: %s"),E.Name,*Error),BuildWadSourceLighting(Source,Bytes,E.Name,{},M,Error))){return false;}
        TestEqual(FString(E.Name)+TEXT(" marker + prop owners"),M.Illuminators.Num(),E.Markers+E.Props);
        TestEqual(FString(E.Name)+TEXT(" effective animation count"),M.Animations.Num(),E.Animations);
        TestEqual(FString(E.Name)+TEXT(" ordered terrain/glow directives"),M.Directives.Num(),E.Directives);
        TArray<int32> Input,Doors,Output,Phase,NewPhases;
        for(const auto& Sector:M.Sectors){Input.Add(Sector.AuthoredLight);}
        Doors.Init(256,M.Sectors.Num());Phase.Init(0,M.Sectors.Num());
        if(!TestTrue(FString(E.Name)+TEXT(" all-map source arithmetic input"),NativeWadLightRules::Recalculate(M,Input,Doors,170,Output,Error))){return false;}
        if(!TestTrue(FString(E.Name)+TEXT(" all-map animation input"),NativeWadLightRules::Animate(M,70,Output,Phase,Output,NewPhases,Error))){return false;}
        TestFalse(TEXT("source kernel output remains bytes"),Output.ContainsByPredicate([](int32 L){return L<0||L>255;}));

        TestEqual(TEXT("authored sector count retained"),M.Sectors.Num(),Source.Sectors.Num());
        TArray<int32> IndependentInvalid;
        for(int32 Id=0;Id<Source.Lines.Num();++Id)
        {
            const auto& L=Source.Lines[Id];
            if((L.Flags&4)&&(!Source.Sides.IsValidIndex(uint16(L.Side[0]))||!Source.Sides.IsValidIndex(uint16(L.Side[1]))
                ||!Source.Sectors.IsValidIndex(Source.Sides[uint16(L.Side[0])].Sector)||!Source.Sectors.IsValidIndex(Source.Sides[uint16(L.Side[1])].Sector)))
            {IndependentInvalid.Add(Id);}
        }
        TestTrue(TEXT("every unaddressable source line ID explicitly recorded"),M.UnaddressableTwoSidedLines==IndependentInvalid);
        const int32 ExpectedInvalid=FString(E.Name)==TEXT("L64")?133:FString(E.Name)==TEXT("L52")?9:FString(E.Name)==TEXT("L74")?33:0;
        TestEqual(TEXT("bounded source invalid-edge census"),IndependentInvalid.Num(),ExpectedInvalid);Excluded+=IndependentInvalid.Num();

        FSHAHash Hash;FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Hash.Hash);TestEqual(TEXT("original file digest"),M.SourceMapSha1,Hash.ToString());
        int32 MapMarkers=0,MapGlows=0;
        for(const auto& O:M.Illuminators)
        {
            ++Owners;MapMarkers+=O.SourceType==924;Markers+=O.SourceType==924;Props+=O.SourceType!=924;
            Negative+=O.SourceType==924&&O.Illumination<0;Zero+=O.SourceType==924&&O.Illumination==0;
            TestEqual(TEXT("full source type preserved"),O.SourceType,int32(uint16(Source.Things[O.ThingId].Type)));
            TestEqual(TEXT("original owner center"),O.CenterSector,O.AuthoredSector);
            TestFalse(TEXT("authored centers not falsely edited"),O.bUsesEditedCenter);
            TestFalse(TEXT("all original lighting centers resolve without fallback"),O.bUsedSourceFallback);
        }
        TestEqual(FString(E.Name)+TEXT(" exact924 count"),MapMarkers,E.Markers);
        for(const auto& D:M.Directives){MapGlows+=D.MarkerType==1526;}
        TestEqual(FString(E.Name)+TEXT(" raw glow count"),MapGlows,E.Glows);Glows+=MapGlows;
        for(const auto& A:M.Animations){++Animations;++Rates.FindOrAdd(A.Rate);}
        for(const auto& S:M.Sectors)
        {
            if(!S.Neighbors.Contains(S.Sector)){continue;}++Self;
            TestTrue(TEXT("only original three self-neighbor sectors"),(FString(E.Name)==TEXT("L20")&&S.Sector==112)
                ||(FString(E.Name)==TEXT("L151")&&S.Sector==38)||(FString(E.Name)==TEXT("L10")&&S.Sector==219));
        }
        if(FString(E.Name)==TEXT("L64"))
        {
            const auto* A=M.Illuminators.FindByPredicate([](const auto& O){return O.ThingId==6;});
            const auto* B=M.Illuminators.FindByPredicate([](const auto& O){return O.ThingId==246;});
            if(!TestNotNull(TEXT("actual positive owner6"),A)||!TestNotNull(TEXT("actual negative owner246"),B)){return false;}
            TestEqual(TEXT("actual mixed source center281"),A->CenterSector,281);TestEqual(TEXT("same center281"),B->CenterSector,281);
            TestEqual(TEXT("source Torch +128"),A->Illumination,128);TestEqual(TEXT("source marker -50"),B->Illumination,-50);
            const auto* Other=M.Illuminators.FindByPredicate([](const auto& O){return O.CenterSector!=281;});
            if(!TestNotNull(TEXT("another real center"),Other)){return false;}
            TMap<int32,FVector2D> Edits;Edits.Add(6,Other->CenterPosition);FWadSourceLightingMetadata Edited;
            if(!TestTrue(TEXT("edited XY re-resolves original BLOCKMAP"),BuildWadSourceLighting(Source,Bytes,E.Name,Edits,Edited,Error))){return false;}
            const auto& Changed=Edited.Illuminators[0];
            TestEqual(TEXT("authored sector is not overwritten"),Changed.AuthoredSector,281);
            TestEqual(TEXT("current center owns illumination"),Changed.CenterSector,Other->CenterSector);
            TestTrue(TEXT("edited center retained exactly"),Changed.CenterPosition==Other->CenterPosition);TestTrue(TEXT("editor override marked"),Changed.bUsesEditedCenter);
        }
        if(FString(E.Name)==TEXT("L60")||FString(E.Name)==TEXT("L151")||FString(E.Name)==TEXT("L40"))
        {
            const int32 S=FString(E.Name)==TEXT("L60")?135:FString(E.Name)==TEXT("L151")?35:92;
            const auto* Active=M.Animations.FindByPredicate([&](const auto& A){return A.Sector==S;});
            if(FString(E.Name)==TEXT("L40")){if(!TestNotNull(TEXT("later lava replaces glow"),Active)){return false;}TestEqual(TEXT("source lava rate"),Active->Rate,40);}
            else{TestNull(TEXT("later water disables earlier glow"),Active);}
        }
    }
    TestEqual(TEXT("source illuminating owners"),Owners,655);TestEqual(TEXT("invisible924"),Markers,200);TestEqual(TEXT("source props"),Props,455);
    TestEqual(TEXT("negative924"),Negative,51);TestEqual(TEXT("zero924"),Zero,5);TestEqual(TEXT("rawglow"),Glows,78);
    TestEqual(TEXT("effective glow"),Rates.FindRef(70),75);TestEqual(TEXT("lava"),Rates.FindRef(40),75);TestEqual(TEXT("acid"),Rates.FindRef(20),1);
    TestEqual(TEXT("all source animations"),Animations,151);TestEqual(TEXT("authored self edges"),Self,3);TestEqual(TEXT("explicit undefined-source edges"),Excluded,175);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceLightArithmetic,"AAOriginal.WAD.Lighting.SourceFeedbackArithmetic",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceLightArithmetic::RunTest(const FString&)
{
    using namespace SourceLightingMetadataTests;
    TestEqual(TEXT("signed marker word"),NativeWadLightRules::SourceIllumination(924,65344),-192);
    TestEqual(TEXT("packed brazier family retains basic41 rule"),NativeWadLightRules::SourceIllumination(0x4029,0),64);
    TestFalse(TEXT("packed Torch is not exact27"),NativeWadLightRules::IsIlluminatingType(0x401b));
    TestFalse(TEXT("projectiles do not acquire invented light"),NativeWadLightRules::IsIlluminatingType(4001));
    TArray<int32> Out;FString Error;
    auto M=Kernel(1);Owner(M,0,0,924,10);
    TestTrue(TEXT("isolated two-pass kernel"),Run(M,{20},Out));TestTrue(TEXT("isolated owner applied after BOTH passes"),Out==TArray<int32>{40});
    TestTrue(TEXT("live feedback second frame"),Run(M,Out,Out));TestTrue(TEXT("feedback not reseeded from authored20"),Out==TArray<int32>{60});
    M=Kernel(1);Owner(M,6,0,27,128);M.ThingCount=300;Owner(M,246,0,924,-50);
    TestTrue(TEXT("actual mixed order"),Run(M,{200},Out));TestEqual(TEXT("clamp each owner, not aggregate"),Out[0],205);
    M.Illuminators.Reset();Owner(M,0,0,924,-50);Owner(M,1,0,27,128);
    TestTrue(TEXT("opposite authored order"),Run(M,{200},Out));TestEqual(TEXT("reversing owner order changes outcome"),Out[0],255);
    M=Kernel(1);Owner(M,0,0,924,32767);TestTrue(TEXT("signed16 intermediate"),Run(M,{20},Out));TestEqual(TEXT("signed16 conversion occurs before byte clamp"),Out[0],255);
    // Pass1 wraps20+32767 negative ->0; pass2 32767 ->255. A single aggregate would differ.
    M=Kernel(2);M.Sectors[0].Neighbors={1};M.Sectors[1].Neighbors={0};Owner(M,0,0,924,10);
    TestTrue(TEXT("synchronous source reads"),Run(M,{100,0},Out));TestTrue(TEXT("first two-pass frame"),Out==TArray<int32>({110,10}));
    TestTrue(TEXT("next source feedback frame"),Run(M,Out,Out));TestTrue(TEXT("prior output persists"),Out==TArray<int32>({120,20}));
    M=Kernel(1);M.Sectors[0].Neighbors={0};Owner(M,0,0,924,10);Run(M,{20},Out);TestEqual(TEXT("self edge retained"),Out[0],40);
    M=Kernel(3);M.Sectors[0].Neighbors={1,2};M.Sectors[1].Neighbors={0,2};M.Sectors[2].Neighbors={0,1};
    M.Directives={NativeWadLightRules::DirectiveFor(0,1526,0)};NativeWadLightRules::ResolveAnimations(M.Directives,3,M.Animations,Error);
    TestTrue(TEXT("previous glow propagates two passes"),Run(M,{190,0,0},Out));TArray<int32> Phase;
    TestTrue(TEXT("glow runs after source feedback"),NativeWadLightRules::Animate(M,1,Out,TArray<int32>{0,0,0},Out,Phase,Error));
    TestTrue(TEXT("source neighbor coupling"),Out==TArray<int32>({193,47,47}));
    M=Kernel(1);M.Directives={NativeWadLightRules::DirectiveFor(0,1526,0)};NativeWadLightRules::ResolveAnimations(M.Directives,1,M.Animations,Error);
    const int32 Ticks[]={0,1,2,29,58,70,117},Lights[]={190,193,196,249,191,155,189};
    for(int32 I=0;I<UE_ARRAY_COUNT(Ticks);++I)
    {
        TestTrue(TEXT("source sine step"),NativeWadLightRules::Animate(M,uint16(Ticks[I]),TArray<int32>{0},TArray<int32>{0},Out,Phase,Error));
        TestEqual(TEXT("source uint16 phase"),Phase[0],(Ticks[I]*560)&65535);TestEqual(TEXT("source signed sine/truncation"),Out[0],Lights[I]);
    }
    NativeWadLightRules::Animate(M,1,TArray<int32>{0},TArray<int32>{65530},Out,Phase,Error);TestEqual(TEXT("phase wraps16bit"),Phase[0],554);
    // Three-sector door bridge. Intermediate average, adjacent-door weight, then own-door weight.
    M=Kernel(3);M.Sectors[0].Neighbors={1};M.Sectors[1].Neighbors={0,2};M.Sectors[2].Neighbors={1};M.Sectors[1].bDoor=true;
    Run(M,{200,100,0},Out,{256,0,256});TestTrue(TEXT("closed door blocks source transfer"),Out==TArray<int32>({0,0,0}));
    Run(M,{200,100,0},Out,{256,128,256});TestTrue(TEXT("half-open multiplication/order"),Out==TArray<int32>({50,25,0}));
    Run(M,{200,100,0},Out,{256,256,256});TestTrue(TEXT("open door source two passes"),Out==TArray<int32>({200,100,0}));
    M.Sectors[1].bOutside=true;Run(M,{200,100,0},Out,{256,0,256},180);TestEqual(TEXT("outside source priority over own closed door"),Out[1],180);
    M=Kernel(3);M.Sectors[0].Neighbors={0,1,2};M.Sectors[1].Neighbors={0};M.Sectors[2].Neighbors={0};
    Run(M,{255,255,255},Out);TestEqual(TEXT("integer percentages are33/33/34 and coefficients84/84/87"),Out[0],253);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceLightPreflight,"AAOriginal.WAD.Lighting.SourceMetadataPreflight",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceLightPreflight::RunTest(const FString&)
{
    using namespace SourceLightingMetadataTests;
    FString Error;auto M=Kernel(1);TArray<int32> Out={77},Phases={88};
    auto Bad=M;Bad.Sectors[0].Neighbors={1};TestFalse(TEXT("invalid adjacency refuses atomically"),NativeWadLightRules::Recalculate(Bad,TArray<int32>{20},TArray<int32>{256},200,Out,Error));TestEqual(TEXT("output retained"),Out[0],77);
    Owner(M,0,0,924,10);Bad=M;const auto DuplicateOwner=Bad.Illuminators[0];Bad.Illuminators.Add(DuplicateOwner);
    TestFalse(TEXT("duplicate owner rejected"),NativeWadLightRules::Validate(Bad,Error));
    Bad=M;Bad.Illuminators[0].Illumination=11;TestFalse(TEXT("not guessed owner power"),NativeWadLightRules::Validate(Bad,Error));
    Bad=M;Bad.Illuminators[0].CenterPosition.X=std::numeric_limits<double>::infinity();TestFalse(TEXT("nonfinite center"),NativeWadLightRules::Validate(Bad,Error));
    Bad=M;Bad.Directives={NativeWadLightRules::DirectiveFor(0,1526,0)};TestFalse(TEXT("oneThing cannot be owner and marker"),NativeWadLightRules::Validate(Bad,Error));
    TestFalse(TEXT("bad byte rejected"),NativeWadLightRules::Recalculate(M,TArray<int32>{256},TArray<int32>{256},200,Out,Error));
    TestFalse(TEXT("bad door rejected"),NativeWadLightRules::Recalculate(M,TArray<int32>{20},TArray<int32>{257},200,Out,Error));
    TestFalse(TEXT("bad phase rejected"),NativeWadLightRules::Animate(M,1,TArray<int32>{20},TArray<int32>{65536},Out,Phases,Error));
    TestEqual(TEXT("failed phase does not overwrite"),Phases[0],88);
    FDoomWadMapData Source;TArray<uint8> Bytes;FWadSourceLightingMetadata Result;Result.Version=77;
    if(!TestTrue(TEXT("originalL30"),Original(TEXT("L30"),Source,Bytes))){return false;}
    auto Truncated=Bytes;Truncated.SetNum(15);TestFalse(TEXT("truncated original refuses"),BuildWadSourceLighting(Source,Truncated,TEXT("L30"),{},Result,Error));TestEqual(TEXT("no partial metadata write"),Result.Version,77);
    auto Mismatch=Source;++Mismatch.Things[0].X;TestFalse(TEXT("loaded source mismatch refuses"),BuildWadSourceLighting(Mismatch,Bytes,TEXT("L30"),{},Result,Error));
    TMap<int32,FVector2D> Edits;Edits.Add(999999,FVector2D::ZeroVector);TestFalse(TEXT("unknown editor light ID refuses"),BuildWadSourceLighting(Source,Bytes,TEXT("L30"),Edits,Result,Error));
    const int32 Id=Source.Things.IndexOfByPredicate([](const auto& T){return uint16(T.Type)==924;});
    Edits.Reset();Edits.Add(Id,FVector2D(40000,0));TestFalse(TEXT("out-of-range editor source center refuses"),BuildWadSourceLighting(Source,Bytes,TEXT("L30"),Edits,Result,Error));
    TestEqual(TEXT("all failures leave destination intact"),Result.Version,77);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceLightLegacyMode,"AAOriginal.WAD.Lighting.SourceMetadataLeavesLegacyMode",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceLightLegacyMode::RunTest(const FString&)
{
    using namespace SourceLightingMetadataTests;
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);if(!TestNotNull(TEXT("world"),World)){return false;}
    auto* Level=World->SpawnActor<ANativeWadLevel>();if(!TestNotNull(TEXT("level"),Level)){World->DestroyWorld(false);return false;}
    const auto* P=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
    if(!TestNotNull(TEXT("native sectors"),P)){World->DestroyWorld(false);return false;}
    auto& S=P->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level)->AddDefaulted_GetRef();S.Light=73;
    auto* Floor=NewObject<UStaticMeshComponent>(Level);Floor->SetMobility(EComponentMobility::Movable);Floor->SetupAttachment(Level->GetRootComponent());Floor->RegisterComponent();Floor->SetRelativeLocation(FVector(10,20,30));S.Floor=Floor;
    Floor->SetCustomPrimitiveDataFloat(0,0.125f);
    const FString PrimitiveBefore=UNativeWadArtEditor::DescribePrimitiveData(Floor);
    TestTrue(TEXT("Readonly QA observes live runtime data when editable defaults are empty"),
        Floor->GetDefaultCustomPrimitiveData().Data.IsEmpty()&&PrimitiveBefore.Contains(TEXT("\"runtime\""))&&PrimitiveBefore.Contains(TEXT("0.125")));
    const auto Before=Floor->GetComponentTransform();const auto Materials=Floor->GetNumMaterials();FString Error;auto M=Kernel(1);Owner(M,0,0,924,-100);
    TestFalse(TEXT("old default remains legacy"),Level->IsSourceLightFeedbackEnabled());
    TestTrue(TEXT("metadata only install"),Level->InstallSourceLightingMetadata(M,Error));
    TestFalse(TEXT("refresh cannot opt in model"),Level->IsSourceLightFeedbackEnabled());TestEqual(TEXT("metadata stored"),Level->GetSourceLightingMetadata().Version,1);
    TestEqual(TEXT("baseline light is not overwritten"),S.Light,73);TestTrue(TEXT("floor transform retained"),Floor->GetComponentTransform().Equals(Before));TestEqual(TEXT("floor materials retained"),Floor->GetNumMaterials(),Materials);
    TestEqual(TEXT("Metadata installation preserves real renderer CPD, not just editor defaults"),UNativeWadArtEditor::DescribePrimitiveData(Floor),PrimitiveBefore);
    auto Bad=M;Bad.ThingCount=0;TestFalse(TEXT("invalid install refused"),Level->InstallSourceLightingMetadata(Bad,Error));TestEqual(TEXT("good metadata survived rejection"),Level->GetSourceLightingMetadata().ThingCount,20);
    World->DestroyWorld(false);return true;
}
