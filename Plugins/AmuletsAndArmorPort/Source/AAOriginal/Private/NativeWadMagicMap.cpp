#include "NativeWadMagicMap.h"

namespace
{
    bool Integer(const FString& Token, int32& Value)
    {
        if (Token.IsEmpty() || Token.Len()>9) { return false; }
        for (TCHAR C : Token) { if (C<TEXT('0') || C>TEXT('9')) { return false; } }
        return LexTryParseString(Value,*Token);
    }
    double Cross(FVector2D A,FVector2D B) { return A.X*B.Y-A.Y*B.X; }
}

void FWadMagicMap::StartOwner(const FString& Owner,int32 Power)
{
    if (Owner.IsEmpty() || Power<1 || Power>4) { return; }
    Owners.Add(Owner,Power);
    Tier=Power;
    Features=Rotate|Translucent;
    if (Power==2 || Power==4) { Features|=Objects|Creatures; }
    if (Power>=3) { Features|=AllWalls; }
    bOn=true;
}

void FWadMagicMap::SynchronizeOwners(const TMap<FString,int32>& Active)
{
    for (auto It=Owners.CreateIterator();It;++It) { if (!Active.Contains(It.Key())) { It.RemoveCurrent(); } }
    // Ordinary removals deliberately retain last-started features, not max/summed power.
    for (const auto& Entry:Active)
    { if (!Owners.Contains(Entry.Key)) { StartOwner(Entry.Key,Entry.Value); } }
    // Deliberate correction to EFFECT.C's acknowledged stale-map bug on final power>1 removal.
    if (Owners.IsEmpty()) { bOn=false; }
}

bool FWadMagicMap::ToggleDisplay()
{
    if (!bOn && Owners.IsEmpty()) { return false; }
    bOn=!bOn; return true;
}

void FWadMagicMap::Command(int32 RowKey)
{
    if(RowKey!=1&&!bOn){return;}
    switch(RowKey)
    {
    case 1: ToggleDisplay(); break;
    case 2: Features^=Rotate; break;
    case 3: Features^=Translucent; break;
    case 4: Features^=Transparent; break;
    // CLIENT.C gates zoom row5/6 behind god mode; the port has no god-mode input.
    case 7: Position=(Position+1)%6; break;
    default: break;
    }
}

bool FWadMagicMap::IsSeen(const FString& Map,int32 Index) const
{ const auto* Lines=Seen.Find(Map); return Lines && Lines->Contains(Index); }
void FWadMagicMap::Observe(const FString& Map,int32 Index)
{ const auto* T=Topology.Find(Map);if(T&&Index>=0&&Index<T->Count){Seen.FindOrAdd(Map).Add(Index);} }
void FWadMagicMap::BindTopology(const FString& Map,int32 Count,const FString& Hash)
{
    if(Map.IsEmpty()||Count<0||Count>65536||Hash.Len()!=8){return;}
    const auto* Old=Topology.Find(Map);
    if(!Old||Old->Count!=Count||Old->Hash!=Hash){Seen.Remove(Map);Topology.Add(Map,{Count,Hash});}
}
bool FWadMagicMap::Eligible(uint16 Flags,bool bSeen,uint8 Mode)
{ return !(Flags&0x80) && (bSeen || (Flags&0x8100) || (Mode&AllWalls)); }

FIntPoint FWadMagicMap::Location(FIntRect R) const
{
    switch(Position)
    {
    case 0:return R.Min+FIntPoint(3,3);
    case 1:return {R.Max.X-3-Size,R.Min.Y+3};
    case 2:return {R.Max.X-3-Size,R.Max.Y-3-Size};
    case 3:case 5:return {R.Min.X+3,R.Max.Y-3-Size}; // Preserve original bottom-center placement bug.
    default:return {R.Min.X+((R.Max.X-R.Min.X-Size)>>1),R.Min.Y+((R.Max.Y-R.Min.Y-Size)>>1)};
    }
}

