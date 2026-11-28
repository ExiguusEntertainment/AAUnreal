#include "NativeWadLightRules.h"
#include "Misc/SecureHash.h"

namespace
{
    bool Fail(FString& Error, const TCHAR* Reason) { Error=Reason;return false; }
    bool SameRule(const FWadSourceLightDirective& A,const FWadSourceLightDirective& B)
    { return A.ThingId==B.ThingId&&A.MarkerType==B.MarkerType&&A.Sector==B.Sector&&A.AnimationType==B.AnimationType
        &&A.Rate==B.Rate&&A.Center==B.Center&&A.Radius==B.Radius; }
    template<typename TRead>
    int32 Weighted(TConstArrayView<int32> Neighbors,TRead Read,int32 Excluded=INDEX_NONE)
    {
        int32 Count=Neighbors.Num()-int32(Neighbors.Contains(Excluded)),Remaining=100,Total=0;
        for(const int32 N:Neighbors)
        {
            if(N==Excluded){continue;}
            const int32 Percent=Remaining/Count--;Remaining-=Percent;
            Total+=Read(N)*((Percent<<8)/100);
        }
        return Total>>8;
    }
}

bool NativeWadLightRules::IsIlluminatingType(int32 Type)
{ return Type==924||Type==27||Type==38||(Type>0&&Type<=65535&&(Type&4095)==41); }
int32 NativeWadLightRules::SourceIllumination(int32 Type,uint16 Data)
{ return !IsIlluminatingType(Type)?0:Type==924?(Data<32768?int32(Data):int32(Data)-65536):Type==27?128:Type==38?255:64; }
bool NativeWadLightRules::IsAnimationDirective(int32 Type)
{ return Type==1526||Type==1503||Type==1504||Type==1505||Type==1506||Type==1507||Type==1543; }
FWadSourceLightDirective NativeWadLightRules::DirectiveFor(int32 ThingId,int32 Type,int32 Sector)
{
    FWadSourceLightDirective D;D.ThingId=ThingId;D.MarkerType=Type;D.Sector=Sector;
    if(Type==1526){D.AnimationType=1;D.Rate=70;D.Center=190;D.Radius=60;}
    else if(Type==1506){D.AnimationType=1;D.Rate=40;D.Center=128;D.Radius=64;}
    else if(Type==1543){D.AnimationType=1;D.Rate=20;D.Center=192;D.Radius=32;}
    return D;
}

bool NativeWadLightRules::ResolveAnimations(TConstArrayView<FWadSourceLightDirective> Directives,int32 Count,
    TArray<FWadSourceLightDirective>& Result,FString& Error)
{
    if(Count<1||Count>0x4000){return Fail(Error,TEXT("Invalid source lighting sector count."));}
    TArray<FWadSourceLightDirective> Last;Last.SetNum(Count);int32 Previous=INDEX_NONE;
    for(const auto& D:Directives)
    {
        if(D.ThingId<=Previous||!IsAnimationDirective(D.MarkerType)||D.Sector<0||D.Sector>=Count
            ||!SameRule(D,DirectiveFor(D.ThingId,D.MarkerType,D.Sector)))
        {return Fail(Error,TEXT("Malformed, unordered or duplicate source animation directive."));}
        Previous=D.ThingId;Last[D.Sector]=D;
    }
    TArray<FWadSourceLightDirective> New;
    for(const auto& D:Last){if(D.AnimationType){New.Add(D);}}
    Result=MoveTemp(New);Error.Reset();return true;
}

