#include "NativeWadLevel.h"
#include "NativeWadScenery.h"
#include "NativeWadScenerySource.h"
#include "Misc/SecureHash.h"

namespace
{
    bool LookupMatches(const FWadSourceLightingMetadata& Metadata,TConstArrayView<uint8> Bytes,FString& Error)
    {
        if(Bytes.Num()<12||Bytes.Num()>16*1024*1024){Error=TEXT("Source feedback needs bounded cooked original lookup bytes.");return false;}
        FSHAHash Hash;FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Hash.Hash);
        if(Hash.ToString()!=Metadata.SourceMapSha1){Error=TEXT("Cooked source lookup digest differs from lighting metadata.");return false;}return true;
    }
}

bool ANativeWadLevel::SetSourceLightFeedbackEnabled(bool bEnabled,TConstArrayView<uint8> Bytes,FString& Error)
{
    if(bEnabled)
    {
        if(Bytes.IsEmpty()){Bytes=SourceLightLookupBytes;}
        if(!NativeWadLightRules::Validate(SourceLightingMetadata,Error)||SourceLightingMetadata.Sectors.Num()!=Sectors.Num()
            ||!LookupMatches(SourceLightingMetadata,Bytes,Error)){return false;}
        for(const auto& Illuminator:SourceLightingMetadata.Illuminators)
        {
            if(Illuminator.SourceType==924){continue;}
            const auto* Prop=Cast<ANativeWadScenery>(Illuminator.BoundActor);
            if(!Prop||Prop->GetSourceThingId()!=Illuminator.ThingId||(Prop->GetSourceType()!=0&&Prop->GetSourceType()!=Illuminator.SourceType))
            {Error=TEXT("Source feedback candidate requires each original illuminating prop bound exactly once.");return false;}
        }
        for(const auto& Sector:SourceLightingMetadata.Sectors)
        {
            if(!Sector.bDoor){continue;}
            const auto* Door=Doors.FindByPredicate([&](const auto& D){return D.Sector==Sector.Sector&&D.OpenZ>D.ClosedZ;});
            if(!Door){Error=TEXT("Source feedback candidate lacks a native range for an original door.");return false;}
        }
    }
    // Prepare all fallible runtime data before changing mode, lookup or state.
    TSharedPtr<NativeWadLightRules::FPreparedKernel> Prepared;
    TArray<FVector2D> Positions;TArray<int32> Centers;TArray<uint8> Copy;
    if(bEnabled)
    {
        Prepared=MakeShared<NativeWadLightRules::FPreparedKernel>();
        if(!Prepared->Initialize(SourceLightingMetadata,Error)){return false;}
        Copy.Append(Bytes.GetData(),Bytes.Num());
        for(const auto& O:SourceLightingMetadata.Illuminators)
        {
            FVector2D XY=O.CenterPosition;int32 Sector=O.CenterSector;
            if(O.SourceType!=924)
            {
                const FVector Local=GetActorTransform().InverseTransformPosition(O.BoundActor->GetActorLocation());XY=FVector2D(Local.X,Local.Y);
                if(!FMath::IsFinite(XY.X)||!FMath::IsFinite(XY.Y)||XY.X<MIN_int16||XY.X>MAX_int16||XY.Y<MIN_int16||XY.Y>MAX_int16)
                {Error=TEXT("Candidate illumination owner has an invalid actual center.");return false;}
                if(XY!=O.CenterPosition)
                {
                    bool Fallback=false;
                    if(!NativeWadScenerySource::ResolveMountSector(Copy,FIntPoint(FMath::FloorToInt(XY.X),FMath::FloorToInt(XY.Y)),Sector,Fallback,Error)
                        ||!Sectors.IsValidIndex(Sector)){return false;}
                }
            }
            Positions.Add(XY);Centers.Add(Sector);
        }
    }
    Modify();bUseSourceLightFeedback=bEnabled;
    if(bEnabled){SourceLightLookupBytes=MoveTemp(Copy);}
    SourceLightKernel=MoveTemp(Prepared);SourceLightState=bEnabled?NativeWadLightRules::InitialState(SourceLightingMetadata):FWadSourceLightState{};
    SourceOwnerPositions=MoveTemp(Positions);SourceOwnerCenters=MoveTemp(Centers);SourceLightingError.Reset();SourceRestoredDigest.Reset();
    SourceLookupResolutions=0;SourceLastStepCount=0;SourceLastMilliseconds=0;
    RefreshSectorLights();if(bEnabled){SetActorTickEnabled(true);}Error.Reset();return true;
}

