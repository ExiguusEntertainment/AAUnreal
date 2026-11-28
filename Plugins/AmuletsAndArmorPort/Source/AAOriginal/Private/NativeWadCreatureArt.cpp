#include "NativeWadCreatureArt.h"
#include "NativeWadRules.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

TConstArrayView<FWadCampaignCreatureSource> GetWadCampaignCreatureSources()
{
    constexpr int32 Apollo=1010;
    constexpr int32 Boulder=1026;
    static constexpr FWadCampaignCreatureSource Creatures[] = {
        {4080, TEXT("Evil Knight"), TEXT("EVILKNT"), TEXT("EKNTW001.CPC"), TEXT("EKNTD001.CPC"),
            TEXT("EKNTD008.CPC"), TEXT("EKNTA001.CPC"), nullptr, 3000, 300, 400, 30, 15, 10, 180, 0},
        {1025, TEXT("Electric Skeleton"), TEXT("NEWSKEL"), TEXT("SKWK1A.CPC"), TEXT("SKDY1A.CPC"),
            TEXT("SKDY1F.CPC"), TEXT("SKAT1A.CPC"), TEXT("MISSIL12/1.CPC"), 3000, 500, 600, 30, 15, 10, 180, 100},
        {1035, TEXT("Bad Knight"), TEXT("BADKNIT"), TEXT("BADBOYA1.CPC"), TEXT("BADBOYJ1.CPC"),
            TEXT("BADBOYJ4.CPC"), TEXT("BADBOYF1.CPC"), nullptr, 5000, 500, 700, 35, 25, 10, 180, 0},
        {Apollo, TEXT("Apollo"), TEXT("APOLLO"), TEXT("APCR001.CPC"), TEXT("APCR001.CPC"),
            TEXT("APCR001.CPC"), TEXT("APA001.CPC"), TEXT("PBALL/PBALL001.CPC"),
            30000, 500, 150, 30, 50, 10, 315, NativeWadRules::PlasmaDamage, TEXT("L72")},
        {1011, TEXT("Legionary"), TEXT("ROMAN"), TEXT("RMNWLK01.CPC"), TEXT("ROMNDI01.CPC"),
            TEXT("ROMNDI04.CPC"), TEXT("RMNATK01.CPC"), nullptr, 6000, 500, 600, 55, 20, 10, 135, 0},
        {Boulder, TEXT("Boulder"), TEXT("BOULDER"), TEXT("BOLDR001.CPC"), TEXT("BOLDR001.CPC"),
            TEXT("BOLDR001.CPC"), TEXT("BOLDR001.CPC"), nullptr, 5000, 0, 10000, 0, 60, 30, 0, 0},
        {1002, TEXT("Blue Wizard"), TEXT("REDWIZ"), TEXT("NWIZW001.CPC"), TEXT("NWIZD001.CPC"),
            TEXT("NWIZD08.CPC"), TEXT("NWIZA01.CPC"), TEXT("MISSILE5/1.CPC"), 3000, 1000, 0, 35, 20, 12, 315, 500},
        {1006, TEXT("Evil Orange Wizard"), TEXT("WIZEVL"), TEXT("WIZEVL8.CPC"), TEXT("WIZHRT1.CPC"),
            TEXT("WIZEVL7G.CPC"), TEXT("WIZEVL1.CPC"), TEXT("HMISS6/1.CPC"),
            3000, 500, 400, 60, 20, 12, 315, 1000, nullptr, 4020},
        {1005, TEXT("Wyvern"), TEXT("DRAGON"), TEXT("DSTF.CPC"), TEXT("DHTF.CPC"),
            TEXT("DIE06.CPC"), TEXT("DATF.CPC"), TEXT("MISSILE5/1.CPC"), 16000, 0, 1000, 92, 55, 15, 270, 500},
        {1021, TEXT("Orange Dragon"), TEXT("DRAGON2"), TEXT("DRFLY01.CPC"), TEXT("DRFLY04.CPC"),
            TEXT("DRGDD08.CPC"), TEXT("DRFLY01.CPC"), TEXT("MISSILE6/1.CPC"), 20000, 0, 1000, 90, 50, 7, 270, 750},
        {1037, TEXT("Momma Dragon"), TEXT("DRAGON4"), TEXT("D1F001.CPC"), TEXT("D1F006.CPC"),
            TEXT("D1G001.CPC"), TEXT("D1F008.CPC"), TEXT("MISSILE6/1.CPC"), 30000, 0, 400, 90, 80, 7, 270, 750},
        {1022, TEXT("Hydra"), TEXT("HYDRA"), TEXT("HYW001.CPC"), TEXT("HYDD001.CPC"),
            TEXT("HYDD004.CPC"), TEXT("HYA001.CPC"), TEXT("MISSIL16/1.CPC"), 8000, 500, 450, 72, 42, 10, 90, 250},
        {1012, TEXT("Banshee"), TEXT("BANSHEE"), TEXT("BANW001.CPC"), TEXT("BAND001.CPC"),
            TEXT("BAND008.CPC"), TEXT("BANA001.CPC"), nullptr, 5000, 500, 100, 250, 15, 10, 135, 0},
        {1017, TEXT("Wolf"), TEXT("DOG"), TEXT("FRTBIT1.CPC"), TEXT("DGDYFT2.CPC"),
            TEXT("DGDYFT5.CPC"), TEXT("FRTBIT1.CPC"), nullptr, 700, 100, 300, 60, 20, 20, 90, 0},
        {1018, TEXT("Elf Archer"), TEXT("ARCHER"), TEXT("ARCHER01.CPC"), TEXT("ARCDI01.CPC"),
            TEXT("ARCDI04.CPC"), TEXT("ARCHER05.CPC"), TEXT("ARROW1/BOLT2.CPC"), 700, 500, 100, 60, 20, 10, 270, 300},
        {1019, TEXT("Druid"), TEXT("DRUID"), TEXT("DRUDW001.CPC"), TEXT("DRUDD001.CPC"),
            TEXT("DRUDD008.CPC"), TEXT("DRUID001.CPC"), TEXT("HMISS12/1.CPC"), 2000, 500, 150, 60, 20, 10, 180, 100, nullptr, 4018},
        {33787, TEXT("Ulfilas Druid"), TEXT("DRUID"), TEXT("DRUDW001.CPC"), TEXT("DRUDD001.CPC"),
            TEXT("DRUDD008.CPC"), TEXT("DRUID001.CPC"), TEXT("MISSIL16/1.CPC"), 2500, 500, 150, 60, 20, 10, 180, 250, nullptr, 0, TEXT("Druid33787")},
        {46075, TEXT("Jugurtha"), TEXT("DRUID"), TEXT("DRUDW001.CPC"), TEXT("DRUDD001.CPC"),
            TEXT("DRUDD008.CPC"), TEXT("DRUID001.CPC"), TEXT("MISSIL16/1.CPC"), 4500, 500, 350, 60, 20, 10, 315, 250, nullptr, 0, TEXT("Druid46075")},
        {1014, TEXT("Eastern Archer"), TEXT("EVILARCH"), TEXT("EARCAT01.CPC"), TEXT("EARCDI02.CPC"),
            TEXT("EARCDI04.CPC"), TEXT("EARCAT01.CPC"), TEXT("ARROW4/1.CPC"), 4000, 500, 500, 60, 20, 10, 315, 600},
        {1015, TEXT("Mighty Hunter"), TEXT("GIANTARC"), TEXT("GODATK01.CPC"), TEXT("GODDIE01.CPC"),
            TEXT("GODDIE04.CPC"), TEXT("GODATK01.CPC"), TEXT("MISSILE7/1.CPC"), 18000, 500, 0, 0, 30, 10, 315, 1000},
        {1023, TEXT("Griffon"), TEXT("GRIFFON"), TEXT("GRFLY001.CPC"), TEXT("GRFDI001.CPC"),
            TEXT("GRFDI008.CPC"), TEXT("GRIFA001.CPC"), nullptr, 5000, 500, 350, 75, 30, 15, 315, 0},
        {1003, TEXT("Shadow"), TEXT("SHADOW"), TEXT("SHADOW1.CPC"), TEXT("SHAD_FH1.CPC"),
            TEXT("SHAD_DEA.CPC"), TEXT("SHAD_FA1.CPC"), nullptr, 4000, 500, 150, 55, 32, 20, 315, 0},
        {1007, TEXT("Gargoyle"), TEXT("BIFF"), TEXT("BIFFWF1.CPC"), TEXT("BIFFHTF.CPC"),
            TEXT("BIFFDI7.CPC"), TEXT("BIFFATF.CPC"), nullptr, 5000, 100, 400, 65, 32, 10, 315, 0},
        {1020, TEXT("Lich"), TEXT("LICH"), TEXT("LICHW001.CPC"), TEXT("LICHD001.CPC"),
            TEXT("LICHD008.CPC"), TEXT("LICHA001.CPC"), TEXT("MISSILE2/1.CPC"), 8000, 0, 400, 60, 20, 10, 315, 300},
        {1036, TEXT("Elmore"), TEXT("ELMORE"), TEXT("ELWLK01.CPC"), TEXT("ELDIE01.CPC"),
            TEXT("ELDIE08.CPC"), TEXT("ELATK01.CPC"), nullptr, 7000, 0, 400, 55, 20, 10, 315, 0, TEXT("L152")},
        {16368,TEXT("Bronze Evil Knight"),TEXT("EVILKNT"),TEXT("EKNTW001.CPC"),TEXT("EKNTD001.CPC"),TEXT("EKNTD008.CPC"),TEXT("EKNTA001.CPC"),nullptr,2500,0,300,50,15,10,90,0,nullptr,0,TEXT("EvilKnight16368"),false},
        {24560,TEXT("Silver Evil Knight"),TEXT("EVILKNT"),TEXT("EKNTW001.CPC"),TEXT("EKNTD001.CPC"),TEXT("EKNTD008.CPC"),TEXT("EKNTA001.CPC"),nullptr,4000,400,450,50,15,10,90,0,nullptr,0,TEXT("EvilKnight24560"),false},
        {33803,TEXT("Mithril Bad Knight"),TEXT("BADKNIT"),TEXT("BADBOYA1.CPC"),TEXT("BADBOYJ1.CPC"),TEXT("BADBOYJ4.CPC"),TEXT("BADBOYF1.CPC"),nullptr,4000,500,475,60,25,10,90,0,nullptr,0,TEXT("BadKnight33803"),false},
        {37886,TEXT("Obsidian Hydra"),TEXT("HYDRA"),TEXT("HYW001.CPC"),TEXT("HYDD001.CPC"),TEXT("HYDD004.CPC"),TEXT("HYA001.CPC"),TEXT("MISSILE3/1.CPC"),5000,500,450,72,42,10,180,2500,nullptr,0,TEXT("Hydra37886"),false},
        {54268,TEXT("Mattan"),TEXT("LICH"),TEXT("LICHW001.CPC"),TEXT("LICHD001.CPC"),TEXT("LICHD008.CPC"),TEXT("LICHA001.CPC"),TEXT("MISSILE2/1.CPC"),20000,0,1000,60,20,10,270,1000,nullptr,0,TEXT("Mattan54268"),false},
    };
    return Creatures;
}
TConstArrayView<FWadRetreatCreatureSource> GetWadRetreatCreatureSources()
{
    static const FWadRetreatCreatureSource Creatures[] = {
        {3000, TEXT("Bronze Barbarian"), TEXT("BRONZE"), TEXT("BRONZ01.CPC"), TEXT("DEDBZ02.CPC"), TEXT("DEDBZ04.CPC"),
            TEXT("BRZATK01.CPC"), 2500, 500, 350, 60, 20, 12, 14, 180, 0, 0, 0},
        {3001, TEXT("Black Barbarian"), TEXT("GUS"), TEXT("GUS01.CPC"), TEXT("DEDGUS02.CPC"), TEXT("DEDGUS04.CPC"),
            TEXT("GSATK01.CPC"), 3000, 500, 400, 60, 20, 10, 7, 270, 5000, 5000, 50},
        {3002, TEXT("White Barbarian"), TEXT("PHIL"), TEXT("PHIL01.CPC"), TEXT("BDDED02.CPC"), TEXT("BDDED04.CPC"),
            TEXT("BRDATK01.CPC"), 2500, 1000, 250, 60, 20, 10, 5, 270, 250, 10000, 25},
        {3003, TEXT("Blue Barbarian"), TEXT("BLUE"), TEXT("BLUWK01.CPC"), TEXT("BLUDED02.CPC"), TEXT("BLUDED04.CPC"),
            TEXT("BLUATK01.CPC"), 2500, 750, 300, 60, 20, 10, 7, 180, 0, 0, 0},
        {1033, TEXT("Skeleton Knight"), TEXT("SKELKNT"), TEXT("SWNW01.CPC"), TEXT("SDNW01.CPC"), TEXT("SDNW04.CPC"),
            TEXT("2SA001.CPC"), 2500, 100, 300, 30, 15, 10, 7, 90, 0, 0, 0},
    };
    return Creatures;
}

