#include "Misc/AutomationTest.h"
#include <limits>
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadLevel.h"
#include "NativeWadScenery.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadLightSource.h"

namespace SourceLightingFeedbackTests
{
    bool LoadOriginal(const FString& Name,TArray<uint8>& Bytes,FDoomWadMapData* Result=nullptr)
    {
        const FString Filename=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe")/(Name+TEXT(".MAP"));
        if(!FFileHelper::LoadFileToArray(Bytes,*Filename)||Bytes.Num()<12){return false;}
        if(!Result){return true;}
        const auto U32=[&](int32 At){return uint32(Bytes[At])|uint32(Bytes[At+1])<<8|uint32(Bytes[At+2])<<16|uint32(Bytes[At+3])<<24;};
        const uint32 Count=U32(4),Directory=U32(8);if(uint64(Directory)+uint64(Count)*16>uint64(Bytes.Num())){return false;}
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
            const auto* Data=Lumps.Find(Key);const int32 Stride=sizeof(Records[0]);if(!Data||Data->Num()%Stride){return false;}
            Records.SetNumUninitialized(Data->Num()/Stride);if(Data->Num()){FMemory::Memcpy(Records.GetData(),Data->GetData(),Data->Num());}return true;
        };
        return Read(TEXT("VERTEXES"),Result->Vertices)&&Read(TEXT("LINEDEFS"),Result->Lines)&&Read(TEXT("SIDEDEFS"),Result->Sides)
            &&Read(TEXT("SECTORS"),Result->Sectors)&&Read(TEXT("THINGS"),Result->Things);
    }
    struct FFeedbackFixture
    {
        UWorld* World=nullptr;ANativeWadLevel* Level=nullptr;FWadSourceLightingMetadata Metadata;TArray<uint8> Bytes;
        FString Error;bool bReady=false;
        FFeedbackFixture(const FWadSourceLightingMetadata* Actual=nullptr,const TArray<uint8>* ActualBytes=nullptr,bool bEnable=true)
        {
            World=UWorld::CreateWorld(EWorldType::Game,false);if(!World){return;}Level=World->SpawnActor<ANativeWadLevel>();if(!Level){return;}
            if(Actual){Metadata=*Actual;Bytes=*ActualBytes;}
            else
            {
                if(!LoadOriginal(TEXT("L30"),Bytes)){return;}
                Metadata.Version=NativeWadLightRules::MetadataVersion;Metadata.SourceMapName=TEXT("ControlledFeedbackFixture");Metadata.ThingCount=20;
                FSHAHash Hash;FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Hash.Hash);Metadata.SourceMapSha1=Hash.ToString();
                for(int32 I=0;I<2;++I){auto& S=Metadata.Sectors.AddDefaulted_GetRef();S.Sector=I;S.AuthoredLight=20;S.Neighbors={1-I};}
                auto& O=Metadata.Illuminators.AddDefaulted_GetRef();O.ThingId=0;O.SourceType=924;O.AccessoryData=10;O.Illumination=10;O.AuthoredSector=O.CenterSector=1;
                Metadata.Directives={NativeWadLightRules::DirectiveFor(1,1526,0)};
                if(!NativeWadLightRules::ResolveAnimations(Metadata.Directives,2,Metadata.Animations,Error)){return;}
                Metadata.Sectors[0].bDoor=true;
                const auto* Doors=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Doors"));if(!Doors){return;}
                auto& Door=Doors->ContainerPtrToValuePtr<TArray<FAAWadNativeDoor>>(Level)->AddDefaulted_GetRef();Door.Sector=0;Door.ClosedZ=0;Door.OpenZ=100;
                const auto* Rules=FindFProperty<FBoolProperty>(ANativeWadLevel::StaticClass(),TEXT("bUseSectorLightingRules"));if(!Rules){return;}Rules->SetPropertyValue_InContainer(Level,true);
            }
            const auto* P=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));if(!P){return;}
            auto& Native=*P->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
            for(const auto& M:Metadata.Sectors)
            {
                auto& S=Native.AddDefaulted_GetRef();S.Light=M.AuthoredLight;
                const auto Surface=[&](double Z)
                {
                    auto* C=NewObject<UStaticMeshComponent>(Level);C->SetMobility(EComponentMobility::Movable);C->SetupAttachment(Level->GetRootComponent());C->RegisterComponent();C->SetRelativeLocation(FVector(0,0,Z));return C;
                };
                S.Floor=Surface(0);S.Ceiling=Surface(100);
            }
            bReady=Level->InstallSourceLightingMetadata(Metadata,Error)&&(!bEnable||Level->SetSourceLightFeedbackEnabled(true,Bytes,Error));
        }
        ~FFeedbackFixture(){if(World){World->DestroyWorld(false);}}
        bool Valid() const{return World&&Level&&bReady;}
        void Tick(double Delta){Level->Tick(float(Delta));}
        bool Drain()
        {
            for(int32 I=0;I<100&&Level->GetSourceLightState().PendingTicks>=1.0;++I){Tick(0.0);}
            return Level->GetSourceLightState().PendingTicks<1.0;
        }
    };
    bool SameState(const FWadSourceLightState& A,const FWadSourceLightState& B)
    {return A.Version==B.Version&&A.SourceMapSha1==B.SourceMapSha1&&A.Levels==B.Levels&&A.Phases==B.Phases
        &&A.PendingTicks==B.PendingTicks&&A.CompletedSteps==B.CompletedSteps;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceFeedbackClock,"AAOriginal.WAD.Lighting.SourceFeedbackClockAndPresentation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceFeedbackClock::RunTest(const FString&)
{
    using namespace SourceLightingFeedbackTests;
    FWadSourceLightState Reference;
    for(const int32 FPS:{30,70,144})
    {
        FFeedbackFixture F;if(!TestTrue(TEXT("controlled source feedback fixture: ")+F.Error,F.Valid())){return false;}
        for(int32 Frame=0;Frame<FPS;++Frame){F.Tick(1.0/FPS);}
        const auto State=F.Level->GetSourceLightState();TestEqual(TEXT("one second executes70 fixed source steps"),State.CompletedSteps,uint64(70));
        TestEqual(TEXT("source phase uses70 one-tick advances"),State.Phases[0],39200);TestEqual(TEXT("source glow at tick70"),State.Levels[0],155);
        if(FPS==30){Reference=State;}else{TestTrue(TEXT("FPS partition leaves identical source bytes/phases"),State.Levels==Reference.Levels&&State.Phases==Reference.Phases);}
        TestTrue(TEXT("only floating-point fractional residue differs"),State.PendingTicks<0.0001);
    }
    FFeedbackFixture F;if(!F.Valid()){return false;}
    F.Tick(1.0);TestEqual(TEXT("long-frame work is bounded"),F.Level->GetSourceLightingLastStepCount(),16);
    TestEqual(TEXT("debt is retained, not silently dropped"),F.Level->GetSourceLightState().PendingTicks,54.0);
    TestTrue(TEXT("subsequent frames drain all debt"),F.Drain());TestEqual(TEXT("all original70 steps eventually execute"),F.Level->GetSourceLightState().CompletedSteps,uint64(70));
    const auto Before=F.Level->GetSourceLightState();
    F.Level->SetOutsideLightLevel(45);F.Level->SetSectorCeilingZ(0,50);F.Level->SetSectorFloorZ(1,8);
    TestTrue(TEXT("persistent display override accepted"),F.Level->SetSectorLightLevel(0,12));
    TestTrue(TEXT("sky/door/floor/presentation callbacks cannot add feedback steps"),SameState(Before,F.Level->GetSourceLightState()));
    TestEqual(TEXT("override immediately displayed"),F.Level->GetSectorLightLevel(0),12);
    F.Tick(1.0/70.0);const auto After=F.Level->GetSourceLightState();
    TestEqual(TEXT("one normal tick advances exactly once"),After.CompletedSteps,Before.CompletedSteps+1);
    TestEqual(TEXT("persistent override survives moving-sector source update"),F.Level->GetSectorLightLevel(0),12);
    TestTrue(TEXT("manual12 does not overwrite source feedback byte"),After.Levels[0]!=12);
    const auto Frozen=After;F.Level->ClearSectorLightOverride(0);
    TestEqual(TEXT("clear reveals current hidden source state"),F.Level->GetSectorLightLevel(0),Frozen.Levels[0]);
    TestTrue(TEXT("clear is presentation only"),SameState(Frozen,F.Level->GetSourceLightState()));
    TestTrue(TEXT("normal feedback work has a finite performance sample"),FMath::IsFinite(F.Level->GetSourceLightingLastMilliseconds())&&F.Level->GetSourceLightingLastMilliseconds()>=0.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceFeedbackCenters,"AAOriginal.WAD.Lighting.SourceFeedbackCookedCenters",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceFeedbackCenters::RunTest(const FString&)
{
    using namespace SourceLightingFeedbackTests;
    FDoomWadMapData Map;TArray<uint8> Bytes;FWadSourceLightingMetadata M;FString Error;
    if(!TestTrue(TEXT("originalL64"),LoadOriginal(TEXT("L64"),Bytes,&Map))||!TestTrue(TEXT("original metadata"),BuildWadSourceLighting(Map,Bytes,TEXT("L64"),{},M,Error))){return false;}
    const auto* Torch=M.Illuminators.FindByPredicate([](const auto& O){return O.ThingId==6;});
    const auto* Other=M.Illuminators.FindByPredicate([](const auto& O){return O.CenterSector!=281;});
    if(!TestNotNull(TEXT("source torch6"),Torch)||!TestNotNull(TEXT("other source center"),Other)){return false;}
    const auto Original=*Torch;const auto Target=*Other;
    // Controlled movement fixture: keep the real source topology/lookup but one source torch only.
    M.Illuminators={Original};for(auto& S:M.Sectors){S.bDoor=false;}
    FFeedbackFixture F(&M,&Bytes,false);if(!TestTrue(TEXT("real-source center fixture"),F.Valid())){return false;}
    auto* Prop=F.World->SpawnActor<ANativeWadScenery>();FWadSceneryDefinition Definition;
    Prop->Configure(*F.Level,nullptr,Definition,Original.CenterSector,Original.ThingId,false,Original.SourceType);
    Prop->SetActorLocation(FVector(Original.CenterPosition,30));M.Illuminators[0].BoundActor=Prop;
    if(!TestTrue(TEXT("bind exact real source owner"),F.Level->InstallSourceLightingMetadata(M,Error))
        ||!TestTrue(TEXT("opt-in uses cooked bytes"),F.Level->SetSourceLightFeedbackEnabled(true,Bytes,Error))){return false;}
    F.Tick(1.0/70.0);TestEqual(TEXT("authored center281"),F.Level->GetSourceIlluminationOwnerSector(6),281);
    TestEqual(TEXT("unchanged XY uses cached original sector"),F.Level->GetSourceLightingLookupResolutions(),uint64(0));
    Prop->SetActorLocation(FVector(Target.CenterPosition,60));F.Tick(0.0);
    TestEqual(TEXT("actual edited center owns light"),F.Level->GetSourceIlluminationOwnerSector(6),Target.CenterSector);
    TestEqual(TEXT("only actual XY movement runs original BLOCKMAP"),F.Level->GetSourceLightingLookupResolutions(),uint64(1));
    Prop->AddActorWorldOffset(FVector(0,0,20));F.Tick(0.0);F.Tick(1.0/70.0);
    TestEqual(TEXT("Z/billboard changes cannot trigger redundant lookups"),F.Level->GetSourceLightingLookupResolutions(),uint64(1));
    const auto Before=F.Level->GetSourceLightState();Prop->Destroy();F.Tick(1.0/70.0);
    TestEqual(TEXT("destroyed source prop stops illuminating"),F.Level->GetSourceIlluminationOwnerSector(6),INDEX_NONE);
    NativeWadLightRules::FPreparedKernel Kernel;if(!Kernel.Initialize(M,Error)){return false;}
    auto Expected=Before;TArray<int32> Doors;Doors.Init(256,M.Sectors.Num());
    TestTrue(TEXT("source step omits removed owner"),Kernel.Step(Expected,Doors,255,TArray<int32>{INDEX_NONE},Error));
    TestTrue(TEXT("real level follows no-owner kernel after destruction"),F.Level->GetSourceLightState().Levels==Expected.Levels);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWadSourceFeedbackPersistence,"AAOriginal.WAD.Lighting.SourceFeedbackCheckpointStrictness",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWadSourceFeedbackPersistence::RunTest(const FString&)
{
    using namespace SourceLightingFeedbackTests;
    FFeedbackFixture F;if(!F.Valid()){return false;}F.Tick(0.5/70.0);F.Tick(0.1);
    F.Level->SetSectorLightLevel(0,17);FWadLevelCheckpoint Checkpoint;
    if(!TestTrue(TEXT("real level checkpoint writer"),F.Level->WriteCheckpoint(Checkpoint,true))){return false;}
    TestEqual(TEXT("additive state version"),Checkpoint.SourceLighting.Version,1);
    TestTrue(TEXT("fractional source clock captured"),Checkpoint.SourceLighting.PendingTicks>0&&Checkpoint.SourceLighting.PendingTicks<1);
    const FString Slot=TEXT("NativeSourceLightingTest-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT {UGameplayStatics::DeleteGameInSlot(Slot,0);};
    auto* Class=FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadCheckpoint"));
    if(!TestNotNull(TEXT("loaded checkpoint class"),Class)){return false;}
    auto* Save=static_cast<UNativeWadCheckpoint*>(NewObject<USaveGame>(GetTransientPackage(),Class));Save->Level=Checkpoint;
    if(!TestTrue(TEXT("actual disk save"),UGameplayStatics::SaveGameToSlot(Save,Slot,0))){return false;}
    auto* Loaded=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    if(!TestNotNull(TEXT("actual disk read"),Loaded)){return false;}
    FFeedbackFixture Fresh;if(!Fresh.Valid()){return false;}
    TestTrue(TEXT("fresh actor restores native level/checkpoint"),Fresh.Level->RestoreCheckpoint(Loaded->Level));
    TestTrue(TEXT("live bytes/phases/remainder/steps/hash survive disk"),SameState(F.Level->GetSourceLightState(),Fresh.Level->GetSourceLightState()));
    TestEqual(TEXT("actual restore recorded the complete accepted payload"),Fresh.Level->GetRestoredSourceLightingDigest(),NativeWadLightRules::StateDigest(Loaded->Level.SourceLighting));
    TestEqual(TEXT("persistent native display override also survives"),Fresh.Level->GetSectorLightLevel(0),17);
    F.Tick(0.5/70.0);Fresh.Tick(0.5/70.0);TestTrue(TEXT("continuation uses restored fraction, not a reseeded clock"),SameState(F.Level->GetSourceLightState(),Fresh.Level->GetSourceLightState()));
    const auto Before=Fresh.Level->GetSourceLightState();
    for(int32 Case=0;Case<11;++Case)
    {
        auto Bad=Loaded->Level;
        if(Case==0){Bad.SourceLighting.Levels.Reset();}
        if(Case==1){Bad.SourceLighting.SourceMapSha1=TEXT("wrong-source");}
        if(Case==2){Bad.SourceLighting.Phases[0]=65536;}
        if(Case==3){Bad.SourceLighting.PendingTicks=-1;}
        if(Case==4){Bad.SourceLighting.Version=99;}
        if(Case==5){Bad.SourceLighting.Version=0;}
        if(Case==6){Bad.SourceLighting.PendingTicks=std::numeric_limits<double>::quiet_NaN();}
        if(Case==7){Bad.SourceLighting.PendingTicks=std::numeric_limits<double>::infinity();}
        if(Case==8){Bad.SourceLighting.PendingTicks=NativeWadLightRules::MaximumPendingTicks+1.0;}
        if(Case==9){Bad.SourceLighting.Levels[0]=256;}
        if(Case==10){Bad.SourceLighting.Phases[0]=-1;}
        TestFalse(TEXT("malformed modern or disguised-legacy checkpoint rejected"),Fresh.Level->RestoreCheckpoint(Bad));
        TestTrue(TEXT("rejected payload does not mutate live state"),SameState(Before,Fresh.Level->GetSourceLightState()));
    }
    FFeedbackFixture Legacy(nullptr,nullptr,false);if(!Legacy.Valid()){return false;}
    TestFalse(TEXT("old map remains legacy by default"),Legacy.Level->IsSourceLightFeedbackEnabled());
    TestFalse(TEXT("modern B save cannot silently enable a legacy map"),Legacy.Level->RestoreCheckpoint(Loaded->Level));
    auto Old=Loaded->Level;Old.SourceLighting={};TestTrue(TEXT("genuine absent old payload remains compatible"),Fresh.Level->RestoreCheckpoint(Old));
    TestEqual(TEXT("legacy absence starts clean source phase only on opted-in map"),Fresh.Level->GetSourceLightState().CompletedSteps,uint64(0));
    TestEqual(TEXT("old saved persistent manual override retained"),Fresh.Level->GetSectorLightLevel(0),17);
    const auto* Raw=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("SourceLightLookupBytes"));
    if(!TestNotNull(TEXT("private cooked lookup"),Raw)){return false;}
    auto& Stored=*Raw->ContainerPtrToValuePtr<TArray<uint8>>(Fresh.Level);auto KeptBytes=Stored;Stored.Reset();
    FVector2D HeightsBefore,HeightsAfter;Fresh.Level->GetSectorSurfaceHeights(0,HeightsBefore);
    auto BadMap=Old;BadMap.SectorHeights.Add(0,FVector2D(0,50));
    TestFalse(TEXT("even legacy absence cannot restore into a corrupt enabled map"),Fresh.Level->RestoreCheckpoint(BadMap));
    Fresh.Level->GetSectorSurfaceHeights(0,HeightsAfter);TestTrue(TEXT("map-integrity refusal precedes geometry mutation"),HeightsBefore==HeightsAfter);
    Stored=MoveTemp(KeptBytes);

    return true;
}