FIntPoint FWadMagicMap::Project(FVector2D Point,FVector2D Center,double Yaw) const
{
    int64 X=FMath::FloorToInt64(Point.X)-FMath::FloorToInt64(Center.X),Y=FMath::FloorToInt64(Point.Y)-FMath::FloorToInt64(Center.Y);
    if (Features&Rotate)
    {
        const double Angle=FMath::DegreesToRadians(90.0-Yaw);
        const int64 S=FMath::RoundToInt64(FMath::Sin(Angle)*65536.0),C=FMath::RoundToInt64(FMath::Cos(Angle)*65536.0);
        const int64 RX=(X*C-Y*S)>>16,RY=(X*S+Y*C)>>16;
        X=RX;Y=RY;
    }
    return {int32((X*Zoom)>>16)+(Size>>1),-int32((Y*Zoom)>>16)+(Size>>1)};
}

void FWadMagicMap::Line(TArray<uint8>& Page,FIntPoint A,FIntPoint B,uint8 Color)
{
    int64 X1=A.X,Y1=A.Y,X2=B.X,Y2=B.Y,DX=X2-X1,DY=Y2-Y1;
    // OVERHEAD.C's integer clipping order, then fixed-point major-axis raster.
    if(X1<0 && X2>=0){Y1+=(-X1*DY)/DX;X1=0;}
    if(X2<0 && X1>=0){Y2+=(-X2*DY)/DX;X2=0;}
    if(X1>=Size && X2<Size){Y1+=((Size-1-X1)*DY)/DX;X1=Size-1;}
    if(X2>=Size && X1<Size){Y2+=((Size-1-X2)*DY)/DX;X2=Size-1;}
    if(Y1>=Size && Y2<Size){X1+=((Size-1-Y1)*DX)/DY;Y1=Size-1;}
    if(Y2>=Size && Y1<Size){X2+=((Size-1-Y2)*DX)/DY;Y2=Size-1;}
    if(Y1<0 && Y2>=0){X1+=(-Y1*DX)/DY;Y1=0;}
    if(Y2<0 && Y1>=0){X2+=(-Y2*DX)/DY;Y2=0;}
    if((X1<0&&X2<0)||(X1>=Size&&X2>=Size)||(Y1<0&&Y2<0)||(Y1>=Size&&Y2>=Size)){return;}
    X1=FMath::Clamp<int64>(X1,0,Size-1);X2=FMath::Clamp<int64>(X2,0,Size-1);
    Y1=FMath::Clamp<int64>(Y1,0,Size-1);Y2=FMath::Clamp<int64>(Y2,0,Size-1);
    DX=X2-X1;DY=Y2-Y1;
    if(FMath::Abs(DX)>FMath::Abs(DY))
    {
        const int64 Step=DX?DY*65536/DX:0,Direction=X1<X2?1:-1;
        int64 Fract=Y1*65536;
        for(int64 X=X1;;X+=Direction,Fract+=Step*Direction)
        {Page[FMath::Clamp<int32>(int32(Fract>>16),0,Size-1)*Size+X]=Color;if(X==X2){break;}}
    }
    else
    {
        const int64 Step=DY?DX*65536/DY:0,Direction=Y1<Y2?1:-1;
        int64 Fract=X1*65536;
        for(int64 Y=Y1;;Y+=Direction,Fract+=Step*Direction)
        {Page[Y*Size+FMath::Clamp<int32>(int32(Fract>>16),0,Size-1)]=Color;if(Y==Y2){break;}}
    }
}

