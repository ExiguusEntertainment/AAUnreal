#include "NativeWadImport.h"

#include "Engine/Scene.h"

void ConfigureWadLumen(FPostProcessSettings& Settings)
{
    Settings.bOverride_DynamicGlobalIlluminationMethod = true;
    Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
    Settings.bOverride_ReflectionMethod = true;
    Settings.ReflectionMethod = EReflectionMethod::Lumen;
    Settings.bOverride_LumenSceneDetail = true;
    Settings.LumenSceneDetail = 3.0f;
    Settings.bOverride_LumenSceneLightingUpdateSpeed = true;
    Settings.LumenSceneLightingUpdateSpeed = 4.0f;
    Settings.bOverride_LumenFinalGatherLightingUpdateSpeed = true;
    Settings.LumenFinalGatherLightingUpdateSpeed = 4.0f;
    Settings.bOverride_AutoExposureMinBrightness = true;
    Settings.bOverride_AutoExposureMaxBrightness = true;
    Settings.AutoExposureMinBrightness = -2.0f;
    Settings.AutoExposureMaxBrightness = -2.0f;
}

#if WITH_EDITOR

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DoomWadLoader.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"
#include "UObject/SavePackage.h"
#include "WadSurfaceTextures.h"

namespace
{
    constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;
    constexpr float SlabThickness = 10.0f;
    constexpr float WallMeshSize = 128.0f;
    constexpr uint16 DoorMarker = 1500;
    constexpr uint16 DoorLockMarker = 1501;
    constexpr uint16 LevelLinkMarker = 1518;
    constexpr uint16 DoorRequiredItemMarker = 1527;
    constexpr uint16 TwoSidedLine = 0x0004;
    constexpr float TicksPerSecond = 70.0f;
    const FName SurfaceSlot(TEXT("Surface"));

    struct FWadMeshDescription
    {
        FMeshDescription Description;
        FStaticMeshAttributes Attributes{Description};
        FPolygonGroupID Group;

        FWadMeshDescription()
        {
            Attributes.Register();
            Attributes.GetVertexInstanceUVs().SetNumChannels(1);
            Group = Description.CreatePolygonGroup();
            Attributes.GetPolygonGroupMaterialSlotNames()[Group] = SurfaceSlot;
        }

        FVertexID AddVertex(const FVector3f& Position)
        {
            const FVertexID Vertex = Description.CreateVertex();
            Attributes.GetVertexPositions()[Vertex] = Position;
            return Vertex;
        }

        void AddTriangle(const TStaticArray<FVertexID, 3>& Vertices, const TStaticArray<FVector2f, 3>& UVs)
        {
            const auto Positions = Attributes.GetVertexPositions();
            const FVector3f Edge = Positions[Vertices[1]] - Positions[Vertices[0]];
            const FVector3f Other = Positions[Vertices[2]] - Positions[Vertices[0]];
            const FVector3f Normal = FVector3f::CrossProduct(Other, Edge).GetSafeNormal();
            if (Normal.IsNearlyZero())
            {
                return;
            }
            const FVector2f Delta = UVs[1] - UVs[0];
            const FVector2f OtherDelta = UVs[2] - UVs[0];
            const float UVSign = FMath::Sign(Delta.X * OtherDelta.Y - Delta.Y * OtherDelta.X);
            const FVector3f Tangent = (UVSign * (OtherDelta.Y * Edge - Delta.Y * Other)).GetSafeNormal(UE_SMALL_NUMBER, Edge.GetSafeNormal());
            FVertexInstanceID Corners[3];
            for (int32 Index = 0; Index < 3; ++Index)
            {
                const FVertexInstanceID Corner = Description.CreateVertexInstance(Vertices[Index]);
                Attributes.GetVertexInstanceNormals()[Corner] = Normal;
                Attributes.GetVertexInstanceTangents()[Corner] = Tangent;
                Attributes.GetVertexInstanceBinormalSigns()[Corner] = -UVSign;
                Attributes.GetVertexInstanceUVs().Set(Corner, 0, UVs[Index]);
                Corners[Index] = Corner;
            }
            Description.CreateTriangle(Group, Corners);
        }

        void AddQuad(FVertexID A, FVertexID B, FVertexID C, FVertexID D)
        {
            AddTriangle({A, B, C}, {FVector2f(0,0), FVector2f(0,-1), FVector2f(1,-1)});
            AddTriangle({A, C, D}, {FVector2f(0,0), FVector2f(1,-1), FVector2f(1,0)});
        }
    };

    bool SaveAsset(UObject& Asset, const TCHAR* Extension)
    {
        UPackage* Package = Asset.GetOutermost();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        return UPackage::SavePackage(Package, &Asset, *Filename, Args);
    }

    bool HasTexture(const FString& Name)
    {
        return !Name.IsEmpty() && Name != TEXT("-") && !Name.StartsWith(TEXT("F_SKY"));
    }

    bool ContainsPoint(const FDoomWadSectorFootprint& Footprint, const FVector2D& Point)
    {
        for (int32 Index = 0; Index < Footprint.Triangles.Num(); Index += 3)
        {
            const FVector2D& A = Footprint.Vertices[Footprint.Triangles[Index]];
            const FVector2D& B = Footprint.Vertices[Footprint.Triangles[Index + 1]];
            const FVector2D& C = Footprint.Vertices[Footprint.Triangles[Index + 2]];
            if (FVector2D::CrossProduct(B - A, Point - A) >= 0.0
                && FVector2D::CrossProduct(C - B, Point - B) >= 0.0
                && FVector2D::CrossProduct(A - C, Point - C) >= 0.0)
            {
                return true;
            }
        }
        return false;
    }
}

struct FAAWadNativeImport
{
    FString AssetRoot;
    TMap<FString, UTexture2D*> Textures;
    TMap<FString, UMaterialInterface*> Materials;
    TArray<UObject*> AssetsToSave;
    TStaticArray<UMaterialInterface*, 3> Parents;
    TStaticArray<UStaticMesh*, 3> WallMeshes;
    bool bAssetError = false;

    static bool BuildDoors(const FDoomWadMapData& Map, TArray<FAAWadNativeDoor>& Doors, FOutputDevice& Output)
    {
        TArray<int32> Markers;
        for (int32 Index = 0; Index < Map.Things.Num(); ++Index)
        {
            const FWadThing& Thing = Map.Things[Index];
            if (Thing.Type == DoorMarker || ((Thing.Type == DoorLockMarker || Thing.Type == DoorRequiredItemMarker) && Thing.Flags != 0))
            {
                Markers.Add(Index);
            }
        }
        if (Markers.IsEmpty())
        {
            return true;
        }

        TMap<int32, int32> MarkerSectors;
        TArray<int16> Ceilings;
        Ceilings.Reserve(Map.Sectors.Num());
        for (int32 Sector = 0; Sector < Map.Sectors.Num(); ++Sector)
        {
            Ceilings.Add(Map.Sectors[Sector].CeilingHeight);
            FDoomWadSectorFootprint Footprint;
            if (!UDoomWadLoader::GetLastWadSectorFootprint(Sector, Footprint))
            {
                continue;
            }
            for (const int32 Marker : Markers)
            {
                const FWadThing& Thing = Map.Things[Marker];
                if (ContainsPoint(Footprint, FVector2D(Thing.X, Thing.Y)))
                {
                    if (MarkerSectors.Contains(Marker))
                    {
                        Output.Logf(TEXT("WAD door marker %d touches multiple sectors."), Marker);
                        return false;
                    }
                    MarkerSectors.Add(Marker, Sector);
                }
            }
        }
        TSet<int32> RestrictedSectors;
        for (const int32 Marker : Markers)
        {
            if (const int32* Sector = MarkerSectors.Find(Marker); Sector && Map.Things[Marker].Type != DoorMarker)
            {
                RestrictedSectors.Add(*Sector);
            }
        }
        for (const int32 Marker : Markers)
        {
            if (Map.Things[Marker].Type != DoorMarker)
            {
                continue;
            }
            const int32* Sector = MarkerSectors.Find(Marker);
            if (!Sector || Doors.ContainsByPredicate([Sector](const FAAWadNativeDoor& Door) { return Door.Sector == *Sector; }))
            {
                Output.Logf(TEXT("WAD door marker %d has no unique sector."), Marker);
                return false;
            }
            int32 LowestCeiling = MAX_int32;
            for (const FWadLineDef& Line : Map.Lines)
            {
                if ((Line.Flags & TwoSidedLine) == 0)
                {
                    continue;
                }
                const int32 Front = Map.GetSectorForSide(Line.Side[0]);
                const int32 Back = Map.GetSectorForSide(Line.Side[1]);
                const int32 Neighbor = Front == *Sector ? Back : Back == *Sector ? Front : INDEX_NONE;
                if (Ceilings.IsValidIndex(Neighbor) && Neighbor != *Sector)
                {
                    LowestCeiling = FMath::Min(LowestCeiling, int32(Ceilings[Neighbor]));
                }
            }
            const int16 Floor = Map.Sectors[*Sector].FloorHeight;
            if (LowestCeiling == MAX_int32 || LowestCeiling - 16 <= Floor)
            {
                Output.Logf(TEXT("WAD door sector %d has no valid open height."), *Sector);
                return false;
            }
            // The original loader leaves a 16-unit lip and uses a 70 Hz game clock.
            Doors.Add({ *Sector, Marker, float(Floor), float(LowestCeiling - 16),
                105.0f / TicksPerSecond, 140.0f / TicksPerSecond, RestrictedSectors.Contains(*Sector) });
            Ceilings[*Sector] = Floor;
        }
        return true;
    }