bool ANativeWadLevel::InitializeSourceLightFeedback(FString& Error)
{
    if(!bUseSourceLightFeedback){Error=TEXT("Source feedback is disabled.");return false;}
    if(SourceLightKernel){return true;}
    if(SourceLightingMetadata.Sectors.Num()!=Sectors.Num()||!LookupMatches(SourceLightingMetadata,SourceLightLookupBytes,Error)){return false;}
    auto Kernel=MakeShared<NativeWadLightRules::FPreparedKernel>();
    if(!Kernel->Initialize(SourceLightingMetadata,Error)){return false;}
    SourceLightKernel=MoveTemp(Kernel);SourceLightState=NativeWadLightRules::InitialState(SourceLightingMetadata);
    SourceOwnerPositions.Reset();SourceOwnerCenters.Reset();
    for(const auto& O:SourceLightingMetadata.Illuminators){SourceOwnerPositions.Add(O.CenterPosition);SourceOwnerCenters.Add(O.CenterSector);}
    if(!UpdateSourceOwnerCenters(Error)){SourceLightKernel.Reset();SourceLightState={};return false;}return true;
}

bool ANativeWadLevel::UpdateSourceOwnerCenters(FString& Error)
{
    auto Positions=SourceOwnerPositions;auto Centers=SourceOwnerCenters;
    for(int32 I=0;I<SourceLightingMetadata.Illuminators.Num();++I)
    {
        const auto& Illuminator=SourceLightingMetadata.Illuminators[I];
        if(Illuminator.SourceType==924){continue;}
        if(!IsValid(Illuminator.BoundActor)){Centers[I]=INDEX_NONE;continue;}
        const FVector Local=GetActorTransform().InverseTransformPosition(Illuminator.BoundActor->GetActorLocation());
        const FVector2D XY(Local.X,Local.Y);
        if(XY==Positions[I]&&Centers[I]!=INDEX_NONE){continue;}
        if(!FMath::IsFinite(XY.X)||!FMath::IsFinite(XY.Y)||XY.X<MIN_int16||XY.X>MAX_int16||XY.Y<MIN_int16||XY.Y>MAX_int16)
        {Error=TEXT("Moved source illumination owner lies outside signed16 XY bounds.");return false;}
        bool Fallback=false;int32 Sector=INDEX_NONE;
        ++SourceLookupResolutions;
        if(!NativeWadScenerySource::ResolveMountSector(SourceLightLookupBytes,FIntPoint(FMath::FloorToInt(XY.X),FMath::FloorToInt(XY.Y)),Sector,Fallback,Error)
            ||!Sectors.IsValidIndex(Sector)){return false;}
        // Original ordinary ObjectSetUpSectors uses0 when its lookup is invalid.
        Positions[I]=XY;Centers[I]=Sector;
    }
    SourceOwnerPositions=MoveTemp(Positions);SourceOwnerCenters=MoveTemp(Centers);Error.Reset();return true;
}

void ANativeWadLevel::PresentSourceLightState()
{
    if(SourceLightState.Version!=NativeWadLightRules::FeedbackStateVersion||SourceLightState.Levels.Num()!=Sectors.Num()){return;}
    for(int32 I=0;I<Sectors.Num();++I)
    {
        // Persistent native overrides affect display only. They do not seed the
        // original feedback bytes or replace animated source owners.
        const int32* Override=LightOverrides.Find(I);ApplySectorLight(I,Override?*Override:SourceLightState.Levels[I]);
    }
}