namespace
{
    FWadArtRequest Request(const FString& Name,const FString& Picture,int32 Type,const TCHAR* Role,TConstArrayView<uint8> Table={})
    {
        FWadArtRequest Out;Out.AssetName=Name;Out.Picture=Picture;Out.Role=Role;
        if(Type>0){Out.SourceTypes.Add(Type);}Out.ColorTable.Append(Table.GetData(),Table.Num());return Out;
    }
    bool Palette(int32 Index,TArray<uint8>& Table,FOutputDevice& Output)
    {
        if(Index==0){Table.Reset();return true;}
        const FString Path=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/COLORIZE")
            /FString::Printf(TEXT("COLORT%02d"),Index);
        if(Index<0||Index>15||!FFileHelper::LoadFileToArray(Table,*Path)||Table.Num()!=256)
        {Output.Logf(TEXT("Missing original creature palette%d."),Index);return false;}return true;
    }
    FWadCreatureArtSet Four(int32 Type,const FString& Name,const FString& Resource,const TCHAR* Standing,const TCHAR* Hurt,
        const TCHAR* Downed,const TCHAR* Attacking,TConstArrayView<uint8> Table)
    {
        FWadCreatureArtSet Out;
        Out.Standing=Request(Name,Resource/Standing,Type,TEXT("standing"),Table);
        Out.Hurt=Request(Name+TEXT("_Hurt"),Resource/Hurt,Type,TEXT("hurt"),Table);
        Out.Downed=Request(Name+TEXT("_Down"),Resource/Downed,Type,TEXT("downed"),Table);
        Out.Attacking=Request(Name+TEXT("_Attack"),Resource/Attacking,Type,TEXT("attacking"),Table);return Out;
    }
}
bool ResolveWadCampaignCreatureArt(const FWadCampaignCreatureSource& Source,FWadCreatureArtSet& Out,FOutputDevice& Output)
{
    TArray<uint8> Table;
    if(Source.Type>=4096&&!Source.ArtName){Output.Logf(TEXT("Full creature%d requires an explicit native art name."),Source.Type);return false;}
    if(!Palette(Source.ArtName?Source.Type>>12:0,Table,Output)){return false;}
    const FString Name=Source.ArtName?Source.ArtName:Source.Resource;
    auto Parsed=Four(Source.Type,Name,Source.Resource,Source.Standing,Source.Hurt,Source.Downed,Source.Attacking,Table);
    if(Source.Bolt)
    {
        const FString BoltName=Source.Type==1025?TEXT("MagicDart")
            :(Source.Type==33787||Source.Type==46075)?TEXT("PackedDruid_Bolt"):Name+TEXT("_Bolt");
        Parsed.Bolt=Request(BoltName,Source.Bolt,Source.Type,TEXT("projectile"));
    }
    Out=MoveTemp(Parsed);return true;
}
bool ResolveWadRetreatCreatureArt(const FWadRetreatCreatureSource& Source,FWadCreatureArtSet& Out,FOutputDevice& Output)
{
    auto Parsed=Four(Source.Type,Source.Resource,Source.Resource,Source.Standing,Source.Hurt,Source.Downed,Source.Attacking,{});
    if(Source.MaxDartRange){Parsed.Bolt=Request(TEXT("MagicDart"),TEXT("MISSIL12/1.CPC"),Source.Type,TEXT("projectile"));}
    Out=MoveTemp(Parsed);return true;
}
bool ResolveWadSpecialCreatureArt(bool bElymas,FWadCreatureArtSet& Out,FOutputDevice& Output)
{
    TArray<uint8> Table;
    if(!Palette(bElymas?12:0,Table,Output)){return false;}
    auto Parsed=bElymas?Four(50158,TEXT("Elymas"),TEXT("WIZEVL"),TEXT("WIZEVL8.CPC"),TEXT("WIZHRT1.CPC"),TEXT("WIZEVL7G.CPC"),TEXT("WIZEVL1.CPC"),Table)
        :Four(1032,TEXT("WIZORNG"),TEXT("WIZORNG"),TEXT("OWWALK01.CPC"),TEXT("OWDIE01.CPC"),TEXT("OWDIE08.CPC"),TEXT("OWA01.CPC"),{});
    if(bElymas){Parsed.Bolt=Request(TEXT("HomingFireball"),TEXT("HMISS6/1.CPC"),50158,TEXT("projectile"));}
    Out=MoveTemp(Parsed);return true;
}
bool ResolveWadKnightArt(bool bMounted,FWadCreatureArtSet& Out,FOutputDevice& Output)
{
    const int32 Type=bMounted?NativeWadRules::ChurchMountedKnight:NativeWadRules::ChurchFootKnight;
    FWadCreatureArtSet Parsed;
    const FString Name=bMounted?TEXT("MountedKnight"):TEXT("FootKnight");
    Parsed.Standing=Request(Name,bMounted?TEXT("KNITHRS/KHSRUN03.CPC"):TEXT("KXW/KXW001.CPC"),Type,TEXT("standing"));
    Parsed.Attacking=bMounted?Parsed.Standing:Request(Name+TEXT("Attack"),TEXT("KXW/KXA005.CPC"),Type,TEXT("attacking"));
    Parsed.Hurt=Request(Name+TEXT("Hurt"),bMounted?TEXT("KNITHRS/KHSDI02.CPC"):TEXT("KXW/KXD001.CPC"),Type,TEXT("hurt"));
    Parsed.Downed=Request(Name+TEXT("Down"),bMounted?TEXT("KNITHRS/KHSDI08.CPC"):TEXT("KXW/KXD008.CPC"),Type,TEXT("downed"));
    Parsed.Bolt=Request(bMounted?TEXT("KnightPiercingBolt"):TEXT("KnightGrayBolt"),bMounted?TEXT("ARROW7/1.CPC"):TEXT("ARROW1/BOLT0.CPC"),Type,TEXT("projectile"));
    Out=MoveTemp(Parsed);return true;
}