TArray<uint8> FWadMagicMap::Rasterize(const FString& Map,TConstArrayView<FLine> Lines,
    TConstArrayView<FMarker> Markers,FVector2D Center,double Yaw,double PlayerMarkerRadius) const
{
    TArray<uint8> Page;Page.Init(0,Size*Size);
    for(int32 I=0;I<Lines.Num();++I)
    {
        const auto& L=Lines[I];if(!Eligible(L.Flags,IsSeen(Map,I),Features)){continue;}
        Line(Page,Project(L.Wall.From,Center,Yaw),Project(L.Wall.To,Center,Yaw),LineColor(L.Flags));
    }
    const auto DrawMarker=[&](const FMarker& M,uint8 Color)
    {
        const FIntPoint P=Project(M.Position,Center,Yaw);
        const int32 R=FMath::Max(0,int32(int64(M.Radius)*Zoom>>16));
        Line(Page,P,P,M.bExtraData?Color+16:Color);
        Line(Page,P+FIntPoint(-R,-R),P+FIntPoint(R,-R),31);
        Line(Page,P+FIntPoint(-R,R),P+FIntPoint(R,R),31);
        Line(Page,P+FIntPoint(-R,-R),P+FIntPoint(-R,R),31);
        Line(Page,P+FIntPoint(R,-R),P+FIntPoint(R,R),31);
    };
    if(Features&Objects)
    {for(const auto& M:Markers){if(!M.bInvisible&&(!M.bCreature||(Features&Creatures))){DrawMarker(M,31);}}}
    DrawMarker({Center,PlayerMarkerRadius,false,false,false},160);
    return Page;
}

bool FWadMagicMap::CanObserve(TConstArrayView<FLine> Lines,int32 Target,FVector Eye,FRotator View,
    double Fov,double Aspect,int32 Sample,FBox2D Crop)
{
    if(!Lines.IsValidIndex(Target)||Sample<0||!FMath::IsFinite(Fov)||Fov<=0||Fov>=179||!FMath::IsFinite(Aspect)||Aspect<=0){return false;}
    const auto& W=Lines[Target].Wall;if(W.Surfaces.IsEmpty()){return false;}
    const auto& Band=W.Surfaces[(Sample/9)%W.Surfaces.Num()];
    if(Band.Top<=Band.Bottom){return false;}
    const double Side=Cross(W.To-W.From,FVector2D(Eye.X,Eye.Y)-W.From);
    if((Side>0)!=Band.bBack){return false;}
    const double U=double(1+(Sample%3))/4.0,V=double(1+(Sample/3)%3)/4.0;
    const FVector2D XY=FMath::Lerp(W.From,W.To,U);
    const FVector Point(XY,FMath::Lerp(Band.Bottom,Band.Top,V)),Delta=Point-Eye;
    const FRotationMatrix Rotation(View);
    const double Depth=FVector::DotProduct(Delta,Rotation.GetUnitAxis(EAxis::X));
    const double Extent=Depth*FMath::Tan(FMath::DegreesToRadians(Fov*0.5));
    if(Depth<=1.0||FMath::Abs(FVector::DotProduct(Delta,Rotation.GetUnitAxis(EAxis::Y)))>=Extent
        ||FMath::Abs(FVector::DotProduct(Delta,Rotation.GetUnitAxis(EAxis::Z)))>=Extent/Aspect){return false;}
    const FVector2D Projected(0.5+FVector::DotProduct(Delta,Rotation.GetUnitAxis(EAxis::Y))/(2*Extent),
        0.5-FVector::DotProduct(Delta,Rotation.GetUnitAxis(EAxis::Z))*Aspect/(2*Extent));
    if(!Crop.IsInside(Projected)){return false;}
    const FVector2D Ray(Delta.X,Delta.Y),Origin(Eye.X,Eye.Y);
    for(int32 I=0;I<Lines.Num();++I)
    {
        if(I==Target){continue;}const auto& Occluder=Lines[I].Wall;const FVector2D Edge=Occluder.To-Occluder.From;
        const double Den=Cross(Ray,Edge);if(FMath::Abs(Den)<0.000001){continue;}
        const FVector2D Offset=Occluder.From-Origin;
        const double T=Cross(Offset,Edge)/Den,S=Cross(Offset,Ray)/Den;
        if(T<=0.0001||T>=0.9999||S<0||S>1){continue;}
        const double Z=Eye.Z+Delta.Z*T;
        for(const auto& Other:Occluder.Surfaces)
        {if(Other.Blend==EWadWallBlend::Opaque&&((Cross(Edge,Origin-Occluder.From)>0)==Other.bBack)
            &&Z>Other.Bottom&&Z<Other.Top){return false;}}
    }
    return true;
}

