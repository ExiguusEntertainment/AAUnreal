#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceNull.h"
#include "../../AAOriginal/Private/NativeWadCreatureArt.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCreatureArtSources,"AAOriginal.WAD.Art.SharedCreatureSourceRecipes",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalCreatureArtSources::RunTest(const FString& Parameters)
{
    FOutputDeviceNull Log;const auto Sources=GetWadCampaignCreatureSources();
    const auto Find=[&](int32 Type)->const FWadCampaignCreatureSource*
    {for(const auto& Source:Sources){if(Source.Type==Type){return &Source;}}return nullptr;};
    FWadCreatureArtSet Ulfilas,Jugurtha;
    const auto* U=Find(33787);const auto* J=Find(46075);
    if(!U||!J||!ResolveWadCampaignCreatureArt(*U,Ulfilas,Log)||!ResolveWadCampaignCreatureArt(*J,Jugurtha,Log)){return false;}
    TestEqual(TEXT("Existing full Ulfilas art name remains exact"),Ulfilas.Standing.AssetName,FString(TEXT("Druid33787")));
    TestEqual(TEXT("Existing full Jugurtha art name remains exact"),Jugurtha.Standing.AssetName,FString(TEXT("Druid46075")));
    TestEqual(TEXT("Both use the actual DRUID standing source frame"),Ulfilas.Standing.Picture,FString(TEXT("DRUID/DRUDW001.CPC")));
    TestEqual(TEXT("Packed Druid bolt retains established shared name"),Ulfilas.Bolt.AssetName,FString(TEXT("PackedDruid_Bolt")));
    TestEqual(TEXT("Packed Druid bolt has its own uncolored source picture"),Jugurtha.Bolt.Picture,FString(TEXT("MISSIL16/1.CPC")));
    TestTrue(TEXT("Full identity is preserved instead of masked base1019"),Ulfilas.Standing.SourceTypes.Num()==1&&Ulfilas.Standing.SourceTypes[0]==33787
        &&Jugurtha.Standing.SourceTypes.Num()==1&&Jugurtha.Standing.SourceTypes[0]==46075);
    const FString Root=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
    TArray<uint8> Table8,Table11;FFileHelper::LoadFileToArray(Table8,*(Root/TEXT("COLORIZE/COLORT08")));
    FFileHelper::LoadFileToArray(Table11,*(Root/TEXT("COLORIZE/COLORT11")));
    TestTrue(TEXT("Ulfilas exact original variant table"),Table8.Num()==256&&Ulfilas.Standing.ColorTable==Table8);
    TestTrue(TEXT("Jugurtha exact original variant table"),Table11.Num()==256&&Jugurtha.Attacking.ColorTable==Table11);
    TestTrue(TEXT("Actual imported bolt has no owner-color remapping"),Ulfilas.Bolt.ColorTable.IsEmpty()&&Jugurtha.Bolt.ColorTable.IsEmpty());
    TestFalse(TEXT("Current actual static frame is unmirrored"),Ulfilas.Standing.bMirrored);
    for(const auto& Source:Sources)
    {
        FWadCreatureArtSet Art;if(!TestTrue(TEXT("Every explicit source recipe resolves without actor creation"),ResolveWadCampaignCreatureArt(Source,Art,Log))){return false;}
        for(const FWadArtRequest* Request:{&Art.Standing,&Art.Hurt,&Art.Downed,&Art.Attacking,&Art.Bolt})
        {if(!Request->Picture.IsEmpty()){TestTrue(FString::Printf(TEXT("Actual source CPC exists: %s"),*Request->Picture),FPaths::FileExists(Root/TEXT("OBJS")/Request->Picture));}}
    }
    FWadCreatureArtSet Elymas,Mounted,Foot;
    if(!ResolveWadSpecialCreatureArt(true,Elymas,Log)||!ResolveWadKnightArt(true,Mounted,Log)||!ResolveWadKnightArt(false,Foot,Log)){return false;}
    TestTrue(TEXT("Elymas retains full50158 identity and original variant12"),Elymas.Standing.SourceTypes[0]==50158&&Elymas.Standing.ColorTable.Num()==256);
    TestEqual(TEXT("Mounted attack intentionally reuses standing material"),Mounted.Attacking.AssetName,Mounted.Standing.AssetName);
    TestEqual(TEXT("Foot attack uses its original distinct native name"),Foot.Attacking.AssetName,FString(TEXT("FootKnightAttack")));
    TestEqual(TEXT("Mounted native original projectile frame"),Mounted.Bolt.Picture,FString(TEXT("ARROW7/1.CPC")));
    const auto* Hydra=Find(37886);const auto* Mattan=Find(54268);FWadCreatureArtSet HA,MA;
    if(!Hydra||!Mattan||!ResolveWadCampaignCreatureArt(*Hydra,HA,Log)||!ResolveWadCampaignCreatureArt(*Mattan,MA,Log)){return false;}
    TestEqual(TEXT("New full Hydra request does not alias base-family bolt"),HA.Bolt.AssetName,FString(TEXT("Hydra37886_Bolt")));
    TestEqual(TEXT("Mattan request does not alias base-family bolt"),MA.Bolt.AssetName,FString(TEXT("Mattan54268_Bolt")));
    return true;
}
