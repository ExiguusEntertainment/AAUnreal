#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/OutputDeviceNull.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadItemSource.h"

namespace
{
    using namespace NativeWadRules;
    struct FDangerousItemFixture
    {
        UGameInstance* Instance = nullptr;
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;
        ANativeWadLevel* Level = nullptr;
        explicit FDangerousItemFixture(const TCHAR* SavedOptions = nullptr, const TCHAR* Extra = TEXT(""), bool bFloor = false)
        {
            Instance = NewObject<UGameInstance>(GEngine);
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            const FString Options = SavedOptions ? FString(SavedOptions) : FString(TEXT("/Game/Maps/L30?WadClass=2?WadCreated=1?WadRuneRules=1")
                TEXT("?WadLevel=100?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=10000")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadReady=-1?WadItems=300,301,302,303,304,305,306,307,308"));
            World->URL = FURL(nullptr, *(Options + Extra), TRAVEL_Absolute);
            if (bFloor)
            {
                Level = World->SpawnActor<ANativeWadLevel>();
                const auto* Property = FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(), TEXT("Sectors"));
                if (!Level || !Property) { return; }
                auto& Sectors = *Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
                auto& Sector = Sectors.AddDefaulted_GetRef();
                Sector.RepairedBoundaryPoints = {{-500,-500},{500,-500},{500,-500},{500,500},
                    {500,500},{-500,500},{-500,500},{-500,-500}};
                Sector.Floor = NewObject<UStaticMeshComponent>(Level);
                Sector.Ceiling = NewObject<UStaticMeshComponent>(Level);
                Sector.Floor->SetupAttachment(Level->GetRootComponent());
                Sector.Ceiling->SetupAttachment(Level->GetRootComponent());
                Sector.Ceiling->SetRelativeLocation(FVector(0,0,256));
                Sector.Floor->RegisterComponent(); Sector.Ceiling->RegisterComponent();
                Level->SetSectorFloorZ(0,0);
            }
            Player = World->SpawnActor<ANativeWadPlayer>(FVector(100,0,32), FRotator::ZeroRotator);
            auto* Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller); Controller->Possess(Player);
            Player->DispatchBeginPlay();
            if (!SavedOptions) { Player->InitializeItemIdentification(false); }
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }
        ~FDangerousItemFixture()
        {
            if (Instance) { Instance->Shutdown(); }
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Valid() const { return Player && Input; }
        bool Press(FKey Key) const
        {
            if (!Input) { return false; }
            for (const auto& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }
        bool Ready(int32 Type) const
        {
            // Same take/ready targets as the original banner, followed by its
            // normal bound USE key. No damage or inventory state setter is used.
            if (!Press(EKeys::I)) { return false; }
            Player->StoreMouseHandInInventory();
            const int32 Slot = Player->GetCarriedItems().Find(Type);
            if (Slot == INDEX_NONE) { return false; }
            Player->ClickInventorySlot(Slot);
            Player->ClickInventorySlot(INDEX_NONE);
            return Press(EKeys::I) && (Player->IsDefeated() || Player->GetReadyItem() == Type);
        }
        bool Wear(int32 Type, int32 Location) const
        {
            Player->StoreMouseHandInInventory();
            if (!Player->CollectItem(Type) || !Player->TakeInventoryItem(Player->GetCarriedItems().Num()-1)) { return false; }
            Player->ClickJewelrySlot(Location);
            return Player->GetMouseHandItem() == 0;
        }
        bool Cast(const TCHAR* Runes) const
        {
            const FKey Keys[] = {EKeys::NumPadOne,EKeys::NumPadTwo,EKeys::NumPadThree,EKeys::NumPadFour,EKeys::NumPadFive,
                EKeys::NumPadSix,EKeys::NumPadSeven,EKeys::NumPadEight,EKeys::NumPadNine};
            const int32 Previous = Player->GetRunes().Len();
            for (int32 I=0; I<Previous; ++I) { if (!Press(EKeys::BackSpace)) { return false; } }
            for (const TCHAR* R=Runes; *R; ++R)
            { if (*R<TEXT('1') || *R>TEXT('9') || !Press(Keys[*R-TEXT('1')])) { return false; } }
            return Press(EKeys::RightMouseButton);
        }
        int32 Drops(int32 Type) const
        {
            int32 Count=0;
            for (TActorIterator<ANativeWadPickup> It(World); It; ++It)
            { if (It->IsDropped() && !It->IsActorBeingDestroyed() && It->GetItemType()==Type) { ++Count; } }
            return Count;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalDangerousItemSource, "AAOriginal.WAD.Gameplay.DangerousItemSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalDangerousItemSource::RunTest(const FString& Parameters)
{
    TMap<int32,FWadAuthoredItem> Items;
    FOutputDeviceNull Output;
    if (!TestTrue(TEXT("Decode actual source object resources and INVDESC"), ReadWadAuthoredItems({825,826,827,907,954},Items,Output))) { return false; }
    const auto* Catalog=static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr,UNativeWadHudData::ObjectPath));
    if (!TestNotNull(TEXT("Refreshed saved item catalog exists"),Catalog)) { return false; }
    const int32 Types[]={825,826,827,907};
    const uint8 Masks[]={DamageFlag(EProjectileDamage::Poison),DamageFlag(EProjectileDamage::Fire),NormalDamage,DamageFlag(EProjectileDamage::Electricity)};
    for (int32 I=0; I<4; ++I)
    {
        const auto* Item=Items.Find(Types[I]);
        const auto* Saved=Catalog->ItemDetails.Find(Types[I]);
        if (!TestNotNull(TEXT("Original dangerous type parses"),Item) || !TestNotNull(TEXT("Dangerous type is present in saved catalog"),Saved)) { return false; }
        const auto& D=Item->Details;
        TestEqual(TEXT("Source power and trigger are preserved"),I==3?D.ReadyDamagePower:D.UseDamagePower,I==3?200:1000);
        TestEqual(TEXT("Source typed mask is preserved"),I==3?D.ReadyDamageMask:D.UseDamageMask,Masks[I]);
        TestEqual(TEXT("The other trigger stays empty"),I==3?D.UseDamagePower:D.ReadyDamagePower,0);
        TestEqual(TEXT("Catalog keeps source USE power"),Saved->UseDamagePower,D.UseDamagePower);
        TestEqual(TEXT("Catalog keeps source USE mask"),Saved->UseDamageMask,D.UseDamageMask);
        TestEqual(TEXT("Catalog keeps source READY power"),Saved->ReadyDamagePower,D.ReadyDamagePower);
        TestEqual(TEXT("Catalog keeps source READY mask"),Saved->ReadyDamageMask,D.ReadyDamageMask);
        if (I<3)
        {
            TestTrue(TEXT("Source potion is now usable, consumed and identified on use"),D.bUsable&&D.bConsumed&&D.bIdentifyOnUse);
        }
        else
        {
            TestEqual(TEXT("Lightning wand USE retains projectile8"),D.WandProjectile,8);
            TestEqual(TEXT("Lightning wand retains1000 mana firing cost"),D.WandManaCost,1000);
            TestFalse(TEXT("READY damage does not make the wand a consumed potion"),D.bConsumed);
        }
    }
    const auto& Orb=Items.FindChecked(954).Details;
    TestTrue(TEXT("Defense Orb remains its separate raw health cost, never typed damage"),Orb.UseDamagePower==0&&Orb.ReadyDamagePower==0&&Orb.Status==InvulnerableStatus);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalDangerousPotionUse, "AAOriginal.WAD.Gameplay.DangerousPotionUse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalDangerousPotionUse::RunTest(const FString& Parameters)
{
    const int32 Types[]={825,826,827};
    const uint8 Masks[]={DamageFlag(EProjectileDamage::Poison),DamageFlag(EProjectileDamage::Fire),NormalDamage};
    const int32 ResistAmulets[]={606,607,0};
    for (int32 I=0; I<3; ++I)
    {
        for (const bool bResist : {false,true})
        {
            FDangerousItemFixture F;
            if (!F.Valid() || !F.Wear(415,Ring1Slot) || (bResist&&ResistAmulets[I]&&!F.Wear(ResistAmulets[I],NeckSlot))) { return false; }
            auto* P=F.Player;
            if (!TestTrue(TEXT("Explicit fixture potion can be collected and readied"),P->CollectItem(Types[I])&&F.Ready(Types[I]))) { return false; }
            const int32 H=P->GetHealth(),Poison=P->GetPoisonLevel(),M=P->GetMana();
            TestFalse(TEXT("Readying a harmful potion does not teach its identity"),P->HasIdentifiedItemType(Types[I]));
            const auto Hit=ResolvePlayerDamage(1000,Masks[I],P->GetResistanceMask(),0,P->GetArmorRating());
            if (!TestTrue(TEXT("Bound LeftControl invokes the actual USE trigger"),F.Press(EKeys::LeftControl))) { return false; }
            TestEqual(TEXT("USE applies the typed health formula including resistance and shielding"),P->GetHealth(),H-Hit.Health);
            TestEqual(TEXT("Poison USE applies the typed poison side effect"),P->GetPoisonLevel(),Poison+Hit.Poison);
            TestEqual(TEXT("These typed actions do not spend mana"),P->GetMana(),M);
            TestEqual(TEXT("The used owner is consumed exactly once"),P->GetItemCount(Types[I]),0);
            TestTrue(TEXT("Successful harmful USE learns the full item identity"),P->HasIdentifiedItemType(Types[I]));
            TestEqual(TEXT("Consumption clears ready ownership"),P->GetReadyItem(),0);
        }
    }
    FDangerousItemFixture Protected;
    if (!Protected.Valid() || !Protected.Cast(TEXT("1547"))) { return false; }
    for (int32 Type : Types)
    {
        if (!Protected.Player->CollectItem(Type)||!Protected.Ready(Type)) { return false; }
        const int32 H=Protected.Player->GetHealth(),Poison=Protected.Player->GetPoisonLevel();
        Protected.Press(EKeys::LeftControl);
        TestEqual(TEXT("Invulnerability blocks all harmful potion health loss"),Protected.Player->GetHealth(),H);
        TestEqual(TEXT("Invulnerability also blocks poison accumulation"),Protected.Player->GetPoisonLevel(),Poison);
        TestEqual(TEXT("Blocked damage still consumes the used source potion"),Protected.Player->GetItemCount(Type),0);
        TestTrue(TEXT("Blocked damage still records actual use identification"),Protected.Player->HasIdentifiedItemType(Type));
    }
    FDangerousItemFixture Ward(nullptr,TEXT("?WadHealth=100"));
    if (!Ward.Valid()||!Ward.Cast(TEXT("924"))||!Ward.Player->CollectItem(827)||!Ward.Ready(827)) { return false; }
    const int32 Mana=Ward.Player->GetMana();
    Ward.Press(EKeys::LeftControl);
    TestTrue(TEXT("Death Ward keeps an overkilled potion user alive at zero"),!Ward.Player->IsDefeated()&&Ward.Player->GetHealth()==0);
    TestEqual(TEXT("Ward charges the entire typed health hit"),Ward.Player->GetMana(),Mana-1000);
    TestEqual(TEXT("Ward does not retain the used object"),Ward.Player->GetItemCount(827),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalLightningWandReady, "AAOriginal.WAD.Gameplay.LightningWandReady",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalLightningWandReady::RunTest(const FString& Parameters)
{
    FDangerousItemFixture F;
    if (!F.Valid()||!F.Wear(415,Ring1Slot)||!F.Wear(617,NeckSlot)||!F.Player->CollectItem(907)) { return false; }
    auto* P=F.Player;
    const int32 H=P->GetHealth();
    TestEqual(TEXT("Taking a wand into the retained mouse hand has no READY cost"),P->GetHealth(),H);
    const auto Hit=ResolvePlayerDamage(200,DamageFlag(EProjectileDamage::Electricity),P->GetResistanceMask(),0,P->GetArmorRating());
    if (!TestTrue(TEXT("Original ready-hand target activates the wand"),F.Ready(907))) { return false; }
    TestEqual(TEXT("READY uses electricity resistance and shielding"),P->GetHealth(),H-Hit.Health);
    TestEqual(TEXT("READY does not consume the source wand"),P->GetItemCount(907),1);
    const int32 Once=P->GetHealth();
    P->SwapMouseAndReady();
    TestEqual(TEXT("UNREADY causes no second damage charge"),P->GetHealth(),Once);
    P->SwapMouseAndReady();
    TestEqual(TEXT("A genuine new READY costs another typed hit"),P->GetHealth(),Once-Hit.Health);
    const int32 BeforeDispel=P->GetHealth();
    P->ReceiveTypedHit(1,SpecialDamageFlag(ESpecialDamage::Dispel));
    TestEqual(TEXT("Dispel reREADY charges even with no temporary spell to remove"),P->GetHealth(),BeforeDispel-Hit.Health);
    TestEqual(TEXT("Repeated triggers never duplicate the wand"),P->GetItemCount(907),1);
    TestFalse(TEXT("READY itself does not identify a wand through potion-use rules"),P->HasIdentifiedItemType(907));
    const int32 BeforeUseHealth=P->GetHealth(),BeforeUseMana=P->GetMana();
    if (!TestTrue(TEXT("The existing wand USE still runs through bound LeftControl"),F.Press(EKeys::LeftControl))) { return false; }
    TestEqual(TEXT("Wand USE spends its original firing mana"),P->GetMana(),BeforeUseMana-1000);
    TestEqual(TEXT("Wand USE does not repeat its READY damage"),P->GetHealth(),BeforeUseHealth);
    TestEqual(TEXT("Firing does not consume the wand"),P->GetItemCount(907),1);
    int32 Bolts=0;
    UClass* BoltClass=FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadBolt"));
    if (!TestNotNull(TEXT("Actual native projectile class exists"),BoltClass)) { return false; }
    for (TActorIterator<AActor> It(F.World); It; ++It) { Bolts+=It->IsA(BoltClass); }
    TestEqual(TEXT("Normal wand USE creates exactly one actual projectile"),Bolts,1);

    FDangerousItemFixture Plate(nullptr,TEXT("?WadClass=1"));
    if (!Plate.Valid()) { return false; }
    const int32 ArmorTypes[]={708,710,710,711,709,712};
    for (int32 Slot=0; Slot<EquipmentSlotCount; ++Slot)
    {
        if (!Plate.Player->CollectItem(ArmorTypes[Slot])||!Plate.Player->TakeInventoryItem(Plate.Player->GetCarriedItems().Num()-1)) { return false; }
        Plate.Player->ClickArmorSlot(Slot);
        TestEqual(TEXT("Real plate armor occupies its normal body target"),Plate.Player->GetMouseHandItem(),0);
    }
    TestEqual(TEXT("Source plate protection contributes its real weighted armor rating"),Plate.Player->GetArmorRating(),20);
    TestEqual(TEXT("A real plate body slot supplies electricity amplification class2"),Plate.Player->GetArmorClass(HeadSlot),2);
    if (!Plate.Player->CollectItem(907)) { return false; }
    // Fix the existing random armor-location roll on both the expected and real
    // typed paths. No world tick occurs during this normal take/ready action.
    FMath::RandInit(471);
    const int32 ArmorClass=Plate.Player->GetArmorClass(FMath::RandRange(0,EquipmentSlotCount-1));
    const auto PlateHit=ResolvePlayerDamage(200,DamageFlag(EProjectileDamage::Electricity),0,ArmorClass,Plate.Player->GetArmorRating());
    const int32 PlateH=Plate.Player->GetHealth();
    FMath::RandInit(471);
    if (!Plate.Ready(907)) { return false; }
    TestEqual(TEXT("READY electricity uses the real randomly selected armor class"),Plate.Player->GetHealth(),PlateH-PlateHit.Health);

    FDangerousItemFixture Protected;
    if (!Protected.Valid()||!Protected.Cast(TEXT("1547"))||!Protected.Player->CollectItem(907)||!Protected.Ready(907)) { return false; }
    TestEqual(TEXT("Invulnerability blocks the wand's READY damage"),Protected.Player->GetHealth(),10000);
    Protected.Player->ReceiveTypedHit(1,SpecialDamageFlag(ESpecialDamage::Dispel));
    TestEqual(TEXT("Dispel removes the sole temporary protection before reREADY damage"),Protected.Player->GetHealth(),9800);
    TestEqual(TEXT("The lightning wand survives Dispel"),Protected.Player->GetItemCount(907),1);
    FDangerousItemFixture Ward(nullptr,TEXT("?WadHealth=100"));
    if (!Ward.Valid()||!Ward.Cast(TEXT("924"))||!Ward.Player->CollectItem(907)) { return false; }
    const int32 WardMana=Ward.Player->GetMana();
    if (!Ward.Ready(907)) { return false; }
    TestTrue(TEXT("Ward protects a lethal typed READY without discarding its owner"),!Ward.Player->IsDefeated()&&Ward.Player->GetHealth()==0&&Ward.Player->GetReadyItem()==907);
    TestEqual(TEXT("The READY overkill charges Ward's full typed health amount"),Ward.Player->GetMana(),WardMana-200);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalDangerousItemOwnership, "AAOriginal.WAD.Gameplay.DangerousItemOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalDangerousItemOwnership::RunTest(const FString& Parameters)
{
    FDangerousItemFixture Fatal(nullptr,TEXT("?WadHealth=100"),true);
    if (!Fatal.Valid()||!Fatal.Player->CollectItem(825)||!Fatal.Player->CollectItem(827)||!Fatal.Ready(827)) { return false; }
    Fatal.Press(EKeys::LeftControl);
    TestTrue(TEXT("Normal unprotected lethal USE defeats the player"),Fatal.Player->IsDefeated());
    TestEqual(TEXT("The fatal potion is removed before defeat drops are dispatched"),Fatal.Player->GetItemCount(827),0);
    TestEqual(TEXT("The already-consumed potion does not also appear as a death drop"),Fatal.Drops(827),0);
    TestEqual(TEXT("A separate owned object proves the actual death-drop path ran"),Fatal.Drops(825),1);
    Fatal.Press(EKeys::LeftControl);
    TestEqual(TEXT("A second input after defeat cannot duplicate the other death drop"),Fatal.Drops(825),1);

    FDangerousItemFixture WandFatal(nullptr,TEXT("?WadHealth=100"),true);
    if (!WandFatal.Valid()||!WandFatal.Player->CollectItem(907)||!WandFatal.Ready(907)) { return false; }
    TestTrue(TEXT("Lethal READY follows the normal defeat transition"),WandFatal.Player->IsDefeated());
    TestEqual(TEXT("A nonconsumed lethal wand leaves inventory once"),WandFatal.Player->GetItemCount(907),0);
    TestEqual(TEXT("The nonconsumed lethal wand produces one death drop"),WandFatal.Drops(907),1);

    FDangerousItemFixture F;
    if (!F.Valid()||!F.Player->CollectItem(907)||!F.Ready(907)) { return false; }
    const FURL TownURL(nullptr,TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"),TRAVEL_Absolute);
    if (!F.World->SetGameMode(TownURL)) { return false; }
    const FString Slot=TEXT("NativeDangerousItemTest-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot,0); };
    if (!TestTrue(TEXT("Save real readied wand ownership through the checkpoint writer"),F.Player->SaveWadCheckpoint(Slot))) { return false; }
    const auto* Save=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    if (!TestNotNull(TEXT("Actual serialized checkpoint reloads"),Save)) { return false; }
    const FString Options=Save->LevelPackage+TEXT("?")+Save->PlayerOptions;
    FDangerousItemFixture Restored(*Options);
    if (!Restored.Valid()) { return false; }
    TestEqual(TEXT("Fresh actor checkpoint restoration retains ready identity"),Restored.Player->GetReadyItem(),907);
    TestEqual(TEXT("Native checkpoint reconstruction deliberately does not repeat READY damage"),Restored.Player->GetHealth(),F.Player->GetHealth());
    TestEqual(TEXT("Checkpoint restoration contains one wand"),Restored.Player->GetItemCount(907),1);
    Restored.Player->SwapMouseAndReady(); Restored.Player->SwapMouseAndReady();
    TestEqual(TEXT("Only a later actual reREADY charges the restored wand"),Restored.Player->GetHealth(),F.Player->GetHealth()-200);
    return true;
}
