#pragma once
#include "NativeWadLevel.h"
#include "Algo/Find.h"

struct FWadAmbientMarkerDefinition
{
    int32 Sound=0;
    int32 Radius=500;
    int32 MaxVolume=255;
    bool bLoop=true;
};
// Literal3D_IO.C special-object definitions; hard/soft wind deliberately share3004.
inline FWadAmbientMarkerDefinition ResolveWadAmbientMarker(int32 Type)
{
    switch(Type)
    {
    case 1508:return {3010};case 1509:return {3008};case 1510:return {3009};
    case 1511:return {3004,500,128};case 1512:return {3004};
    case 1513:return {3003};case 1514:return {3007};case 1515:return {3019};
    case 1516:return {3006};case 1517:return {3001};case 1528:return {3000};
    case 1529:return {3002};case 1530:return {3011};case 1531:return {3012};
    case 1532:return {3013};case 1533:return {3014};case 1534:return {3015};
    case 1535:return {3016};case 1536:return {3018};default:return {};
    }
}
inline int32 WadAmbientVolume(const FWadAmbientEmitter& Emitter,const FVector2D& Listener)
{
    if(Emitter.Radius<=0){return 0;}
    // Source uses integer XY and integer distance/volume. Height does not attenuate.
    const FVector2D Point(FMath::FloorToDouble(Listener.X),FMath::FloorToDouble(Listener.Y));
    const int32 Distance=FMath::FloorToInt(FVector2D::Distance(Point,FVector2D(Emitter.Position)));
    return Distance>=Emitter.Radius?(Emitter.bLoop?0:10):int32(int64(FMath::Clamp(Emitter.MaxVolume,0,255))*(Emitter.Radius-Distance)/Emitter.Radius);
}
inline TMap<int32,int32> CalculateWadAmbientVoices(TConstArrayView<FWadAmbientEmitter> Emitters,const FVector2D& Listener)
{
    TMap<int32,int32> Leaders,Voices;
    for(int32 Index=0;Index<Emitters.Num();++Index)
    {
        const auto& E=Emitters[Index];int32 Leader=Index;
        if(E.Group!=0)
        {
            if(const int32* Existing=Leaders.Find(E.Group)){Leader=*Existing;}else{Leaders.Add(E.Group,Index);}
        }
        int32& Volume=Voices.FindOrAdd(Leader);Volume=FMath::Max(Volume,WadAmbientVolume(E,Listener));
    }
    for(auto It=Voices.CreateIterator();It;++It){if(It.Value()==0){It.RemoveCurrent();}}
    return Voices;
}

// Source binding/legacy migration is shared by normal import and metadata-only
// refresh. Existing fields and PCM overrides remain authoritative.
inline bool MergeWadAmbientEmitters(TConstArrayView<FWadAmbientEmitter> Existing,TConstArrayView<FWadAmbientEmitter> Expected,
    TArray<FWadAmbientEmitter>& Updated,FOutputDevice& Output)
{
    TArray<int32> LegacyIds;
    for(const auto& E:Expected){if(E.MarkerType==1513||E.MarkerType==1514){LegacyIds.Add(E.SourceThingId);}}
        TMap<int32,int32> Bindings;TSet<int32> Used;
        for(int32 Index=0;Index<Existing.Num();++Index)
        {
            const auto& E=Existing[Index];if(E.SourceThingId==INDEX_NONE){continue;}
            if(Bindings.Contains(E.SourceThingId)||Algo::FindByPredicate(Expected,[&](const auto& Source){return Source.SourceThingId==E.SourceThingId;})==nullptr)
            {Output.Log(TEXT("Ambient refresh found a duplicate/invalid saved source Thing binding."));return false;}
            Bindings.Add(E.SourceThingId,Index);Used.Add(Index);
        }
        // The previous serialized format stored exactly the stream/waterfall
        // markers in THINGS order. Recover their IDs without discarding edits.
        const bool bLegacy=Existing.Num()==LegacyIds.Num()
            &&Algo::FindByPredicate(Existing,[](const auto& E){return E.SourceThingId!=INDEX_NONE||E.MarkerType!=0;})==nullptr;
        if(bLegacy)
        {
            for(int32 Index=0;Index<LegacyIds.Num();++Index){Bindings.Add(LegacyIds[Index],Index);Used.Add(Index);}
        }
        for(const auto& E:Expected)
        {
            if(Bindings.Contains(E.SourceThingId)|| (E.MarkerType!=1513&&E.MarkerType!=1514)){continue;}
            int32 Match=INDEX_NONE;
            for(int32 Index=0;Index<Existing.Num();++Index)
            {
                const auto& Old=Existing[Index];
                if(Used.Contains(Index)||Old.Sound!=E.Sound||!FVector2D(Old.Position).Equals(FVector2D(E.Position),0.001)){continue;}
                if(Match!=INDEX_NONE){Match=INDEX_NONE;break;}Match=Index;
            }
            if(Match!=INDEX_NONE){Bindings.Add(E.SourceThingId,Match);Used.Add(Match);}
        }
        TArray<FWadAmbientEmitter> Result;
        for(const auto& E:Expected)
        {
            FWadAmbientEmitter Value=E;
            if(const int32* Index=Bindings.Find(E.SourceThingId))
            {Value=Existing[*Index];Value.SourceThingId=E.SourceThingId;Value.MarkerType=E.MarkerType;}
            Result.Add(Value);
        }
        for(int32 Index=0;Index<Existing.Num();++Index)
        {if(!Used.Contains(Index)){Result.Add(Existing[Index]);}} // Retain unbound authored/custom emitters.
    Updated=MoveTemp(Result);
    return true;
}
