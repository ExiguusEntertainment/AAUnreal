#include "NativeWadArtRequests.h"
#include "NativeWadCreatureArt.h"
#include "NativeWadScenerySource.h"
#include "Misc/Paths.h"
#include "NativeWadRules.h"

namespace
{
    struct FFixed {const TCHAR* Name;const TCHAR* Picture;};
    // Moved from real importer callsites; their producers query this same table.
    constexpr FFixed Fixed[]={
        {TEXT("Berry"),TEXT("BERI/BERI.CPC")},
        {TEXT("ChurchKey"),TEXT("KEY2/KEY2.CPC")},
        {TEXT("ChurchPiercingBolt"),TEXT("ARROW7/1.CPC")},
        {TEXT("ElfGuardian"),TEXT("ARCHER/ARCHER01.CPC")},
        {TEXT("ElfGuardianAttack"),TEXT("ARCHER/ARCDI01.CPC")},
        {TEXT("ElfGuardianDowned"),TEXT("ARCHER/ARCDI04.CPC")},
        {TEXT("ElfGuardianHurt"),TEXT("ARCHER/ARCHER05.CPC")},
        {TEXT("ElymasNotes"),TEXT("SCRL8/SCRL8.CPC")},
        {TEXT("FeatherfallPotion"),TEXT("POTION17/POTION17.CPC")},
        {TEXT("GaisericMagician"),TEXT("PHIL/PHIL01.CPC")},
        {TEXT("GaisericMagicianAttack"),TEXT("PHIL/BDDED02.CPC")},
        {TEXT("GaisericMagicianDowned"),TEXT("PHIL/BDDED04.CPC")},
        {TEXT("GaisericMagicianHurt"),TEXT("PHIL/BRDATK01.CPC")},
        {TEXT("GeneratedFlyingPotion"),TEXT("POTION05/POTION05.CPC")},
        {TEXT("GuardianBolt"),TEXT("ARROW1/BOLT2.CPC")},
        {TEXT("HomingFireball"),TEXT("HMISS6/1.CPC")},
        {TEXT("IronDagger"),TEXT("DAGGER/DAGGER.CPC")},
        {TEXT("MagicDart"),TEXT("MISSIL12/1.CPC")},
        {TEXT("OrbOfTheUndead"),TEXT("ORB6/ORB6.CPC")},
        {TEXT("ParalyzeMissile"),TEXT("EARTHBND/1.CPC")},
        {TEXT("PoisonMissile"),TEXT("MISSIL16/1.CPC")},
        {TEXT("WIZORNG"),TEXT("WIZORNG/OWWALK01.CPC")},
        {TEXT("WIZORNG_Attack"),TEXT("WIZORNG/OWA01.CPC")},
        {TEXT("WIZORNG_Down"),TEXT("WIZORNG/OWDIE08.CPC")},
        {TEXT("WIZORNG_Hurt"),TEXT("WIZORNG/OWDIE01.CPC")},
        {TEXT("AndrewNecklace"),TEXT("NECKLACE/NECKLACE.CPC")},
        {TEXT("ElmoreKey3"),TEXT("KEY3/KEY3.CPC")},
        {TEXT("ElmoreKey5"),TEXT("KEY5/KEY5.CPC")},
        {TEXT("ElmoreKey6"),TEXT("KEY6/KEY6.CPC")},
        {TEXT("ElmoreKey8"),TEXT("KEY8/KEY8.CPC")},
        {TEXT("JugurthasJournal"),TEXT("BOOK/BOOK.CPC")},
        {TEXT("Key1"),TEXT("KEY1/KEY1.CPC")},
        {TEXT("Key3"),TEXT("KEY3/KEY3.CPC")},
        {TEXT("Key4"),TEXT("KEY4/KEY4.CPC")},
        {TEXT("Key6"),TEXT("KEY6/KEY6.CPC")},
        {TEXT("Key8"),TEXT("KEY8/KEY8.CPC")},
        {TEXT("Key9"),TEXT("KEY9/KEY9.CPC")},
        {TEXT("OrbOfDefense"),TEXT("ORB5/ORB5.CPC")},
        {TEXT("OrbOfDestruction"),TEXT("ORB7/ORB7.CPC")},
        {TEXT("WandOfDisintegration"),TEXT("WAND18/WAND18.CPC")},
    };
}
bool ReadWadFixedArtRequest(const FString& Key,FWadArtRequest& Out)
{
    for(const auto& Entry:Fixed)
    {
        if(Key.Equals(Entry.Name,ESearchCase::IgnoreCase))
        {FWadArtRequest Parsed;Parsed.AssetName=Entry.Name;Parsed.Picture=Entry.Picture;Parsed.Role=TEXT("fixed_importer_binding");Out=MoveTemp(Parsed);return true;}
    }
    return false;
}
bool ResolveWadScriptMissileArt(int32 Type,FWadArtRequest& Out)
{
    const auto* Projectile=NativeWadRules::FindWandProjectileByType(Type);
    const TCHAR* Picture=Projectile?Projectile->Art:Type==4016?TEXT("EARTHBND/1.CPC")
        :Type==NativeWadRules::PoisonCrossbowBolt?TEXT("ARROW2/1.CPC"):nullptr;
    if(!Picture){return false;}FWadArtRequest Parsed;Parsed.AssetName=FString::Printf(TEXT("Missile%d"),Type);
    Parsed.Picture=Picture;Parsed.SourceTypes.Add(Type);Parsed.Role=TEXT("script_projectile");Out=MoveTemp(Parsed);return true;
}
bool ReadWadSharedProjectileArtRequests(TArray<FWadArtRequest>& Out)
{
    TArray<FWadArtRequest> Requests;
    FWadArtRequest Plasma;Plasma.AssetName=TEXT("PlasmaProjectile");Plasma.Picture=TEXT("PBALL/PBALL001.CPC");Plasma.Role=TEXT("plasma_projectile");Requests.Add(Plasma);
    for(const auto& Ammo:NativeWadRules::CrossbowAmmo)
    {FWadArtRequest Request;Request.AssetName=FString::Printf(TEXT("Ammo%d"),Ammo.Missile);Request.Picture=Ammo.Art;Request.SourceTypes.Add(Ammo.Missile);Request.Role=TEXT("crossbow_projectile");Requests.Add(MoveTemp(Request));}
    for(const auto& Projectile:NativeWadRules::WandProjectiles)
    {
        if(Projectile.Type==4017){continue;} // Exact original native producer exclusion.
        FWadArtRequest Request;Request.AssetName=FString::Printf(TEXT("Wand%d"),Projectile.Type);Request.Picture=Projectile.Art;
        Request.SourceTypes.Add(Projectile.Type);Request.Role=TEXT("wand_projectile");Requests.Add(MoveTemp(Request));
    }
    Out=MoveTemp(Requests);return true;
}
bool ReadWadSharedProjectileArtRequest(const FString& Key,FWadArtRequest& Out)
{
    TArray<FWadArtRequest> Requests;ReadWadSharedProjectileArtRequests(Requests);
    for(const auto& Request:Requests){if(Request.AssetName.Equals(Key,ESearchCase::IgnoreCase)){Out=Request;return true;}}return false;
}
bool ReadWadCreatureArtRequests(TArray<FWadArtRequest>& Out,FOutputDevice& Output)
{
    TArray<FWadArtRequest> Requests;TMap<FString,int32> ByName;
    const auto Add=[&](const FWadArtRequest& Request)
    {
        if(Request.AssetName.IsEmpty()){return true;}const FString Key=Request.AssetName.ToUpper();
        if(const int32* Existing=ByName.Find(Key))
        {
            auto& Old=Requests[*Existing];
            if(!Old.Picture.Equals(Request.Picture,ESearchCase::IgnoreCase)||Old.ColorTable!=Request.ColorTable||Old.bMirrored!=Request.bMirrored)
            {Output.Logf(TEXT("Ambiguous actual importer art request %s."),*Request.AssetName);return false;}
            for(int32 Type:Request.SourceTypes){Old.SourceTypes.AddUnique(Type);}Old.SourceTypes.Sort();return true;
        }
        ByName.Add(Key,Requests.Add(Request));return true;
    };
    const auto AddSet=[&](const FWadCreatureArtSet& Set)
    {return Add(Set.Standing)&&Add(Set.Hurt)&&Add(Set.Downed)&&Add(Set.Attacking)&&Add(Set.Bolt);};
    for(const auto& Source:GetWadCampaignCreatureSources())
    {FWadCreatureArtSet Set;if(!ResolveWadCampaignCreatureArt(Source,Set,Output)||!AddSet(Set)){return false;}}
    for(const auto& Source:GetWadRetreatCreatureSources())
    {FWadCreatureArtSet Set;if(!ResolveWadRetreatCreatureArt(Source,Set,Output)||!AddSet(Set)){return false;}}
    for(bool bElymas:{false,true}){FWadCreatureArtSet Set;if(!ResolveWadSpecialCreatureArt(bElymas,Set,Output)||!AddSet(Set)){return false;}}
    for(bool bMounted:{false,true}){FWadCreatureArtSet Set;if(!ResolveWadKnightArt(bMounted,Set,Output)||!AddSet(Set)){return false;}}
    for(const auto& Entry:Fixed){FWadArtRequest Request;if(!ReadWadFixedArtRequest(Entry.Name,Request)||!Add(Request)){return false;}}
    for(const auto& Projectile:NativeWadRules::WandProjectiles)
    {FWadArtRequest Request;if(!ResolveWadScriptMissileArt(Projectile.Type,Request)||!Add(Request)){return false;}}
    for(int32 Type:{4016,NativeWadRules::PoisonCrossbowBolt})
    {FWadArtRequest Request;if(!ResolveWadScriptMissileArt(Type,Request)||!Add(Request)){return false;}}
    TArray<FWadArtRequest> Shared;ReadWadSharedProjectileArtRequests(Shared);
    for(const auto& Request:Shared){if(!Add(Request)){return false;}}
    Requests.Sort([](const auto& A,const auto& B){return A.AssetName<B.AssetName;});Out=MoveTemp(Requests);return true;
}