    static bool MatchesTopology(const ANativeWadLevel& Actor, const FDoomWadMapData& Map)
    {
        if (Actor.Sectors.Num() != Map.Sectors.Num() || Actor.Vertices.Num() != Map.Vertices.Num()
            || Actor.Lines.Num() != Map.Lines.Num() || Actor.Sides.Num() != Map.Sides.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < Map.Vertices.Num(); ++Index)
        {
            if (Actor.Vertices[Index] != FIntPoint(Map.Vertices[Index].X, Map.Vertices[Index].Y))
            {
                return false;
            }
        }
        for (int32 Index = 0; Index < Map.Lines.Num(); ++Index)
        {
            const FWadLineDef& Line = Map.Lines[Index];
            const FAAWadNativeLine& Saved = Actor.Lines[Index];
            if (Saved.Endpoints != FIntPoint(Line.From, Line.To) || Saved.Sides != FIntPoint(Line.Side[0], Line.Side[1])
                || Saved.Flags != Line.Flags)
            {
                return false;
            }
        }
        for (int32 Index = 0; Index < Map.Sides.Num(); ++Index)
        {
            if (Actor.Sides[Index].Sector != Map.GetSectorForSide(Index))
            {
                return false;
            }
        }
        return true;
    }

    static ANativeWadLevel* FindMatchingLevel(UWorld& World, const FDoomWadMapData& Map)
    {
        TActorIterator<ANativeWadLevel> Levels(&World);
        if (!Levels || !MatchesTopology(**Levels, Map))
        {
            return nullptr;
        }
        ANativeWadLevel* Actor = *Levels;
        ++Levels;
        return Levels ? nullptr : Actor;
    }

    static void SetOutdoorSectors(ANativeWadLevel& Actor, const FDoomWadMapData& Map)
    {
        for (int32 Index = 0; Index < Map.Sectors.Num(); ++Index)
        {
            FString Floor;
            FString Ceiling;
            UDoomWadLoader::GetLastWadSectorTextureNames(Index, Floor, Ceiling);
            Actor.Sectors[Index].bOutside = Ceiling.StartsWith(TEXT("F_SKY")) || (Map.Sectors[Index].Trigger & 1) != 0;
        }
        Actor.bUseSectorLightingRules = true;
    }

    static bool AddPlayerStart(UWorld& World, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        const FWadThing* Start = Map.Things.FindByPredicate([](const FWadThing& Thing) { return Thing.Type == 1; });
        if (!Start)
        {
            Output.Log(TEXT("The source map has no player-one start."));
            return false;
        }
        const FVector2D XY(Start->X, Start->Y);
        int32 Sector = INDEX_NONE;
        for (int32 Index = 0; Index < Map.Sectors.Num(); ++Index)
        {
            FDoomWadSectorFootprint Footprint;
            if (UDoomWadLoader::GetLastWadSectorFootprint(Index, Footprint) && ContainsPoint(Footprint, XY))
            {
                Sector = Index;
                break;
            }
        }
        if (Sector == INDEX_NONE)
        {
            Output.Log(TEXT("The source player start falls outside the sector footprints."));
            return false;
        }
        for (TActorIterator<APlayerStart> Existing(&World); Existing; ++Existing)
        {
            if (Existing->PlayerStartTag == TEXT("AA.PlayerOne"))
            {
                return true;
            }
            Output.Log(TEXT("The level already contains a user-authored Player Start."));
            return true;
        }
        const FVector Position(XY, Map.Sectors[Sector].FloorHeight + ANativeWadPlayer::CapsuleHalfHeight + 2.0);
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        APlayerStart* PlayerStart = World.SpawnActor<APlayerStart>(Position, FRotator(0, Start->Angle * 45.0, 0), Params);
        if (!PlayerStart)
        {
            return false;
        }
        PlayerStart->GetCapsuleComponent()->SetCapsuleSize(ANativeWadPlayer::CapsuleRadius, ANativeWadPlayer::CapsuleHalfHeight);
        PlayerStart->PlayerStartTag = TEXT("AA.PlayerOne");
        PlayerStart->SetActorLabel(TEXT("Source Player One Start"));
        Output.Logf(TEXT("Imported player one start at (%d, %d) in sector %d."), Start->X, Start->Y, Sector);
        return true;
    }

    static bool BuildLevelLinks(const FDoomWadMapData& Map, TArray<FAAWadNativeLevelLink>& Links, FOutputDevice& Output)
    {
        for (int32 Index = 0; Index < Map.Things.Num(); ++Index)
        {
            const FWadThing& Thing = Map.Things[Index];
            if (Thing.Type != LevelLinkMarker || Thing.Flags == 0)
            {
                continue;
            }
            int32 Sector = INDEX_NONE;
            for (int32 Candidate = 0; Candidate < Map.Sectors.Num(); ++Candidate)
            {
                FDoomWadSectorFootprint Footprint;
                if (UDoomWadLoader::GetLastWadSectorFootprint(Candidate, Footprint)
                    && ContainsPoint(Footprint, FVector2D(Thing.X, Thing.Y)))
                {
                    Sector = Candidate;
                    break;
                }
            }
            if (Sector == INDEX_NONE)
            {
                Output.Logf(TEXT("WAD level link marker %d falls outside the sector footprints."), Index);
                return false;
            }
            // The original client sends the map number on sector entry and arrives at start zero.
            const FString Destination = FString::Printf(TEXT("/Game/Maps/L%d.L%d"), Thing.Flags, Thing.Flags);
            Links.RemoveAll([Sector](const FAAWadNativeLevelLink& Link) { return Link.Sector == Sector; });
            Links.Add({Sector, Index, TSoftObjectPtr<UWorld>(FSoftObjectPath(Destination))});
            Output.Logf(TEXT("Imported WAD level link thing %d in sector %d to %s."), Index, Sector, *Destination);
        }
        return true;
    }

