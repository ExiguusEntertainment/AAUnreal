#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"

namespace
{
    struct FMovementSpellFixture
    {
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        APlayerController* Controller = nullptr;
        UInputComponent* Input = nullptr;

        explicit FMovementSpellFixture(int32 Class = NativeWadRules::CitizenClass, const TCHAR* OwnedEffects = nullptr)
        {
            World = UWorld::CreateWorld(EWorldType::Game, false);
            if (!World) { return; }
            World->SetGameInstance(NewObject<UGameInstance>());
            for (const TCHAR* Option : {TEXT("WadCreated=1"), TEXT("WadRuneRules=1"), TEXT("WadLevel=100"),
                TEXT("WadAttributes=100,100,100,100,100,100"), TEXT("WadMaxMana=30000"), TEXT("WadMana=30000")})
            { World->URL.AddOption(Option); }
            World->URL.AddOption(*FString::Printf(TEXT("WadClass=%d"), Class));
            FString Items;
            for (int32 Rune = 1; Rune <= 9; ++Rune)
            {
                if (Rune != 1) { Items += TEXT(","); }
                Items += FString::FromInt(NativeWadRules::RuneItemForClass(Class, Rune));
            }
            World->URL.AddOption(*(TEXT("WadItems=") + Items));
            if (OwnedEffects) { World->URL.AddOption(*(TEXT("WadOwnedEffects=") + FString(OwnedEffects))); }
            Player = World->SpawnActor<ANativeWadPlayer>(FVector(0, 0, 60), FRotator::ZeroRotator);
            Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }

