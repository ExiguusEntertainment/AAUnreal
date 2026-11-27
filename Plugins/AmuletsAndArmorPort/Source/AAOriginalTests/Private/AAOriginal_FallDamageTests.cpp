#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadPlayerEffects.h"
#include "NativeWadRules.h"
#include "UObject/UnrealType.h"

namespace
{
    // Use actual character movement/collision and normal bound inputs. No test
    // calls ProcessLanded or injects a fabricated impact velocity into production.
    struct FFallFixture
    {
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        APlayerController* Controller = nullptr;
        UInputComponent* Input = nullptr;
        double FloorZ = 0.0;

        explicit FFallFixture(int32 Class = NativeWadRules::CitizenClass, const TCHAR* Effects = nullptr,
            bool bWater = false)
        {
            World = UWorld::CreateWorld(EWorldType::Game, false);
            if (!World) { return; }
            // As with the existing movement regressions, this fixture needs no
            // initialized campaign subsystem or standalone world context.
            World->SetGameInstance(NewObject<UGameInstance>());
            for (const TCHAR* Option : {TEXT("WadCreated=1"), TEXT("WadRuneRules=1"), TEXT("WadLevel=100"),
                TEXT("WadAttributes=100,100,100,100,100,100"), TEXT("WadMaxHealth=30000"), TEXT("WadHealth=30000"),
                TEXT("WadMaxMana=30000"), TEXT("WadMana=30000"), TEXT("WadFood=2000"), TEXT("WadWater=2000")})
            { World->URL.AddOption(Option); }
            World->URL.AddOption(*FString::Printf(TEXT("WadClass=%d"), Class));
            FString Items;
            for (int32 Rune = 1; Rune <= 9; ++Rune)
            {
                if (Rune != 1) { Items += TEXT(","); }
                Items += FString::FromInt(NativeWadRules::RuneItemForClass(Class, Rune));
            }
            World->URL.AddOption(*(TEXT("WadItems=") + Items));
            if (Effects) { World->URL.AddOption(*(TEXT("WadOwnedEffects=") + FString(Effects))); }
            if (bWater)
            {
                ANativeWadLevel* Level = World->SpawnActor<ANativeWadLevel>();
                const FArrayProperty* Property = FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(), TEXT("Sectors"));
                if (!Level || !Property) { return; }
                auto& Sectors = *Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
                FAAWadNativeSector& Sector = Sectors.AddDefaulted_GetRef();
                Sector.bWater = true;
                Sector.FloorDepth = 32;
                Sector.RepairedBoundaryPoints = {{-1000, -1000}, {1000, -1000}, {1000, -1000}, {1000, 1000},
                    {1000, 1000}, {-1000, 1000}, {-1000, 1000}, {-1000, -1000}};
                const auto Surface = [&]()
                {
                    auto* Result = NewObject<UStaticMeshComponent>(Level);
                    Result->SetMobility(EComponentMobility::Movable);
                    Result->SetupAttachment(Level->GetRootComponent());
                    Result->RegisterComponent();
                    return Result;
                };
                Sector.Floor = Surface();
                Sector.Ceiling = Surface();
                Sector.Ceiling->SetRelativeLocation(FVector(0, 0, 2000));
                Sector.WalkingFloor = Surface();
                Sector.WalkingFloor->SetRelativeLocation(FVector(0, 0, -32));
                Level->SetSectorFloorZ(0, 0);
                FloorZ = -32.0;
                if (!Box(0.0, ANativeWadLevel::WaterSurfaceChannel)) { return; }
            }
            if (!Box(FloorZ, ECC_WorldStatic)) { return; }
            Player = World->SpawnActor<ANativeWadPlayer>(FVector(0, 0, ANativeWadPlayer::CapsuleHalfHeight + 0.2), FRotator::ZeroRotator);
            Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
            Player->GetCharacterMovement()->bRunPhysicsWithNoController = true;
        }

