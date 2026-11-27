#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/OutputDeviceNull.h"
#include "Internationalization/Regex.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadAmbient.h"
#include "NativeWadPlayer.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadOptions.h"

namespace
{
    FWadAmbientEmitter Marker(int32 Type,int32 Id,FVector Position=FVector::ZeroVector,int32 Group=0)
    {
        const auto D=ResolveWadAmbientMarker(Type);FWadAmbientEmitter E;
        E.SourceThingId=Id;E.MarkerType=Type;E.Sound=D.Sound;E.Position=Position;E.Group=Group;
        E.Radius=D.Radius;E.MaxVolume=D.MaxVolume;E.bLoop=D.bLoop;return E;
    }
    bool SourceSample(int32 Sound,FWadSoundSample& Sample)
    {
        TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*(FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/SOUNDS.RES")))||Bytes.Num()<14){return false;}
        const auto U=[&](int32 P,int32 N){uint32 V=0;for(int32 I=0;I<N;++I){V|=uint32(Bytes[P+I])<<(I*8);}return V;};
        const uint32 Start=U(4,4),Count=U(12,2);if(uint64(Start)+uint64(Count)*39>uint64(Bytes.Num())){return false;}
        const FString Wanted=FString::Printf(TEXT("snd#%d"),Sound);
        for(uint32 I=0;I<Count;++I)
        {
            const int32 P=Start+39*I;ANSICHAR Name[15]={};FMemory::Memcpy(Name,Bytes.GetData()+P+4,14);
            if(FString(ANSI_TO_TCHAR(Name))!=Wanted){continue;}
            const uint32 Offset=U(P+18,4),Length=U(P+22,4);if(Length==0||uint64(Offset)+Length>Start){return false;}
            Sample.UnsignedSamples.Append(Bytes.GetData()+Offset,Length);return true;
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAmbientMarkerSource,"AAOriginal.WAD.Audio.AmbientMarkerSource",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAmbientMarkerSource::RunTest(const FString& Parameters)
{
    FString Source;if(!FFileHelper::LoadFileToString(Source,*(FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Source/3D_IO.C")))){return false;}
    TMap<FString,int32> Constants;
    const FRegexPattern Defines(TEXT("#define\\s+(SPECIAL_OBJECT_AREA_SOUND_[A-Z0-9_]+|SOUND_[A-Z0-9_]+)\\s+([0-9]+)"));
    FRegexMatcher Definition(Defines,Source);
    while(Definition.FindNext()){Constants.Add(Definition.GetCaptureGroup(1),FCString::Atoi(*Definition.GetCaptureGroup(2)));}
    const FRegexPattern Cases(TEXT("case\\s+(SPECIAL_OBJECT_AREA_SOUND_[A-Z0-9_]+):([\\s\\S]*?)break\\s*;"));FRegexMatcher Match(Cases,Source);
    int32 Count=0;
    while(Match.FindNext())
    {
        const FString Name=Match.GetCaptureGroup(1),Body=Match.GetCaptureGroup(2);
        const FRegexPattern Fields(TEXT("AreaSoundCreate\\(\\s*x,\\s*y,\\s*([0-9]+),\\s*([0-9]+),\\s*(AREA_SOUND_TYPE_[A-Z_]+)"));
        FRegexMatcher Args(Fields,Body);
        const FRegexPattern SampleName(TEXT("(SOUND_[A-Z0-9_]+)\\s*\\)\\s*;"));FRegexMatcher Sound(SampleName,Body);
        if(!Args.FindNext()||!Sound.FindNext()||!Constants.Contains(Name)||!Constants.Contains(Sound.GetCaptureGroup(1))){AddError(TEXT("Could not independently read original ambient marker"));return false;}
        const auto Native=ResolveWadAmbientMarker(Constants[Name]);
        TestEqual(TEXT("Source marker uses its literal sample ID"),Native.Sound,Constants[Sound.GetCaptureGroup(1)]);
        TestEqual(TEXT("Source radius matches"),Native.Radius,FCString::Atoi(*Args.GetCaptureGroup(1)));
        TestEqual(TEXT("Source maximum volume matches, including half-volume soft wind"),Native.MaxVolume,FCString::Atoi(*Args.GetCaptureGroup(2)));
        TestTrue(TEXT("These original markers are loops"),Native.bLoop&&Args.GetCaptureGroup(3)==TEXT("AREA_SOUND_TYPE_LOOP"));
        FWadSoundSample PCM;if(!TestTrue(TEXT("Exact original ambient sample exists and has real PCM bytes"),SourceSample(Native.Sound,PCM))){return false;}
        ++Count;
    }
    TestEqual(TEXT("All nineteen original ambient marker definitions are covered"),Count,19);
    TestEqual(TEXT("Hard and soft wind share3004 explicitly in the original source"),ResolveWadAmbientMarker(1512).Sound,ResolveWadAmbientMarker(1511).Sound);
    for(int32 Other:{0,1503,1506,1520,1526,1543}){TestEqual(TEXT("Non-audio special markers do not become sounds"),ResolveWadAmbientMarker(Other).Sound,0);}
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAmbientMigration,"AAOriginal.WAD.Audio.AmbientMigration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAmbientMigration::RunTest(const FString& Parameters)
{
    TArray<FWadAmbientEmitter> Expected{Marker(1509,10),Marker(1513,30,FVector(100,200,5))};
    FWadAmbientEmitter Legacy;Legacy.Position=FVector(123,456,78);Legacy.Sound=777;Legacy.Group=999;
    Legacy.Radius=321;Legacy.MaxVolume=42;Legacy.bLoop=false;
    TArray<FWadAmbientEmitter> Existing{Legacy},Updated;FOutputDeviceNull Output;
    if(!MergeWadAmbientEmitters(Existing,Expected,Updated,Output)){return false;}
    TestEqual(TEXT("The new source marker is added without duplicating the legacy one"),Updated.Num(),2);
    TestEqual(TEXT("Old stream/waterfall source order recovers its exact Thing ID"),Updated[1].SourceThingId,30);
    TestTrue(TEXT("Legacy authored audio overrides survive migration"),Updated[1].Position==Legacy.Position&&Updated[1].Sound==777
        &&Updated[1].Group==999&&Updated[1].Radius==321&&Updated[1].MaxVolume==42&&!Updated[1].bLoop);
    TestEqual(TEXT("New fireplace marker receives actual source defaults"),Updated[0].Sound,3008);
    if(!MergeWadAmbientEmitters(Updated,Expected,Updated,Output)){return false;}
    TestEqual(TEXT("Repeated refresh is idempotent even when output aliases existing data"),Updated.Num(),2);
    TestEqual(TEXT("Idempotence retains the saved override"),Updated[1].Sound,777);
    auto Custom=Marker(1536,INDEX_NONE,FVector(2000,2000,0));Custom.MarkerType=0;Updated.Add(Custom);
    TArray<FWadAmbientEmitter> WithCustom;
    if(!MergeWadAmbientEmitters(Updated,Expected,WithCustom,Output)){return false;}
    TestEqual(TEXT("Unbound custom emitters remain independent"),WithCustom.Num(),3);
    TestEqual(TEXT("The custom emitter's chosen sample is retained"),WithCustom.Last().Sound,3018);
    auto Duplicate=WithCustom;Duplicate.Add(WithCustom[0]);
    TestFalse(TEXT("Duplicate saved source bindings are refused before any output mutation"),MergeWadAmbientEmitters(Duplicate,Expected,WithCustom,Output));
    TestEqual(TEXT("Refused duplicate binding leaves the prior result unchanged"),WithCustom.Num(),3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAmbientSpatialGroups,"AAOriginal.WAD.Audio.AmbientSpatialGroups",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAmbientSpatialGroups::RunTest(const FString& Parameters)
{
    auto Soft=Marker(1511,0,FVector(0,0,10000),7),Hard=Marker(1512,1,FVector(1000,0,-10000),7);
    TestEqual(TEXT("Soft wind peak is the original integer128"),WadAmbientVolume(Soft,FVector2D::ZeroVector),128);
    TestEqual(TEXT("Integer horizontal distance controls linear falloff"),WadAmbientVolume(Soft,FVector2D(250,0)),64);
    TestEqual(TEXT("Loop volume is zero at the exact source radius"),WadAmbientVolume(Soft,FVector2D(500,0)),0);
    auto Point=Marker(1509,2);
    TestEqual(TEXT("Source3-4-5 radial distance and integer division are retained"),WadAmbientVolume(Point,FVector2D(3,4)),252);
    TestEqual(TEXT("Negative fractional listener positions use the source signed-integer coordinate"),WadAmbientVolume(Point,FVector2D(-0.5,0)),254);
    TArray<FWadAmbientEmitter> E{Soft,Hard};auto Voices=CalculateWadAmbientVoices(E,FVector2D(1000,0));
    TestEqual(TEXT("Grouped members create one source voice"),Voices.Num(),1);
    TestTrue(TEXT("The first source member remains the voice/pan owner even when another is loudest"),Voices.Contains(0)&&Voices[0]==255);
    Voices=CalculateWadAmbientVoices(E,FVector2D(0,0));
    TestTrue(TEXT("Moving back updates the same owner and its maximum member volume"),Voices.Num()==1&&Voices.Contains(0)&&Voices[0]==128);
    Voices=CalculateWadAmbientVoices(E,FVector2D(5000,0));TestTrue(TEXT("All members out of range stop the group voice"),Voices.IsEmpty());
    E={Point,Point};Voices=CalculateWadAmbientVoices(E,FVector2D::ZeroVector);
    TestEqual(TEXT("Group zero means independent voices, not a shared global group"),Voices.Num(),2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAmbientMutedPlayback,"AAOriginal.WAD.Audio.AmbientMutedPlayback",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAmbientMutedPlayback::RunTest(const FString& Parameters)
{
    const float Master=FApp::GetVolumeMultiplier();FApp::SetVolumeMultiplier(0.0f);
    ON_SCOPE_EXIT{FApp::SetVolumeMultiplier(Master);};
    UGameInstance* Instance=NewObject<UGameInstance>(GEngine);Instance->InitializeStandalone();UWorld* World=Instance->GetWorld();
    ON_SCOPE_EXIT{Instance->Shutdown();if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}};
    if(!World){return false;}
    auto* Level=World->SpawnActor<ANativeWadLevel>();auto* Listener=World->SpawnActor<ANativeWadPlayer>();auto* Controller=World->SpawnActor<APlayerController>();
    if(!Level||!Listener||!Controller){return false;}World->AddController(Controller);Controller->Possess(Listener);
    const auto* EmittersProperty=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("AmbientEmitters"));
    const auto* SamplesProperty=FindFProperty<FMapProperty>(ANativeWadLevel::StaticClass(),TEXT("AmbientSamples"));
    if(!EmittersProperty||!SamplesProperty){return false;}
    auto& Emitters=*EmittersProperty->ContainerPtrToValuePtr<TArray<FWadAmbientEmitter>>(Level);
    auto& Samples=*SamplesProperty->ContainerPtrToValuePtr<TMap<int32,FWadSoundSample>>(Level);
    Emitters.Add(Marker(1509,10));FWadSoundSample PCM;if(!SourceSample(3008,PCM)){return false;}Samples.Add(3008,PCM);
    const FString Description=Level->DescribeWadAmbientAudio();
    TestTrue(TEXT("Readonly audio metadata includes exact source identity and real sample bytes"),Description.Contains(TEXT("source_thing"))&&Description.Contains(TEXT("sha1"))&&Description.Contains(TEXT("22502")));
    Level->DispatchBeginPlay();
    if(!World->GetAudioDevice()){AddInfo(TEXT("No audio device: source metadata/group/migration tests ran; live muted playback requires the rendered audio review."));return true;}
    Level->Tick(0.11f);
    TestEqual(TEXT("Actual muted fireplace loop is playing through the normal ambient tick"),Level->CountPlayingAmbientEmitters(3008),1);
    TestTrue(TEXT("The real procedural source is marked as an infinite loop"),Level->DescribeWadAmbientAudio().Replace(TEXT(" "),TEXT("")).Contains(TEXT("\"infinite_duration\":true")));
    Listener->SetActorLocation(FVector(10000,0,0));Level->Tick(0.11f);
    TestEqual(TEXT("Leaving the source horizontal radius stops the actual loop"),Level->CountPlayingAmbientEmitters(3008),0);
    Listener->SetActorLocation(FVector::ZeroVector);Level->Tick(0.11f);
    TestEqual(TEXT("Reentering starts the same source loop again"),Level->CountPlayingAmbientEmitters(3008),1);
    const auto* PlayingProperty=FindFProperty<FMapProperty>(ANativeWadLevel::StaticClass(),TEXT("PlayingEmitters"));
    UAudioComponent* ActualVoice=PlayingProperty?PlayingProperty->ContainerPtrToValuePtr<TMap<int32,TObjectPtr<UAudioComponent>>>(Level)->FindRef(0).Get():nullptr;
    TestNotNull(TEXT("The real ambient voice is owned by the source leader"),ActualVoice);
    Level->EndPlay(EEndPlayReason::LevelTransition);
    TestTrue(TEXT("Level transition stops the actual audio component, not just its tracking record"),!IsValid(ActualVoice)||!ActualVoice->IsPlaying());
    TestEqual(TEXT("Actual level-transition lifecycle releases ambient voices"),Level->CountPlayingAmbientEmitters(3008),0);
    TestEqual(TEXT("Automated playback kept the application's master gain muted"),FApp::GetVolumeMultiplier(),0.0f);
    TestTrue(TEXT("Runtime playback never overwrites original saved PCM"),Samples.FindChecked(3008).UnsignedSamples==PCM.UnsignedSamples);
    return true;
}