        ~FMovementSpellFixture() { if (World) { World->DestroyWorld(false); } }
        bool Valid() const { return Player && Controller && Input; }
        bool Press(const FKey& Key) const
        {
            if (!Input) { return false; }
            for (const auto& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }
        bool Cast(const TCHAR* Runes) const
        {
            const FKey Keys[] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree,
                EKeys::NumPadFour, EKeys::NumPadFive, EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
            const int32 PreviousRunes = Player->GetRunes().Len();
            for (int32 Index = 0; Index < PreviousRunes; ++Index) { if (!Press(EKeys::BackSpace)) { return false; } }
            for (const TCHAR* Rune = Runes; *Rune; ++Rune)
            { if (*Rune < TEXT('1') || *Rune > TEXT('9') || !Press(Keys[*Rune - TEXT('1')])) { return false; } }
            return Press(EKeys::RightMouseButton);
        }
        bool Wear(int32 Type, int32 Location) const
        {
            if (!Player->CollectItem(Type) || !Press(EKeys::I)) { return false; }
            Player->ClickInventorySlot(Player->GetCarriedItems().Num() - 1);
            Player->ClickJewelrySlot(Location);
            const bool bWorn = Player->GetJewelryInventorySlot(Location) == Player->GetCarriedItems().Num() - 1;
            return Press(EKeys::I) && bWorn;
        }
        void Remove(int32 Location) const
        {
            Press(EKeys::I);
            Player->ClickJewelrySlot(Location);
            Press(EKeys::I);
        }
        UBoxComponent* Box(const FVector& Position, const FVector& Extent) const
        {
            AActor* Owner = World->SpawnActor<AActor>();
            if (!Owner) { return nullptr; }
            UBoxComponent* Shape = NewObject<UBoxComponent>(Owner);
            Owner->SetRootComponent(Shape);
            Shape->SetBoxExtent(Extent);
            Shape->SetCollisionProfileName(TEXT("BlockAll"));
            Shape->SetWorldLocation(Position);
            Shape->RegisterComponent();
            return Shape;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSourceJumpMovement, "AAOriginal.WAD.Gameplay.SourceJumpMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSourceJumpMovement::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    TestEqual(TEXT("Source Citizen jump power is 75 shifted eight bits"), SourceJumpPower(CitizenClass, 0), 19200);
    TestEqual(TEXT("Source Rogue jump power has its original fifty-percent bonus"), SourceJumpPower(RogueClass, 0), 28800);
    TestEqual(TEXT("Source jump modifiers clamp after addition"), SourceJumpPower(RogueClass, 34000), 60000);
    TestEqual(TEXT("Negative total power clamps to zero"), SourceJumpPower(CitizenClass, -30000), 0);
    TestEqual(TEXT("Leap adds after the source stat clamp, then converts to its word16 parameter"), int32(SourceLeapPower(60000, 8000)), 2464);
    TestEqual(TEXT("Main source vertical velocity conversion includes the falling step's shift"), SourceVerticalSpeed(19200), 328.125);
    TestEqual(TEXT("Normal source gravity converts to the native time unit"), SourceGravityAcceleration(1024), 1225.0);
    TestEqual(TEXT("The old sixteen-times-too-slow feather cap is corrected"), FeatherfallSpeed, 256.34765625);

    FMovementSpellFixture Fixture;
    if (!TestTrue(TEXT("Source jump fixture is ready"), Fixture.Valid())) { return false; }
    auto* Player = Fixture.Player;
    auto* Movement = Player->GetCharacterMovement();
    TestEqual(TEXT("The actual movement component uses source normal gravity"), Movement->GetGravityZ(), -1225.0f);
    Movement->SetMovementMode(MOVE_Walking);
    Movement->Velocity = FVector::ZeroVector;
    Fixture.Press(EKeys::SpaceBar);
    TestEqual(TEXT("Ordinary Space invokes the source-scaled jump"), Movement->Velocity.Z, 328.125);
    TestTrue(TEXT("A ground jump immediately begins falling physics"), Movement->IsFalling());
    Fixture.Press(EKeys::SpaceBar);
    TestEqual(TEXT("Repeated Space cannot double jump without flight"), Movement->Velocity.Z, 328.125);
    Player->ReceiveTrapParalysis();
    Movement->SetMovementMode(MOVE_Walking);
    Fixture.Press(EKeys::SpaceBar);
    TestEqual(TEXT("Paralysis still prevents an ordinary jump"), Movement->Velocity.Z, 0.0);

    FMovementSpellFixture Flying(CitizenClass, TEXT("1~spell:41,1,-1,1,30"));
    if (!TestTrue(TEXT("Flight jump fixture is ready"), Flying.Valid())) { return false; }
    auto* FlyingMovement = Flying.Player->GetCharacterMovement();
    FlyingMovement->SetMovementMode(MOVE_Flying);
    FlyingMovement->Velocity.Z = 5;
    Flying.Press(EKeys::SpaceBar);
    TestEqual(TEXT("Airborne flight Space adds the source three-eighths thrust"), FlyingMovement->Velocity.Z, 5.0 + SourceVerticalSpeed(7200));
    TestTrue(TEXT("Flight thrust keeps flying physics"), FlyingMovement->IsFlying());

    FMovementSpellFixture Jewelry(RogueClass);
    if (!TestTrue(TEXT("Source jump jewelry fixture is ready"), Jewelry.Valid())) { return false; }
    for (int32 Location = Ring1Slot; Location <= Ring4Slot; ++Location)
    { if (!TestTrue(TEXT("Actual Bank jumping ring equips in its original location"), Jewelry.Wear(434, Location))) { return false; } }
    if (!TestTrue(TEXT("Actual jumping amulet equips independently"), Jewelry.Wear(612, NeckSlot))) { return false; }
    TestEqual(TEXT("Distinct worn object owners supply all jump modifiers"), Jewelry.Player->GetStatusPower(JumpStatus), 34000);
    TestEqual(TEXT("Equipped Rogue jump power reaches the source cap"), Jewelry.Player->GetWadJumpPower(), 60000);
    Jewelry.Remove(NeckSlot);
    TestEqual(TEXT("Removing the amulet removes only its modifier"), Jewelry.Player->GetWadJumpPower(), 52800);
    FMovementSpellFixture Rings;
    if (!TestTrue(TEXT("Separate ring removal fixture is ready"), Rings.Valid())) { return false; }
    for (int32 Location = Ring1Slot; Location <= Ring4Slot; ++Location)
    { if (!Rings.Wear(434, Location)) { return false; } }
    Rings.Remove(Ring1Slot);
    TestEqual(TEXT("Removing one ring preserves the other three owners"), Rings.Player->GetWadJumpPower(), 37200);

    FMovementSpellFixture Feather(CitizenClass, TEXT("1~item:817,3,-1,1,30"));
    if (!TestTrue(TEXT("Featherfall fixture is ready"), Feather.Valid())) { return false; }
    const FVector Falling = Feather.Player->GetCharacterMovement()->NewFallVelocity(FVector(0, 0, -500), FVector(0, 0, -1225), 0.1f);
    TestEqual(TEXT("The real custom falling integrator enforces the corrected feather cap"), Falling.Z, -FeatherfallSpeed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSweptBlink, "AAOriginal.WAD.Gameplay.SweptBlink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSweptBlink::RunTest(const FString& Parameters)
{
    FMovementSpellFixture Fixture;
    if (!TestTrue(TEXT("Swept Blink fixture is ready"), Fixture.Valid())) { return false; }
    auto* Player = Fixture.Player;
    auto* Movement = Player->GetCharacterMovement();
    Movement->SetMovementMode(MOVE_Falling);
    Movement->Velocity = FVector(4, 8, -12);
    Fixture.Controller->SetControlRotation(FRotator(60, 90, 0));
    TestTrue(TEXT("Open-space Blink makes progress"), Player->BlinkWadForward(500));
    TestTrue(TEXT("Blink uses horizontal yaw and preserves vertical position"), Player->GetActorLocation().Equals(FVector(0, 500, 60), 0.01));
    TestTrue(TEXT("Blink preserves existing movement velocity"), Movement->Velocity.Equals(FVector(4, 8, -12)));
    TestTrue(TEXT("Airborne Blink keeps the movement mode"), Movement->IsFalling());

    Player->SetActorLocation(FVector(0, 0, 60), false, nullptr, ETeleportType::TeleportPhysics);
    Fixture.Controller->SetControlRotation(FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("A thin blocking wall lies midway along the Blink path"), Fixture.Box(FVector(100, 0, 60), FVector(1, 100, 100)))) { return false; }
    TestTrue(TEXT("Blocked Blink may make partial forward progress"), Player->BlinkWadForward(600));
    TestTrue(TEXT("The capsule stops on the near side of the thin wall"), Player->GetActorLocation().X > 0 && Player->GetActorLocation().X <= 89.01);
    TestTrue(TEXT("The wall cannot change Blink's height"), FMath::IsNearlyEqual(Player->GetActorLocation().Z, 60.0));
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(WadBlinkTest), false, Player);
    TestFalse(TEXT("Swept Blink leaves no capsule penetration"), Fixture.World->OverlapBlockingTestByChannel(Player->GetActorLocation(), FQuat::Identity,
        ECC_Pawn, FCollisionShape::MakeCapsule(Player->CapsuleRadius, Player->CapsuleHalfHeight), Query));
    Player->SetActorLocation(FVector(0, 300, 60), false, nullptr, ETeleportType::TeleportPhysics);
    if (!TestNotNull(TEXT("A low lintel blocks capsule headroom along a second route"), Fixture.Box(FVector(100, 300, 90), FVector(1, 80, 10)))) { return false; }
    Player->BlinkWadForward(500);
    TestTrue(TEXT("Blink cannot pass a route blocked only at head height"), Player->GetActorLocation().X <= 99 && FMath::IsNearlyEqual(Player->GetActorLocation().Z, 60.0));
    Player->ReceiveTrapParalysis();
    const FVector Before = Player->GetActorLocation();
    TestFalse(TEXT("Paralysis prevents Blink movement"), Player->BlinkWadForward(500));
    TestTrue(TEXT("A refused Blink leaves the pawn in place"), Player->GetActorLocation().Equals(Before));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMovementRuneSpells, "AAOriginal.WAD.Gameplay.MovementRuneSpells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalMovementRuneSpells::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (int32 Definition : {8, 50, 19, 58, 62, 91})
    {
        const auto* Spell = Algo::FindByPredicate(SupportedSpells, [&](const auto& Entry) { return Entry.SourceDefinition == Definition; });
        if (!TestNotNull(TEXT("All six source movement spells have native definitions"), Spell)) { return false; }
        TArray<FString> SourceLines;
        const FString SourcePath = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC")
            / FString::Printf(TEXT("SPL%05d.TXT"), Definition);
        if (!TestTrue(TEXT("The actual original movement spell resource can be read"), FFileHelper::LoadFileToStringArray(SourceLines, *SourcePath))) { return false; }
        TArray<FString> Fields;
        for (const FString& Line : SourceLines)
        {
            const FString Value = Line.TrimStartAndEnd();
            if (!Value.IsEmpty() && !Value.StartsWith(TEXT("#"))) { Fields.Add(Value); }
        }
        const auto Field = [&](const TCHAR* Name)
        {
            const int32 Index = Fields.Find(Name);
            return Index != INDEX_NONE && Fields.IsValidIndex(Index + 1) ? Fields[Index + 1] : FString();
        };
        FString SourceRunes = Field(TEXT("ACCESS CODE"));
        SourceRunes.ReplaceInline(TEXT(","), TEXT(""));
        SourceRunes.ReplaceInline(TEXT("0"), TEXT(""));
        SourceRunes.ReplaceInline(TEXT(" "), TEXT(""));
        TestEqual(TEXT("Movement spell runes match its original source definition"), FString(Spell->Runes), SourceRunes);
        TestEqual(TEXT("Movement spell cost matches its original source definition"), Spell->Mana, FCString::Atoi(*Field(TEXT("SPELL COST"))));
        TestEqual(TEXT("Movement spell hardness matches its original source definition"), Spell->Hardness, FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
        TestEqual(TEXT("Movement spell power matches its original source definition"), Spell->Power, FCString::Atoi(*Field(TEXT("SPELL POWER"))));
        TestEqual(TEXT("Movement spell duration matches its original source definition"), Spell->DurationTicks, FCString::Atoi(*Field(TEXT("SPELL DURATION"))));
        TestEqual(TEXT("Movement spell school matches its original source definition"),
            Spell->School == ESpellSchool::Mage ? FString(TEXT("MAGE")) : Spell->School == ESpellSchool::Cleric ? FString(TEXT("CLERIC")) : FString(TEXT("ARCANE")),
            Field(TEXT("SPELL SYSTEM")));
        TestEqual(TEXT("Movement spell effect matches its original source definition"),
            Spell->Effect == ESpellEffect::Leap ? FString(TEXT("PLAYER LEAP")) : Spell->Effect == ESpellEffect::Blink ? FString(TEXT("JUMP FORWARD")) : FString(TEXT("ADD PLAYER EFFECT")),
            Field(TEXT("EFFECT TYPE")));
        FMovementSpellFixture Fixture(Spell->School == ESpellSchool::Mage ? MageClass : Spell->School == ESpellSchool::Cleric ? PriestClass : CitizenClass);
        if (!TestTrue(TEXT("Source movement caster is ready"), Fixture.Valid())) { return false; }
        auto* Player = Fixture.Player;
        auto* Movement = Player->GetCharacterMovement();
        Movement->SetMovementMode(MOVE_Walking);
        Movement->Velocity = FVector::ZeroVector;
        const FVector Start = Player->GetActorLocation();
        const int32 Mana = Player->GetMana();
        const int32 Casts = Player->GetSuccessfulCasts();
        if (!TestTrue(TEXT("Movement spell uses physical keypad runes and RightMouse"), Fixture.Cast(Spell->Runes))) { return false; }
        TestEqual(TEXT("Movement spell charges the exact original cost"), Player->GetMana(), Mana - Spell->Mana);
        TestEqual(TEXT("Movement spell increments the successful cast counter"), Player->GetSuccessfulCasts(), Casts + 1);
        if (Spell->Effect == ESpellEffect::Leap)
        {
            TestEqual(TEXT("Leap immediately applies its source extra jump power"), Movement->Velocity.Z, SourceVerticalSpeed(27200));
            TestEqual(TEXT("Instant Leap creates no temporary owner"), Player->GetOwnedEffectCount(), 0);
        }
        else if (Spell->Effect == ESpellEffect::Blink)
        {
            TestTrue(TEXT("A successful Blink travels its original horizontal distance"), Player->GetActorLocation().Equals(Start + FVector(Spell->Power, 0, 0), 0.01));
            TestEqual(TEXT("Instant Blink creates no temporary owner"), Player->GetOwnedEffectCount(), 0);
        }
        else
        {
            const FString Owner = FWadPlayerEffects::SpellOwner(Definition);
            TestEqual(TEXT("Low gravity retains the source spell identity"), Player->GetOwnedEffectRemaining(Owner), 30.0);
            TestEqual(TEXT("Low gravity halves source gravity once"), Movement->GetGravityZ(), -612.5f);
            Player->Tick(1.25f);
            TestEqual(TEXT("Low gravity's real player timer advances"), Player->GetOwnedEffectRemaining(Owner), 28.75);
            Fixture.Cast(Spell->Runes);
            TestEqual(TEXT("Recasting Low Gravity refreshes instead of duplicating"), Player->GetOwnedEffectCount(), 1);
            TestEqual(TEXT("Recasting Low Gravity restores its duration"), Player->GetOwnedEffectRemaining(Owner), 30.0);
            TestEqual(TEXT("Recasting never quarters gravity"), Movement->GetGravityZ(), -612.5f);
            if (!TestTrue(TEXT("Flight amulet can coexist with a low-gravity spell"), Fixture.Wear(FlyingAmulet, NeckSlot))) { return false; }
            TestFalse(TEXT("Low gravity is ignored during flight"), Player->HasLowGravity());
            TestEqual(TEXT("Low gravity does not scale the flight gravity setting"), Movement->GetGravityZ(), -1225.0f);
            Fixture.Remove(NeckSlot);
            TestEqual(TEXT("Ground/fall gravity resumes its half strength after flight is removed"), Movement->GetGravityZ(), -612.5f);
        }
        Player->RestoreWadVital(10, Spell->Mana - Player->GetMana());
        const int32 BeforeRefusal = Player->GetSuccessfulCasts();
        const FVector RefusalPosition = Player->GetActorLocation();
        const FVector RefusalVelocity = Movement->Velocity;
        Fixture.Cast(Spell->Runes);
        TestEqual(TEXT("Mana equal to movement spell cost refuses without charge"), Player->GetMana(), Spell->Mana);
        TestEqual(TEXT("A refused movement spell does not count as successful"), Player->GetSuccessfulCasts(), BeforeRefusal);
        TestTrue(TEXT("A refused movement cast preserves position and velocity"), Player->GetActorLocation().Equals(RefusalPosition) && Movement->Velocity.Equals(RefusalVelocity));
    }

    // A valid modern save may contain the same status from distinct school owners.
    const FString StackedEffects = FString::Printf(TEXT("1~spell:62,0,%d,1,5~spell:91,0,%d,1,10"), int32(LowGravityStatus), int32(LowGravityStatus));
    FMovementSpellFixture Stacked(CitizenClass, *StackedEffects);
    if (!TestTrue(TEXT("Restored independent gravity owners are valid"), Stacked.Valid())) { return false; }
    auto* Movement = Stacked.Player->GetCharacterMovement();
    TestEqual(TEXT("Two owners still produce only one half-gravity multiplier"), Movement->GetGravityZ(), -612.5f);
    Stacked.Player->Tick(5.1f);
    TestEqual(TEXT("One owner's expiry preserves the other source effect"), Stacked.Player->GetOwnedEffectCount(), 1);
    TestEqual(TEXT("Half gravity remains until the final owner expires"), Movement->GetGravityZ(), -612.5f);
    Stacked.Player->Tick(5.0f);
    TestEqual(TEXT("Final owner expiry restores normal gravity"), Movement->GetGravityZ(), -1225.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSourceJumpArc, "AAOriginal.WAD.Gameplay.SourceJumpArc",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSourceJumpArc::RunTest(const FString& Parameters)
{
    FMovementSpellFixture Fixture;
    if (!TestTrue(TEXT("Physical jump arc fixture is ready"), Fixture.Valid())) { return false; }
    if (!TestNotNull(TEXT("A physical floor supports the collision/landing test"), Fixture.Box(FVector(0, 0, -2), FVector(1000, 1000, 2)))) { return false; }
    auto* Player = Fixture.Player;
    auto* Movement = Player->GetCharacterMovement();
    Player->SetActorLocation(FVector(0, 0, Player->CapsuleHalfHeight + 0.1), false, nullptr, ETeleportType::TeleportPhysics);
    Movement->bRunPhysicsWithNoController = true;
    Movement->SetMovementMode(MOVE_Walking);
    Movement->TickComponent(1.0f / 120.0f, LEVELTICK_All, nullptr);
    const double Start = Player->GetActorLocation().Z;
    Fixture.Press(EKeys::SpaceBar);
    double Apex = Start;
    bool bLanded = false;
    for (int32 Frame = 0; Frame < 150; ++Frame)
    {
        Movement->TickComponent(1.0f / 120.0f, LEVELTICK_All, nullptr);
        Apex = FMath::Max(Apex, Player->GetActorLocation().Z);
        if (Frame > 5 && Movement->IsMovingOnGround()) { bLanded = true; break; }
    }
    // This is deliberately a continuous source-scale approximation. The main
    // parabola rises 43.9453 units; original fixed-point ordering rises about43.
    TestTrue(TEXT("Real collision movement reaches a source-scale ordinary jump apex"), Apex - Start > 41 && Apex - Start < 47);
    TestTrue(TEXT("The real jump falls back onto the physical floor"), bLanded);
    TestTrue(TEXT("Landing leaves the capsule above the floor"), Player->GetActorLocation().Z >= Player->CapsuleHalfHeight);
    return true;
}