        ~FFallFixture() { if (World) { World->DestroyWorld(false); } }
        bool Valid() const { return World && Player && Controller && Input; }
        UBoxComponent* Box(double Top, ECollisionChannel Channel) const
        {
            AActor* Owner = World->SpawnActor<AActor>();
            if (!Owner) { return nullptr; }
            UBoxComponent* Shape = NewObject<UBoxComponent>(Owner);
            Owner->SetRootComponent(Shape);
            Shape->SetBoxExtent(FVector(1000, 1000, 2));
            Shape->SetCollisionProfileName(TEXT("BlockAll"));
            Shape->SetCollisionObjectType(Channel);
            Shape->SetWorldLocation(FVector(0, 0, Top - 2));
            Shape->RegisterComponent();
            return Shape;
        }
        bool Press(const FKey& Key) const
        {
            for (const auto& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }
        bool CastImpact() const
        {
            return Press(EKeys::NumPadEight) && Press(EKeys::NumPadOne) && Press(EKeys::RightMouseButton);
        }
        void Tick() const
        {
            constexpr float Delta = 1.0f / 240.0f;
            Player->Tick(Delta);
            Player->GetCharacterMovement()->TickComponent(Delta, LEVELTICK_All, nullptr);
        }
        bool Drop(double FeetZ) const
        {
            auto* Movement = Player->GetCharacterMovement();
            Movement->StopMovementImmediately();
            Player->SetActorLocation(FVector(0, 0, FeetZ + ANativeWadPlayer::CapsuleHalfHeight), false, nullptr, ETeleportType::TeleportPhysics);
            Movement->SetMovementMode(MOVE_Falling);
            for (int32 Frame = 0; Frame < 2400; ++Frame)
            {
                Tick();
                if (Movement->IsMovingOnGround()) { return true; }
                if (Player->GetHealth() <= 0) { return false; }
            }
            return false;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSourceFallFormula, "AAOriginal.WAD.Gameplay.SourceFallFormula",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSourceFallFormula::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    TestEqual(TEXT("Original per-class maximum safe fall velocity"), SourceMaxSafeFallVelocity, 31000);
    for (int32 Velocity : {19200, 0, -19200, -28800, -31000, -31001, -33431})
    { TestEqual(TEXT("Upward motion, ordinary jumps and the inclusive small-spill boundary are harmless"), SourceLandingDamage(Velocity), 0); }
    TestEqual(TEXT("The first nonignored quotient is 38, producing 52 normal damage"), SourceLandingDamage(-33432), 52);
    TestEqual(TEXT("An exact 4000-unit excess truncates to 62, then adds 38"), SourceLandingDamage(-35000), 100);
    TestEqual(TEXT("The squared excess makes larger falls substantially worse"), SourceLandingDamage(-50000), 1172);
    TestEqual(TEXT("Last uncapped quotient 1739"), SourceLandingDamage(-142359), 31980);
    TestEqual(TEXT("The next quotient reaches the original 32000 cap"), SourceLandingDamage(-142360), 32000);
    TestEqual(TEXT("Extreme downward velocity saturates without overflowing the square"), SourceLandingDamage(MIN_int32), 32000);
    for (int32 Velocity : {-31000, -33432, -35000, -50000})
    { TestEqual(TEXT("Source and native vertical units round-trip exactly at formula boundaries"), SourceZVelocityFromNative(SourceVerticalSpeed(Velocity)), Velocity); }
    TestEqual(TEXT("Measured minus 600 native units per second rounds to original ZV"), SourceZVelocityFromNative(-600.0), -35109);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPhysicalFallDamage, "AAOriginal.WAD.Gameplay.PhysicalFallDamage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalPhysicalFallDamage::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    int32 Previous = 0;
    for (double Height : {250.0, 500.0})
    {
        FFallFixture Fixture;
        if (!TestTrue(TEXT("Physical fall fixture exists"), Fixture.Valid())) { return false; }
        const int32 Before = Fixture.Player->GetHealth();
        if (!TestTrue(TEXT("A real gravity-driven drop lands on the collision floor"), Fixture.Drop(Height))) { return false; }
        const int32 Damage = Before - Fixture.Player->GetHealth();
        const int32 Approximate = SourceLandingDamage(SourceZVelocityFromNative(-FMath::Sqrt(2.0 * SourceGravityAcceleration(SourceNormalGravity) * Height)));
        TestTrue(TEXT("Measured collision damage matches the source-scale continuous drop within integration tolerance"),
            FMath::Abs(Damage - Approximate) <= Approximate / 10 + 30);
        TestTrue(TEXT("A higher real drop causes more than the previous drop"), Damage > Previous);
        Previous = Damage;
        const int32 After = Fixture.Player->GetHealth();
        for (int32 Frame = 0; Frame < 30; ++Frame) { Fixture.Tick(); }
        TestEqual(TEXT("Remaining grounded does not replay landing damage"), Fixture.Player->GetHealth(), After);
    }
    for (int32 Class : {CitizenClass, RogueClass})
    {
        FFallFixture Fixture(Class);
        if (!Fixture.Valid() || !Fixture.Drop(0.2)) { return false; }
        const int32 Before = Fixture.Player->GetHealth();
        Fixture.Press(EKeys::SpaceBar);
        bool bLanded = false;
        for (int32 Frame = 0; Frame < 600; ++Frame)
        {
            Fixture.Tick();
            if (Frame > 5 && Fixture.Player->GetCharacterMovement()->IsMovingOnGround()) { bLanded = true; break; }
        }
        TestTrue(TEXT("The real ordinary/Rogue jump returns to its physical floor"), bLanded);
        TestEqual(TEXT("Source ordinary and Rogue jumps do not hurt on landing"), Fixture.Player->GetHealth(), Before);
    }
    FFallFixture Teleport;
    if (!Teleport.Valid()) { return false; }
    const int32 Before = Teleport.Player->GetHealth();
    Teleport.Player->SetActorLocation(FVector(0, 0, 1200), false, nullptr, ETeleportType::TeleportPhysics);
    TestEqual(TEXT("Spawning/repositioning high above the floor is not itself impact damage"), Teleport.Player->GetHealth(), Before);
    Teleport.Player->SetActorLocation(FVector(0, 0, ANativeWadPlayer::CapsuleHalfHeight + 0.2), false, nullptr, ETeleportType::TeleportPhysics);
    Teleport.Player->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    for (int32 Frame = 0; Frame < 10; ++Frame) { Teleport.Tick(); }
    TestEqual(TEXT("A large teleport-height difference fabricates no landing impact"), Teleport.Player->GetHealth(), Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalShockAbsorption, "AAOriginal.WAD.Gameplay.ShockAbsorption",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalShockAbsorption::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const auto* Spell = Algo::FindByPredicate(SupportedSpells, [](const auto& Entry) { return Entry.SourceDefinition == 73; });
    if (!TestNotNull(TEXT("Source Resist Impact definition 73 exists"), Spell)) { return false; }
    TArray<FString> Input;
    const FString Path = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC/SPL00073.TXT");
    if (!TestTrue(TEXT("Read actual original Resist Impact metadata"), FFileHelper::LoadFileToStringArray(Input, *Path))) { return false; }
    TArray<FString> Fields;
    for (const FString& Line : Input)
    { const FString Clean = Line.TrimStartAndEnd(); if (!Clean.IsEmpty() && !Clean.StartsWith(TEXT("#"))) { Fields.Add(Clean); } }
    const auto Field = [&](const TCHAR* Name)
    { const int32 Index = Fields.Find(Name); return Index != INDEX_NONE && Fields.IsValidIndex(Index + 1) ? Fields[Index + 1] : FString(); };
    FString Runes = Field(TEXT("ACCESS CODE"));
    Runes.ReplaceInline(TEXT(","), TEXT("")); Runes.ReplaceInline(TEXT("0"), TEXT("")); Runes.ReplaceInline(TEXT(" "), TEXT(""));
    TestEqual(TEXT("Resist Impact uses the original runes"), FString(Spell->Runes), Runes);
    TestEqual(TEXT("Resist Impact uses the original 500 mana cost"), Spell->Mana, FCString::Atoi(*Field(TEXT("SPELL COST"))));
    TestEqual(TEXT("Resist Impact uses the original casting hardness"), Spell->Hardness, FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
    TestEqual(TEXT("Resist Impact uses its original duration"), Spell->DurationTicks, FCString::Atoi(*Field(TEXT("SPELL DURATION"))));
    TestEqual(TEXT("Resist Impact uses its original power"), Spell->Power, FCString::Atoi(*Field(TEXT("SPELL POWER"))));
    TestEqual(TEXT("The actual source subtype is SHOCK ABSORB"), Field(TEXT("EFFECT SUBTYPE")), FString(TEXT("SHOCK ABSORB")));
    TestTrue(TEXT("Resist Impact is a Cleric status effect with its named native status"),
        Spell->School == ESpellSchool::Cleric && Spell->Effect == ESpellEffect::Status && Spell->Status == ShockAbsorbStatus);
    FFallFixture Fixture(PriestClass);
    if (!Fixture.Valid() || !Fixture.Drop(0.2)) { return false; }
    const int32 Mana = Fixture.Player->GetMana(), Casts = Fixture.Player->GetSuccessfulCasts();
    if (!TestTrue(TEXT("Cast Resist Impact using normal keypad 8/1 and RightMouse"), Fixture.CastImpact())) { return false; }
    TestEqual(TEXT("The ordinary cast charges the original cost"), Fixture.Player->GetMana(), Mana - 500);
    TestEqual(TEXT("The ordinary cast is successful"), Fixture.Player->GetSuccessfulCasts(), Casts + 1);
    TestEqual(TEXT("The source spell owns its 30-second protection"), Fixture.Player->GetOwnedEffectRemaining(TEXT("spell:73")), 30.0);
    const int32 Before = Fixture.Player->GetHealth();
    if (!TestTrue(TEXT("Protected player still physically falls and lands"), Fixture.Drop(500))) { return false; }
    TestEqual(TEXT("Shock Absorption prevents actual floor-impact damage"), Fixture.Player->GetHealth(), Before);
    Fixture.Player->ReceiveTypedHit(100, NormalDamage);
    TestEqual(TEXT("Shock Absorption is not general damage immunity"), Fixture.Player->GetHealth(), Before - 100);
    Fixture.Player->Tick(5.0f);
    if (!Fixture.CastImpact()) { return false; }
    TestEqual(TEXT("Recasting refreshes the same source owner"), Fixture.Player->GetOwnedEffectCount(), 1);
    TestEqual(TEXT("Recasting restores the original duration"), Fixture.Player->GetOwnedEffectRemaining(TEXT("spell:73")), 30.0);
    Fixture.Player->Tick(30.1f);
    TestEqual(TEXT("Shock Absorption expires through the real player effect timer"), Fixture.Player->GetStatusPower(ShockAbsorbStatus), 0);
    const int32 After = Fixture.Player->GetHealth();
    if (!Fixture.Drop(250)) { return false; }
    TestTrue(TEXT("An actual impact hurts again after protection expires"), Fixture.Player->GetHealth() < After);
    Fixture.Player->RestoreWadVital(10, 500 - Fixture.Player->GetMana());
    const int32 RefusedCasts = Fixture.Player->GetSuccessfulCasts();
    Fixture.CastImpact();
    TestEqual(TEXT("Mana equal to the cost refuses the original spell"), Fixture.Player->GetMana(), 500);
    TestEqual(TEXT("A refused cast creates no new protection"), Fixture.Player->GetStatusPower(ShockAbsorbStatus), 0);
    TestEqual(TEXT("A refused cast is not counted"), Fixture.Player->GetSuccessfulCasts(), RefusedCasts);
    // Symbolic ordinal survives independent Invulnerability/Shock enum appends.
    const FString Saved = FString::Printf(TEXT("1~spell:73,%d,%d,1,12.5"), int32(EWadTimedEffectKind::Status), int32(ShockAbsorbStatus));
    FFallFixture Restored(PriestClass, *Saved);
    if (!Restored.Valid()) { return false; }
    TestEqual(TEXT("Restored protection preserves its exact source owner/time"), Restored.Player->GetOwnedEffectRemaining(TEXT("spell:73")), 12.5);
    TestTrue(TEXT("A restored source protection handles a real physical impact"), Restored.Drop(250));
    TestEqual(TEXT("Restored Shock Absorption prevents impact damage"), Restored.Player->GetHealth(), 30000);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalFallMovementEffects, "AAOriginal.WAD.Gameplay.FallMovementEffects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalFallMovementEffects::RunTest(const FString& Parameters)
{
    const FString FeatherState = FString::Printf(TEXT("1~item:817,%d,-1,1,30"), int32(EWadTimedEffectKind::FeatherFall));
    FFallFixture Feather(NativeWadRules::CitizenClass, *FeatherState);
    if (!Feather.Valid() || !TestTrue(TEXT("Featherfall still physically lands from high above"), Feather.Drop(500))) { return false; }
    TestEqual(TEXT("The source minus 15000 feather cap stays below damaging impact speed"), Feather.Player->GetHealth(), 30000);

    const FString GravityState = FString::Printf(TEXT("1~spell:62,%d,%d,1,30"),
        int32(EWadTimedEffectKind::Status), int32(NativeWadRules::LowGravityStatus));
    FFallFixture Gravity(NativeWadRules::CitizenClass, *GravityState);
    if (!Gravity.Valid() || !Gravity.Drop(500)) { return false; }
    const int32 GravityDamage = 30000 - Gravity.Player->GetHealth();
    TestTrue(TEXT("Low Gravity changes actual impact speed rather than granting blanket fall immunity"),
        GravityDamage > 650 && GravityDamage < 900);

    const FString FlightState = FString::Printf(TEXT("1~spell:41,%d,-1,1,30"), int32(EWadTimedEffectKind::Flight));
    FFallFixture Flying(NativeWadRules::CitizenClass, *FlightState);
    if (!Flying.Valid()) { return false; }
    Flying.Player->SetActorLocation(FVector(0, 0, 100), false, nullptr, ETeleportType::TeleportPhysics);
    for (int32 Frame = 0; Frame < 480; ++Frame) { Flying.Tick(); }
    TestTrue(TEXT("Active flight retains flying physics at a floor contact"), Flying.Player->GetCharacterMovement()->IsFlying());
    TestEqual(TEXT("Native flying contacts do not fabricate a falling landing event"), Flying.Player->GetHealth(), 30000);

    FFallFixture Water(NativeWadRules::CitizenClass, nullptr, true);
    if (!Water.Valid() || !Water.Drop(250)) { return false; }
    TestTrue(TEXT("A sinking player reaches the actual floor below the water surface"),
        FMath::Abs(Water.Player->GetActorLocation().Z - (Water.FloorZ + ANativeWadPlayer::CapsuleHalfHeight)) < 3.0);
    TestTrue(TEXT("Water is not an invented exemption from source floor-impact damage"), Water.Player->GetHealth() < 30000);
    const FString WalkingState = FString::Printf(TEXT("1~spell:71,%d,-1,1,30"), int32(EWadTimedEffectKind::WaterWalk));
    FFallFixture Walking(NativeWadRules::CitizenClass, *WalkingState, true);
    if (!Walking.Valid() || !Walking.Drop(250)) { return false; }
    TestTrue(TEXT("The real Water Walk effect selects the visible surface as the collision floor"),
        FMath::Abs(Walking.Player->GetActorLocation().Z - ANativeWadPlayer::CapsuleHalfHeight) < 3.0);
    TestTrue(TEXT("Water Walk also retains source normal impact damage"), Walking.Player->GetHealth() < 30000);
    return true;
}
