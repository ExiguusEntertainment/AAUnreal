#include "NativeWadLightSource.h"
#include "NativeWadScenerySource.h"
#include "NativeWadLevel.h"
#include "Misc/SecureHash.h"

namespace
{
    bool MatchesOriginal(const FDoomWadMapData& Map,TConstArrayView<uint8> Bytes,FString& Error)
    {
        const auto Fail=[&](){Error=TEXT("Original lighting MAP is malformed or differs from the loaded source records.");return false;};
        if(Bytes.Num()<12||(FMemory::Memcmp(Bytes.GetData(),"PWAD",4)&&FMemory::Memcmp(Bytes.GetData(),"IWAD",4))){return Fail();}
        const auto U32=[&](int32 At){return uint32(Bytes[At])|uint32(Bytes[At+1])<<8|uint32(Bytes[At+2])<<16|uint32(Bytes[At+3])<<24;};
        const uint32 Count=U32(4),Directory=U32(8);
        if(uint64(Directory)+uint64(Count)*16>uint64(Bytes.Num())){return Fail();}
        TMap<FString,TConstArrayView<uint8>> Lumps;
        for(uint32 I=0;I<Count;++I)
        {
            const int32 At=int32(Directory+I*16);const uint32 Offset=U32(At),Size=U32(At+4);
            if(uint64(Offset)+Size>uint64(Bytes.Num())){return Fail();}
            ANSICHAR Label[9]={};FMemory::Memcpy(Label,Bytes.GetData()+At+8,8);const FString Key=ANSI_TO_TCHAR(Label);
            if(Lumps.Contains(Key)){return Fail();}Lumps.Add(Key,Bytes.Slice(int32(Offset),int32(Size)));
        }
        const auto Match=[&](const TCHAR* Key,const auto& Records)
        {
            const auto* Data=Lumps.Find(Key);const int64 Size=int64(Records.Num())*sizeof(Records[0]);
            return Data&&Data->Num()==Size&&(Size==0||!FMemory::Memcmp(Data->GetData(),Records.GetData(),Size));
        };
        return Match(TEXT("VERTEXES"),Map.Vertices)&&Match(TEXT("LINEDEFS"),Map.Lines)&&Match(TEXT("SIDEDEFS"),Map.Sides)
            &&Match(TEXT("SECTORS"),Map.Sectors)&&Match(TEXT("THINGS"),Map.Things)?true:Fail();
    }
    bool CenterSector(TConstArrayView<uint8> Bytes,FVector2D Position,int32 Count,int32& Sector,bool& Fallback,FString& Error)
    {
        if(!FMath::IsFinite(Position.X)||!FMath::IsFinite(Position.Y)||Position.X<MIN_int16||Position.X>MAX_int16
            ||Position.Y<MIN_int16||Position.Y>MAX_int16)
        {Error=TEXT("Source lighting center lies outside signed16 source XY bounds.");return false;}
        // Original ObjectGetX/Y16 use arithmetic fixed-point shifts (floor).
        if(!NativeWadScenerySource::ResolveMountSector(Bytes,FIntPoint(FMath::FloorToInt(Position.X),FMath::FloorToInt(Position.Y)),Sector,Fallback,Error)
            ||Sector<0||Sector>=Count){return false;}
        return true;
    }
}