bool ReadWadSceneryArtRequest(const FString& Resource,int32 PictureId,FWadArtRequest& Out)
{
    if(PictureId<0){return false;}
    TMap<int32,FString> Resources;FString Error;
    if(!NativeWadScenerySource::LoadOrdinaryCatalog(Resources,Error)){return false;}
    // The older real scenery producer also mounts four inventory decorations.
    // Keep its exact shared tables and INVDESC guard; never broaden this to an
    // arbitrary inventory resource whose generated name merely looks similar.
    const auto AddLegacy=[&](const TMap<int32,FString>& Legacy)
    {
        for(const auto& [Type,Name]:Legacy)
        {
            const FString Description=FPaths::ProjectDir()/FString::Printf(
                TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/INVDESC/DES%05d.TXT"),Type);
            if(!FPaths::FileExists(Description)){continue;}
            if(const auto* Existing=Resources.Find(Type);Existing&&*Existing!=Name){return false;}
            Resources.Add(Type,Name);
        }
        return true;
    };
    if(!AddLegacy(NativeWadScenerySource::GetLegacyBaseResources())
        ||!AddLegacy(NativeWadScenerySource::GetLegacyRetreatResources())){return false;}
    FWadArtRequest Request;const FString Canonical=Resource.ToUpper();
    for(const auto& [Type,Name]:Resources)
    {
        if(Name!=Canonical){continue;}
        if(Type<=0||Type>MAX_uint16){return false;}
        Request.SourceTypes.Add(Type);
    }
    if(Request.SourceTypes.IsEmpty()){return false;}
    FWadScenerySource Source;
    if(!NativeWadScenerySource::Load(Canonical,Source,Error)){return false;}
    bool bReferenced=false;
    for(const auto& Frame:Source.Frames)
    {for(int32 Signed:Frame.Pictures){if(FMath::Abs(Signed)==PictureId){bReferenced=true;}}}
    const FString* File=Source.PictureFiles.Find(PictureId);
    if(!bReferenced||!File){return false;}
    Request.AssetName=FString::Printf(TEXT("Scenery_%s_%d"),*Canonical,PictureId);
    Request.Picture=Canonical/(*File);
    Request.SourceTypes.Sort();
    Request.Role=TEXT("ordinary_scenery_unmirrored_texture_frame");
    // SpriteMaterial imports this same raw CPC once; each scenery view retains
    // the sign of its own INF picture reference when mounting the material.
    Request.bMirrored=false;
    Out=MoveTemp(Request);return true;
}