FString FWadMagicMap::Encode() const
{
    FString Result=FString::Printf(TEXT("1,%d,%d,%d,%d,%d"),bOn?1:0,Features,Tier,Position,Zoom);
    TArray<FString> Maps;Seen.GetKeys(Maps);Maps.Sort();
    for(const auto& Map:Maps)
    {
        const auto* T=Topology.Find(Map);if(!T){continue;}
        Result+=FString::Printf(TEXT("~%s,%d,%s"),*Map,T->Count,*T->Hash);TArray<int32> Indices=Seen[Map].Array();Indices.Sort();
        for(int32 I:Indices){Result+=FString::Printf(TEXT(",%d"),I);}
    }
    return Result;
}

bool FWadMagicMap::Decode(const FString& Payload)
{
    if(Payload.Len()>1048576){return false;}TArray<FString> Rows;Payload.ParseIntoArray(Rows,TEXT("~"),false);
    if(Rows.IsEmpty()||Rows.Num()>129){return false;}TArray<FString> Header;Rows[0].ParseIntoArray(Header,TEXT(","),false);
    int32 Values[6]={};if(Header.Num()!=6){return false;}
    for(int32 I=0;I<6;++I){if(!Integer(Header[I],Values[I])){return false;}}
    if(Values[0]!=1||Values[1]>1||Values[2]>127||Values[3]>4||Values[4]>5||Values[5]!=0x2000){return false;}
    const int32 TierFeatures=Values[3]==2?Objects|Creatures:Values[3]==3?AllWalls:
        Values[3]==4?AllWalls|Objects|Creatures:0;
    // Only rotate/transparency/translucency are player toggles. Tier selection is
    // the last started individual effect, so corrupt saves cannot invent all-wall sight.
    if((Values[2]&(AllWalls|Objects|Creatures))!=TierFeatures||(Values[2]&Perspective)
        ||(Values[3]==0&&(Values[1]!=0||!Owners.IsEmpty()))){return false;}
    FWadMagicMap Parsed;Parsed.Owners=Owners;Parsed.bOn=Values[1]!=0;Parsed.Features=uint8(Values[2]);
    Parsed.Tier=Values[3];Parsed.Position=Values[4];
    for(int32 R=1;R<Rows.Num();++R)
    {
        TArray<FString> Fields;Rows[R].ParseIntoArray(Fields,TEXT(","),false);
        if(Fields.Num()<3||Fields[0].IsEmpty()||Fields[0].Len()>128||Parsed.Seen.Contains(Fields[0])||Fields.Num()>65539){return false;}
        int32 Count;if(!Integer(Fields[1],Count)||Count>65536||Fields[2].Len()!=8){return false;}
        for(TCHAR C:Fields[2]){if(!((C>=TEXT('0')&&C<=TEXT('9'))||(C>=TEXT('A')&&C<=TEXT('F')))){return false;}}
        Parsed.Topology.Add(Fields[0],{Count,Fields[2]});
        for(TCHAR C:Fields[0]){if(!((C>=TEXT('A')&&C<=TEXT('Z'))||(C>=TEXT('a')&&C<=TEXT('z'))||(C>=TEXT('0')&&C<=TEXT('9'))||C==TEXT('_'))){return false;}}
        auto& Lines=Parsed.Seen.Add(Fields[0]);
        for(int32 I=3;I<Fields.Num();++I){int32 Index;if(!Integer(Fields[I],Index)||Index>=Count||Lines.Contains(Index)){return false;}Lines.Add(Index);}
    }
    *this=MoveTemp(Parsed);return true;
}