bool NativeWadLightRules::Validate(const FWadSourceLightingMetadata& M,FString& Error)
{
    const int32 Count=M.Sectors.Num();
    if(M.Version!=MetadataVersion||Count<1||Count>0x4000||M.ThingCount<0||M.ThingCount>65535||M.LineCount<0||M.LineCount>65535)
    {return Fail(Error,TEXT("Invalid source lighting version or bounds."));}
    int32 PreviousLine=INDEX_NONE;
    for(const int32 Line:M.UnaddressableTwoSidedLines)
    {
        if(Line<=PreviousLine||Line>=M.LineCount){return Fail(Error,TEXT("Invalid excluded source light edge ID/order."));}
        PreviousLine=Line;
    }
    if(M.SourceMapName.IsEmpty()||M.SourceMapSha1.Len()!=40)
    {return Fail(Error,TEXT("Missing source light map identity/digest."));}
    for(const TCHAR C:M.SourceMapSha1)
    {if(!FChar::IsHexDigit(C)){return Fail(Error,TEXT("Malformed source light digest."));}}
    for(int32 Sector=0;Sector<Count;++Sector)
    {
        const auto& S=M.Sectors[Sector];int32 Previous=INDEX_NONE;
        if(S.Sector!=Sector||S.AuthoredLight<0||S.AuthoredLight>255)
        {return Fail(Error,TEXT("Invalid authored source sector lighting."));}
        for(const int32 N:S.Neighbors)
        {
            if(N<=Previous||N<0||N>=Count||!M.Sectors[N].Neighbors.Contains(Sector))
            {return Fail(Error,TEXT("Invalid source lighting adjacency/order."));}
            Previous=N;
        }
    }
    int32 Previous=INDEX_NONE;
    for(const auto& O:M.Illuminators)
    {
        if(O.ThingId<=Previous||O.ThingId>=M.ThingCount||!IsIlluminatingType(O.SourceType)||O.AccessoryData<0||O.AccessoryData>65535
            ||O.Illumination!=SourceIllumination(O.SourceType,uint16(O.AccessoryData))||O.AuthoredSector<0||O.AuthoredSector>=Count
            ||O.CenterSector<0||O.CenterSector>=Count||!FMath::IsFinite(O.AuthoredPosition.X)||!FMath::IsFinite(O.AuthoredPosition.Y)
            ||!FMath::IsFinite(O.CenterPosition.X)||!FMath::IsFinite(O.CenterPosition.Y))
        {return Fail(Error,TEXT("Invalid, unordered or duplicate source illuminator."));}
        Previous=O.ThingId;
    }
    for(const auto& D:M.Directives)
    {
        if(D.ThingId>=M.ThingCount||M.Illuminators.ContainsByPredicate([&](const auto& O){return O.ThingId==D.ThingId;}))
        {return Fail(Error,TEXT("Animation ThingID out of bounds or duplicated by an illuminator."));}
    }
    TArray<FWadSourceLightDirective> Effective;
    if(!ResolveAnimations(M.Directives,Count,Effective,Error)){return false;}
    if(Effective.Num()!=M.Animations.Num()){return Fail(Error,TEXT("Effective source animation count mismatch."));}
    for(int32 I=0;I<Effective.Num();++I)
    {if(!SameRule(Effective[I],M.Animations[I])){return Fail(Error,TEXT("Effective source animation/order mismatch."));}}
    Error.Reset();return true;
}

