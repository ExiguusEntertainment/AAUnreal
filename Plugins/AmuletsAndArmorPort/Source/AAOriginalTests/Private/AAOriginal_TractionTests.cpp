#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"

namespace
{
    struct FTractionFixture
    {
        UGameInstance* Instance = nullptr;
        UWorld* World = nullptr;
        ANativeWadLevel* Level = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;
        UStaticMeshComponent* Floor = nullptr;
        TArray<FAAWadNativeSector>* Sectors = nullptr;
        explicit FTractionFixture(int32 Friction = 2, int32 Class = NativeWadRules::CitizenClass,
            const TCHAR* SavedOptions = nullptr, bool bFlow = false)
        {
            Instance = NewObject<UGameInstance>(GEngine);
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            FString Options = SavedOptions ? FString(SavedOptions) : FString(TEXT("/Game/Maps/L30?WadCreated=1?WadRuneRules=1?WadLevel=100")
                TEXT("?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=30000")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=2000?WadWater=2000?WadReady=-1"));
            World->URL = FURL(nullptr, *Options, TRAVEL_Absolute);
            if (!SavedOptions)
            {
                World->URL.AddOption(*FString::Printf(TEXT("WadClass=%d"), Class));
                FString Items;
                for (int32 Rune = 1; Rune <= 9; ++Rune)
                { if (Rune != 1) { Items += TEXT(","); } Items += FString::FromInt(NativeWadRules::RuneItemForClass(Class, Rune)); }
                World->URL.AddOption(*(TEXT("WadItems=") + Items));
            }
            Level = World->SpawnActor<ANativeWadLevel>();
            const FArrayProperty* Property = FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(), TEXT("Sectors"));
            if (!Level || !Property) { return; }
            Sectors = Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
            FAAWadNativeSector& Sector = Sectors->AddDefaulted_GetRef();
            Sector.SourceFriction = Friction;
            Sector.RepairedBoundaryPoints = {{-2000,-2000},{2000,-2000},{2000,-2000},{2000,2000},
                {2000,2000},{-2000,2000},{-2000,2000},{-2000,-2000}};
            const auto Surface = [&](const TCHAR* Name)
            {
                auto* Component = NewObject<UStaticMeshComponent>(Level, Name);
                Component->SetMobility(EComponentMobility::Movable);
                Component->SetupAttachment(Level->GetRootComponent());
                return Component;
            };
            Floor = Surface(TEXT("Sector_0_Floor"));
            Floor->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
            Floor->SetRelativeScale3D(FVector(40,40,0.02));
            Floor->SetRelativeLocation(FVector(0,0,-1));
            Floor->SetCollisionProfileName(TEXT("BlockAll"));
            Floor->RegisterComponent();
            Sector.Floor = Floor;
            Sector.Ceiling = Surface(TEXT("Sector_0_Ceiling"));
            Sector.Ceiling->SetRelativeLocation(FVector(0,0,2000));
            Sector.Ceiling->RegisterComponent();
            Level->SetSectorFloorZ(0,-1); // Initialize saved native topology for footprint fallback.
            if (bFlow)
            {
                Sector.FlowImpulse = FVector2D(2,0);
                const FArrayProperty* Flows = FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(), TEXT("FlowSectors"));
                if (!Flows) { return; }
                Flows->ContainerPtrToValuePtr<TArray<int32>>(Level)->Add(0);
            }
            Player = World->SpawnActor<ANativeWadPlayer>(FVector(0,0,NativeWadRules::SourceNormalFriction + ANativeWadPlayer::CapsuleHalfHeight), FRotator::ZeroRotator);
            auto* Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
            for (int32 Frame = 0; Frame < 120; ++Frame) { Tick(); }
        }
        ~FTractionFixture()
        {
            if (Instance) { Instance->Shutdown(); }
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Valid() const { return Player && Input && Floor && Floor->GetStaticMesh() && Player->GetCharacterMovement()->IsMovingOnGround(); }
        void Tick(float InputForward = 0.0f) const
        {
            constexpr float Delta = 1.0f/240.0f;
            Player->AddMovementInput(FVector::ForwardVector, InputForward);
            Player->Tick(Delta);
            Player->GetCharacterMovement()->TickComponent(Delta, LEVELTICK_All, nullptr);
        }
        bool Press(FKey Key) const
        {
            for (const auto& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }
        bool Cast(const TCHAR* Runes) const
        {
            const int32 Count = Player->GetRunes().Len();
            for (int32 Index = 0; Index < Count; ++Index) { if (!Press(EKeys::BackSpace)) { return false; } }
            const FKey Keys[] = {EKeys::NumPadOne,EKeys::NumPadTwo,EKeys::NumPadThree,EKeys::NumPadFour,
                EKeys::NumPadFive,EKeys::NumPadSix,EKeys::NumPadSeven,EKeys::NumPadEight,EKeys::NumPadNine};
            for (const TCHAR* Digit = Runes; *Digit; ++Digit)
            { if (*Digit < TEXT('1') || *Digit > TEXT('9') || !Press(Keys[*Digit-TEXT('1')])) { return false; } }
            return Press(EKeys::RightMouseButton);
        }
        double Coast(double Seconds) const
        {
            auto* Movement = Player->GetCharacterMovement();
            Movement->StopMovementImmediately();
            Movement->Velocity = FVector(120,0,0);
            const double Start = Player->GetActorLocation().X;
            for (int32 Frame = 0; Frame < FMath::RoundToInt(Seconds*240); ++Frame) { Tick(); }
            return Player->GetActorLocation().X-Start;
        }
        bool WearRing(int32 Location) const
        {
            if (!Player->CollectItem(432) || !Player->TakeInventoryItem(Player->GetCarriedItems().Num()-1)) { return false; }
            Player->ClickJewelrySlot(Location);
            return Player->GetMouseHandItem()==0 && Player->GetJewelryInventorySlot(Location)!=INDEX_NONE;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalTerrainFrictionLaw,"AAOriginal.WAD.Gameplay.TerrainFrictionLaw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalTerrainFrictionLaw::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    TestEqual(TEXT("Old saved maps default to original normal friction"), FAAWadNativeSector().SourceFriction,12);
    for (int32 F : {2,12,14,16,24})
    {
        TestTrue(TEXT("Continuous extension retains the source's unrounded one-tick drag"),
            FMath::IsNearlyEqual(SourceGroundRetention(F,1.0/70.0),1.0-F/256.0,1.e-12));
        TestTrue(TEXT("Substeps compose without a frame-rate-dependent drag"),FMath::IsNearlyEqual(
            SourceGroundRetention(F,0.5),FMath::Square(SourceGroundRetention(F,0.25)),1.e-12));
    }
    FTractionFixture Normal(12),Ice(2),Mud(14),Lava(24);
    if (!Normal.Valid() || !Ice.Valid() || !Mud.Valid() || !Lava.Valid()) { AddError(TEXT("Real native floor fixtures must settle")); return false; }
    const double NormalDistance=Normal.Coast(1),IceDistance=Ice.Coast(1),MudDistance=Mud.Coast(1),LavaDistance=Lava.Coast(1);
    TestTrue(TEXT("Actual source ice coasts farther than normal walking"),IceDistance>NormalDistance*2.0);
    TestTrue(TEXT("Mud and lava stop more quickly through the real movement component"),MudDistance<NormalDistance && LavaDistance<MudDistance);
    TestTrue(TEXT("Actual grounded velocity follows the source normal drag within tiny-speed UE cutoff"),
        FMath::Abs(Normal.Player->GetVelocity().X-120*SourceGroundRetention(12,1))<1.5);
    TestEqual(TEXT("Ground friction resolves actual supporting floor identity"),Ice.Level->GetSupportingWadFriction(Ice.Floor,FVector(5000,5000,0)),2);
    TestEqual(TEXT("Unknown collision uses saved center-footprint fallback"),Ice.Level->GetSupportingWadFriction(nullptr,FVector::ZeroVector),2);
    TestEqual(TEXT("Outside-footprint fallback is normal"),Ice.Level->GetSupportingWadFriction(nullptr,FVector(5000,5000,0)),12);
    Normal.Player->GetCharacterMovement()->StopMovementImmediately();
    Ice.Player->GetCharacterMovement()->StopMovementImmediately();
    for (int32 Frame=0;Frame<12;++Frame) { Normal.Tick(1); Ice.Tick(1); }
    TestTrue(TEXT("Ice also reduces actual input acceleration rather than only braking"),Ice.Player->GetVelocity().X<Normal.Player->GetVelocity().X*0.3);
    for (int32 Frame=0;Frame<1000;++Frame) { Ice.Tick(1); }
    TestTrue(TEXT("The existing native walk speed cap is retained"),Ice.Player->GetVelocity().Size2D()<=Ice.Player->GetCharacterMovement()->MaxWalkSpeed+0.1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalTractionRuneSpells,"AAOriginal.WAD.Gameplay.TractionRuneSpells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalTractionRuneSpells::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (int32 Definition : {47,77})
    {
        const auto* Spell=Algo::FindByPredicate(SupportedSpells,[&](const auto& S){return S.SourceDefinition==Definition;});
        if (!TestNotNull(TEXT("Both original Cast of Sand definitions are implemented"),Spell)) { return false; }
        TArray<FString> Raw,Lines;
        if (!FFileHelper::LoadFileToStringArray(Raw,*(FPaths::ProjectDir()/FString::Printf(
            TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC/SPL%05d.TXT"),Definition)))) { return false; }
        for (const FString& Line:Raw) { const FString V=Line.TrimStartAndEnd();if (!V.IsEmpty()&&!V.StartsWith(TEXT("#"))) { Lines.Add(V); } }
        const auto Field=[&](const TCHAR* Name){const int32 I=Lines.Find(Name);return Lines.IsValidIndex(I+1)&&I!=INDEX_NONE?Lines[I+1]:FString();};
        FString Runes=Field(TEXT("ACCESS CODE"));Runes.ReplaceInline(TEXT(","),TEXT(""));Runes.RemoveFromEnd(TEXT("0"));
        TestEqual(TEXT("Native runes match actual source"),FString(Spell->Runes),Runes);
        TestEqual(TEXT("Native cost matches actual source"),Spell->Mana,FCString::Atoi(*Field(TEXT("SPELL COST"))));
        TestEqual(TEXT("Native hardness matches actual source"),Spell->Hardness,FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
        TestEqual(TEXT("Native duration matches actual source"),Spell->DurationTicks,FCString::Atoi(*Field(TEXT("SPELL DURATION"))));
        TestEqual(TEXT("Source subtype is STICKY FEET"),Field(TEXT("EFFECT SUBTYPE")),FString(TEXT("STICKY FEET")));
        TestEqual(TEXT("Source power is one"),Spell->Power,1);
        TestEqual(TEXT("No source cost scaling was omitted"),FCString::Atoi(*Field(TEXT("SPELL COST MOD"))),0);
        TestEqual(TEXT("No source power scaling was omitted"),FCString::Atoi(*Field(TEXT("SPELL POWER MOD"))),0);
        FTractionFixture Fixture(2,Definition==47?CitizenClass:PriestClass);
        if (!Fixture.Valid() || !Fixture.Cast(Spell->Runes)) { AddError(TEXT("Normal owned-rune input must cast traction"));return false; }
        auto* Player=Fixture.Player;
        const FString Owner=FString::Printf(TEXT("spell:%d"),Definition);
        TestEqual(TEXT("Normal rune cast charges exact mana"),Player->GetMana(),29000);
        TestEqual(TEXT("The original source definition owns thirty seconds"),Player->GetOwnedEffectRemaining(Owner),30.0);
        TestEqual(TEXT("Traction forces actual supporting ice friction to twelve"),Player->GetWadGroundFriction(Fixture.Floor),12);
        const double TractionDistance=Fixture.Coast(1);
        TestTrue(TEXT("The spell actually shortens physical ice coasting"),TractionDistance<45.0);
        Fixture.Cast(Spell->Runes);
        TestEqual(TEXT("Recasting refreshes the same owner without duplication"),Player->GetOwnedEffectCount(),1);
        TestEqual(TEXT("Recasting restores exactly thirty seconds"),Player->GetOwnedEffectRemaining(Owner),30.0);
        Player->Tick(30.1f);
        TestEqual(TEXT("Expiry restores source ice friction"),Player->GetWadGroundFriction(Fixture.Floor),2);
        const double IceDistance=Fixture.Coast(1);
        TestTrue(TEXT("Actual sliding resumes after owner expiry"),IceDistance>TractionDistance*2);
        Player->RestoreWadVital(10,1000-Player->GetMana());
        const int32 Successful=Player->GetSuccessfulCasts();
        Fixture.Cast(Spell->Runes);
        TestEqual(TEXT("Mana equal to the source cost refuses traction"),Player->GetMana(),1000);
        TestEqual(TEXT("A refused cast adds no temporary owner"),Player->GetOwnedEffectCount(),0);
        TestEqual(TEXT("A refused cast does not increment success"),Player->GetSuccessfulCasts(),Successful);
        TestEqual(TEXT("A refused cast leaves actual ice friction active"),Player->GetWadGroundFriction(Fixture.Floor),2);
    }
    const FString TwoOwners=FString::Printf(TEXT("/Game/Maps/L30?WadClass=0?WadCreated=1?WadOwnedEffects=1~spell:47,0,%d,1,1~spell:77,0,%d,1,3"),int32(StickyFeetStatus),int32(StickyFeetStatus));
    FTractionFixture Grouped(2,CitizenClass,*TwoOwners);
    if (!Grouped.Valid()) { return false; }
    Grouped.Player->Tick(1.1f);
    TestEqual(TEXT("A second spell owner keeps normal friction after the first expires"),Grouped.Player->GetWadGroundFriction(Grouped.Floor),12);
    Grouped.Player->Tick(2.0f);
    TestEqual(TEXT("Final owner expiry restores terrain friction"),Grouped.Player->GetWadGroundFriction(Grouped.Floor),2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalTractionRingPersistence,"AAOriginal.WAD.Gameplay.TractionRingPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalTractionRingPersistence::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FTractionFixture Fixture;
    if (!Fixture.Valid() || !Fixture.WearRing(Ring1Slot)) { AddError(TEXT("Saved source ring432 catalog must import and equip"));return false; }
    auto* Player=Fixture.Player;
    TestEqual(TEXT("Original ring READY supplies sticky feet"),Player->GetStatusPower(StickyFeetStatus),1);
    TestEqual(TEXT("Worn ring432 supplies normal floor friction"),Player->GetWadGroundFriction(Fixture.Floor),12);
    TestTrue(TEXT("Normal spell owner can coexist with equipped object owner"),Fixture.Cast(TEXT("765")));
    const FURL TownURL(nullptr,TEXT("/Game/Maps/Town?game=/Script/AAOriginal.NativeWadTownGameMode"),TRAVEL_Absolute);
    if (!Fixture.World->SetGameMode(TownURL)) { return false; }
    const FString Slot=TEXT("NativeTraction-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot,0); };
    if (!TestTrue(TEXT("Save actual readied ring and temporary traction owner"),Player->SaveWadCheckpoint(Slot))) { return false; }
    USaveGame* Raw=UGameplayStatics::LoadGameFromSlot(Slot,0);
    if (!Raw) { return false; }
    const auto* Saved=static_cast<UNativeWadCheckpoint*>(Raw);
    FTractionFixture Restored(2,CitizenClass,*(Saved->LevelPackage+TEXT("?")+Saved->PlayerOptions));
    if (!Restored.Valid()) { return false; }
    TestTrue(TEXT("Reopening restores the same worn ring"),Restored.Player->GetJewelryInventorySlot(Ring1Slot)!=INDEX_NONE);
    TestEqual(TEXT("Reopening retains one temporary spell owner, not a duplicated ring owner"),Restored.Player->GetOwnedEffectCount(),1);
    TestEqual(TEXT("Reopening retains normal friction on the real icy supporting floor"),Restored.Player->GetWadGroundFriction(Restored.Floor),12);
    Restored.Player->ClickJewelrySlot(Ring1Slot);
    TestEqual(TEXT("UNREADY removes its object owner despite the source subtype typo"),Restored.Player->GetStatusPower(StickyFeetStatus),1);
    Restored.Player->Tick(31.0f);
    TestEqual(TEXT("After ring removal and final timer expiry, ice friction returns"),Restored.Player->GetWadGroundFriction(Restored.Floor),2);
    TestEqual(TEXT("Ring UNREADY has not mistakenly added or removed speed bonuses"),Restored.Player->GetStatusPower(SpeedStatus),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalTractionFlowAndAir,"AAOriginal.WAD.Gameplay.TractionFlowAndAir",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalTractionFlowAndAir::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FTractionFixture Current(2,CitizenClass,nullptr,true);
    if (!Current.Valid() || !Current.WearRing(Ring1Slot)) { return false; }
    Current.Player->GetCharacterMovement()->StopMovementImmediately();
    const FVector Start=Current.Player->GetActorLocation();
    TestTrue(TEXT("Traction leaves authored current input active"),Current.Level->GetCurrentInput(Start).X>0);
    for (int32 Frame=0;Frame<240;++Frame) { Current.Tick(); }
    TestTrue(TEXT("A real player wearing traction still moves with the current"),Current.Player->GetActorLocation().X>Start.X+3);
    FTractionFixture Normal(12),Ice(2);
    if (!Normal.Valid() || !Ice.Valid()) { return false; }
    for (auto* F : {&Normal,&Ice})
    {
        F->Player->SetActorLocation(FVector(0,0,1000),false,nullptr,ETeleportType::TeleportPhysics);
        F->Player->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        F->Player->GetCharacterMovement()->Velocity=FVector(120,0,0);
        for (int32 Frame=0;Frame<24;++Frame) { F->Tick(); }
    }
    TestTrue(TEXT("Ground terrain does not change airborne horizontal movement"),FMath::IsNearlyEqual(Normal.Player->GetVelocity().X,Ice.Player->GetVelocity().X,0.01));
    for (auto* F : {&Normal,&Ice})
    {
        F->Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
        // Invoke actual component physics directly; no effect timer toggles this comparison back to falling.
        F->Player->GetCharacterMovement()->Velocity=FVector(120,0,0);
        for (int32 Frame=0;Frame<24;++Frame) { F->Player->GetCharacterMovement()->TickComponent(1.0f/240.0f,LEVELTICK_All,nullptr); }
    }
    TestTrue(TEXT("Ground terrain does not change existing flying horizontal physics"),FMath::IsNearlyEqual(Normal.Player->GetVelocity().X,Ice.Player->GetVelocity().X,0.01));
    return true;
}