    bool InstallSurfaceEnvironment(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        FOutputDevice& Output)
    {
        constexpr int32 Water = 1505;
        constexpr int32 Depth = 1521;
        constexpr int32 FlowSlow = 1523;
        constexpr int32 FlowFast = 1525;
        constexpr int32 WallUpSlow = 1537;
        constexpr int32 WallDownFast = 1542;
        constexpr int32 WaterDepth = 20;
        constexpr double TexelsPerSourceScrollUnit = 70.0 / 64.0;
        TArray<uint8> Animation;
        int32 SideStart = 0, SectorStart = 0, SideCount = 0, SectorCount = 0;
        const auto ReadWord = [&Animation](int32 Offset)
        {
            return uint16(Animation[Offset] | (uint16(Animation[Offset + 1]) << 8));
        };
        if (Map.Things.ContainsByPredicate([](const FWadThing& Thing)
            { return (Thing.Type >= FlowSlow && Thing.Type <= FlowFast) || (Thing.Type >= WallUpSlow && Thing.Type <= WallDownFast); }))
        {
            FString Anims = FPaths::GetPath(WadPath) / TEXT("MAP.ANI");
            if (!FPaths::FileExists(Anims))
            {
                Anims = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/MAP.ANI");
            }
            if (!FFileHelper::LoadFileToArray(Animation, *Anims) || Animation.Num() < 24)
            {
                Output.Log(TEXT("Could not read the source map animation states."));
                return false;
            }
            // The legacy header includes four on-disk pointer slots. The state tables follow it.
            SideStart = 24 + 16 * ReadWord(0);
            SideCount = ReadWord(2);
            SectorCount = ReadWord(4);
            SectorStart = SideStart + 42 * SideCount;
            if (SectorStart + 66 * SectorCount > Animation.Num())
            {
                return false;
            }
        }
        Actor.ScrollingSides.Reset();
        Actor.ScrollingSectors.Reset();
        Actor.FlowSectors.Reset();
        for (FAAWadNativeSector& Sector : Actor.Sectors)
        {
            Sector.FloorDepth = 0;
            Sector.bWater = false;
            Sector.FlowImpulse = FVector2D::ZeroVector;
            Sector.FloorScrollRate = FVector2D::ZeroVector;
        }
        for (FAAWadNativeSide& Side : Actor.Sides)
        {
            Side.ScrollRate = FVector2D::ZeroVector;
        }
        int32 WaterSectors = 0;
        for (const FWadThing& Thing : Map.Things)
        {
            const FVector2D Position(Thing.X, Thing.Y);
            if (Thing.Type == Water || Thing.Type == Depth || (Thing.Type >= FlowSlow && Thing.Type <= FlowFast))
            {
                const int32 Index = Actor.FindWadSector(Position);
                if (!Actor.Sectors.IsValidIndex(Index))
                {
                    Output.Logf(TEXT("Surface marker %d at %s is outside the map."), Thing.Type, *Position.ToString());
                    return false;
                }
                FAAWadNativeSector& Sector = Actor.Sectors[Index];
                if (Thing.Type == Water)
                {
                    Sector.bWater = true;
                    Sector.FloorDepth = WaterDepth;
                    ++WaterSectors;
                }
                else if (Thing.Type == Depth)
                {
                    Sector.FloorDepth = Thing.Flags;
                }
                else
                {
                    const int32 State = (Thing.Type - FlowSlow) * 8 + Thing.Angle;
                    if (Thing.Angle >= 8 || State >= SectorCount)
                    {
                        return false;
                    }
                    const int32 Offset = SectorStart + 66 * State;
                    Sector.FloorScrollRate = TexelsPerSourceScrollUnit * FVector2D(int16(ReadWord(Offset + 26)), int16(ReadWord(Offset + 28)));
                    Sector.FlowImpulse = FVector2D(int16(ReadWord(Offset + 30)), int16(ReadWord(Offset + 32)));
                    Actor.ScrollingSectors.AddUnique(Index);
                    Actor.FlowSectors.AddUnique(Index);
                }
            }
            else if (Thing.Type >= WallUpSlow && Thing.Type <= WallDownFast)
            {
                const int32 State = Thing.Type - WallUpSlow;
                int32 BestSide = INDEX_NONE;
                double BestDistance = TNumericLimits<double>::Max();
                for (const FWadLineDef& Line : Map.Lines)
                {
                    const FWadVertex& From = Map.Vertices[Line.From];
                    const FWadVertex& To = Map.Vertices[Line.To];
                    const FVector2D A(From.X, From.Y), B(To.X, To.Y);
                    const FVector2D Edge = B - A;
                    if (Edge.IsNearlyZero())
                    {
                        continue;
                    }
                    const double Projection = FMath::Clamp(FVector2D::DotProduct(Position - A, Edge) / Edge.SizeSquared(), 0.0, 1.0);
                    const double Distance = (Position - (A + Edge * Projection)).SizeSquared();
                    const int32 Side = Line.Side[FVector2D::CrossProduct(Edge, Position - A) > 0 ? 1 : 0];
                    if (Distance < BestDistance && Actor.Sides.IsValidIndex(Side))
                    {
                        BestSide = Side;
                        BestDistance = Distance;
                    }
                }
                if (BestSide != INDEX_NONE && State < SideCount)
                {
                    const int32 Offset = SideStart + 42 * State;
                    Actor.Sides[BestSide].ScrollRate = TexelsPerSourceScrollUnit * FVector2D(
                        int16(ReadWord(Offset + 38)), int16(ReadWord(Offset + 40)));
                    Actor.ScrollingSides.AddUnique(BestSide);
                }
            }
        }
        bool bDepthChanged = false;
        for (int32 Index = 0; Index < Actor.Sectors.Num(); ++Index)
        {
            FAAWadNativeSector& Sector = Actor.Sectors[Index];
            if (!Sector.Floor || (!Sector.WalkingFloor && !Sector.FloorDepth))
            {
                continue;
            }
            if (Sector.FloorDepth)
            {
                if (!Sector.WalkingFloor)
                {
                    Sector.WalkingFloor = AddMesh(Actor, Sector.Floor->GetStaticMesh(),
                        FString::Printf(TEXT("WalkingFloor_%04d"), Index), true, false);
                    Sector.WalkingFloor->SetVisibility(false);
                    Sector.WalkingFloor->SetCastShadow(false);
                    Sector.WalkingFloor->RegisterComponent();
                }
                FVector Location = Sector.Floor->GetRelativeLocation();
                Location.Z -= Sector.FloorDepth;
                Sector.WalkingFloor->SetRelativeLocation(Location);
                Sector.Floor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }
            else
            {
                Sector.WalkingFloor->DestroyComponent();
                Sector.WalkingFloor = nullptr;
                Sector.Floor->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            }
            bDepthChanged = true;
        }
        if (bDepthChanged)
        {
            for (int32 Index = 0; Index < Actor.Lines.Num(); ++Index)
            {
                Actor.UpdateLine(Index, true);
            }
        }
        Output.Logf(TEXT("Installed %d water markers, %d current sectors and %d scrolling sides."),
            WaterSectors, Actor.FlowSectors.Num(), Actor.ScrollingSides.Num());
        return true;
    }

    static bool InstallAmbientEmitters(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        FOutputDevice& Output)
    {
        constexpr int32 StreamMarker = 1513;
        constexpr int32 WaterfallMarker = 1514;
        constexpr int32 StreamSound = 3003;
        constexpr int32 WaterfallSound = 3007;
        TSet<int32> Sounds;
        Actor.AmbientEmitters.Reset();
        for (const FWadThing& Thing : Map.Things)
        {
            if (Thing.Type == StreamMarker || Thing.Type == WaterfallMarker)
            {
                const int32 Sound = Thing.Type == StreamMarker ? StreamSound : WaterfallSound;
                const int32 Sector = Actor.FindWadSector(FVector2D(Thing.X, Thing.Y));
                FVector2D Heights;
                const double Z = Actor.GetSectorSurfaceHeights(Sector, Heights) ? Heights.X : 0.0;
                Actor.AmbientEmitters.Add({FVector(Thing.X, Thing.Y, Z), Sound, Thing.Flags});
                Sounds.Add(Sound);
            }
        }
        FString Archive = FPaths::GetPath(WadPath) / TEXT("SOUNDS.RES");
        if (!FPaths::FileExists(Archive))
        {
            Archive = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/SOUNDS.RES");
        }
        if (!ImportWadSoundSamples(Archive, MoveTemp(Sounds), Actor.AmbientSamples, Output))
        {
            return false;
        }
        if (!Actor.AmbientEmitters.IsEmpty())
        {
            Output.Logf(TEXT("Installed %d stream and waterfall emitters."), Actor.AmbientEmitters.Num());
        }
        return true;
    }