static bool RecalculatePrepared(const FWadSourceLightingMetadata& M,TConstArrayView<int32> Input,
    TConstArrayView<int32> DoorWeights,int32 Outside,TArray<int32>& Result,FString& Error,TConstArrayView<int32> Centers)
{
    const int32 Count=M.Sectors.Num();
    if(Input.Num()!=Count||DoorWeights.Num()!=Count||Outside<0||Outside>255)
    {return Fail(Error,TEXT("Source lighting input dimensions/range mismatch."));}
    for(int32 I=0;I<Count;++I)
    {if(Input[I]<0||Input[I]>255||DoorWeights[I]<0||DoorWeights[I]>256){return Fail(Error,TEXT("Source byte/door weight out of range."));}}
    TArray<int32> Current;Current.Append(Input.GetData(),Input.Num());
    for(int32 Pass=0;Pass<2;++Pass)
    {
        TArray<int32> Next=Current; // Isolated sectors retain THIS pass's input.
        for(int32 Sector=0;Sector<Count;++Sector)
        {
            const auto& S=M.Sectors[Sector];
            if(S.bOutside){Next[Sector]=Outside;continue;}
            if(S.Neighbors.IsEmpty()){continue;}
            const int32 Across=Weighted(S.Neighbors,[&](int32 Neighbor)
            {
                const auto& Through=M.Sectors[Neighbor];
                // Bound the generated-door record logically. Original LIGHT.C
                // reads adjDoors[reg] even after its initialized record count.

                const int32 Others=Through.Neighbors.Num()-int32(Through.Neighbors.Contains(Sector));
                return Through.bDoor&&Others>0
                    ? (Weighted(Through.Neighbors,[&](int32 N){return Current[N];},Sector)*DoorWeights[Neighbor])>>8
                    : Current[Neighbor];
            });
            Next[Sector]=S.bDoor?(Across*DoorWeights[Sector])>>8:Across;
        }
        for(int32 Id=0;Id<M.Illuminators.Num();++Id)
        {
            const auto& Owner=M.Illuminators[Id];
            const int32 Center=Centers.IsEmpty()?Owner.CenterSector:Centers[Id];
            if(Center==INDEX_NONE){continue;} // An actually destroyed bound prop.

            // LIGHT.C uses a signed16 accumulator before its byte clamp.
            const uint16 Bits=uint16(Next[Center]+Owner.Illumination);
            const int32 Sum=Bits<32768?int32(Bits):int32(Bits)-65536;
            Next[Center]=FMath::Clamp(Sum,0,255);
        }
        Current=MoveTemp(Next);
    }
    Result=MoveTemp(Current);Error.Reset();return true;
}

static bool AnimatePrepared(const FWadSourceLightingMetadata& M,uint16 DeltaTicks,TConstArrayView<int32> Input,
    TConstArrayView<int32> Phases,TArray<int32>& Lights,TArray<int32>& NewPhases,FString& Error)
{
    const int32 Count=M.Sectors.Num();
    if(Input.Num()!=Count||Phases.Num()!=Count){return Fail(Error,TEXT("Source animation state dimension mismatch."));}
    for(int32 I=0;I<Count;++I)
    {if(Input[I]<0||Input[I]>255||Phases[I]<0||Phases[I]>65535){return Fail(Error,TEXT("Source animation state range mismatch."));}}
    TArray<int32> Output,OutputPhases;Output.Append(Input.GetData(),Count);OutputPhases.Append(Phases.GetData(),Count);
    for(const auto& A:M.Animations)
    {
        const uint16 Phase=uint16(uint64(Phases[A.Sector])+((uint64(DeltaTicks)*A.Rate)<<3));
        OutputPhases[A.Sector]=Phase;
        const int32 Sine=int32(FMath::Sin(double(Phase)*(2.0*UE_DOUBLE_PI/65536.0))*65536.0);
        Output[A.Sector]=uint8(A.Center+((int64(A.Radius)*Sine)>>16));
    }
    Lights=MoveTemp(Output);NewPhases=MoveTemp(OutputPhases);Error.Reset();return true;
}


