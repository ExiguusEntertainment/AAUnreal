#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Internationalization/Regex.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "NativeWadArt.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadCreatureArt.h"

namespace
{
    const int32 Types[]={16368,24560,33803,37886,54268};
    FString Original(){return FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor");}
    bool Record(int32 Type,TArray<FString>& Fields)
    {
        FString S;if(!FFileHelper::LoadFileToString(S,*(Original()/TEXT("Include/CREDATA.H")))){return false;}
        FRegexMatcher M(FRegexPattern(FString::Printf(TEXT("/\\*[^*]*\\*/\\s*(%d\\s*,[^}]+)"),Type)),S);if(!M.FindNext()){return false;}
        M.GetCaptureGroup(1).TrimStartAndEnd().ParseIntoArray(Fields,TEXT(","),true);for(auto& F:Fields){F.TrimStartAndEndInline();}return Fields.Num()==43;
    }
    const FWadCampaignCreatureSource* Source(int32 Type)
    {for(const auto& S:GetWadCampaignCreatureSources()){if(S.Type==Type){return &S;}}return nullptr;}
    struct FFixture
    {
        double Volume=FApp::GetVolumeMultiplier();UGameInstance* Instance=NewObject<UGameInstance>(GEngine);
        UWorld* World=nullptr;ANativeWadLevel* Level=nullptr;ANativeWadPlayer* Player=nullptr;
        explicit FFixture(bool Water=false)
        {
            FApp::SetVolumeMultiplier(0.0);Instance->InitializeStandalone();World=Instance->GetWorld();if(!World){return;}
            World->URL=FURL(nullptr,TEXT("/Game/Maps/L61?WadCreated=1?WadClass=0?WadLevel=1?WadMaxHealth=30000?WadHealth=30000?WadMaxMana=30000?WadMana=30000?WadFood=2000?WadWater=2000?WadItems=?WadReady=-1"),TRAVEL_Absolute);
            Level=World->SpawnActor<ANativeWadLevel>();if(!Level){return;}
            const auto* P=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));if(!P){return;}
            auto& S=P->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level)->AddDefaulted_GetRef();S.bWater=Water;
            S.RepairedBoundaryPoints={{-1200,-1200},{1200,-1200},{1200,-1200},{1200,1200},{1200,1200},{-1200,1200},{-1200,1200},{-1200,-1200}};
            S.Floor=NewObject<UStaticMeshComponent>(Level);S.Ceiling=NewObject<UStaticMeshComponent>(Level);
            S.Floor->SetupAttachment(Level->GetRootComponent());S.Ceiling->SetupAttachment(Level->GetRootComponent());S.Ceiling->SetRelativeLocation(FVector(0,0,256));
            S.Floor->RegisterComponent();S.Ceiling->RegisterComponent();Level->SetSectorFloorZ(0,0);
            Player=World->SpawnActor<ANativeWadPlayer>(FVector(500,0,34),FRotator::ZeroRotator);auto* C=World->SpawnActor<APlayerController>();if(!Player||!C){return;}
            World->AddController(C);C->Possess(Player);Player->DispatchBeginPlay();
            Player->GetCharacterMovement()->Activate(true);
        }
        ~FFixture(){Instance->Shutdown();if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}FApp::SetVolumeMultiplier(Volume);}
        ANativeWadGuardian* Enemy(int32 Type,double Y=0)
        {
            const double Half=Type==37886||Type==1022?60.0:Type==33803?29.5:Type==54268?31.5:31.0;
            auto* E=World->SpawnActor<ANativeWadGuardian>(FVector(0,Y,Half+2),FRotator::ZeroRotator);if(!E){return nullptr;}
            FWadEncounterProfile P;P.SourceType=Type;P.CollisionHalfHeight=Half;P.SpriteSize=FVector2D(80,Half*2);
            auto* M=UMaterial::GetDefaultMaterial(MD_Surface);E->Configure(nullptr,M,M,M,M,M,P);
            E->Tags.Add(*FString::Printf(TEXT("PackedFamily%d"),Type));E->DispatchBeginPlay();return E;
        }
        TArray<ANativeWadPickup*> Drops()const
        {TArray<ANativeWadPickup*> Out;for(TActorIterator<ANativeWadPickup> I(World);I;++I){if(I->IsDropped()&&!I->IsActorBeingDestroyed()){Out.Add(*I);}}return Out;}
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedFamilySource,"AAOriginal.WAD.Gameplay.PackedEncounterSource",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedFamilySource::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FFixture Fixture;if(!Fixture.Player){return false;}
    for(int32 Type:Types)
    {
        TArray<FString> F;if(!Record(Type,F)){return false;}
        auto* Actor=Fixture.Enemy(Type);if(!Actor){return false;}const auto& P=Actor->GetEncounterProfile();
        TestEqual(TEXT("Full source identity is not masked"),P.SourceType,FCString::Atoi(*F[0]));
        TestTrue(TEXT("Source families are grounded and blocked by walls"),F[3]==TEXT("FALSE")&&F[4]==TEXT("FALSE")&&!P.bCanFly);
        TestEqual(TEXT("Source update period"),P.UpdateTicks,FCString::Atoi(*F[7]));
        TestEqual(TEXT("Source walking step conversion"),P.WalkSpeed,float(FCString::Atoi(*F[8])*70/(P.UpdateTicks+(P.UpdateTicks>>1))));
        TestEqual(TEXT("Source melee range"),P.MeleeRange,float(FCString::Atoi(*F[9])));TestEqual(TEXT("Source minimum missile range"),P.MinBoltRange,float(FCString::Atoi(*F[10])));
        TestEqual(TEXT("Source maximum missile range"),P.BoltRange,FCString::Atoi(*F[11]));TestEqual(TEXT("Source melee delay"),P.MeleeDelayUpdates,FCString::Atoi(*F[12]));
        TestEqual(TEXT("Source missile delay"),P.RangedDelayUpdates,FCString::Atoi(*F[13]));TestEqual(TEXT("Source health"),P.MaxHealth,FCString::Atoi(*F[14]));
        TestEqual(TEXT("Source regeneration"),P.RegenPerUpdate,FCString::Atoi(*F[15]));TestEqual(TEXT("Exact source missile"),P.SourceMissileType,FCString::Atoi(*F[16]));
        TestEqual(TEXT("Source melee strength"),P.MeleeDamage,FCString::Atoi(*F[18]));TestEqual(TEXT("Source retreat health"),P.RetreatHealth,FCString::Atoi(*F[21]));
        TestEqual(TEXT("Source creature combat acceptance"),P.ChanceAttackCreature,FCString::Atoi(*F[38]));TestEqual(TEXT("Source continuation probability"),P.ChanceContinueAttack,FCString::Atoi(*F[39]));
        const bool Plate=Type==16368||Type==24560||Type==33803;
        TestEqual(TEXT("Actual CREDATA armor type is respected"),P.ArmorClass,Plate?2:0);
        TestTrue(TEXT("Plate mapping is proven by source enum"),!Plate||F[22]==TEXT("EQUIP_ARMOR_TYPE_BREASTPLATE_PLATE"));
        TestEqual(TEXT("Only sourceDIPPER retains dipper navigation"),P.bDipper,Type==37886);
        const auto* S=Source(Type);if(!TestNotNull(TEXT("Shared enabled recipe used by actual importer"),S)){return false;}TestTrue(TEXT("Gameplay enablement follows runtime implementation"),S->bGameplayEnabled);
        FWadCreatureArtSet Art;if(!ResolveWadCampaignCreatureArt(*S,Art,*GLog)){return false;}TArray<uint8> Table;
        if(!FFileHelper::LoadFileToArray(Table,*(Original()/TEXT("Resources/ArtFinal/COLORIZE")/FString::Printf(TEXT("COLORT%02d"),Type>>12)))){return false;}
        TestTrue(TEXT("Actual importer and manifest share the exact source variant bytes"),Art.Standing.ColorTable==Table&&Art.Hurt.ColorTable==Table&&Art.Downed.ColorTable==Table&&Art.Attacking.ColorTable==Table);
        TestFalse(TEXT("Source static front frame is not guessed mirrored"),Art.Standing.bMirrored);
        TestTrue(TEXT("Art provenance retains full identity"),Art.Standing.SourceTypes.Contains(Type));
        if(S->Bolt){TestTrue(TEXT("Projectile art is independently source-colored"),Art.Bolt.ColorTable.IsEmpty());}
        Actor->Destroy();
    }
    TestEqual(TEXT("Widened arithmetic retains source68% at maximum public damage"),
        CreaturePiercingBonus(MAX_int32,2,65535),int32(int64(MAX_int32)*68/100));
    for(int32 Color=0;Color<16;++Color)
    {
        const int32 Full=4080+(Color<<12);
        TestEqual(TEXT("Source chain piercing scales2% per color-adjusted armor level"),CreaturePiercingBonus(199,1,Full),199*2*(2+Color)/100);
        TestEqual(TEXT("Source plate piercing scales4% per color-adjusted armor level"),CreaturePiercingBonus(199,2,Full),199*4*(2+Color)/100);
        TestEqual(TEXT("Cloth receives no palette piercing bonus"),CreaturePiercingBonus(199,0,Full),0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedFamilyTyped,"AAOriginal.WAD.Gameplay.PackedEncounterTypedDamage",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedFamilyTyped::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FFixture F;if(!F.Player){return false;}
    for(int32 Type:Types)
    {
        auto* E=F.Enemy(Type);if(!E){return false;}int32 HP=E->GetEncounterHealth();
        const int32 Bonus=CreaturePiercingBonus(100,E->GetEncounterProfile().ArmorClass,Type);
        TestTrue(TEXT("Actual creature resolver accepts source piercing"),E->TakeHit(100,nullptr,DamageFlag(EProjectileDamage::Piercing)));
        TestEqual(TEXT("Source color-dependent bonus reaches actual health"),E->GetEncounterHealth(),HP-100-Bonus);
        E->ResetForGenerator();HP=E->GetEncounterHealth();
        const uint8 Resist=Type==37886?DamageFlag(EProjectileDamage::Fire):Type==54268?DamageFlag(EProjectileDamage::ManaDrain):0;
        if(Resist){TestFalse(TEXT("Exact family resistance blocks real typed injury"),E->TakeHit(100,nullptr,Resist));TestEqual(TEXT("Resisted hit keeps health"),E->GetEncounterHealth(),HP);}
        if(Type==37886){TestTrue(TEXT("Obsidian Hydra does not inherit ordinary Hydra poison resistance"),E->TakeHit(100,nullptr,DamageFlag(EProjectileDamage::Poison)));TestEqual(TEXT("Unresisted poison transfers normally"),E->GetPoisonLevel(),10);}
        E->Destroy();
    }
    // Source0 and base4080 preserve the previous plate8% and chain4% conversions.
    for(int32 Type:{0,4080}){TestEqual(TEXT("Base palette0 plate behavior preserved"),CreaturePiercingBonus(100,2,Type),8);TestEqual(TEXT("Base palette0 chain behavior preserved"),CreaturePiercingBonus(100,1,Type),4);}
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedFamilyTreasure,"AAOriginal.WAD.Gameplay.PackedEncounterTreasure",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedFamilyTreasure::RunTest(const FString& Parameters)
{
    FString SourceText;if(!FFileHelper::LoadFileToString(SourceText,*(Original()/TEXT("Source/CRELOGIC.C")))){return false;}
    const TMap<int32,FString> Names={{16368,TEXT("bronzeEvilKnightList")},{24560,TEXT("silverEvilKnightList")},{33803,TEXT("mithrilBadKnightList")}};
    for(const auto& [Type,Name]:Names)
    {
        FRegexMatcher M(FRegexPattern(Name+TEXT("\\[12\\]\\s*=\\s*\\{([^}]+)\\}")),SourceText);if(!M.FindNext()){return false;}
        TArray<FString> Fields;M.GetCaptureGroup(1).ParseIntoArray(Fields,TEXT(","),true);if(Fields.Num()!=12){return false;}
        for(int32 Roll=0;Roll<=65535;++Roll)
        {if(ResolveWadCreatureTreasure(Type,uint16(Roll)).ItemType!=FCString::Atoi(*Fields[Roll%12].TrimStartAndEnd())){AddError(TEXT("Full16-bit resolver input differs; source RNG sequence remains separate"));return false;}}
    }
    TestEqual(TEXT("Mattan releases original mana-drain wand908"),ResolveWadCreatureTreasure(54268,0).ItemType,908);
    TestEqual(TEXT("Source Obsidian Hydra has no invented ordinary treasure"),ResolveWadCreatureTreasure(37886,0).ItemType,0);
    FFixture F;if(!F.Player){return false;}
    for(int32 Type:{16368,24560,33803,54268})
    {
        auto* E=F.Enemy(Type);if(!E){return false;}const int32 Life=E->GetCreatureLife();
        TestTrue(TEXT("Ordinary source damage defeats actual family actor"),E->TakeHit(30000));
        auto Drops=F.Drops();if(!TestEqual(TEXT("One catalog-backed ordinary item per life"),Drops.Num(),1)){return false;}
        const int32 Item=Drops[0]->GetItemType();const auto* Art=Drops[0]->FindComponentByClass<UStaticMeshComponent>();TestTrue(TEXT("Every new table has real world art"),Art&&Art->GetMaterial(0));
        E->TakeHit(30000);FWadLevelCheckpoint Saved;if(!F.Level->WriteCheckpoint(Saved)){return false;}
        if(!TestTrue(TEXT("Restore actual defeated life/drop without reroll"),F.Level->RestoreCheckpoint(Saved))){return false;}
        TestEqual(TEXT("Saved ordinary item identity survives"),F.Drops()[0]->GetItemType(),Item);TestEqual(TEXT("Restore creates no duplicate"),F.Drops().Num(),1);
        const int32 Before=F.Player->GetItemCount(Item);TestTrue(TEXT("Normal world pickup collection handles exact packed type"),F.Drops()[0]->Collect(*F.Player));
        if(NativeWadRules::CoinValueCopper(Item)==0){TestEqual(TEXT("Equipment enters inventory under its full type"),F.Player->GetItemCount(Item),Before+1);F.Player->ConsumeItem(Item);}
        E->ResetForGenerator();E->SetSpawned(true);TestEqual(TEXT("Reuse creates a new life"),E->GetCreatureLife(),Life+1);
        TestTrue(TEXT("New life releases one source item again"),E->TakeHit(30000));TestEqual(TEXT("Exactly one new ordinary drop"),F.Drops().Num(),1);
        for(auto* Drop:F.Drops()){Drop->Destroy();}E->Destroy();
    }
    // Every source branch, not just the branch selected during the actual deaths, must be collectible.
    TSet<int32> Items;
    for(int32 Type:{16368,24560,33803,54268}){for(uint16 Roll=0;Roll<12;++Roll){Items.Add(ResolveWadCreatureTreasure(Type,Roll).ItemType);}}
    for(int32 Type:Items)
    {
        auto* Drop=ANativeWadPickup::SpawnDropped(*F.Level,Type,FVector(0,0,32));if(!TestNotNull(TEXT("All branch catalogs/rules/world art exist"),Drop)){return false;}
        TestTrue(TEXT("Actual ordinary collection supports every new packed/currency branch"),Drop->Collect(*F.Player));
        if(NativeWadRules::CoinValueCopper(Type)==0){F.Player->ConsumeItem(Type);}
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedDipperCheckpoint,"AAOriginal.WAD.Gameplay.PackedDipperCheckpoint",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedDipperCheckpoint::RunTest(const FString& Parameters)
{
    FFixture F(true);if(!F.Player){return false;}TArray<ANativeWadGuardian*> Actors;
    for(int32 Type:{1022,37886})
    {
        auto* E=F.Enemy(Type,Type==1022?-200:200);if(!E){return false;}E->RestoreEncounterHealth(0);E->Tick(3.0f);
        TestTrue(TEXT("Completed source sink death removes base and packed bodies"),E->IsDefeated()&&E->IsHidden());
        TestTrue(TEXT("Original120-pixel body leaves34 pixels before being removed"),FMath::IsNearlyEqual(E->GetDipDepth(),86.0,0.01));Actors.Add(E);
    }
    FWadLevelCheckpoint Saved;if(!TestTrue(TEXT("Write both hidden defeated source lives"),F.Level->WriteCheckpoint(Saved))){return false;}
    TestEqual(TEXT("Actual bDipper property, not one hardcoded type, controls hidden-life persistence"),Saved.Encounters.Num(),2);
    UClass* C=FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadCheckpoint"));if(!C){return false;}
    auto* Save=static_cast<UNativeWadCheckpoint*>(NewObject<USaveGame>(GetTransientPackage(),C));Save->Level=Saved;TArray<uint8> Bytes;
    if(!UGameplayStatics::SaveGameToMemory(Save,Bytes)){return false;}auto* Loaded=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromMemory(Bytes));if(!Loaded){return false;}
    for(auto* E:Actors){E->ResetForGenerator();E->SetSpawned(true);E->SetActorLocation(FVector(0,E->GetActorLocation().Y,62));TestFalse(TEXT("Generator reuse restores visibility"),E->IsHidden());}
    if(!TestTrue(TEXT("Actual serialized checkpoint reopens both source sink deaths"),F.Level->RestoreCheckpoint(Loaded->Level))){return false;}
    for(auto* E:Actors){TestTrue(TEXT("Reopen derives hidden corpse state without resurrecting it"),E->IsDefeated()&&E->IsHidden());}
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedFamilyMissiles,"AAOriginal.WAD.Gameplay.PackedEncounterMissiles",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedFamilyMissiles::RunTest(const FString& Parameters)
{
    for(int32 Type:{37886,54268})
    {
        FFixture F(true);if(!F.Player){return false;}F.Player->SetActorLocation(FVector(400,0,34));auto* E=F.Enemy(Type);if(!E){return false;}
        if(Type==37886){E->RestoreEncounterHealth(100);} // The source dipper fires only while retreating.
        E->Tick(0.01f);E->Tick(1.0f);TestEqual(TEXT("Existing source AI launches the restored family projectile"),E->GetBoltsFired(),1);
        AActor* Bolt=nullptr;for(TActorIterator<AActor> I(F.World);I;++I){if(I->GetClass()->GetFName()==TEXT("NativeWadBolt")){Bolt=*I;break;}}
        if(!TestNotNull(TEXT("Actual projectile actor"),Bolt)){return false;}
        const int32 HP=F.Player->GetHealth(),MP=F.Player->GetMana();
        F.Player->SetActorLocation(FVector(400,100,34));Bolt->Tick(0.05f);TestTrue(TEXT("These source projectiles are straight, never homing aliases"),FMath::IsNearlyZero(Bolt->GetActorLocation().Y,0.001));
        F.Player->SetActorLocation(FVector(400,0,34));Bolt->Tick(0.3f);TestTrue(TEXT("Real sphere sweep reaches the ordinary player capsule"),Bolt->IsActorBeingDestroyed());
        if(Type==54268){TestEqual(TEXT("Mattan's original4001 drains1000 mana"),F.Player->GetMana(),MP-1000);TestEqual(TEXT("Mana damage follows original typed health damage"),F.Player->GetHealth(),HP-1000);}
        else
        {
            TestEqual(TEXT("Source Pull is not ordinary poison/health damage"),F.Player->GetHealth(),HP);TestEqual(TEXT("Pull does not invent mana damage"),F.Player->GetMana(),MP);
            TestTrue(TEXT("Isolated fixture activates real movement for impulse accumulation"),F.Player->GetCharacterMovement()->IsActive());
            F.Player->GetCharacterMovement()->ApplyAccumulatedForces(0.01f);
            TestTrue(TEXT("Actual2500 Pull blast accelerates player toward its impact origin"),F.Player->GetVelocity().X<0.0);
        }
    }
    return true;
}

namespace
{
    bool ExpectedPixels(const FWadArtRequest& Request,EWadArtPalette Domain,TArray<FColor>& Out,FIntPoint& Size)
    {
        const FString Art=Original()/TEXT("Resources/ArtFinal");TArray<uint8> Cpc;TArray<FColor> Palette;
        if(!FFileHelper::LoadFileToArray(Cpc,*(Art/TEXT("OBJS")/Request.Picture))||Cpc.Num()<4
            ||!NativeWadArt::LoadPalette(Domain,Palette)||Palette.Num()!=256){return false;}
        const auto U16=[&](int32 I){return int32(Cpc[I])|int32(Cpc[I+1])<<8;};Size={U16(2),U16(0)};
        if(Size.X<=0||Size.Y<=0||Cpc.Num()<4+Size.X*4){return false;}Out.Init(FColor(0,0,0,0),Size.X*Size.Y);
        for(int32 X=0;X<Size.X;++X)
        {
            const int32 I=4+X*4,At=U16(I),Top=Cpc[I+2],Bottom=Cpc[I+3];if(Top==255&&Bottom==255){continue;}
            if(Top>Bottom||Bottom>=Size.Y||At<4+Size.X*4||At+Bottom-Top>=Cpc.Num()){return false;}
            for(int32 Y=Top;Y<=Bottom;++Y)
            {
                const uint8 Raw=Cpc[At+Y-Top];if(!Raw){continue;}const int32 Index=Request.ColorTable.IsEmpty()?Raw:Request.ColorTable[Raw];
                Out[Y*Size.X+X]=Palette[Index];
            }
        }
        return true;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedFamilyImported,"AAOriginal.WAD.Import.PackedEncounterArtAndCounts",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedFamilyImported::RunTest(const FString& Parameters)
{
    const TMap<FString,TArray<int32>> Maps={{TEXT("L153"),{0,0,0,2,0}},{TEXT("L231"),{10,1,0,0,0}},{TEXT("L75"),{7,0,0,0,0}},
        {TEXT("L10"),{0,15,0,0,0}},{TEXT("L51"),{0,0,22,0,0}},{TEXT("L44"),{0,0,0,5,0}},{TEXT("L61"),{0,0,0,3,1}}};
    int32 Total=0;
    for(const auto& [Map,Expected]:Maps)
    {
        auto* World=LoadObject<UWorld>(nullptr,*(TEXT("/Game/Maps/")+Map+TEXT(".")+Map));if(!TestNotNull(TEXT("Saved native map after bounded encounter refresh"),World)){return false;}
        TMap<int32,int32> Counts;TSet<FName> Tags;
        for(TActorIterator<ANativeWadGuardian> I(World);I;++I)
        {
            const int32 Type=I->GetSourceType();if(!MakeArrayView(Types).Contains(Type)){continue;}
            ++Counts.FindOrAdd(Type);++Total;if(!TestFalse(TEXT("Every restored actor has a checkpoint identity"),I->Tags.IsEmpty())){return false;}
            TestFalse(TEXT("Primary identities do not duplicate"),Tags.Contains(I->Tags[0]));Tags.Add(I->Tags[0]);
            TestEqual(TEXT("Look retains full packed identity"),I->GetLookSourceType(),Type);TestNotNull(TEXT("Actor retains valid visible art"),I->GetPortraitMaterial());
        }
        for(int32 Index=0;Index<5;++Index)
        {
            const int32 Type=Types[Index];TestEqual(TEXT("Exact source placement count"),Counts.FindRef(Type),Expected[Index]);if(!Expected[Index]){continue;}
            const auto* S=Source(Type);if(!S){return false;}FWadCreatureArtSet Art;if(!ResolveWadCampaignCreatureArt(*S,Art,*GLog)){return false;}
            const FWadArtRequest* Requests[]={&Art.Standing,&Art.Hurt,&Art.Downed,&Art.Attacking,&Art.Bolt};
            for(const auto* R:Requests)
            {
                if(R->Picture.IsEmpty()){continue;}
                const FString Name=TEXT("M_")+R->AssetName;
                auto* Material=LoadObject<UMaterialInterface>(nullptr,*(TEXT("/Game/Maps/")+Map+TEXT("_Assets/Things/")+Name+TEXT(".")+Name));
                if(!TestNotNull(TEXT("Shared actual-importer recipe produced saved original art"),Material)){return false;}
                // Saved editor graph is authoritative under NullRHI, where an
                // inactive SM5 shader-cache query need not list any textures.
                UTexture2D* Texture=nullptr;
                for(const auto& Expression:Material->GetMaterial()->GetExpressionCollection().Expressions)
                {if(auto* Sample=Cast<UMaterialExpressionTextureSample>(Expression))
                    {if(auto* Candidate=Cast<UTexture2D>(Sample->Texture)){Texture=Candidate;break;}}}
                if(!TestNotNull(TEXT("Saved original material contains its source texture sample"),Texture)){return false;}
                const auto* Provenance=NativeWadArt::GetSource(*Texture);
                if(!TestNotNull(TEXT("Rendered texture records its real original source/palette domain"),Provenance)){return false;}
                TestEqual(TEXT("Exact resource path is source-proven, never inferred fromRGB"),Provenance->ResourcePath,TEXT("OBJS/")+R->Picture);
                TestTrue(TEXT("Original variant table is retained after world-palette materialization"),Provenance->ColorTable==R->ColorTable);
                TArray<FColor> Pixels;FIntPoint Size;TArray64<uint8> Actual;
                if(!ExpectedPixels(*R,Provenance->Palette,Pixels,Size)||!Texture->Source.GetMipData(Actual,0)){return false;}
                TestEqual(TEXT("Original padded width"),Texture->Source.GetSizeX(),int64(Size.X));TestEqual(TEXT("Original padded height"),Texture->Source.GetSizeY(),int64(Size.Y));
                TestTrue(TEXT("Saved stance/projectile pixels match source CPC plus exact authored variant table"),Actual.Num()==Pixels.Num()*int64(sizeof(FColor))
                    &&FMemory::Memcmp(Actual.GetData(),Pixels.GetData(),Actual.Num())==0);
            }
        }
    }
    TestEqual(TEXT("All66 exact authored encounters restored"),Total,66);return true;
}
