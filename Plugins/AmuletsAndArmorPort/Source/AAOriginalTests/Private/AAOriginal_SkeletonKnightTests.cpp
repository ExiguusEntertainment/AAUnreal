#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInterface.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadRules.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    FString OriginalRoot()
    { return FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor"); }
    bool SkeletonRecord(TArray<FString>& Fields)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text,*(OriginalRoot()/TEXT("Include/CREDATA.H")))) { return false; }
        const FRegexPattern Pattern(TEXT("Skeleton Knight \\(no-electricity\\) \\*/\\s*([^}]+)"));
        FRegexMatcher Match(Pattern,Text);
        if (!Match.FindNext()) { return false; }
        Match.GetCaptureGroup(1).TrimStartAndEnd().ParseIntoArray(Fields,TEXT(","),true);
        for (FString& Field:Fields) { Field.TrimStartAndEndInline(); }
        return Fields.Num()==43;
    }
    bool NativeFloor(ANativeWadLevel& Level)
    {
        const FArrayProperty* Property=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
        if (!Property) { return false; }
        auto& Sectors=*Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(&Level);
        auto& Sector=Sectors.AddDefaulted_GetRef();
        Sector.RepairedBoundaryPoints={{-500,-500},{500,-500},{500,-500},{500,500},{500,500},{-500,500},{-500,500},{-500,-500}};
        Sector.Floor=NewObject<UStaticMeshComponent>(&Level);
        Sector.Ceiling=NewObject<UStaticMeshComponent>(&Level);
        Sector.Floor->SetupAttachment(Level.GetRootComponent());
        Sector.Ceiling->SetupAttachment(Level.GetRootComponent());
        Sector.Ceiling->SetRelativeLocation(FVector(0,0,256));
        Sector.Floor->RegisterComponent();Sector.Ceiling->RegisterComponent();
        Level.SetSectorFloorZ(0,0);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSkeletonKnightSource,"AAOriginal.WAD.Gameplay.SkeletonKnightSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalSkeletonKnightSource::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    TArray<FString> Fields;
    if (!TestTrue(TEXT("Read the original nonelectric Skeleton Knight definition"),SkeletonRecord(Fields))) { return false; }
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if (!World) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Knight=World->SpawnActor<ANativeWadGuardian>();
    if (!Knight) { return false; }
    FWadEncounterProfile Seed;
    Seed.SourceType=1033;
    Knight->Configure(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,Seed);
    const FWadEncounterProfile& Profile=Knight->GetEncounterProfile();
    TestEqual(TEXT("Exact source identity"),Profile.SourceType,FCString::Atoi(*Fields[0]));
    TestEqual(TEXT("Original grounded berserker family"),Fields[1],FString(TEXT("NAV_LOGIC_PKG_BERSERKER_A")));
    TestTrue(TEXT("The original canFly field is false and native remains grounded"),Fields[3]==TEXT("FALSE")&&!Profile.bCanFly);
    TestEqual(TEXT("Original update ticks"),Profile.UpdateTicks,FCString::Atoi(*Fields[7]));
    TestEqual(TEXT("Original step size and update interval set the walk cap"),Profile.WalkSpeed,
        float(FCString::Atoi(*Fields[8])*70/(Profile.UpdateTicks+(Profile.UpdateTicks>>1))));
    TestEqual(TEXT("Original melee range"),Profile.MeleeRange,float(FCString::Atoi(*Fields[9])));
    TestEqual(TEXT("Original melee delay"),Profile.MeleeDelayUpdates,FCString::Atoi(*Fields[12]));
    TestEqual(TEXT("Original health"),Profile.MaxHealth,FCString::Atoi(*Fields[14]));
    TestEqual(TEXT("Original regeneration"),Profile.RegenPerUpdate,FCString::Atoi(*Fields[15]));
    TestEqual(TEXT("This family has no source missile"),Profile.SourceMissileType,FCString::Atoi(*Fields[16]));
    TestEqual(TEXT("Original melee damage"),Profile.MeleeDamage,FCString::Atoi(*Fields[18]));
    TestTrue(TEXT("Source specifically resists poison"),Fields[20].Contains(TEXT("EFFECT_DAMAGE_POISON"))
        && Profile.ResistanceMask==DamageFlag(EProjectileDamage::Poison));
    TestEqual(TEXT("Original low-health retreat threshold"),Profile.RetreatHealth,FCString::Atoi(*Fields[21]));
    TestEqual(TEXT("Source default vision90 becomes a half-angle45"),Profile.VisionHalfAngle,45.0f);
    TestEqual(TEXT("Original creature-combat acceptance"),Profile.ChanceAttackCreature,FCString::Atoi(*Fields[38]));
    TestEqual(TEXT("Original creature-combat continuation"),Profile.ChanceContinueAttack,FCString::Atoi(*Fields[39]));
    TestTrue(TEXT("The source uses normal corpse decay, not a dipper sink"),Fields[35]==TEXT("DEATH_LOGIC_NORMAL_DECAY")&&!Profile.bDipper);
    TestEqual(TEXT("Source default treasure is none"),Fields[23],FString(TEXT("CRELOGIC_DFT_TREASURE")));
    TestEqual(TEXT("No ordinary treasure is invented"),ResolveWadCreatureTreasure(1033,1).ItemType,0);
    TArray<FString> Info;
    if (!FFileHelper::LoadFileToStringArray(Info,*(OriginalRoot()/TEXT("Resources/ArtFinal/OBJS/SKELKNT/SKELKNT.INF")))) { return false; }
    TestEqual(TEXT("Actual object definition supplies radius15"),int32(Profile.CollisionRadius),FCString::Atoi(*Info[2]));
    const TCHAR* Pictures[]={TEXT("SWNW01.CPC"),TEXT("SDNW01.CPC"),TEXT("SDNW04.CPC"),TEXT("2SA001.CPC")};
    for (const TCHAR* Name:Pictures)
    {
        TArray<uint8> Raw;
        if (!FFileHelper::LoadFileToArray(Raw,*(OriginalRoot()/TEXT("Resources/ArtFinal/OBJS/SKELKNT")/Name))||Raw.Num()<4) { return false; }
        const int32 Height=Raw[0]|(int32(Raw[1])<<8);
        TestEqual(TEXT("The actual standing/hurt/death/attack sprites preserve height77"),Height,77);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSkeletonKnightLifecycle,"AAOriginal.WAD.Gameplay.SkeletonKnightLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalSkeletonKnightLifecycle::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if (!World) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Level=World->SpawnActor<ANativeWadLevel>();
    auto* Knight=World->SpawnActor<ANativeWadGuardian>(FVector(0,0,40.5),FRotator::ZeroRotator);
    if (!Level||!Knight||!NativeFloor(*Level)) { return false; }
    const FName Tag(TEXT("WAD Creature Thing 65"));
    Knight->Tags.Add(Tag);
    FWadEncounterProfile Profile;
    Profile.Name=TEXT("Skeleton Knight");
    Profile.SourceType=1033;
    Profile.SpriteSize=FVector2D(76,77);
    Profile.CollisionHalfHeight=38.5f;
    auto* Plane=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane"));
    auto* Standing=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Maps/L34_Assets/Things/M_SKELKNT.M_SKELKNT"));
    if (!TestNotNull(TEXT("Existing original L34 Skeleton Knight material is reusable"),Standing)||!Plane) { return false; }
    const FTransform Before=Knight->GetActorTransform();
    const FString Name=Knight->GetPathName();
    Knight->Configure(Plane,Standing,Standing,Standing,Standing,nullptr,Profile);
    Knight->RestoreEncounterHealth(2500);
    TestEqual(TEXT("Configured family keeps source1033 rather than generic Elf rules"),Knight->GetSourceType(),1033);
    TestEqual(TEXT("Look uses the original Skeleton description"),Knight->GetLookSourceType(),1033);
    TestTrue(TEXT("Reconfiguration keeps actor identity/tag/transform"),Knight->GetPathName()==Name&&Knight->ActorHasTag(Tag)
        &&Knight->GetActorTransform().Equals(Before));
    TestEqual(TEXT("Actual source radius"),Knight->GetCapsuleComponent()->GetUnscaledCapsuleRadius(),15.0f);
    TestEqual(TEXT("Actual source walk speed"),Knight->GetCharacterMovement()->MaxWalkSpeed,70.0f);
    TestFalse(TEXT("The Skeleton Knight cannot fly"),Knight->GetCharacterMovement()->IsFlying());
    TestFalse(TEXT("Pure poison is refused by the actual source resistance"),Knight->TakeHit(300,nullptr,DamageFlag(EProjectileDamage::Poison)));
    TestEqual(TEXT("Poison resistance preserves full health"),Knight->GetEncounterHealth(),2500);
    TestEqual(TEXT("Poison resistance prevents the poison status"),Knight->GetPoisonLevel(),0);
    TestTrue(TEXT("Normal damage is accepted"),Knight->TakeHit(100));
    TestEqual(TEXT("Normal damage is not converted to immunity"),Knight->GetEncounterHealth(),2400);
    Knight->Tick(10.0f/70.0f+0.00001f);
    TestEqual(TEXT("An actual source update regenerates exactly one health"),Knight->GetEncounterHealth(),2401);
    TestEqual(TEXT("The real regeneration counter advances"),Knight->GetRegeneratedHealth(),1);
    FWadLevelCheckpoint Alive;
    if (!Level->WriteCheckpoint(Alive)) { return false; }
    Knight->TakeHit(1);
    if (!Level->RestoreCheckpoint(Alive)) { return false; }
    TestEqual(TEXT("An alive source knight restores its exact health"),Knight->GetEncounterHealth(),2401);
    TestEqual(TEXT("Restore keeps source identity"),Knight->GetSourceType(),1033);
    TestTrue(TEXT("Normal damage can defeat this source family"),Knight->TakeHit(5000));
    TestTrue(TEXT("Defeat is real and handles the no-treasure life"),Knight->IsDefeated()&&Knight->HasHandledDefeatTreasure());
    int32 Drops=0;
    for (TActorIterator<ANativeWadPickup> It(World);It;++It) { Drops+=It->IsDropped()?1:0; }
    TestEqual(TEXT("No source ordinary treasure is invented on death"),Drops,0);
    FWadLevelCheckpoint Dead;
    if (!Level->WriteCheckpoint(Dead)) { return false; }
    Knight->ResetForGenerator();
    if (!Level->RestoreCheckpoint(Dead)) { return false; }
    TestTrue(TEXT("A reopened dead life stays defeated and cannot drop treasure"),Knight->IsDefeated()&&Knight->HasHandledDefeatTreasure());
    const int32 Life=Knight->GetCreatureLife();
    Knight->ResetForGenerator();
    Knight->SetSpawned(true);
    TestTrue(TEXT("Generator reuse starts a new living source life"),Knight->GetCreatureLife()>Life&&!Knight->IsDefeated());
    TestEqual(TEXT("Reuse restores original2500 health"),Knight->GetEncounterHealth(),2500);
    TestEqual(TEXT("Reuse keeps source walk speed"),Knight->GetCharacterMovement()->MaxWalkSpeed,70.0f);
    TestFalse(TEXT("Reuse keeps the source poison resistance"),Knight->TakeHit(300,nullptr,DamageFlag(EProjectileDamage::Poison)));
    TestTrue(TEXT("Original tags/checkpoint identity remain stable across reuse"),Knight->ActorHasTag(Tag)&&Knight->GetPathName()==Name);
    return true;
}