bool NativeWadLightRules::Recalculate(const FWadSourceLightingMetadata& M,TConstArrayView<int32> Input,
    TConstArrayView<int32> Doors,int32 Outside,TArray<int32>& Result,FString& Error)
{
    return Validate(M,Error)&&RecalculatePrepared(M,Input,Doors,Outside,Result,Error,{});
}
bool NativeWadLightRules::Animate(const FWadSourceLightingMetadata& M,uint16 Delta,TConstArrayView<int32> Input,
    TConstArrayView<int32> Phases,TArray<int32>& Lights,TArray<int32>& NewPhases,FString& Error)
{
    return Validate(M,Error)&&AnimatePrepared(M,Delta,Input,Phases,Lights,NewPhases,Error);
}
FWadSourceLightState NativeWadLightRules::InitialState(const FWadSourceLightingMetadata& M)
{
    FWadSourceLightState S;S.Version=FeedbackStateVersion;S.SourceMapSha1=M.SourceMapSha1;
    for(const auto& Sector:M.Sectors){S.Levels.Add(Sector.AuthoredLight);S.Phases.Add(0);}return S;
}
bool NativeWadLightRules::ValidateState(const FWadSourceLightingMetadata& M,const FWadSourceLightState& S,FString& Error)
{
    if(S.Version==0)
    {
        if(!S.SourceMapSha1.IsEmpty()||!S.Levels.IsEmpty()||!S.Phases.IsEmpty()||S.PendingTicks!=0.0||S.CompletedSteps!=0)
        {return Fail(Error,TEXT("Legacy lighting state contains an unexpected modern payload."));}
        Error.Reset();return true;
    }
    if(M.Sectors.IsEmpty()||M.SourceMapSha1.IsEmpty()||S.Version!=FeedbackStateVersion||S.SourceMapSha1!=M.SourceMapSha1||S.Levels.Num()!=M.Sectors.Num()
        ||S.Phases.Num()!=M.Sectors.Num()||!FMath::IsFinite(S.PendingTicks)||S.PendingTicks<0.0
        ||S.PendingTicks>MaximumPendingTicks||S.CompletedSteps>uint64(MAX_int64))
    {return Fail(Error,TEXT("Source feedback checkpoint version/identity/dimensions/clock mismatch."));}
    for(int32 I=0;I<S.Levels.Num();++I)
    {if(S.Levels[I]<0||S.Levels[I]>255||S.Phases[I]<0||S.Phases[I]>65535){return Fail(Error,TEXT("Source feedback checkpoint byte/phase outside bounds."));}}
    Error.Reset();return true;
}
bool NativeWadLightRules::FPreparedKernel::Initialize(const FWadSourceLightingMetadata& M,FString& Error)
{
    if(!Validate(M,Error)){return false;}Metadata=M;bReady=true;return true;
}
bool NativeWadLightRules::FPreparedKernel::Step(FWadSourceLightState& S,TConstArrayView<int32> Doors,int32 Outside,
    TConstArrayView<int32> Centers,FString& Error) const
{
    if(!bReady||S.Version!=FeedbackStateVersion||S.CompletedSteps==uint64(MAX_int64)||!ValidateState(Metadata,S,Error)||Centers.Num()!=Metadata.Illuminators.Num())
    {if(Error.IsEmpty()){Error=TEXT("Source feedback kernel/state/owner dimensions are not initialized.");}return false;}
    for(int32 I=0;I<Centers.Num();++I)
    {
        if(Centers[I]<INDEX_NONE||Centers[I]>=Metadata.Sectors.Num()||(Centers[I]==INDEX_NONE&&Metadata.Illuminators[I].SourceType==924))
        {return Fail(Error,TEXT("Invalid live center for an illumination owner."));}
    }
    TArray<int32> Next,Phases;
    if(!RecalculatePrepared(Metadata,S.Levels,Doors,Outside,Next,Error,Centers)
        ||!AnimatePrepared(Metadata,1,Next,S.Phases,Next,Phases,Error)){return false;}
    S.Levels=MoveTemp(Next);S.Phases=MoveTemp(Phases);++S.CompletedSteps;return true;
}

FString NativeWadLightRules::StateDigest(const FWadSourceLightState& S)
{
    // Runtime/restart diagnostic, not a cross-platform network wire format.
    FSHA1 Sha;const FTCHARToUTF8 Source(*S.SourceMapSha1);
    Sha.Update(reinterpret_cast<const uint8*>(Source.Get()),Source.Length());
    Sha.Update(reinterpret_cast<const uint8*>(&S.Version),sizeof(S.Version));
    Sha.Update(reinterpret_cast<const uint8*>(S.Levels.GetData()),S.Levels.Num()*sizeof(int32));
    Sha.Update(reinterpret_cast<const uint8*>(S.Phases.GetData()),S.Phases.Num()*sizeof(int32));
    Sha.Update(reinterpret_cast<const uint8*>(&S.PendingTicks),sizeof(S.PendingTicks));
    Sha.Update(reinterpret_cast<const uint8*>(&S.CompletedSteps),sizeof(S.CompletedSteps));
    Sha.Final();FSHAHash Hash;Sha.GetHash(Hash.Hash);return Hash.ToString();
}
