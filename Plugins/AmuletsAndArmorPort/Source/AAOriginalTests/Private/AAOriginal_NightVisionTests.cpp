#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NativeWadNightVision.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    struct FNightFixture
    {
        UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;
        explicit FNightFixture(int32 Class = NativeWadRules::MageClass, const TCHAR* Saved = nullptr)
        {
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            FString Items;
            for (int32 Rune = 1; Rune <= 9; ++Rune)
            { if (!Items.IsEmpty()) { Items += TEXT(","); } Items += LexToString(NativeWadRules::RuneItemForClass(Class, Rune)); }
            const FString Options = Saved ? FString(Saved) : FString::Printf(
                TEXT("/Game/Maps/L30?WadClass=%d?WadCreated=1?WadRuneRules=1?WadLevel=100")
                TEXT("?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=1000")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadReady=-1?WadItems=%s"), Class, *Items);
            World->URL = FURL(nullptr, *Options, TRAVEL_Absolute);
            Player = World->SpawnActor<ANativeWadPlayer>();
            auto* Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }
        ~FNightFixture()
        {
            Instance->Shutdown();
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Valid() const { return Player && Input; }
        bool Press(const FKey& Key) const
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
            const FKey Keys[] = {EKeys::NumPadOne,EKeys::NumPadTwo,EKeys::NumPadThree,EKeys::NumPadFour,EKeys::NumPadFive,
                EKeys::NumPadSix,EKeys::NumPadSeven,EKeys::NumPadEight,EKeys::NumPadNine};
            const int32 Existing = Player->GetRunes().Len();
            for (int32 Index = 0; Index < Existing; ++Index) { if (!Press(EKeys::BackSpace)) { return false; } }
            for (const TCHAR* Rune = Runes; *Rune; ++Rune)
            { if (*Rune < TEXT('1') || *Rune > TEXT('9') || !Press(Keys[*Rune - TEXT('1')])) { return false; } }
            return Press(EKeys::RightMouseButton);
        }
        bool Use(int32 Type) const
        {
            if (!Player->CollectItem(Type)) { return false; }
            Player->StoreMouseHandInInventory();
            if (!Player->TakeInventoryItem(Player->GetCarriedItems().Num() - 1) || !Player->SwapMouseAndReady()) { return false; }
            Player->StoreMouseHandInInventory();
            return Press(EKeys::LeftControl);
        }
        UMaterialInstanceDynamic* PrivateMid() const
        {
            const FObjectPropertyBase* Property=FindFProperty<FObjectPropertyBase>(ANativeWadPlayer::StaticClass(),TEXT("NightVisionMaterial"));
            return Property?::Cast<UMaterialInstanceDynamic>(Property->GetObjectPropertyValue_InContainer(Player)):nullptr;
        }
        UMaterialInstanceDynamic* Mid() const
        {
            auto* Camera = Player->FindComponentByClass<UCameraComponent>();
            if (!Camera) { return nullptr; }
            for (const auto& Blendable : Camera->PostProcessSettings.WeightedBlendables.Array)
            { if (auto* Material = ::Cast<UMaterialInstanceDynamic>(Blendable.Object.Get())) { return Material; } }
            return nullptr;
        }
        FString Save() const
        {
            const FURL Town(nullptr,TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"),TRAVEL_Absolute);
            if (!World->GetAuthGameMode() && !World->SetGameMode(Town)) { return {}; }
            const FString Slot = TEXT("NativeNightVisionTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot,0); };
            if (!Player->SaveWadCheckpoint(Slot)) { return {}; }
            const auto* Saved = static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
            return Saved ? Saved->LevelPackage + TEXT("?") + Saved->PlayerOptions : FString();
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalNightVisionRunes,"AAOriginal.WAD.Gameplay.NightVisionRunes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalNightVisionRunes::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (const int32 Id : {20,36,57,85,99})
    {
        const auto* Spell = Algo::FindByPredicate(SupportedSpells,[&](const auto& Entry){ return Entry.SourceDefinition==Id; });
        if (!Spell) { return false; }
        TArray<FString> Source;
        if (!FFileHelper::LoadFileToStringArray(Source,*(FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC")/FString::Printf(TEXT("SPL%05d.TXT"),Id)))) { return false; }
        const auto Field = [&](const TCHAR* Name)
        {
            int32 At = Source.IndexOfByPredicate([&](const FString& Line){return Line.TrimStartAndEnd()==Name;});
            if (At < 0 || At >= Source.Num()) { return FString(); }
            for (++At; At<Source.Num(); ++At) { const FString Value=Source[At].TrimStartAndEnd(); if (!Value.IsEmpty()) { return Value; } }
            return FString();
        };
        TestEqual(TEXT("Original metadata selects NightVision"),Field(TEXT("EFFECT SUBTYPE")),FString(TEXT("NIGHT VISION")));
        TestEqual(TEXT("Original source power is retained"),Spell->Power,FCString::Atoi(*Field(TEXT("SPELL POWER"))));
        TestEqual(TEXT("Original source cost is retained"),Spell->Mana,FCString::Atoi(*Field(TEXT("SPELL COST"))));
        TestEqual(TEXT("Original source duration is retained"),Spell->DurationTicks,FCString::Atoi(*Field(TEXT("SPELL DURATION"))));
        TestEqual(TEXT("Original source hardness is retained"),Spell->Hardness,FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
        const int32 Class=Spell->School==ESpellSchool::Mage?MageClass:Spell->School==ESpellSchool::Cleric?PriestClass:KnightClass;
        FNightFixture Fixture(Class);
        if (!Fixture.Valid() || !Fixture.Cast(Spell->Runes)) { return false; }
        const FString Owner=FWadPlayerEffects::SpellOwner(Id);
        TestEqual(TEXT("Ordinary rune keys and RightMouse cast exactly once"),Fixture.Player->GetSuccessfulCasts(),1);
        TestEqual(TEXT("Ordinary casting pays the original mana"),Fixture.Player->GetMana(),30000-Spell->Mana);
        TestEqual(TEXT("The original owner power reaches the real runtime accessor"),Fixture.Player->GetNightVisionPower(),Spell->Power);
        TestEqual(TEXT("The exact source duration enters its definition owner"),Fixture.Player->GetOwnedEffectRemaining(Owner),Spell->DurationTicks/double(TicksPerSecond));
        Fixture.Player->Tick(1.0f);
        if (!Fixture.Cast(Spell->Runes)) { return false; }
        TestEqual(TEXT("Same-definition refresh does not append another owner"),Fixture.Player->GetOwnedEffectCount(),1);
        TestEqual(TEXT("Same-definition refresh renews the original duration"),Fixture.Player->GetOwnedEffectRemaining(Owner),Spell->DurationTicks/double(TicksPerSecond));
        Fixture.Player->RestoreWadVital(10,Spell->Mana-Fixture.Player->GetMana());
        if (!Fixture.Cast(Spell->Runes)) { return false; }
        TestEqual(TEXT("Exactly the mana cost is insufficient in the original rules"),Fixture.Player->GetMana(),Spell->Mana);
        TestEqual(TEXT("Refusal grants neither another cast nor another owner"),Fixture.Player->GetSuccessfulCasts(),2);
        TestEqual(TEXT("Refusal does not change the original timed owner"),Fixture.Player->GetOwnedEffectCount(),1);
    }
    FNightFixture WrongSchool(MageClass);
    if (!WrongSchool.Valid() || !WrongSchool.Cast(TEXT("4397"))) { return false; }
    TestEqual(TEXT("A Mage cannot cast the Cleric-only HolyLight source definition"),WrongSchool.Player->GetSuccessfulCasts(),0);
    TestEqual(TEXT("Wrong-school rune entry does not spend mana"),WrongSchool.Player->GetMana(),30000);
    TestEqual(TEXT("Wrong-school entry grants no temporary vision owner"),WrongSchool.Player->GetNightVisionPower(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalNightVisionOwners,"AAOriginal.WAD.Gameplay.NightVisionOwners",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalNightVisionOwners::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FNightFixture Fixture;
    if (!Fixture.Valid() || !Fixture.Cast(TEXT("957"))) { return false; }
    Fixture.Player->Tick(290.0f);
    if (!Fixture.Cast(TEXT("57"))) { return false; }
    TestEqual(TEXT("Different definitions retain independent owners"),Fixture.Player->GetOwnedEffectCount(),2);
    TestEqual(TEXT("Native visual policy chooses the strongest80, never40+80"),Fixture.Player->GetStatusPower(NightVisionStatus),80);
    Fixture.Player->Tick(10.1f);
    TestEqual(TEXT("Strongest expiry restores surviving weaker owner instead of the original reset-to-zero bug"),Fixture.Player->GetNightVisionPower(),40);
    TestEqual(TEXT("Only the actually expired owner is removed"),Fixture.Player->GetOwnedEffectCount(),1);
    if (!Fixture.Use(244)) { return false; }
    TestEqual(TEXT("Ordinary carrot USE retains its original300 food effect"),Fixture.Player->GetFood(),300);
    TestEqual(TEXT("The carrot is consumed once"),Fixture.Player->GetItemCount(244),0);
    TestEqual(TEXT("Carrot's source5000tick duration is independent"),Fixture.Player->GetOwnedEffectRemaining(TEXT("item:244")),5000.0/TicksPerSecond);
    if (!Fixture.Use(823)) { return false; }
    TestEqual(TEXT("Ordinary NightVision potion USE is now functional and consumed"),Fixture.Player->GetItemCount(823),0);
    TestEqual(TEXT("Potion70 beats spell40 and carrot20 without summing them"),Fixture.Player->GetNightVisionPower(),70);
    TestEqual(TEXT("Potion's original5000tick duration is retained"),Fixture.Player->GetOwnedEffectRemaining(TEXT("item:823")),5000.0/TicksPerSecond);
    Fixture.Player->Tick(float(5000.0/TicksPerSecond+0.1));
    TestEqual(TEXT("Both expired consumable owners leave the surviving longer spell40"),Fixture.Player->GetNightVisionPower(),40);
    Fixture.Player->CollectItem(415);
    Fixture.Player->StoreMouseHandInInventory();
    if (!Fixture.Player->TakeInventoryItem(Fixture.Player->GetCarriedItems().Num()-1)) { return false; }
    Fixture.Player->ClickJewelrySlot(Ring1Slot);
    Fixture.Player->ReceiveTypedHit(1,SpecialDamageFlag(ESpecialDamage::Dispel));
    TestEqual(TEXT("Dispel removes the remaining temporary visual owner"),Fixture.Player->GetNightVisionPower(),0);
    TestEqual(TEXT("Dispel preserves equipped magical shield passives"),Fixture.Player->GetStatusPower(SpellShieldStatus),5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalNightVisionPersistence,"AAOriginal.WAD.Gameplay.NightVisionPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalNightVisionPersistence::RunTest(const FString& Parameters)
{
    FNightFixture Fixture;
    if (!Fixture.Valid() || !Fixture.Cast(TEXT("957")) || !Fixture.Use(244) || !Fixture.Use(823)) { return false; }
    Fixture.Player->Tick(1.25f);
    const FString Options=Fixture.Save();
    if (!TestFalse(TEXT("Actual disk checkpoint writer accepted the effect owners"),Options.IsEmpty())) { return false; }
    FNightFixture Restored(NativeWadRules::MageClass,*Options);
    if (!Restored.Valid()) { return false; }
    TestEqual(TEXT("A new actor restores the strongest source owner"),Restored.Player->GetNightVisionPower(),80);
    for (const TCHAR* Owner : {TEXT("spell:36"),TEXT("item:244"),TEXT("item:823")})
    { TestEqual(TEXT("Each saved owner retains its own exact duration"),Restored.Player->GetOwnedEffectRemaining(Owner),Fixture.Player->GetOwnedEffectRemaining(Owner)); }
    Restored.Player->Tick(301.0f);
    TestEqual(TEXT("All naturally expired owned effects turn the private view off"),Restored.Player->GetNightVisionPower(),0);
    const FString Empty=Restored.Save();
    if (Empty.IsEmpty()) { return false; }
    FNightFixture Expired(NativeWadRules::MageClass,*Empty);
    if (!Expired.Valid()) { return false; }
    TestEqual(TEXT("An explicit empty owned save never resurrects NightVision"),Expired.Player->GetNightVisionPower(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalNightVisionPrivateViews,"AAOriginal.WAD.Interface.NightVisionPrivateViews",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalNightVisionPrivateViews::RunTest(const FString& Parameters)
{
    using namespace NativeWadNightVision;
    FNightFixture A,B;
    if (!A.Valid() || !B.Valid() || !A.Cast(TEXT("957")) || !B.Cast(TEXT("57"))) { return false; }
    auto* MA=A.PrivateMid();auto* MB=B.PrivateMid();
    if (!TestNotNull(TEXT("Saved shared parent produces cameraA's private MID"),MA)||!TestNotNull(TEXT("CameraB has its own private MID"),MB)) { return false; }
    TestTrue(TEXT("Two players never share their runtime power instance"),MA!=MB);
    TestEqual(TEXT("Desired source owner remains80 while rendering may be pending"),A.Player->GetNightVisionPower(),80);
    if (!IsRenderingReady(*MA->Parent)) { TestNull(TEXT("Pending camera never attaches a fallback PP"),A.Mid()); }
    if (!IsRenderingReady(*MB->Parent)) { TestNull(TEXT("Other pending camera never attaches a fallback PP"),B.Mid()); }
    TestEqual(TEXT("CameraA receives its strongest80 power"),MA->K2_GetScalarParameterValue(PowerParameter),80.0f);
    TestEqual(TEXT("CameraB receives only its own40 power"),MB->K2_GetScalarParameterValue(PowerParameter),40.0f);
    A.Player->Tick(301.0f);
    TestNull(TEXT("Expiry detaches cameraA's material rather than mutating a shared parent"),A.Mid());
    TestEqual(TEXT("CameraB's owner and material remain unchanged"),B.Player->GetNightVisionPower(),40);
    TestEqual(TEXT("CameraB's independent scalar is not reset by A"),MB->K2_GetScalarParameterValue(PowerParameter),40.0f);
    TestTrue(TEXT("Visible sky requires matching classification and depth ordering"),IsVisibleSky(SkyStencil,80000,80000));
    TestTrue(TEXT("SkyPass clear far scene depth still excludes original bIsSky pixels"),IsVisibleSky(SkyStencil,80000,100000000));
    TestFalse(TEXT("Sky stencil behind a foreground object must not suppress that object's vision"),IsVisibleSky(SkyStencil,80000,180));
    const FLinearColor Dark(0.05f,0.05f,0.05f,1);
    const FLinearColor Lit=Lift(Dark,80,false);
    TestTrue(TEXT("Neutral source-independent approximation increases dark visibility without green tint"),Lit.R>Dark.R&&Lit.R==Lit.G&&Lit.G==Lit.B);
    TestTrue(TEXT("Sky RGB remains exactly unchanged"),Lift(Dark,80,true)==Dark);
    TestTrue(TEXT("Full brightness remains unchanged"),Lift(FLinearColor::White,80,false)==FLinearColor::White);
    return true;
}