    bool InstallGameplay(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        TArray<FAAWadNativeLevelLink> Links;
        if (!BuildLevelLinks(Map, Links, Output) || !AddPlayerStart(World, Map, Output))
        {
            return false;
        }
        Actor.Modify();
        SetOutdoorSectors(Actor, Map);
        Actor.InitializeTopology();
        if (!InstallSurfaceEnvironment(Actor, Map, WadPath, Output) || !InstallDaySky(Actor, WadPath, Output)
            || !InstallAmbientEmitters(Actor, Map, WadPath, Output))
        {
            return false;
        }
        const FString MapName = FPaths::GetBaseFilename(WadPath).ToUpper();
        if ((MapName == TEXT("L30") && !InstallL30Slice(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L151") && !InstallL151Slice(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L301") && !InstallL301Encounter(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L531") && !InstallL531Script(Actor, Map, WadPath, Output))
            || (MapName == TEXT("L31") && !InstallL31Script(Actor, Map, WadPath, Output)))
        {
            return false;
        }
        Actor.LevelLinks = MoveTemp(Links);
        AWorldSettings* Settings = World.GetWorldSettings();
        Settings->Modify();
        Settings->DefaultGameMode = ANativeWadGameMode::StaticClass();
        return true;
    }

    static bool InstallL531Script(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 201};
        return InstallActivities(Actor, Map, WadPath, TEXT("S531.SRP"), EntryActivities, Output);
    }

    bool InstallL31Script(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 SwitchSide = 1159;
        constexpr int32 EntryActivities[] = {0, 1, 2, 3, 4, 5, 200, 201};
        if (!Actor.Sides.IsValidIndex(SwitchSide)
            || !InstallActivities(Actor, Map, WadPath, TEXT("S31.SRP"), EntryActivities, Output))
        {
            return false;
        }
        const UStaticMeshComponent* Switch = Actor.Sides[SwitchSide].Bands[uint8(EWadWallBand::Lower)];
        Parents[uint8(EWadWallBlend::Opaque)] = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/AAReview/M_AANativeOpaque.M_AANativeOpaque"));
        if (!Switch || !Switch->GetMaterial(0) || !Parents[uint8(EWadWallBlend::Opaque)] || !Texture(TEXT("SWIT1U")))
        {
            Output.Log(TEXT("L31's original switch sidedef or raised texture could not be installed."));
            return false;
        }
        Actor.ActivitySwitchDown = Switch->GetMaterial(0);
        Actor.ActivitySwitchUp = Material(TEXT("SWIT1U"), EWadWallBlend::Opaque);
        return Actor.ActivitySwitchUp != nullptr && !bAssetError;
    }

    static bool UpdateSavedDoors(UWorld& World, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        ANativeWadLevel* Actor = FindMatchingLevel(World, Map);
        if (!Actor)
        {
            Output.Log(TEXT("The open level needs exactly one native WAD actor matching the source map."));
            return false;
        }
        TArray<FAAWadNativeDoor> Doors;
        if (!BuildDoors(Map, Doors, Output))
        {
            return false;
        }
        for (const FAAWadNativeDoor& Door : Doors)
        {
            FVector2D Heights;
            if (!Actor->GetSectorSurfaceHeights(Door.Sector, Heights)
                || !Heights.Equals(FVector2D(Door.ClosedZ, Door.ClosedZ)))
            {
                Output.Logf(TEXT("Door sector %d needs its authored closed height before installing door controls."), Door.Sector);
                return false;
            }
        }
        Actor->Modify();
        Swap(Actor->Doors, Doors);
        if (!SaveAsset(World, *FPackageName::GetMapPackageExtension()))
        {
            Swap(Actor->Doors, Doors);
            return false;
        }
        Output.Logf(TEXT("Saved %d authored WAD doors in %s."), Actor->Doors.Num(), *World.GetOutermost()->GetName());
        return true;
    }

    template <typename T>
    T* FindOrCreateAsset(const FString& Folder, const FString& Name)
    {
        const FString Path = AssetRoot / Folder / Name;
        if (FPackageName::DoesPackageExist(Path))
        {
            return LoadObject<T>(nullptr, *(Path + TEXT(".") + Name));
        }
        UPackage* Package = CreatePackage(*Path);
        T* Asset = FindObject<T>(Package, *Name);
        if (!Asset)
        {
            Asset = NewObject<T>(Package, *Name, AssetFlags);
            FAssetRegistryModule::AssetCreated(Asset);
        }
        return Asset;
    }

    void QueueSave(UObject& Asset)
    {
        Asset.MarkPackageDirty();
        AssetsToSave.AddUnique(&Asset);
    }

    UMaterial* SpriteMaterial(const FString& Name, const FString& PicturePath)
    {
        FIntPoint Size;
        TArray<FColor> Pixels;
        if (!LoadWadSprite(PicturePath, Size, Pixels))
        {
            return nullptr;
        }
        UTexture2D* Picture = FindOrCreateAsset<UTexture2D>(TEXT("Things"), TEXT("T_") + Name);
        UMaterial* Result = FindOrCreateAsset<UMaterial>(TEXT("Things"), TEXT("M_") + Name);
        if (!Picture || !Result)
        {
            return nullptr;
        }
        Picture->Source.Init(Size.X, Size.Y, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
        Picture->CompressionSettings = TC_EditorIcon;
        Picture->MipGenSettings = TMGS_NoMipmaps;
        Picture->AddressX = TA_Clamp;
        Picture->AddressY = TA_Clamp;
        Picture->Filter = TF_Nearest;
        Picture->NeverStream = true;
        Picture->SRGB = true;
        Picture->PostEditChange();
        QueueSave(*Picture);

        Result->SetShadingModel(MSM_Unlit);
        Result->BlendMode = BLEND_Masked;
        Result->TwoSided = true;
        auto& Expressions = Result->GetExpressionCollection();
        Expressions.Empty();
        auto* Sample = NewObject<UMaterialExpressionTextureSample>(Result);
        Sample->Texture = Picture;
        auto* Emissive = NewObject<UMaterialExpressionMultiply>(Result);
        Emissive->A.Connect(0, Sample);
        Emissive->ConstB = 0.35f;
        Expressions.AddExpression(Sample);
        Expressions.AddExpression(Emissive);
        Result->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
        Result->GetEditorOnlyData()->OpacityMask.Connect(4, Sample);
        Result->PostEditChange();
        QueueSave(*Result);
        return Result;
    }

    template<class T>
    static T* ImportSliceThing(UWorld& World, const FDoomWadMapData& Map, int32 ThingId, double CenterZ, const TCHAR* Name)
    {
        const FName Tag(Name);
        for (TActorIterator<T> Actor(&World); Actor; ++Actor)
        {
            if (Actor->ActorHasTag(Tag))
            {
                Actor->Modify();
                return *Actor;
            }
        }
        const FWadThing& Thing = Map.Things[ThingId];
        const FVector Position(Thing.X, Thing.Y, CenterZ);
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        T* Actor = World.SpawnActor<T>(Position, FRotator(0, Thing.Angle * 45.0, 0), Params);
        if (Actor)
        {
            Actor->Tags.Add(Tag);
            Actor->SetActorLabel(Name);
        }
        return Actor;
    }

    static bool InstallActivities(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const TCHAR* ScriptName, TConstArrayView<int32> EntryActivities, FOutputDevice& Output)
    {
        FString ScriptPath = FPaths::GetPath(WadPath) / ScriptName;
        if (!FPaths::FileExists(ScriptPath))
        {
            ScriptPath = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe") / ScriptName;
        }
        TArray<FWadScriptActivity> Activities;
        if (!ImportWadScriptActivities(ScriptPath, EntryActivities, Map.Sectors.Num(), Map.Sides.Num(), Activities, Output))
        {
            Output.Logf(TEXT("Failed to import WAD activities from %s."), *ScriptPath);
            return false;
        }
        if (!ImportWadSounds(FPaths::GetPath(ScriptPath) / TEXT("SOUNDS.RES"), Activities, Actor.ActivitySounds, Output))
        {
            Output.Log(TEXT("Failed to import the sound samples used by the WAD script."));
            return false;
        }
        Actor.ImportedActivities = MoveTemp(Activities);
        Actor.ActivitySectors.Reset();
        for (int32 Index = 0; Index < Map.Sectors.Num(); ++Index)
        {
            const int32 Activity = Map.Sectors[Index].Type;
            Actor.Sectors[Index].Activity = Activity;
            if (Activity && EntryActivities.Contains(Activity))
            {
                Actor.ActivitySectors.Add(Index);
            }
        }
        for (int32 Index = 0; Index < Map.Lines.Num(); ++Index)
        {
            const int32 Activity = Map.Lines[Index].Special;
            Actor.Lines[Index].Activity = EntryActivities.Contains(Activity) ? Activity : 0;
        }
        Output.Logf(TEXT("Installed %d activities from %s."), Actor.ImportedActivities.Num(), ScriptName);
        return true;
    }

    bool InstallL30Slice(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        FOutputDevice& Output)
    {
        if (!Map.Lines.IsValidIndex(1182) || Map.Lines[1182].Special != 40
            || !Map.Sectors.IsValidIndex(177) || Map.Sectors[129].CeilingHeight != 0
            || Map.Sectors[163].Type != 15 || Map.Sectors[167].Type != 19
            || !Map.Sides.IsValidIndex(1636) || !Map.Things.IsValidIndex(300) || Map.Things[9].Type != 1018
            || Map.Things[210].Type != NativeWadRules::Berry || Map.Things[300].Type != NativeWadRules::IronDagger)
        {
            Output.Log(TEXT("The source does not match the L30 script and encounter records."));
            return false;
        }
        constexpr int32 EntryActivities[] = {0, 15, 16, 17, 18, 19, 21, 22, 31, 40, 201};
        if (!InstallActivities(Actor, Map, WadPath, TEXT("S30.SRP"), EntryActivities, Output))
        {
            return false;
        }
        Parents[uint8(EWadWallBlend::Opaque)] = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/AAReview/M_AANativeOpaque.M_AANativeOpaque"));
        const UStaticMeshComponent* OriginalSwitch = Actor.Sides[1636].Bands[uint8(EWadWallBand::Lower)];
        if (!Parents[uint8(EWadWallBlend::Opaque)] || !Texture(TEXT("SWIT1U")) || !OriginalSwitch || !OriginalSwitch->GetMaterial(0))
        {
            return false;
        }
        Actor.ActivitySwitchUp = Material(TEXT("SWIT1U"), EWadWallBlend::Opaque);
        Actor.ActivitySwitchDown = OriginalSwitch->GetMaterial(0);

        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        UMaterial* GuardianArt = SpriteMaterial(TEXT("ElfGuardian"), TEXT("ARCHER/ARCHER01.CPC"));
        UMaterial* DownedArt = SpriteMaterial(TEXT("ElfGuardianDowned"), TEXT("ARCHER/ARCDI04.CPC"));
        UMaterial* AttackArt = SpriteMaterial(TEXT("ElfGuardianAttack"), TEXT("ARCHER/ARCDI01.CPC"));
        UMaterial* HurtArt = SpriteMaterial(TEXT("ElfGuardianHurt"), TEXT("ARCHER/ARCHER05.CPC"));
        UMaterial* BoltArt = SpriteMaterial(TEXT("GuardianBolt"), TEXT("ARROW1/BOLT2.CPC"));
        UMaterial* BerryArt = SpriteMaterial(TEXT("Berry"), TEXT("BERI/BERI.CPC"));
        UMaterial* DaggerArt = SpriteMaterial(TEXT("IronDagger"), TEXT("DAGGER/DAGGER.CPC"));
        Actor.ScriptMissileArt = SpriteMaterial(TEXT("PoisonMissile"), TEXT("MISSIL16/1.CPC"));
        Actor.ScriptMissileMesh = Plane;
        if (!Plane || !GuardianArt || !DownedArt || !AttackArt || !HurtArt || !BoltArt || !BerryArt || !DaggerArt
            || !Actor.ScriptMissileArt)
        {
            Output.Log(TEXT("Failed to save an original L30 encounter sprite."));
            return false;
        }
        const double Floor = Map.Sectors[108].FloorHeight;
        ANativeWadGuardian* Guardian = ImportSliceThing<ANativeWadGuardian>(World, Map, 9, Floor + 28.0, TEXT("L30 Elf Guardian"));
        ANativeWadPickup* Berry = ImportSliceThing<ANativeWadPickup>(World, Map, 210, Floor + 10.0, TEXT("L30 Berry Pickup"));
        ANativeWadPickup* Dagger = ImportSliceThing<ANativeWadPickup>(World, Map, 300, Floor + 10.0, TEXT("L30 Iron Dagger"));
        if (!Guardian || !Berry || !Dagger)
        {
            return false;
        }
        Guardian->Configure(Plane, GuardianArt, DownedArt, AttackArt, HurtArt, BoltArt, FWadEncounterProfile());
        Berry->Configure(Plane, BerryArt, Map.Things[210].Type);
        Dagger->Configure(Plane, DaggerArt, Map.Things[300].Type);
        Output.Log(TEXT("Installed the L30 activities and original Guardian, berry, and iron dagger placements."));
        return !bAssetError;
    }

    bool InstallL301Encounter(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        FOutputDevice& Output)
    {
        constexpr int32 GuardianThing = 246;
        constexpr int32 GuardianSector = 44;
        constexpr int32 EntryActivities[] = {0};
        if (!Map.Things.IsValidIndex(GuardianThing) || Map.Things[GuardianThing].Type != 1018
            || !Map.Sectors.IsValidIndex(GuardianSector)
            || !InstallActivities(Actor, Map, WadPath, TEXT("S301.SRP"), EntryActivities, Output))
        {
            return false;
        }
        const auto LoadGuardianMaterial = [](const TCHAR* Name)
        {
            const FString AssetPath = FString::Printf(TEXT("/Game/Maps/L30_Assets/Things/%s.%s"), Name, Name);
            return LoadObject<UMaterialInterface>(nullptr, *AssetPath);
        };
        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        auto* Stand = LoadGuardianMaterial(TEXT("M_ElfGuardian"));
        auto* Down = LoadGuardianMaterial(TEXT("M_ElfGuardianDowned"));
        auto* Attack = LoadGuardianMaterial(TEXT("M_ElfGuardianAttack"));
        auto* Hurt = LoadGuardianMaterial(TEXT("M_ElfGuardianHurt"));
        auto* Bolt = LoadGuardianMaterial(TEXT("M_GuardianBolt"));
        if (!Plane || !Stand || !Down || !Attack || !Hurt || !Bolt)
        {
            return false;
        }
        ANativeWadGuardian* Guardian = ImportSliceThing<ANativeWadGuardian>(World, Map, GuardianThing,
            Map.Sectors[GuardianSector].FloorHeight + 28.0, TEXT("L301 Elf Guardian 246"));
        if (!Guardian)
        {
            return false;
        }
        Guardian->Configure(Plane, Stand, Down, Attack, Hurt, Bolt, FWadEncounterProfile());
        Output.Log(TEXT("Installed L301's initial activity and authored Guardian thing 246 in sector 44."));
        return true;
    }

    bool InstallL151Slice(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        FOutputDevice& Output)
    {
        constexpr int32 MageThing = 31;
        constexpr int32 PotionThing = 224;
        constexpr int32 EntryActivities[] = {0, 1};
        if (!Map.Sectors.IsValidIndex(31) || Map.Sectors[31].Type != 1 || !Map.Things.IsValidIndex(PotionThing)
            || Map.Things[MageThing].Type != 3002 || Map.Things[PotionThing].Type != NativeWadRules::FeatherfallPotion
            || !InstallActivities(Actor, Map, WadPath, TEXT("S151.SRP"), EntryActivities, Output))
        {
            Output.Log(TEXT("Unable to install the L151 lift and encounter."));
            return false;
        }
        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        UMaterial* Stand = SpriteMaterial(TEXT("GaisericMagician"), TEXT("PHIL/PHIL01.CPC"));
        UMaterial* Down = SpriteMaterial(TEXT("GaisericMagicianDowned"), TEXT("PHIL/BDDED04.CPC"));
        UMaterial* Attack = SpriteMaterial(TEXT("GaisericMagicianAttack"), TEXT("PHIL/BDDED02.CPC"));
        UMaterial* Hurt = SpriteMaterial(TEXT("GaisericMagicianHurt"), TEXT("PHIL/BRDATK01.CPC"));
        UMaterial* Missile = SpriteMaterial(TEXT("MagicDart"), TEXT("MISSIL12/1.CPC"));
        UMaterial* Potion = SpriteMaterial(TEXT("FeatherfallPotion"), TEXT("POTION17/POTION17.CPC"));
        if (!Plane || !Stand || !Down || !Attack || !Hurt || !Missile || !Potion)
        {
            Output.Log(TEXT("Unable to load the original L151 encounter art."));
            return false;
        }
        ANativeWadGuardian* Mage = ImportSliceThing<ANativeWadGuardian>(World, Map, MageThing,
            Map.Sectors[19].FloorHeight + 28.0, TEXT("L151 Gaiseric Magician"));
        ANativeWadPickup* Item = ImportSliceThing<ANativeWadPickup>(World, Map, PotionThing,
            Map.Sectors[4].FloorHeight + 10.0, TEXT("L151 Potion of Featherfalling"));
        if (!Mage || !Item)
        {
            return false;
        }
        FWadEncounterProfile Profile;
        Profile.Name = TEXT("Gaiseric Magician");
        Profile.MaxHealth = 2500;
        Profile.RetreatHealth = 1000;
        Profile.MeleeDamage = 250;
        Profile.BoltDamage = 100;
        Profile.BoltRange = 10000;
        Profile.MeleeDelayUpdates = 10;
        Profile.RangedDelayUpdates = 25;
        Mage->Configure(Plane, Stand, Down, Attack, Hurt, Missile, Profile);
        Item->Configure(Plane, Potion, NativeWadRules::FeatherfallPotion);
        Output.Log(TEXT("Installed the L151 lift, Gaiseric Magician thing 31, and potion thing 224."));
        return !bAssetError;
    }

    bool InstallDaySky(ANativeWadLevel& Actor, const FString& WadPath, FOutputDevice& Output)
    {
        if (!Actor.Sectors.ContainsByPredicate([](const FAAWadNativeSector& Sector) { return Sector.bOutside; }))
        {
            return true;
        }
        FString Info;
        TArray<FString> Tokens;
        if (!FFileHelper::LoadFileToString(Info, *FPaths::ChangeExtension(WadPath, TEXT("I"))))
        {
            return true;
        }
        Info.ParseIntoArrayWS(Tokens);
        if (Tokens.Num() < 2)
        {
            return true;
        }
        const FString& PictureName = Tokens[1];
        FIntPoint Size;
        TArray<FColor> Pixels;
        if (!LoadWadBackdrop(PictureName, Size, Pixels))
        {
            Output.Logf(TEXT("Unable to decode authored WAD backdrop %s."), *PictureName);
            return false;
        }
        const FString Name = FPaths::GetBaseFilename(PictureName).ToUpper();
        UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
        UTexture2D* Picture = FindOrCreateAsset<UTexture2D>(TEXT("Sky"), TEXT("T_") + Name);
        UMaterial* Material = FindOrCreateAsset<UMaterial>(TEXT("Sky"), TEXT("M_") + Name);
        if (!Sphere || !Picture || !Material)
        {
            return false;
        }
        Picture->Source.Init(Size.X, Size.Y, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
        Picture->CompressionSettings = TC_EditorIcon;
        Picture->MipGenSettings = TMGS_NoMipmaps;
        Picture->AddressX = TA_Clamp;
        Picture->AddressY = TA_Clamp;
        Picture->NeverStream = true;
        Picture->SRGB = true;
        Picture->PostEditChange();
        QueueSave(*Picture);

        Material->SetShadingModel(MSM_Unlit);
        Material->TwoSided = true;
        Material->bIsSky = true;
        auto& Expressions = Material->GetExpressionCollection();
        Expressions.Empty();
        auto* Direction = NewObject<UMaterialExpressionCameraVectorWS>(Material);
        auto* UV = NewObject<UMaterialExpressionCustom>(Material);
        UV->Description = TEXT("WAD sky backdrop projection");
        UV->OutputType = CMOT_Float2;
        UV->Inputs.SetNum(1);
        UV->Inputs[0].InputName = TEXT("ViewToCamera");
        UV->Inputs[0].Input.Connect(0, Direction);
        // MapSetBackdrop mirrors the picture horizontally and repeats the pair twice per turn.
        UV->Code = TEXT("float3 View = -normalize(ViewToCamera);\n"
            "float Turns = atan2(View.y, View.x) / 6.28318530718;\n"
            "float U = 1.0 - abs(frac(-2.0 * Turns) * 2.0 - 1.0);\n"
            "float V = saturate(0.6 - asin(clamp(View.z, -1.0, 1.0)) / 1.57079632679);\n"
            "return float2(U, V);");
        auto* Sample = NewObject<UMaterialExpressionTextureSample>(Material);
        Sample->Texture = Picture;
        Sample->Coordinates.Connect(0, UV);
        auto* Brightness = NewObject<UMaterialExpressionScalarParameter>(Material);
        Brightness->ParameterName = TEXT("SkyBrightness");
        Brightness->DefaultValue = 1.0f;
        auto* Emissive = NewObject<UMaterialExpressionMultiply>(Material);
        Emissive->A.Connect(0, Sample);
        Emissive->B.Connect(0, Brightness);
        Expressions.AddExpression(Direction);
        Expressions.AddExpression(UV);
        Expressions.AddExpression(Sample);
        Expressions.AddExpression(Brightness);
        Expressions.AddExpression(Emissive);
        Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
        Material->PostEditChange();
        QueueSave(*Material);

        if (!Actor.DaySky)
        {
            Actor.DaySky = AddMesh(Actor, Sphere, TEXT("AuthoredBackdrop"), false, false, Material);
            Actor.DaySky->SetRelativeScale3D(FVector(1600.0));
            Actor.DaySky->SetCastShadow(false);
            Actor.DaySky->SetVisibleInRayTracing(false);
            Actor.DaySky->RegisterComponent();
        }
        else
        {
            Actor.DaySky->SetMaterial(0, Material);
        }
        Output.Logf(TEXT("Imported WAD backdrop %s (%d x %d)."), *PictureName, Size.X, Size.Y);
        return true;
    }

    UTexture2D* Texture(const FString& Name)
    {
        if (!HasTexture(Name))
        {
            return nullptr;
        }
        if (UTexture2D** Cached = Textures.Find(Name))
        {
            return *Cached;
        }
        UTexture2D* Source = LoadWadSurfaceTexture(Name);
        if (!Source)
        {
            UE_LOG(LogTemp, Warning, TEXT("Native WAD import uses the default material for missing texture %s."), *Name);
            Textures.Add(Name, nullptr);
            return nullptr;
        }
        UTexture2D* Asset = FindOrCreateAsset<UTexture2D>(TEXT("Textures"), TEXT("T_") + Name);
        if (!Asset)
        {
            bAssetError = true;
            return nullptr;
        }
        FTexture2DMipMap& Mip = Source->GetPlatformData()->Mips[0];
        const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        Asset->Source.Init(Source->GetSizeX(), Source->GetSizeY(), 1, 1, TSF_BGRA8, Data);
        Mip.BulkData.Unlock();
        Asset->CompressionSettings = TC_EditorIcon;
        Asset->MipGenSettings = TMGS_NoMipmaps;
        Asset->NeverStream = true;
        Asset->Filter = TF_Nearest;
        Asset->SRGB = true;
        Asset->PostEditChange();
        QueueSave(*Asset);
        Textures.Add(Name, Asset);
        return Asset;
    }

    UMaterialInterface* Material(const FString& TextureName, EWadWallBlend Blend)
    {
        const uint8 Index = uint8(Blend);
        UTexture2D* Image = Texture(TextureName);
        if (!Image)
        {
            return Parents[Index];
        }
        const FString Name = FString::Printf(TEXT("MI_%d_%s"), Index, *TextureName);
        if (UMaterialInterface** Cached = Materials.Find(Name))
        {
            return *Cached;
        }
        UMaterialInstanceConstant* Asset = FindOrCreateAsset<UMaterialInstanceConstant>(TEXT("Materials"), Name);
        if (!Asset)
        {
            bAssetError = true;
            return Parents[Index];
        }
        Asset->SetParentEditorOnly(Parents[Index]);
        Asset->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("SurfaceTexture")), Image);
        Asset->PostEditChange();
        QueueSave(*Asset);
        Materials.Add(Name, Asset);
        return Asset;
    }

    UStaticMesh* BuildMesh(const FString& Name, FWadMeshDescription& Geometry, UMaterialInterface* Material,
        bool bDistanceField, TArray<FKConvexElem> Hulls = {})
    {
        UStaticMesh* Mesh = FindOrCreateAsset<UStaticMesh>(TEXT("Meshes"), Name);
        if (!Mesh || Geometry.Description.Triangles().Num() == 0)
        {
            return nullptr;
        }
        Mesh->GetStaticMaterials().Reset();
        Mesh->GetStaticMaterials().Emplace(Material, SurfaceSlot, SurfaceSlot);
        Mesh->SetNumSourceModels(1);
        FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
        Settings.bRecomputeNormals = false;
        Settings.bRecomputeTangents = false;
        Settings.bGenerateLightmapUVs = false;
        Settings.bUseFullPrecisionUVs = true;
        Settings.DistanceFieldResolutionScale = bDistanceField ? 1.5f : 0.0f;
        Mesh->CreateMeshDescription(0, MoveTemp(Geometry.Description));
        Mesh->CommitMeshDescription(0);
        Mesh->CreateBodySetup();
        UBodySetup* Body = Mesh->GetBodySetup();
        Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
        Body->AggGeom.ConvexElems = MoveTemp(Hulls);
        Body->InvalidatePhysicsData();
        Mesh->Build(false);
        QueueSave(*Mesh);
        return Mesh;
    }

    bool LoadWallMeshes()
    {
        const TCHAR* Names[] = {TEXT("Opaque"), TEXT("Masked"), TEXT("Translucent")};
        for (uint8 Blend = 0; Blend < UE_ARRAY_COUNT(Names); ++Blend)
        {
            const FString Name = FString(TEXT("M_AANative")) + Names[Blend];
            const FString MaterialPath = FString(TEXT("/Game/AAReview/")) + Name + TEXT(".") + Name;
            Parents[Blend] = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
            if (!Parents[Blend])
            {
                return false;
            }
            FWadMeshDescription Geometry;
            const FVertexID A = Geometry.AddVertex(FVector3f(0,0,0));
            const FVertexID B = Geometry.AddVertex(FVector3f(0,0,WallMeshSize));
            const FVertexID C = Geometry.AddVertex(FVector3f(WallMeshSize,0,WallMeshSize));
            const FVertexID D = Geometry.AddVertex(FVector3f(WallMeshSize,0,0));
            Geometry.AddQuad(A, B, C, D);
            const bool bOpaque = Blend == uint8(EWadWallBlend::Opaque);
            if (bOpaque)
            {
                // Keep the textured face on the original linedef and give the solid side thickness for Lumen.
                const FVertexID BackA = Geometry.AddVertex(FVector3f(0,SlabThickness,0));
                const FVertexID BackB = Geometry.AddVertex(FVector3f(0,SlabThickness,WallMeshSize));
                const FVertexID BackC = Geometry.AddVertex(FVector3f(WallMeshSize,SlabThickness,WallMeshSize));
                const FVertexID BackD = Geometry.AddVertex(FVector3f(WallMeshSize,SlabThickness,0));
                Geometry.AddQuad(BackD, BackC, BackB, BackA);
                Geometry.AddQuad(B, A, BackA, BackB);
                Geometry.AddQuad(C, B, BackB, BackC);
                Geometry.AddQuad(D, C, BackC, BackD);
                Geometry.AddQuad(A, D, BackD, BackA);
            }
            WallMeshes[Blend] = BuildMesh(FString(TEXT("SM_Wall_")) + Names[Blend], Geometry,
                Parents[Blend], bOpaque);
            if (!WallMeshes[Blend])
            {
                return false;
            }
        }
        return true;
    }

    UStaticMeshComponent* AddMesh(ANativeWadLevel& Actor, UStaticMesh* Mesh, const FString& Name, bool bCollision,
        bool bAffectsLumen, UMaterialInterface* Override = nullptr)
    {
        UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(&Actor, *Name, RF_Transactional);
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetupAttachment(Actor.GetRootComponent());
        Component->SetCanEverAffectNavigation(false);
        Component->SetGenerateOverlapEvents(false);
        Component->SetCollisionProfileName(bCollision ? TEXT("BlockAll") : TEXT("NoCollision"));
        Component->SetAffectDistanceFieldLighting(bAffectsLumen);
        Component->SetEmissiveLightSource(bAffectsLumen);
        Component->SetStaticMesh(Mesh);
        if (Override)
        {
            Component->SetMaterial(0, Override);
        }
        Actor.AddInstanceComponent(Component);
        return Component;
    }

    UStaticMeshComponent* AddPlane(ANativeWadLevel& Actor, int32 SectorIndex, const FDoomWadSectorFootprint& Footprint,
        const FWadSector& Sector, bool bCeiling, const FString& TextureName, const FVector2D& Origin)
    {
        UMaterialInterface* SurfaceMaterial = Material(TextureName, EWadWallBlend::Opaque);
        const UTexture2D* Image = Textures.FindRef(TextureName);
        const FVector2D Size = Image ? FVector2D(Image->Source.GetSizeX(), Image->Source.GetSizeY()) : FVector2D(64.0, 64.0);
        const bool bSky = TextureName.StartsWith(TEXT("F_SKY"));
        const float Thickness = bCeiling ? SlabThickness : -SlabThickness;
        FWadMeshDescription Geometry;
        TArray<FVertexID> Front;
        TArray<FVertexID> Back;
        TArray<FVector2f> UVs;
        TArray<FKConvexElem> Hulls;
        Front.Reserve(Footprint.Vertices.Num());
        Back.Reserve(Footprint.Vertices.Num());
        UVs.Reserve(Footprint.Vertices.Num());
        Hulls.Reserve(Footprint.Triangles.Num() / 3);
        for (const FVector2D& Point : Footprint.Vertices)
        {
            const FVector2D Local = Point - Origin;
            Front.Add(Geometry.AddVertex(FVector3f(Local.X, Local.Y, 0.0f)));
            Back.Add(Geometry.AddVertex(FVector3f(Local.X, Local.Y, Thickness)));
            UVs.Emplace(Point.X / Size.X, Point.Y / Size.Y);
        }
        TMap<FIntPoint, FIntPoint> Boundary;
        for (int32 Index = 0; Index < Footprint.Triangles.Num(); Index += 3)
        {
            const int32 A = Footprint.Triangles[Index];
            const int32 B = Footprint.Triangles[Index + (bCeiling ? 1 : 2)];
            const int32 C = Footprint.Triangles[Index + (bCeiling ? 2 : 1)];
            Geometry.AddTriangle({Front[A], Front[B], Front[C]}, {UVs[A], UVs[B], UVs[C]});
            Geometry.AddTriangle({Back[A], Back[C], Back[B]}, {UVs[A], UVs[C], UVs[B]});
            FKConvexElem& Hull = Hulls.AddDefaulted_GetRef();
            for (const int32 Vertex : {A, B, C})
            {
                const FVector2D Point = Footprint.Vertices[Vertex] - Origin;
                Hull.VertexData.Emplace(Point, 0.0);
                Hull.VertexData.Emplace(Point, Thickness);
            }
            Hull.UpdateElemBox();
            for (const FIntPoint Edge : {FIntPoint(A,B), FIntPoint(B,C), FIntPoint(C,A)})
            {
                const FIntPoint Key(FMath::Min(Edge.X, Edge.Y), FMath::Max(Edge.X, Edge.Y));
                if (!Boundary.Remove(Key))
                {
                    Boundary.Add(Key, Edge);
                }
            }
        }
        for (const TPair<FIntPoint, FIntPoint>& Pair : Boundary)
        {
            const FIntPoint Edge = Pair.Value;
            Geometry.AddQuad(Front[Edge.Y], Front[Edge.X], Back[Edge.X], Back[Edge.Y]);
        }
        const FString Name = FString::Printf(TEXT("Sector_%03d_%s"), SectorIndex, bCeiling ? TEXT("Ceiling") : TEXT("Floor"));
        UStaticMesh* Mesh = BuildMesh(TEXT("SM_") + Name, Geometry, SurfaceMaterial, !bSky, MoveTemp(Hulls));
        if (!Mesh)
        {
            return nullptr;
        }
        UStaticMeshComponent* Component = AddMesh(Actor, Mesh, Name, true, !bSky);
        Component->SetRelativeLocation(FVector(Origin, bCeiling ? Sector.CeilingHeight : Sector.FloorHeight));
        Component->SetVisibility(!bSky);
        Component->SetDefaultCustomPrimitiveDataVector4(0, FVector4(1.0,1.0,0.0,0.0));
        Actor.ApplyLight(*Component, FMath::Min<int32>(Sector.Light, 255), true);
        return Component;
    }

    bool Populate(ANativeWadLevel& Actor, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        if (!BuildDoors(Map, Actor.Doors, Output))
        {
            return false;
        }
        TSet<int32> ReferencedSectors;
        for (const FWadLineDef& Line : Map.Lines)
        {
            for (int32 Side : Line.Side)
            {
                ReferencedSectors.Add(Map.GetSectorForSide(Side));
            }
        }
        Actor.Sectors.SetNum(Map.Sectors.Num());
        for (int32 Index = 0; Index < Map.Sectors.Num(); ++Index)
        {
            FAAWadNativeSector& Sector = Actor.Sectors[Index];
            FWadSector SourceSector = Map.Sectors[Index];
            Sector.Light = SourceSector.Light;
            if (!ReferencedSectors.Contains(Index))
            {
                // Unused records still occupy their original slot so script and sector indices remain stable.
                Output.Logf(TEXT("Retained unused WAD sector %d without mesh geometry."), Index);
                continue;
            }
            FDoomWadSectorFootprint Footprint;
            if (!UDoomWadLoader::GetLastWadSectorFootprint(Index, Footprint))
            {
                UE_LOG(LogTemp, Error, TEXT("Native WAD import could not triangulate sector %d."), Index);
                return false;
            }
            FString Floor;
            FString Ceiling;
            UDoomWadLoader::GetLastWadSectorTextureNames(Index, Floor, Ceiling);
            FBox2D Bounds(ForceInit);
            for (const FVector2D& Point : Footprint.Vertices)
            {
                Bounds += Point;
            }
            if (Actor.Doors.ContainsByPredicate([Index](const FAAWadNativeDoor& Door) { return Door.Sector == Index; }))
            {
                SourceSector.CeilingHeight = SourceSector.FloorHeight;
            }
            Sector.Floor = AddPlane(Actor, Index, Footprint, SourceSector, false, Floor, Bounds.GetCenter());
            Sector.Ceiling = AddPlane(Actor, Index, Footprint, SourceSector, true, Ceiling, Bounds.GetCenter());
            if (!Sector.Floor || !Sector.Ceiling)
            {
                return false;
            }
        }
        for (const FWadVertex& Vertex : Map.Vertices)
        {
            Actor.Vertices.Emplace(Vertex.X, Vertex.Y);
        }
        Actor.Sides.SetNum(Map.Sides.Num());
        for (int32 Index = 0; Index < Map.Sides.Num(); ++Index)
        {
            FAAWadNativeSide& Side = Actor.Sides[Index];
            Side.Sector = Map.GetSectorForSide(Index);
            FString Upper;
            FString Lower;
            FString Middle;
            FVector2D Offset;
            UDoomWadLoader::GetWadSideTextureNames(Map.Sides[Index], Upper, Lower, Middle, Offset);
            Side.Textures = {Lower, Middle, Upper};
            Side.TextureOffset = FIntPoint(Offset.X, Offset.Y);
            Side.Bands.SetNum(3);
        }
        for (const FWadLineDef& Line : Map.Lines)
        {
            FAAWadNativeLine& Entry = Actor.Lines.AddDefaulted_GetRef();
            Entry.Endpoints = FIntPoint(Line.From, Line.To);
            Entry.Sides = FIntPoint(Line.Side[0], Line.Side[1]);
            Entry.Flags = Line.Flags;
            const bool bTwoSided = (Line.Flags & TwoSidedLine) != 0
                && Map.GetSectorForSide(Line.Side[0]) != INDEX_NONE && Map.GetSectorForSide(Line.Side[1]) != INDEX_NONE;
            for (const int32 SideId : Line.Side)
            {
                if (!Actor.Sides.IsValidIndex(SideId) || Actor.Sides[SideId].Sector == INDEX_NONE)
                {
                    continue;
                }
                FAAWadNativeSide& Side = Actor.Sides[SideId];
                for (int32 Band = 0; Band < Side.Bands.Num(); ++Band)
                {
                    const bool bMiddle = Band == uint8(EWadWallBand::Middle);
                    if (Side.Bands[Band] || (!bMiddle && !bTwoSided) || !HasTexture(Side.Textures[Band]))
                    {
                        continue;
                    }
                    const EWadWallBlend Blend = bMiddle && bTwoSided
                        ? ((Line.Flags & 0x0020) != 0 ? EWadWallBlend::Translucent : EWadWallBlend::Masked)
                        : EWadWallBlend::Opaque;
                    const FString Name = FString::Printf(TEXT("Side_%04d_%d"), SideId, Band);
                    UStaticMeshComponent* Mesh = AddMesh(Actor, WallMeshes[uint8(Blend)], Name, false,
                        Blend == EWadWallBlend::Opaque, Material(Side.Textures[Band], Blend));
                    Mesh->SetVisibility(false);
                    Actor.ApplyLight(*Mesh, FMath::Min<int32>(Map.Sectors[Side.Sector].Light, 255), true);
                    Side.Bands[Band] = Mesh;
                }
            }
        }
        FAssetCompilingManager::Get().FinishAllCompilation();
        Actor.RegisterAllComponents();
        for (int32 Index = 0; Index < Actor.Lines.Num(); ++Index)
        {
            Actor.UpdateLine(Index, true);
        }
        return !bAssetError;
    }
};

bool SaveNativeWadDoors(UWorld* World, FOutputDevice& Output)
{
    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!World || World->WorldType != EWorldType::Editor || !Map || !World->GetOutermost()->GetName().StartsWith(TEXT("/Game/")))
    {
        Output.Log(TEXT("Door updates require a loaded source map and an open native editor level."));
        return false;
    }
    return FAAWadNativeImport::UpdateSavedDoors(*World, *Map, Output);
}

