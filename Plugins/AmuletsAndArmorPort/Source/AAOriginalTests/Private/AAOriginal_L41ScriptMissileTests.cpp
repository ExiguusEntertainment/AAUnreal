#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "NativeWadScript.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    bool ReadTraps(TArray<FWadScriptActivity>& Out)
    {
        const int32 Entries[] = {11, 20};
        return ImportWadScriptActivities(FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/S41.SRP"),
            Entries, 10000, 10000, Out, *GLog);
    }
    const FWadScriptActivity* Find(const TArray<FWadScriptActivity>& A, int32 Id)
    { return A.FindByPredicate([Id](const auto& Entry) { return Entry.Id == Id; }); }
    TArray<AActor*> Bolts(UWorld* World)
    {
        TArray<AActor*> Out;
        for (TActorIterator<AActor> It(World); It; ++It)
        { if (It->GetClass()->GetFName() == TEXT("NativeWadBolt") && !It->IsActorBeingDestroyed()) { Out.Add(*It); } }
        return Out;
    }
    struct FTrapFixture
    {
        UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
        UWorld* World = nullptr;
        ANativeWadLevel* Level = nullptr;
        ANativeWadPlayer* Player = nullptr;
        double Volume = FApp::GetVolumeMultiplier();
        mutable uint64 TestFrame = GFrameCounter + 1;
        explicit FTrapFixture(const TArray<FWadScriptActivity>& Activities)
        {
            FApp::SetVolumeMultiplier(0.0); // No persistent user Options changes.
            Instance->InitializeStandalone(); World = Instance->GetWorld();
            if (!World) { return; }
            World->URL = FURL(nullptr, TEXT("/Game/Maps/L41?WadCreated=1?WadClass=0?WadLevel=1?WadHealth=30000?WadMaxHealth=30000?WadMana=30000?WadMaxMana=30000?WadFood=2000?WadWater=2000?WadItems=?WadReady=-1"), TRAVEL_Absolute);
            Level = World->SpawnActor<ANativeWadLevel>();
            const auto* Property = FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(), TEXT("ImportedActivities"));
            if (!Level || !Property) { return; }
            *Property->ContainerPtrToValuePtr<TArray<FWadScriptActivity>>(Level) = Activities;
            Player = World->SpawnActor<ANativeWadPlayer>(FVector(-10000, -10000, 100), FRotator::ZeroRotator);
            auto* Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller); Controller->Possess(Player); Player->DispatchBeginPlay();
            // InitializeStandalone's isolated world has not activated movement
            // components as a normally begun gameplay world does.
            Player->GetCharacterMovement()->Activate(true);
        }
        ~FTrapFixture()
        {
            Instance->Shutdown();
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
            FApp::SetVolumeMultiplier(Volume);
        }
        void Advance(float Seconds) const
        {
            // TimerManager deliberately accepts one tick per engine frame. Give
            // these synchronous local-world ticks distinct scoped frame IDs.
            { TGuardValue<uint64> Frame(GFrameCounter,++TestFrame); World->GetTimerManager().Tick(0.0f); }
            { TGuardValue<uint64> Frame(GFrameCounter,++TestFrame); World->GetTimerManager().Tick(Seconds); }
        }
        void DiscardBolts() const { for (auto* Bolt : Bolts(World)) { Bolt->Destroy(); } }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL41TrapSource, "AAOriginal.WAD.Scripts.L41MissileSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalL41TrapSource::RunTest(const FString& Parameters)
{
    TArray<FWadScriptActivity> Activities;
    if (!TestTrue(TEXT("Decode actual original S41.SRP"), ReadTraps(Activities))) { return false; }
    const auto* A = Find(Activities, 11); const auto* B = Find(Activities, 20);
    if (!TestTrue(TEXT("Both original trap activities are present"), A && B)) { return false; }
    const int32 Types[] = {4005, 4007, 4015, 4016, 4009, 4001};
    const FIntVector Starts[] = {{4447,646,90},{4416,613,90},{4477,574,90},{4477,574,90},{4447,646,90},{4416,613,90}};
    const FIntVector Targets[] = {{4000,838,30},{4000,613,30},{4000,388,30},{4000,388,30},{4000,838,30},{4000,613,30}};
    int32 Shot = 0;
    for (const auto& C : A->Commands)
    {
        if (C.Operation != EWadScriptOperation::LaunchMissile) { continue; }
        if (!TestTrue(TEXT("No extra activity11 shots"), Shot < UE_ARRAY_COUNT(Types))) { return false; }
        TestEqual(TEXT("Source trap order"), C.Target, Types[Shot]); TestEqual(TEXT("Source launch XYZ"), C.Position, Starts[Shot]);
        TestEqual(TEXT("Source target XYZ"), C.Aim, Targets[Shot]); TestEqual(TEXT("Source speed"), C.Value, Shot < 3 ? 40 : 50); ++Shot;
    }
    TestEqual(TEXT("All six source activity11 missiles restored"), Shot, 6);
    const FIntVector PullStarts[] = {{6092,4478,-96},{6092,4478,-96},{5971,3858,-96},{6308,3868,-96}};
    const FIntVector PullTargets[] = {{5827,4181,-96},{6480,4171,-96},{6206,4175,-96},{6084,4175,-96}};
    Shot = 0;
    for (const auto& C : B->Commands)
    {
        if (C.Operation != EWadScriptOperation::LaunchMissile) { continue; }
        if (!TestTrue(TEXT("No extra Pull shots"), Shot < 4)) { return false; }
        TestEqual(TEXT("Original Pull type"), C.Target, 4002); TestEqual(TEXT("Pull launch point"), C.Position, PullStarts[Shot]);
        TestEqual(TEXT("Pull target point"), C.Aim, PullTargets[Shot]); TestEqual(TEXT("Pull speed"), C.Value, 50); ++Shot;
    }
    TestEqual(TEXT("Activity20 restores all four Pull shots"), Shot, 4);
    const FVector V = NativeWadRules::ScriptMissileVelocity(FVector(-416,0,-60),50);
    TestEqual(TEXT("SERVER.C preserves full XY speed while descending"), V.X, -3500.0);
    TestEqual(TEXT("Signed integer Z pitch, not normalized3D aim"), V.Z, -420.0);
    TestEqual(TEXT("Vertical-only target has zero pitch in the source"), NativeWadRules::ScriptMissileVelocity(FVector(0,0,100),40), FVector::ZeroVector);
    TestEqual(TEXT("Source Z pitch clamps to50"), NativeWadRules::ScriptMissileVelocity(FVector(1,0,100),40).Z, 3500.0);
    TestEqual(TEXT("Source initial speed narrows to byte"), NativeWadRules::ScriptMissileVelocity(FVector(100,0,0),296).X, 2800.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL41TrapContinuations, "AAOriginal.WAD.Scripts.L41MissileCheckpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalL41TrapContinuations::RunTest(const FString& Parameters)
{
    TArray<FWadScriptActivity> Activities;
    if (!ReadTraps(Activities)) { return false; }
    const auto* A = Find(Activities,11);
    if (!A) { return false; }
    TestEqual(TEXT("Old first wait resumes into restored Dispel"), ResolveWadScriptContinuation(*A,3,INDEX_NONE),3);
    TestEqual(TEXT("Old fourth wait resumes into restored Slow"), ResolveWadScriptContinuation(*A,8,INDEX_NONE),9);
    TestEqual(TEXT("Old final wait resumes into restored ManaDrain"), ResolveWadScriptContinuation(*A,9,INDEX_NONE),11);
    TestEqual(TEXT("Malformed legacy index refused"), ResolveWadScriptContinuation(*A,100,INDEX_NONE),INDEX_NONE);
    TestEqual(TEXT("Unknown source byte position refused"), ResolveWadScriptContinuation(*A,3,99999),INDEX_NONE);
    TArray<uint8> Bytes;
    {
        FTrapFixture F(Activities);
        if (!F.Level || !F.Player) { return false; }
        TestTrue(TEXT("Run real activity11 through the normal interpreter"), F.Level->RunWadActivity(11,F.Player));
        TestEqual(TEXT("First Fire shot launched"), F.Level->GetScriptMissileCount(4005),1);
        FWadLevelCheckpoint CP;
        TestFalse(TEXT("Ordinary save still refuses in-flight missiles"), F.Level->WriteCheckpoint(CP));
        F.DiscardBolts(); CP = {};
        TestTrue(TEXT("Checkpoint records actual pending wait"), F.Level->WriteCheckpoint(CP));
        if (!TestEqual(TEXT("Exactly one pending callback"),CP.Delays.Num(),1)) { return false; }
        TestEqual(TEXT("Pending resume points to Dispel source command"),CP.Delays[0].SourceCommandOffset,A->Commands[3].SourceOffset);
        UClass* SaveClass = FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadCheckpoint"));
        if (!TestNotNull(TEXT("Reflected native SaveGame class"),SaveClass)) { return false; }
        auto* Save = static_cast<UNativeWadCheckpoint*>(NewObject<USaveGame>(GetTransientPackage(),SaveClass)); Save->VisitedLevels.Add(TEXT("L41"),CP);
        TestTrue(TEXT("Serialize callback through actual SaveGame"),UGameplayStatics::SaveGameToMemory(Save,Bytes));
    }
    auto* Save = static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromMemory(Bytes));
    if (!TestNotNull(TEXT("Reopen serialized checkpoint"),Save)) { return false; }
    FTrapFixture F(Activities);
    if (!F.Level || !F.Player) { return false; }
    TestTrue(TEXT("Restore into a new runtime world"),F.Level->RestoreCheckpoint(Save->VisitedLevels.FindChecked(TEXT("L41"))));
    F.Advance(0.51f);
    TestEqual(TEXT("Reopened wait launches Dispel once"),F.Level->GetScriptMissileCount(4007),1);
    TestEqual(TEXT("Prior Fire is not replayed"),F.Level->GetScriptMissileCount(4005),0);
    for (int32 Index=0;Index<3;++Index) { F.Advance(0.51f); }
    F.Advance(1.01f);
    TestEqual(TEXT("Remaining original sequence launches Slow once"),F.Level->GetScriptMissileCount(4009),1);
    TestEqual(TEXT("Remaining original sequence launches ManaDrain once"),F.Level->GetScriptMissileCount(4001),1);
    F.DiscardBolts();
    FWadLevelCheckpoint Legacy = Save->VisitedLevels.FindChecked(TEXT("L41")); Legacy.Delays[0].SourceCommandOffset=INDEX_NONE;
    TestTrue(TEXT("Earlier save without new source-offset property restores"),F.Level->RestoreCheckpoint(Legacy));
    F.Advance(0.51f);
    TestEqual(TEXT("Legacy wait executes exactly its restored Dispel, without restarting"),F.Level->GetScriptMissileCount(4007),2);
    F.DiscardBolts();
    TestTrue(TEXT("Run actual activity20"),F.Level->RunWadActivity(20,F.Player)); F.Advance(0.44f);
    TestEqual(TEXT("Its delayed callback launches all four Pull missiles"),F.Level->GetScriptMissileCount(4002),4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL41TrapImpact, "AAOriginal.WAD.Scripts.L41MissileImpacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalL41TrapImpact::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    TArray<FWadScriptActivity> Original;
    if (!ReadTraps(Original)) { return false; }
    for (int32 Type : {4007,4009,4001,4002})
    {
        const auto* Activity=Find(Original,Type==4002?20:11);
        if (!Activity) { return false; }
        const auto* Command=Activity->Commands.FindByPredicate([Type](const auto& C){return C.Operation==EWadScriptOperation::LaunchMissile && C.Target==Type;});
        if (!Command) { return false; }
        FWadScriptActivity Single; Single.Id=11; Single.Commands.Add(*Command);
        FTrapFixture F({Single});
        if (!F.Player || !F.Level) { return false; }
        const FVector Start(Command->Position),Velocity=ScriptMissileVelocity(FVector(Command->Aim)-Start,Command->Value);
        F.Player->SetActorLocation(Start+Velocity.GetSafeNormal()*80.0,false,nullptr,ETeleportType::TeleportPhysics);
        F.Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
        if (Type==4007)
        {
            F.Player->ReceiveTypedHit(1,SpecialDamageFlag(ESpecialDamage::Slow));
            TestEqual(TEXT("Normal incoming Slow supplies a temporary owner before Dispel"),F.Player->GetStatusPower(SpeedStatus),-20);
        }
        const int32 Health=F.Player->GetHealth(),Mana=F.Player->GetMana();
        TestTrue(TEXT("Launch actual source command via normal activity dispatch"),F.Level->RunWadActivity(11,F.Player));
        const auto Shots=Bolts(F.World);
        if (!TestEqual(TEXT("Exactly one live source missile"),Shots.Num(),1)) { return false; }
        Shots[0]->Tick(0.05f); // Real sphere sweep hits the normal player capsule.
        TestTrue(TEXT("Impact consumes the missile"),Shots[0]->IsActorBeingDestroyed());
        if (Type==4007) { TestEqual(TEXT("Dispel impact removes its temporary effect owner"),F.Player->GetStatusPower(SpeedStatus),0); }
        if (Type==4009) { TestEqual(TEXT("Slow impact applies original negative speed power"),F.Player->GetStatusPower(SpeedStatus),-20); }
        if (Type==4001)
        {
            TestEqual(TEXT("ManaDrain uses source1000 raw mana damage"),F.Player->GetMana(),Mana-1000);
            TestEqual(TEXT("ManaDrain also follows the ordinary typed health rule"),F.Player->GetHealth(),Health-1000);
        }
        else { TestEqual(TEXT("Special control missiles do not inflict ordinary health damage"),F.Player->GetHealth(),Health); }
        if (Type==4002)
        {
            const auto* PendingProperty=FindFProperty<FStructProperty>(UCharacterMovementComponent::StaticClass(),TEXT("PendingImpulseToApply"));
            const FVector Pending=PendingProperty?*PendingProperty->ContainerPtrToValuePtr<FVector>(F.Player->GetCharacterMovement()):FVector::ZeroVector;
            TestTrue(TEXT("Real movement component is active before accepting an impulse"),F.Player->GetCharacterMovement()->IsActive());
            TestTrue(TEXT("Pull queues the source integer damage/90 impulse magnitude"),FMath::IsNearlyEqual(Pending.Size(),236.25,0.001));
            // Consume the actual impulse through the engine's force stage;
            // this isolated world has no normal movement-tick scheduling.
            F.Player->GetCharacterMovement()->ApplyAccumulatedForces(0.01f);
            TestTrue(TEXT("Pull accelerates toward the actual blast origin"),FVector::DotProduct(F.Player->GetVelocity(),-Velocity.GetSafeNormal2D())>0.0);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL41SavedMissileArt, "AAOriginal.WAD.Import.L41ScriptMissileArt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalL41SavedMissileArt::RunTest(const FString& Parameters)
{
    UWorld* World=LoadObject<UWorld>(nullptr,TEXT("/Game/Maps/L41.L41"));
    if (!TestNotNull(TEXT("Saved L41 package after targeted script/art refresh"),World)) { return false; }
    ANativeWadLevel* Level=nullptr;
    for (TActorIterator<ANativeWadLevel> It(World);It;++It) { if (Level) { return false; } Level=*It; }
    if (!Level) { return false; }
    const auto* P=FindFProperty<FMapProperty>(ANativeWadLevel::StaticClass(),TEXT("ScriptMissileMaterials"));
    if (!P) { return false; }
    const auto& Art=*P->ContainerPtrToValuePtr<TMap<int32,TObjectPtr<UMaterialInterface>>>(Level);
    for (int32 Type : {4007,4009,4001,4002})
    {
        const auto* Def=NativeWadRules::FindWandProjectileByType(Type);
        if (!TestNotNull(TEXT("Canonical projectile metadata"),Def)) { return false; }
        TestTrue(TEXT("Original CPC art exists without aliases"),FPaths::FileExists(FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/OBJS")/Def->Art));
        UMaterialInterface* Material=Art.FindRef(Type);
        if (!TestNotNull(TEXT("Each new script type has saved original art"),Material)) { return false; }
        TArray<UTexture*> Textures;
        for (const auto& Expression:Material->GetMaterial()->GetExpressionCollection().Expressions)
        { if (const auto* Sample=Cast<UMaterialExpressionTextureSample>(Expression)) { Textures.AddUnique(Sample->Texture.Get()); } }
        TestTrue(TEXT("Saved original material references real imported pixels"),Textures.ContainsByPredicate([](UTexture* Texture){ const auto* T=Cast<UTexture2D>(Texture);return T && T->Source.GetSizeX()>0 && T->Source.GetSizeY()>0; }));
    }
    return true;
}