bool BuildWadSourceLighting(const FDoomWadMapData& Map,TConstArrayView<uint8> Bytes,const FString& Name,
    const TMap<int32,FVector2D>& EditedCenters,FWadSourceLightingMetadata& Result,FString& Error)
{
    if(!MatchesOriginal(Map,Bytes,Error)){return false;}
    FWadSourceLightingMetadata New;New.Version=NativeWadLightRules::MetadataVersion;
    New.SourceMapName=Name;New.ThingCount=Map.Things.Num();New.LineCount=Map.Lines.Num();
    FSHAHash Hash;FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Hash.Hash);New.SourceMapSha1=Hash.ToString();
    if(Map.Sectors.IsEmpty()||Map.Sectors.Num()>0x4000||Map.Things.Num()>65535||Map.Lines.Num()>65535)
    {Error=TEXT("Source lighting map dimensions are unsupported.");return false;}
    for(int32 I=0;I<Map.Sectors.Num();++I)
    {
        const auto& Source=Map.Sectors[I];auto& S=New.Sectors.AddDefaulted_GetRef();
        S.Sector=I;S.AuthoredLight=uint8(Source.Light);
        // Fixed8-byte fields are not required to be NUL terminated.
        ANSICHAR Label[9]={};FMemory::Memcpy(Label,Source.CeilingTex,8);
        S.bOutside=FString(ANSI_TO_TCHAR(Label)).StartsWith(TEXT("F_SKY"))||(Source.Trigger&1)!=0;
    }
    for(int32 Id=0;Id<Map.Lines.Num();++Id)
    {
        const auto& Line=Map.Lines[Id];
        if(!(Line.Flags&4)){continue;}
        const int32 Front=Map.GetSectorForSide(Line.Side[0]),Back=Map.GetSectorForSide(Line.Side[1]);
        if(Front==INDEX_NONE||Back==INDEX_NONE)
        {
            // Original LIGHT.C111 dereferences even side65535. Explicit bounded
            // native correction: omit this undefined edge, never guess a neighbor.
            New.UnaddressableTwoSidedLines.Add(Id);continue;
        }
        New.Sectors[Front].Neighbors.AddUnique(Back);New.Sectors[Back].Neighbors.AddUnique(Front);
    }
    for(auto& S:New.Sectors){S.Neighbors.Sort();}
    for(const auto& [Id,Position]:EditedCenters)
    {
        if(!Map.Things.IsValidIndex(Id)||!NativeWadLightRules::IsIlluminatingType(uint16(Map.Things[Id].Type)))
        {Error=TEXT("Edited source light center is not an original illuminator.");return false;}
    }
    for(int32 Id=0;Id<Map.Things.Num();++Id)
    {
        const auto& Thing=Map.Things[Id];const int32 Type=uint16(Thing.Type);const FVector2D XY(Thing.X,Thing.Y);
        const bool Owner=NativeWadLightRules::IsIlluminatingType(Type);
        const bool Directive=NativeWadLightRules::IsAnimationDirective(Type);
        if(!Owner&&!Directive&&Type!=1500){continue;}
        int32 Sector=INDEX_NONE;bool Fallback=false;
        if(!CenterSector(Bytes,XY,Map.Sectors.Num(),Sector,Fallback,Error)){return false;}
        if(Owner)
        {
            auto& O=New.Illuminators.AddDefaulted_GetRef();O.ThingId=Id;O.SourceType=Type;O.AccessoryData=Thing.Flags;
            O.Illumination=NativeWadLightRules::SourceIllumination(Type,Thing.Flags);O.AuthoredSector=Sector;
            O.AuthoredPosition=XY;O.CenterPosition=EditedCenters.FindRef(Id);
            if(!EditedCenters.Contains(Id)){O.CenterPosition=XY;}
            O.bUsesEditedCenter=O.CenterPosition!=XY;
            if(!CenterSector(Bytes,O.CenterPosition,Map.Sectors.Num(),O.CenterSector,O.bUsedSourceFallback,Error)){return false;}
        }
        else
        {
            // Invalid original special lookup is ignored by 3D_IO, not mapped
            // to sector0 as ordinary ObjectSetUpSectors does. Refuse before write.
            if(Fallback){Error=TEXT("Original special lighting/door marker has no valid sector.");return false;}
            if(Type==1500){New.Sectors[Sector].bDoor=true;}
            else{New.Directives.Add(NativeWadLightRules::DirectiveFor(Id,Type,Sector));}
        }
    }
    if(!NativeWadLightRules::ResolveAnimations(New.Directives,New.Sectors.Num(),New.Animations,Error)
        ||!NativeWadLightRules::Validate(New,Error)){return false;}
    Result=MoveTemp(New);Error.Reset();return true;
}

bool ANativeWadLevel::InstallSourceLightingMetadata(const FWadSourceLightingMetadata& Metadata,FString& Error)
{
    if(bUseSourceLightFeedback)
    {Error=TEXT("Disable source feedback before replacing metadata; its prepared runtime cache must stay coherent.");return false;}

    if(Metadata.Sectors.Num()!=Sectors.Num()||!NativeWadLightRules::Validate(Metadata,Error)){return false;}
    // Deliberately no lighting refresh, mode change, live-state reset or component mutation in stage1.
    Modify();SourceLightingMetadata=Metadata;return true;
}