bool SaveNativeWadGameplay(UWorld* World, const FString& WadPath, FOutputDevice& Output)
{
    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!World || World->WorldType != EWorldType::Editor || !Map || !World->GetOutermost()->GetName().StartsWith(TEXT("/Game/")))
    {
        return false;
    }
    ANativeWadLevel* Actor = FAAWadNativeImport::FindMatchingLevel(*World, *Map);
    FAAWadNativeImport Import;
    Import.AssetRoot = World->GetOutermost()->GetName() + TEXT("_Assets");
    if (!Actor || !Import.InstallGameplay(*World, *Actor, *Map, WadPath, Output))
    {
        return false;
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    for (UObject* Asset : Import.AssetsToSave)
    {
        if (!SaveAsset(*Asset, *FPackageName::GetAssetPackageExtension()))
        {
            return false;
        }
    }
    if (!SaveAsset(*World, *FPackageName::GetMapPackageExtension()))
    {
        return false;
    }
    Output.Logf(TEXT("Saved WAD gameplay settings in %s."), *World->GetOutermost()->GetName());
    return true;
}

bool SaveNativeWadLevel(const FString& MapPackage, const FString& WadPath, FOutputDevice& Output)
{
    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!Map || !MapPackage.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(MapPackage)
        || FPackageName::DoesPackageExist(MapPackage) || FindPackage(nullptr, *MapPackage))
    {
        Output.Log(TEXT("Native WAD import needs a loaded map and a new /Game/ package path."));
        return false;
    }
    FAAWadNativeImport Import;
    Import.AssetRoot = MapPackage + TEXT("_Assets");
    if (!Import.LoadWallMeshes())
    {
        Output.Log(TEXT("Missing native wall mesh materials. Run scripts/create_wall_materials.py in Unreal first."));
        return false;
    }
    const FName Name(*FPackageName::GetShortName(MapPackage));
    UPackage* Package = CreatePackage(*MapPackage);
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, Name, Package);
    if (!World)
    {
        return false;
    }
    World->SetFlags(RF_Public | RF_Standalone);
    ANativeWadLevel* Actor = World->SpawnActor<ANativeWadLevel>();
    Actor->SetActorLabel(TEXT("Amulets & Armor Level"));
    bool bSaved = Import.Populate(*Actor, *Map, Output) && Import.InstallGameplay(*World, *Actor, *Map, WadPath, Output);
    if (bSaved)
    {
        APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>();
        ConfigureWadLumen(Volume->Settings);
        Volume->bUnbound = true;
        Volume->SetActorLabel(TEXT("Sector Lighting and Lumen"));
        FAssetCompilingManager::Get().FinishAllCompilation();
        for (UObject* Asset : Import.AssetsToSave)
        {
            bSaved &= SaveAsset(*Asset, *FPackageName::GetAssetPackageExtension());
        }
        bSaved = bSaved && SaveAsset(*World, *FPackageName::GetMapPackageExtension());
    }
    if (bSaved)
    {
        FAssetRegistryModule::AssetCreated(World);
        Output.Logf(TEXT("Saved native WAD level %s with %d sectors and %d lines (%d assets)."),
            *MapPackage, Map->Sectors.Num(), Map->Lines.Num(), Import.AssetsToSave.Num());
    }
    World->DestroyWorld(false);
    return bSaved;
}

#endif