void ANativeWadLevel::AdvanceSourceLightFeedback(double Seconds)
{
    SourceLastStepCount=0;SourceLastMilliseconds=0.0;if(!bUseSourceLightFeedback){return;}
    const double Started=FPlatformTime::Seconds();FString Error;
    const auto Fail=[&]()
    {
        if(Error!=SourceLightingError){UE_LOG(LogTemp,Error,TEXT("Source feedback stopped before a step: %s"),*Error);SourceLightingError=Error;}
        SourceLastMilliseconds=(FPlatformTime::Seconds()-Started)*1000.0;
    };
    if(!FMath::IsFinite(Seconds)||Seconds<0.0||!InitializeSourceLightFeedback(Error))
    {if(Error.IsEmpty()){Error=TEXT("Invalid source feedback delta.");}Fail();return;}
    const double Pending=SourceLightState.PendingTicks+Seconds*NativeWadLightRules::FeedbackHz;
    if(!FMath::IsFinite(Pending)||Pending>NativeWadLightRules::MaximumPendingTicks)
    {Error=TEXT("Source feedback clock debt exceeds bounded checkpoint range.");Fail();return;}
    if(!UpdateSourceOwnerCenters(Error)){Fail();return;}
    TArray<int32> Weights;Weights.Init(256,Sectors.Num());
    for(const auto& Door:Doors)
    {
        FVector2D Range;
        if(Door.OpenZ>Door.ClosedZ&&GetSectorSurfaceHeights(Door.Sector,Range))
        {Weights[Door.Sector]=int32(FMath::Clamp((FMath::FloorToDouble(Range.Y)-Door.ClosedZ)/(Door.OpenZ-Door.ClosedZ),0.0,1.0)*256.0);}
    }
    SourceLightState.PendingTicks=Pending;
    while(SourceLightState.PendingTicks+1.e-9>=1.0&&SourceLastStepCount<NativeWadLightRules::MaximumStepsPerFrame)
    {
        if(!SourceLightKernel->Step(SourceLightState,Weights,FMath::Clamp(OutsideLight,0,255),SourceOwnerCenters,Error)){Fail();return;}
        SourceLightState.PendingTicks=FMath::Max(0.0,SourceLightState.PendingTicks-1.0);++SourceLastStepCount;
    }
    PresentSourceLightState();SourceLightingError.Reset();SourceLastMilliseconds=(FPlatformTime::Seconds()-Started)*1000.0;
}

bool ANativeWadLevel::ValidateSourceLightCheckpoint(const FWadSourceLightState& State,FString& Error) const
{
    if(!NativeWadLightRules::ValidateState(SourceLightingMetadata,State,Error)){return false;}
    if(State.Version!=0&&!bUseSourceLightFeedback)
    {Error=TEXT("Modern source feedback checkpoint cannot silently load on a legacy map.");return false;}
    if(bUseSourceLightFeedback)
    {
        if(SourceLightingMetadata.Sectors.Num()!=Sectors.Num()||!NativeWadLightRules::Validate(SourceLightingMetadata,Error)
            ||!LookupMatches(SourceLightingMetadata,SourceLightLookupBytes,Error)){return false;}
        if(!SourceLightKernel)
        {
            // Prove cold initialization can finish before the caller mutates any
            // geometry/script/encounter state, including genuine legacy absence.
            for(const auto& O:SourceLightingMetadata.Illuminators)
            {
                if(O.SourceType==924||!IsValid(O.BoundActor)){continue;}
                const FVector Local=GetActorTransform().InverseTransformPosition(O.BoundActor->GetActorLocation());
                const FVector2D XY(Local.X,Local.Y);
                if(!FMath::IsFinite(XY.X)||!FMath::IsFinite(XY.Y)||XY.X<MIN_int16||XY.X>MAX_int16||XY.Y<MIN_int16||XY.Y>MAX_int16)
                {Error=TEXT("Cold source feedback checkpoint has an invalid current owner center.");return false;}
                if(XY!=O.CenterPosition)
                {
                    int32 Sector=INDEX_NONE;bool Fallback=false;
                    if(!NativeWadScenerySource::ResolveMountSector(SourceLightLookupBytes,FIntPoint(FMath::FloorToInt(XY.X),FMath::FloorToInt(XY.Y)),Sector,Fallback,Error)
                        ||!Sectors.IsValidIndex(Sector)){return false;}
                }
            }
        }
    }
    Error.Reset();return true;
}

bool ANativeWadLevel::RestoreSourceLightState(const FWadSourceLightState& State,FString& Error)
{
    if(!ValidateSourceLightCheckpoint(State,Error)){return false;}
    if(bUseSourceLightFeedback)
    {
        if(!InitializeSourceLightFeedback(Error)){return false;}
        SourceLightState=State.Version==0?NativeWadLightRules::InitialState(SourceLightingMetadata):State;
        SourceRestoredDigest=State.Version==0?FString():NativeWadLightRules::StateDigest(SourceLightState);
        if(!SourceRestoredDigest.IsEmpty())
        {UE_LOG(LogTemp,Display,TEXT("Restored source feedback live bytes/phases/debt/steps digest %s at steps%llu"),*SourceRestoredDigest,SourceLightState.CompletedSteps);}
        SetActorTickEnabled(true);PresentSourceLightState();
    }
    Error.Reset();return true;
}

int32 ANativeWadLevel::GetSourceIlluminationOwnerSector(int32 Id) const
{
    const int32 Index=SourceLightingMetadata.Illuminators.IndexOfByPredicate([&](const auto& O){return O.ThingId==Id;});
    return SourceOwnerCenters.IsValidIndex(Index)?SourceOwnerCenters[Index]:INDEX_NONE;
}
