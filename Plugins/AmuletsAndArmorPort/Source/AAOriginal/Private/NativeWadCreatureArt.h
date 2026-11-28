#pragma once
#include "CoreMinimal.h"
#include "NativeWadArtRequests.h"

struct FWadCampaignCreatureSource
{
    int32 Type;
    const TCHAR* Name;
    const TCHAR* Resource;
    const TCHAR* Standing;
    const TCHAR* Hurt;
    const TCHAR* Downed;
    const TCHAR* Attacking;
    const TCHAR* Bolt;
    int32 Health, Retreat, Melee, MeleeRange, Radius, Step, Vision, BoltDamage;
    const TCHAR* OnlyMap = nullptr;
    int32 HomingType = 0;
    const TCHAR* ArtName = nullptr;
    bool bGameplayEnabled = true;
};
struct FWadRetreatCreatureSource
{
    int32 Type;
    const TCHAR* Name;
    const TCHAR* Resource;
    const TCHAR* Standing;
    const TCHAR* Hurt;
    const TCHAR* Downed;
    const TCHAR* Attacking;
    int32 Health, RetreatHealth, MeleeDamage, MeleeRange, Radius, Step, UpdateTicks, Vision;
    int32 MinDartRange, MaxDartRange, DartDelay;
};
struct FWadCreatureArtSet { FWadArtRequest Standing,Hurt,Downed,Attacking,Bolt; };
AAORIGINAL_API TConstArrayView<FWadCampaignCreatureSource> GetWadCampaignCreatureSources();
AAORIGINAL_API TConstArrayView<FWadRetreatCreatureSource> GetWadRetreatCreatureSources();
AAORIGINAL_API bool ResolveWadCampaignCreatureArt(const FWadCampaignCreatureSource&,FWadCreatureArtSet&,FOutputDevice&);
AAORIGINAL_API bool ResolveWadRetreatCreatureArt(const FWadRetreatCreatureSource&,FWadCreatureArtSet&,FOutputDevice&);
AAORIGINAL_API bool ResolveWadSpecialCreatureArt(bool bElymas,FWadCreatureArtSet&,FOutputDevice&);
AAORIGINAL_API bool ResolveWadKnightArt(bool bMounted,FWadCreatureArtSet&,FOutputDevice&);
