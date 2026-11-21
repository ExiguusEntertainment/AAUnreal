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
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "NativeWadEncounter.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadHudData.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadScenery.h"
#include "NativeWadTown.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "TextureResource.h"
#include "UObject/SavePackage.h"
#include "WadSurfaceTextures.h"

struct FAAWadNativeImport;

namespace
{
    UTexture2D* SaveInterfaceTexture(FAAWadNativeImport& Import, const TCHAR* Folder, const FString& Name,
        FIntPoint Size, const TArray<FColor>& Pixels);
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
    const FName ShellSlot(TEXT("LightingShell"));
    constexpr const TCHAR* ShellMaterialPackage = TEXT("/Game/AAReview/M_AANativeLightingShell");

    UMaterial* LoadOrCreateLightingShell()
    {
        const FString ObjectPath = FString(ShellMaterialPackage) + TEXT(".M_AANativeLightingShell");
        if (FPackageName::DoesPackageExist(ShellMaterialPackage))
        {
            return LoadObject<UMaterial>(nullptr, *ObjectPath);
        }
        UPackage* Package = CreatePackage(ShellMaterialPackage);
        const FName Name(TEXT("M_AANativeLightingShell"));
        if (UMaterial* Existing = FindObject<UMaterial>(Package, *Name.ToString()))
        {
            return Existing;
        }
        UMaterial* Material = NewObject<UMaterial>(Package, Name, AssetFlags);
        // Distance-field generation uses opaque and masked triangles. The zero mask keeps
        // these closing faces out of the raster/depth passes where they overlap actual walls and flats.
        Material->BlendMode = BLEND_Masked;
        Material->SetShadingModel(MSM_Unlit);
        auto* Mask = NewObject<UMaterialExpressionConstant>(Material);
        Mask->R = 0.0f;
        Material->GetExpressionCollection().AddExpression(Mask);
        Material->GetEditorOnlyData()->OpacityMask.Connect(0, Mask);
        Material->PostEditChange();
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    // The generated front lies at Y0 for walls and Z0 for flats. Keep its exact vertices,
    // UVs and material slot, and assign the backs and side caps to their own section.
    bool SplitLightingShell(UStaticMesh& Mesh, FMeshDescription& Description, int32 FrontAxis, UMaterial& Shell)
    {
        if (Description.PolygonGroups().Num() != 1 || Description.Polygons().Num() == 0 || Mesh.GetStaticMaterials().Num() != 1)
        {
            return false;
        }
        FStaticMeshAttributes Attributes(Description);
        const auto Positions = Attributes.GetVertexPositions();
        TArray<FPolygonID> Caps;
        for (const FPolygonID Polygon : Description.Polygons().GetElementIDs())
        {
            for (const FVertexInstanceID Corner : Description.GetPolygonVertexInstances(Polygon))
            {
                if (!FMath::IsNearlyZero(Positions[Description.GetVertexInstanceVertex(Corner)][FrontAxis]))
                {
                    Caps.Add(Polygon);
                    break;
                }
            }
        }
        if (Caps.IsEmpty() || Caps.Num() == Description.Polygons().Num())
        {
            return false;
        }
        const FPolygonGroupID Group = Description.CreatePolygonGroup();
        Attributes.GetPolygonGroupMaterialSlotNames()[Group] = ShellSlot;
        for (const FPolygonID Polygon : Caps)
        {
            Description.SetPolygonPolygonGroup(Polygon, Group);
        }
        Mesh.GetStaticMaterials().Emplace(&Shell, ShellSlot, ShellSlot);
        Mesh.GetSectionInfoMap().Set(0, 1, FMeshSectionInfo(1));
        Mesh.GetOriginalSectionInfoMap().Set(0, 1, FMeshSectionInfo(1));
        return true;
    }

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
        TMap<int32, int32> RequiredItems;
        for (const int32 Marker : Markers)
        {
            if (const int32* Sector = MarkerSectors.Find(Marker); Sector && Map.Things[Marker].Type != DoorMarker)
            {
                RestrictedSectors.Add(*Sector);
                if (Map.Things[Marker].Type == DoorRequiredItemMarker)
                {
                    RequiredItems.Add(*Sector, Map.Things[Marker].Flags);
                }
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
                105.0f / TicksPerSecond, 140.0f / TicksPerSecond,
                RestrictedSectors.Contains(*Sector), RequiredItems.FindRef(*Sector) });
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
            Links.RemoveAll([Sector](const FAAWadNativeLevelLink& Link) { return Link.Sector == Sector; });
            // The source uses place IDs from 20000 up for hardcoded campaign screens.
            if (Thing.Flags >= 20000)
            {
                Links.Add({Sector, Index, {}, Thing.Flags});
                Output.Logf(TEXT("Imported WAD level link thing %d in sector %d to campaign form %d."), Index, Sector, Thing.Flags);
                continue;
            }
            const FString Destination = FString::Printf(TEXT("/Game/Maps/L%d.L%d"), Thing.Flags, Thing.Flags);
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
        const bool bChurchQuestMap = MapName == TEXT("L25") || MapName == TEXT("L20") || MapName == TEXT("L60");
        const bool bRetreatMap = MapName == TEXT("L64") || MapName == TEXT("L65") || MapName == TEXT("L34");
        if ((MapName == TEXT("L30") && !InstallL30Slice(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L151") && !InstallL151Slice(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L301") && !InstallL301Encounter(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L531") && !InstallL531Script(Actor, Map, WadPath, Output))
            || (MapName == TEXT("L31") && !InstallL31Script(Actor, Map, WadPath, Output))
            || (MapName == TEXT("L25") && !InstallL25Script(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L20") && !InstallL20Script(Actor, Map, WadPath, Output))
            || ((bChurchQuestMap || bRetreatMap) && !InstallScenery(World, Actor, Map, bRetreatMap, Output))
            || (bChurchQuestMap && !InstallChurchEncounters(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L60") && !InstallL60Script(World, Actor, Map, WadPath, Output))
            || (bRetreatMap && (!InstallRetreatScript(Actor, Map, WadPath, MapName, Output)
                || !InstallRetreatEncounters(World, Actor, Map, Output)))
            || (MapName == TEXT("L22") && (!InstallL22Script(Actor, Map, WadPath, Output)
                || !InstallL22Journal(World, Actor, Map, Output))))
        {
            return false;
        }
        Actor.LevelLinks = MoveTemp(Links);
        AWorldSettings* Settings = World.GetWorldSettings();
        Settings->Modify();
        Settings->DefaultGameMode = ANativeWadGameMode::StaticClass();
        return true;
    }

    bool InstallRetreatScript(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const FString& MapName, FOutputDevice& Output)
    {
        constexpr int32 L64Entries[] = {0, 1, 2, 3, 4, 201};
        constexpr int32 L65Entries[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 201, 202, 203, 204};
        constexpr int32 L34Entries[] = {0, 1, 2, 3, 4, 5, 200, 202, 203};
        // Some script IDs are absent from the shipped SOUNDS.RES. Import any that a complete archive provides.
        constexpr int32 UnavailableSounds[] = {5, 1508, 1510};
        const TConstArrayView<int32> Entries = MapName == TEXT("L64") ? TConstArrayView<int32>(L64Entries)
            : MapName == TEXT("L65") ? TConstArrayView<int32>(L65Entries) : TConstArrayView<int32>(L34Entries);
        const FString ScriptName = TEXT("S") + MapName.Mid(1) + TEXT(".SRP");
        if (!InstallActivities(Actor, Map, WadPath, *ScriptName, Entries, Output, UnavailableSounds))
        {
            return false;
        }
        Actor.ActivityWallMaterials.Reset();
        Actor.ActivityJournalPages.Reset();
        for (FWadScriptActivity& Activity : Actor.ImportedActivities)
        {
            for (FWadScriptCommand& Command : Activity.Commands)
            {
                if (Command.Operation == EWadScriptOperation::SetSwitch || Command.Operation == EWadScriptOperation::ChangeSideTexture)
                {
                    Command.Operation = EWadScriptOperation::ChangeSideTexture;
                    const FName Name(*Command.Text);
                    if (!Actor.FindActivitySideBand(Command.Target) || !Texture(Command.Text))
                    {
                        Output.Logf(TEXT("Could not install texture %s for scripted sidedef %d."), *Command.Text, Command.Target);
                        return false;
                    }
                    Actor.ActivityWallMaterials.Add(Name, Material(Command.Text, EWadWallBlend::Opaque));
                }
                else if (Command.Operation == EWadScriptOperation::AddJournalPage)
                {
                    const FString Name = FString::Printf(TEXT("NOTE%04d"), Command.Target);
                    FIntPoint Size;
                    TArray<FColor> Pixels;
                    UTexture2D* Page = LoadWadInterfacePicture(TEXT("UI/NOTES/") + Name, Size, Pixels)
                        ? SaveInterfaceTexture(*this, TEXT("Journal"), Name, Size, Pixels) : nullptr;
                    if (!Page)
                    {
                        Output.Logf(TEXT("Could not import the original journal page %s."), *Name);
                        return false;
                    }
                    Actor.ActivityJournalPages.Add(Command.Target, Page);
                }
            }
        }
        return !bAssetError;
    }

    static bool InstallL531Script(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 201};
        return InstallActivities(Actor, Map, WadPath, TEXT("S531.SRP"), EntryActivities, Output);
    }

    bool InstallL31Script(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 1, 2, 3, 4, 5, 200, 201};
        return InstallActivities(Actor, Map, WadPath, TEXT("S31.SRP"), EntryActivities, Output)
            && InstallPuzzleSwitch(Actor, 1159, Output);
    }

    bool InstallL22Script(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 1, 2, 3, 4, 5, 6, 7};
        if (!InstallActivities(Actor, Map, WadPath, TEXT("S22.SRP"), EntryActivities, Output)
            || !InstallPuzzleSwitch(Actor, 1132, Output))
        {
            return false;
        }
        Actor.ScriptMissileMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        UMaterial* Dart = SpriteMaterial(TEXT("MagicDart"), TEXT("MISSIL12/1.CPC"));
        UMaterial* Paralysis = SpriteMaterial(TEXT("ParalyzeMissile"), TEXT("EARTHBND/1.CPC"));
        if (!Actor.ScriptMissileMesh || !Dart || !Paralysis)
        {
            Output.Log(TEXT("Could not install the L22 trap projectile art."));
            return false;
        }
        Actor.ScriptMissileMaterials.Add(4011, Dart);
        Actor.ScriptMissileMaterials.Add(4016, Paralysis);
        return !bAssetError;
    }

    bool InstallL25Script(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map,
        const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 1, 2, 10, 11, 12, 13};
        TArray<FAAWadNativeDoor> Doors;
        if (!InstallActivities(Actor, Map, WadPath, TEXT("S25.SRP"), EntryActivities, Output)
            || !BuildDoors(Map, Doors, Output))
        {
            return false;
        }
        const FString GeneratorPath = FPaths::ChangeExtension(WadPath, TEXT("GEN"));
        TArray<FString> GeneratorLines;
        if (!FFileHelper::LoadFileToStringArray(GeneratorLines, *GeneratorPath))
        {
            return false;
        }
        GeneratorLines.RemoveAll([](const FString& Line) { return !Line.StartsWith(TEXT("G ")); });
        TArray<FString> Fields;
        if (GeneratorLines.Num() == 1)
        {
            GeneratorLines[0].ParseIntoArrayWS(Fields);
        }
        if (Fields.Num() != 11 || FCString::Atoi(*Fields[1]) != NativeWadRules::ChurchKey
            || FCString::Atoi(*Fields[6]) != 0 || FCString::Atoi(*Fields[9]) != 0 || FCString::Atoi(*Fields[10]) != 1)
        {
            Output.Log(TEXT("L25 needs its original inactive, one-shot key generator."));
            return false;
        }
        const FVector2D XY(FCString::Atoi(*Fields[2]), FCString::Atoi(*Fields[3]));
        const int32 Sector = Actor.FindWadSector(XY);
        const float Delay = float(FCString::Atoi(*Fields[5]));
        if (!Map.Sectors.IsValidIndex(Sector) || Delay <= 0 || !Doors.ContainsByPredicate(
            [](const FAAWadNativeDoor& Door) { return Door.RequiredItem == NativeWadRules::ChurchKey; }))
        {
            return false;
        }
        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        auto* Image = SpriteMaterial(TEXT("ChurchKey"), TEXT("KEY2/KEY2.CPC"));
        if (!Plane || !Image)
        {
            return false;
        }
        const FName Tag(TEXT("L25 Key Generator 0"));
        ANativeWadPickup* Key = nullptr;
        for (TActorIterator<ANativeWadPickup> Item(&World); Item; ++Item)
        {
            if (Item->ActorHasTag(Tag))
            {
                Key = *Item;
                Key->Modify();
                break;
            }
        }
        if (!Key)
        {
            FActorSpawnParameters Params;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            const double Yaw = FCString::Atoi(*Fields[4]) * (360.0 / 65536.0);
            Key = World.SpawnActor<ANativeWadPickup>(FVector(XY, Map.Sectors[Sector].FloorHeight + 12.0),
                FRotator(0, Yaw, 0), Params);
            if (!Key)
            {
                return false;
            }
            Key->Tags.Add(Tag);
            Key->SetActorLabel(Tag.ToString());
        }
        Key->Configure(Plane, Image, NativeWadRules::ChurchKey);
        Key->SetAvailable(false);
        Actor.PickupGenerators.Reset();
        Actor.PickupGenerators.Add(0, {Key, Delay});
        Actor.Doors = MoveTemp(Doors);
        Output.Logf(TEXT("Imported L25 generator 0 and its required-item door. Key at %s, delay %.0fs."), *XY.ToString(), Delay);
        return !bAssetError;
    }

    bool InstallL20Script(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        if (!InstallActivities(Actor, Map, WadPath, TEXT("S20.SRP"), EntryActivities, Output)
            || !InstallPuzzleSwitch(Actor, 791, Output))
        {
            return false;
        }
        Actor.ScriptMissileMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        UMaterial* Bolt = SpriteMaterial(TEXT("ChurchPiercingBolt"), TEXT("ARROW7/1.CPC"));
        if (!Actor.ScriptMissileMesh || !Bolt)
        {
            return false;
        }
        Actor.ScriptMissileMaterials.Add(NativeWadRules::PiercingCrossbowBolt, Bolt);
        return !bAssetError;
    }

    bool InstallL60Script(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        constexpr int32 EntryActivities[] = {0, 1, 3, 4, 5, 6, 7, 8, 21, 200, 201, 202};
        if (!InstallActivities(Actor, Map, WadPath, TEXT("S60.SRP"), EntryActivities, Output))
        {
            return false;
        }
        Actor.ScriptObjects.Reset();
        for (const FWadScriptActivity& Activity : Actor.ImportedActivities)
        {
            for (const FWadScriptCommand& Command : Activity.Commands)
            {
                if (Command.Operation == EWadScriptOperation::ActivateGenerator && !Actor.CreatureGenerators.Contains(Command.Target))
                {
                    return false;
                }
                if ((Command.Operation != EWadScriptOperation::ReadObjectAngle && Command.Operation != EWadScriptOperation::SetObjectAngle)
                    || Actor.ScriptObjects.Contains(Command.Target))
                {
                    continue;
                }
                // Source object IDs are the 1-based Thing indices, including player starts.
                const int32 ThingId = Command.Target - 1;
                if (!Map.Things.IsValidIndex(ThingId) || Map.Things[ThingId].Type != 151)
                {
                    return false;
                }
                const FName Tag(*FString::Printf(TEXT("WAD Scenery Thing %d"), ThingId));
                for (TActorIterator<ANativeWadScenery> Statue(&World); Statue; ++Statue)
                {
                    if (Statue->ActorHasTag(Tag))
                    {
                        Actor.ScriptObjects.Add(Command.Target, *Statue);
                        break;
                    }
                }
                if (!Actor.ScriptObjects.Contains(Command.Target))
                {
                    return false;
                }
            }
        }
        if (Actor.ScriptObjects.Num() != 5)
        {
            return false;
        }
        return InstallQuestPickup(World, Actor, Map, NativeWadRules::AndrewNecklace, TEXT("AndrewNecklace"),
            TEXT("NECKLACE/NECKLACE.CPC"), TEXT("L60 Necklace of Andrew"), Output, FWadLevelCheckpoint::CurrentPickupRevision);
    }

    bool ImportSceneryDefinition(const FString& Resource, FWadSceneryDefinition& Definition)
    {
        const FString Directory = FPaths::ProjectDir() /
            TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/OBJS") / Resource;
        const FString ObjectName = Resource == TEXT("DEAD01") ? TEXT("DEAD") : Resource;
        TArray<FString> Lines;
        const FString DefinitionPath = Directory / (ObjectName + TEXT(".INF"));
        if (!FFileHelper::LoadFileToStringArray(Lines, *DefinitionPath))
        {
            return false;
        }
        TArray<int32> Values;
        for (const FString& Line : Lines)
        {
            const int32 Comment = Line.Find(TEXT(";"));
            const FString Number = (Comment == INDEX_NONE ? Line : Line.Left(Comment)).TrimStartAndEnd();
            if (!Number.IsEmpty())
            {
                int32 Value;
                if (!FDefaultValueHelper::ParseInt(Number, Value))
                {
                    return false;
                }
                Values.Add(Value);
            }
        }

        // INF stores the object header, first stance, frame headers, then the picture IDs for those frames.
        constexpr int32 StanceStart = 8;
        constexpr int32 FrameStart = 12;
        constexpr int32 FrameFields = 4;
        if (Values.Num() < FrameStart || Values[0] != 1 || Values[1] < 0 || Values[StanceStart] < 1
            || Values[StanceStart] > (Values.Num() - FrameStart) / FrameFields
            || Values[StanceStart + 1] < 0 || Values[StanceStart + 2] < 0 || Values[StanceStart + 2] > 2)
        {
            return false;
        }
        constexpr int32 Passable = 0x0001;
        constexpr int32 FullyPassable = 0x2000;
        constexpr int32 DoNotSink = 0x0100;
        constexpr int32 StickToCeiling = 0x0200;
        constexpr int32 Raised = 0x0400;
        Definition.Radius = Values[1];
        Definition.bPassable = (Values[2] & (Passable | FullyPassable)) != 0 || Definition.Radius == 0;
        Definition.bStaysAboveLiquid = (Values[7] & DoNotSink) != 0;
        Definition.bHanging = (Values[7] & StickToCeiling) != 0;
        Definition.bRaised = (Values[7] & Raised) != 0;
        Definition.FrameTicks = Values[StanceStart + 1];
        Definition.AnimationType = Values[StanceStart + 2];
        const int32 Frames = Values[StanceStart];
        int32 PictureIndex = FrameStart + FrameFields * Frames;

        const FString FileListPath = Directory / (ObjectName + TEXT(".FL"));
        if (!FFileHelper::LoadFileToStringArray(Lines, *FileListPath))
        {
            return false;
        }
        TMap<int32, FString> PictureFiles;
        for (const FString& Line : Lines)
        {
            TArray<FString> Words;
            Line.ParseIntoArrayWS(Words);
            if (Words.Num() >= 2 && FPaths::GetExtension(Words[0]).Equals(TEXT("CPC"), ESearchCase::IgnoreCase)
                && Words[1].IsNumeric())
            {
                PictureFiles.Add(FCString::Atoi(*Words[1]), Words[0].ToUpper());
            }
        }

        TMap<int32, FWadSceneryView> Pictures;
        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            const int32 AngleCount = Values[FrameStart + FrameFields * Frame];
            if ((AngleCount != 1 && AngleCount != 4 && AngleCount != 8)
                || Values.Num() - PictureIndex < AngleCount)
            {
                return false;
            }
            FWadSceneryFrame& Result = Definition.Frames.AddDefaulted_GetRef();
            for (int32 View = 0; View < AngleCount; ++View)
            {
                const int32 SignedId = Values[PictureIndex++];
                if (SignedId == MIN_int32)
                {
                    return false;
                }
                const int32 PictureId = FMath::Abs(SignedId);
                if (!Pictures.Contains(PictureId))
                {
                    const FString* PictureFile = PictureFiles.Find(PictureId);
                    if (!PictureFile)
                    {
                        return false;
                    }
                    FWadSceneryView NewPicture;
                    NewPicture.Material = SpriteMaterial(FString::Printf(TEXT("Scenery_%s_%d"), *Resource, PictureId),
                        Resource / *PictureFile, &NewPicture.Size);
                    if (!NewPicture.Material)
                    {
                        return false;
                    }
                    Pictures.Add(PictureId, NewPicture);
                }
                FWadSceneryView& ResultView = Result.Views.Add_GetRef(Pictures.FindChecked(PictureId));
                ResultView.bMirrored = SignedId < 0;
            }
        }
        return true;
    }

    bool InstallScenery(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, bool bRetreat, FOutputDevice& Output)
    {
        // These are fixed scenery types from OBJS.FL. Pickup, encounter and script types have their own importers.
        static const TMap<int32, FString> Resources = {
            {11, TEXT("COLUMN")}, {20, TEXT("MOSSROCK")}, {27, TEXT("TORCH1")}, {30, TEXT("GLOWER")},
            {38, TEXT("TROPHY")}, {41, TEXT("BRAFIR")}, {48, TEXT("CB04")}, {79, TEXT("BIGTREE")},
            {81, TEXT("MYROCK1")}, {115, TEXT("BGVASE")}, {137, TEXT("CHAIR01")}, {138, TEXT("TABLE01")},
            {142, TEXT("TOMB3")}, {151, TEXT("STATUE")}, {285, TEXT("RUBL4")}, {294, TEXT("CANDAB1")},
            {296, TEXT("CANDSTK1")}, {669, TEXT("FNTN")}, {3104, TEXT("DEAD01")}, {4071, TEXT("TREE1")},
            {4072, TEXT("TREE2")}, {4074, TEXT("TREE4")}, {4076, TEXT("TREE6")}, {4077, TEXT("TREE7")},
            {4078, TEXT("TREE8")}, {4083, TEXT("TORCH2")}
        };
        static const TMap<int32, FString> RetreatResources = {
            {14, TEXT("BARREL1")}, {15, TEXT("BARREL3")}, {18, TEXT("ROCK_1")}, {25, TEXT("SWPPLT03")},
            {59, TEXT("JAW")}, {65, TEXT("SKULL")}, {72, TEXT("STONE")}, {121, TEXT("STAL2")},
            {124, TEXT("STAL5")}, {134, TEXT("STAL15")}, {136, TEXT("STAL17")},
            {4075, TEXT("TREE5")}, {4079, TEXT("TREE9")}
        };
        UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        if (!Plane)
        {
            return false;
        }
        TMap<int32, FWadSceneryDefinition> Definitions;
        int32 Count = 0;
        int32 PassableCount = 0;
        for (int32 Id = 0; Id < Map.Things.Num(); ++Id)
        {
            const FWadThing& Thing = Map.Things[Id];
            const FString* Resource = Resources.Find(Thing.Type);
            if (!Resource && bRetreat)
            {
                Resource = RetreatResources.Find(Thing.Type);
            }
            if (!Resource)
            {
                continue;
            }
            const int32 Sector = Actor.FindWadSector(FVector2D(Thing.X, Thing.Y));
            if (!Map.Sectors.IsValidIndex(Sector))
            {
                Output.Logf(TEXT("Scenery thing %d (%s) falls outside the authored floor."), Id, **Resource);
                return false;
            }
            if (!Definitions.Contains(Thing.Type) && !ImportSceneryDefinition(*Resource, Definitions.Add(Thing.Type)))
            {
                Output.Logf(TEXT("Could not import scenery thing %d (%s) from its original object definition."), Id, **Resource);
                return false;
            }
            const FString Tag = FString::Printf(TEXT("WAD Scenery Thing %d"), Id);
            ANativeWadScenery* Scenery = ImportSliceThing<ANativeWadScenery>(World, Map, Id, 0.0, *Tag);
            if (!Scenery)
            {
                return false;
            }
            Scenery->SetActorLocationAndRotation(FVector(Thing.X, Thing.Y, 0.0), FRotator(0, Thing.Angle * 45.0, 0));
            const FWadSceneryDefinition& Definition = Definitions.FindChecked(Thing.Type);
            Scenery->Configure(Actor, Plane, Definition, Sector, Id);
            ++Count;
            PassableCount += Definition.bPassable ? 1 : 0;
        }
        Output.Logf(TEXT("Imported %d authored scenery placements (%d solid, %d passable)."), Count, Count - PassableCount, PassableCount);
        return !bAssetError;
    }

    bool InstallRetreatEncounters(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        struct FCreature
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
        // Original placements and art come from THINGS and the object stance tables. CREDATA.H supplies the combat values.
        const FCreature Creatures[] = {
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
        UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        if (!Plane)
        {
            return false;
        }
        for (const FCreature& Creature : Creatures)
        {
            if (!Map.Things.ContainsByPredicate([&Creature](const FWadThing& Thing) { return Thing.Type == Creature.Type; }))
            {
                continue;
            }
            const FString Resource(Creature.Resource);
            FIntPoint Size;
            auto* Standing = SpriteMaterial(Resource, Resource / Creature.Standing, &Size);
            auto* Hurt = SpriteMaterial(Resource + TEXT("_Hurt"), Resource / Creature.Hurt);
            auto* Downed = SpriteMaterial(Resource + TEXT("_Down"), Resource / Creature.Downed);
            auto* Attacking = SpriteMaterial(Resource + TEXT("_Attack"), Resource / Creature.Attacking);
            auto* Dart = Creature.MaxDartRange ? SpriteMaterial(TEXT("MagicDart"), TEXT("MISSIL12/1.CPC")) : nullptr;
            if (!Standing || !Hurt || !Downed || !Attacking || (Creature.MaxDartRange && !Dart))
            {
                return false;
            }
            FWadEncounterProfile Profile;
            Profile.Name = Creature.Name;
            Profile.MaxHealth = Creature.Health;
            Profile.RetreatHealth = Creature.RetreatHealth;
            Profile.MeleeDamage = Creature.MeleeDamage;
            Profile.MeleeDelayUpdates = 10;
            Profile.BoltDamage = Dart ? 100 : 0;
            Profile.MinBoltRange = Creature.MinDartRange;
            Profile.BoltRange = Creature.MaxDartRange;
            Profile.RangedDelayUpdates = Creature.DartDelay;
            Profile.CollisionRadius = Creature.Radius;
            Profile.CollisionHalfHeight = Size.Y * 0.5f;
            Profile.SpriteSize = FVector2D(Size);
            Profile.MeleeRange = Creature.MeleeRange;
            Profile.UpdateTicks = Creature.UpdateTicks;
            Profile.WalkSpeed = Creature.Step * TicksPerSecond / (Creature.UpdateTicks + (Creature.UpdateTicks >> 1));
            Profile.VisionHalfAngle = Creature.Vision * 0.5f;
            int32 Count = 0;
            for (int32 Id = 0; Id < Map.Things.Num(); ++Id)
            {
                const FWadThing& Thing = Map.Things[Id];
                if (Thing.Type != Creature.Type)
                {
                    continue;
                }
                const int32 Sector = Actor.FindWadSector(FVector2D(Thing.X, Thing.Y));
                if (!Map.Sectors.IsValidIndex(Sector))
                {
                    Output.Logf(TEXT("Creature thing %d (%s) falls outside the authored floor."), Id, Creature.Name);
                    return false;
                }
                const FString Tag = FString::Printf(TEXT("WAD Creature Thing %d"), Id);
                auto* Enemy = ImportSliceThing<ANativeWadGuardian>(World, Map, Id,
                    Actor.GetWalkingHeight(Sector) + Profile.CollisionHalfHeight + 2.0, *Tag);
                if (!Enemy)
                {
                    return false;
                }
                Enemy->Configure(Plane, Standing, Downed, Attacking, Hurt, Dart, Profile);
                ++Count;
            }
            Output.Logf(TEXT("Imported %d authored %s placements (thing type %d)."), Count, Creature.Name, Creature.Type);
        }
        return !bAssetError;
    }

    static bool InstallL60Generators(UWorld& World, ANativeWadLevel& Actor, const FString& WadPath, const FWadEncounterProfile& Profile,
        TFunctionRef<void(ANativeWadGuardian&)> Configure, FOutputDevice& Output)
    {
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *FPaths::ChangeExtension(WadPath, TEXT("GEN"))))
        {
            return false;
        }
        Actor.CreatureGenerators.Reset();
        for (const FString& Line : Lines)
        {
            const int32 Comment = Line.Find(TEXT(";"));
            const FString Payload = Comment == INDEX_NONE ? Line : Line.Left(Comment);
            TArray<FString> Words;
            Payload.ParseIntoArrayWS(Words);
            if (Words.IsEmpty())
            {
                continue;
            }
            int32 Fields[10];
            if (Words.Num() != 11 || Words[0] != TEXT("G"))
            {
                return false;
            }
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
            {
                if (!FDefaultValueHelper::ParseInt(Words[Index + 1], Fields[Index]))
                {
                    return false;
                }
            }
            // L60 has finite, initially inactive crossbowman generators with fixed intervals.
            if (Fields[0] != NativeWadRules::ChurchFootKnight || Fields[3] < 0 || Fields[3] > MAX_uint16
                || Fields[4] <= 0 || Fields[5] != 0 || Fields[8] != 0 || Fields[9] <= 0 || Fields[9] > 40)
            {
                return false;
            }
            const FVector2D XY(Fields[1], Fields[2]);
            const int32 Sector = Actor.FindWadSector(XY);
            if (Sector == INDEX_NONE)
            {
                return false;
            }
            const int32 Id = Actor.CreatureGenerators.Num();
            FWadCreatureGenerator& Generator = Actor.CreatureGenerators.Add(Id);
            Generator.Sector = Sector;
            Generator.Interval = Fields[4];
            const FVector Position(XY, Actor.GetWalkingHeight(Sector) + Profile.CollisionHalfHeight + 2.0);
            const FRotator Rotation(0, Fields[3] * (360.0 / 65536.0), 0);
            for (int32 Index = 0; Index < Fields[9]; ++Index)
            {
                const FString Label = FString::Printf(TEXT("L60 Generator %d Knight %d"), Id, Index);
                ANativeWadGuardian* Knight = ImportTaggedActor<ANativeWadGuardian>(World, Position, Rotation, *Label);
                if (!Knight)
                {
                    return false;
                }
                Knight->SetActorLocationAndRotation(Position, Rotation);
                Configure(*Knight);
                Knight->SetSpawned(false);
                Generator.Creatures.Add(Knight);
            }
            Output.Logf(TEXT("Imported L60 knight generator %d with %d releases at %.0fs intervals."), Id, Fields[9], Generator.Interval);
        }
        return Actor.CreatureGenerators.Num() == 4;
    }

    bool InstallChurchEncounters(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath, FOutputDevice& Output)
    {
        using namespace NativeWadRules;
        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        if (!Plane)
        {
            return false;
        }
        for (const bool bMounted : {false, true})
        {
            const int32 Type = bMounted ? ChurchMountedKnight : ChurchFootKnight;
            const FString AssetName = bMounted ? TEXT("MountedKnight") : TEXT("FootKnight");
            auto* Standing = SpriteMaterial(AssetName, bMounted ? TEXT("KNITHRS/KHSRUN03.CPC") : TEXT("KXW/KXW001.CPC"));
            auto* Attack = bMounted ? Standing : SpriteMaterial(AssetName + TEXT("Attack"), TEXT("KXW/KXA005.CPC"));
            auto* Hurt = SpriteMaterial(AssetName + TEXT("Hurt"), bMounted ? TEXT("KNITHRS/KHSDI02.CPC") : TEXT("KXW/KXD001.CPC"));
            auto* Down = SpriteMaterial(AssetName + TEXT("Down"), bMounted ? TEXT("KNITHRS/KHSDI08.CPC") : TEXT("KXW/KXD008.CPC"));
            auto* Bolt = SpriteMaterial(bMounted ? TEXT("KnightPiercingBolt") : TEXT("KnightGrayBolt"),
                bMounted ? TEXT("ARROW7/1.CPC") : TEXT("ARROW1/BOLT0.CPC"));
            if (!Standing || !Attack || !Hurt || !Down || !Bolt)
            {
                return false;
            }
            // CREDATA.H supplies combat values, while the object definitions supply radius and standing sprite size.
            FWadEncounterProfile Profile;
            Profile.Name = bMounted ? TEXT("Mounted Knight of Andrew") : TEXT("Knight of Andrew Crossbowman");
            Profile.MaxHealth = 3000;
            Profile.RetreatHealth = bMounted ? 0 : 500;
            Profile.MeleeDamage = bMounted ? 200 : 0;
            Profile.BoltDamage = ChurchBoltDamage;
            Profile.BoltRange = 1000;
            Profile.MeleeDelayUpdates = 10;
            Profile.RangedDelayUpdates = 20;
            Profile.CollisionRadius = bMounted ? 32.0f : 20.0f;
            Profile.SpriteSize = bMounted ? FVector2D(101, 83) : FVector2D(61, 62);
            Profile.CollisionHalfHeight = float(Profile.SpriteSize.Y * 0.5);
            Profile.WalkSpeed = bMounted ? 154.0f : 105.0f;
            Profile.UpdateTicks = 7;
            Profile.RegenPerUpdate = bMounted ? 5 : 1;
            Profile.MeleeRange = bMounted ? 75.0f : 60.0f;
            Profile.VisionHalfAngle = bMounted ? 67.5f : 90.0f;
            Profile.bKeepsBoltDistance = true;
            const auto Configure = [&](ANativeWadGuardian& Knight)
            {
                Knight.Configure(Plane, Standing, Down, Attack, Hurt, Bolt, Profile);
            };
            int32 Count = 0;
            for (int32 Id = 0; Id < Map.Things.Num(); ++Id)
            {
                const FWadThing& Thing = Map.Things[Id];
                if (Thing.Type != Type)
                {
                    continue;
                }
                const int32 Sector = Actor.FindWadSector(FVector2D(Thing.X, Thing.Y));
                if (!Map.Sectors.IsValidIndex(Sector))
                {
                    Output.Logf(TEXT("%s thing %d falls outside the authored floor."), *Profile.Name, Id);
                    return false;
                }
                const FString Tag = FString::Printf(TEXT("WAD Knight Thing %d"), Id);
                auto* Knight = ImportSliceThing<ANativeWadGuardian>(World, Map, Id,
                    Actor.GetWalkingHeight(Sector) + Profile.CollisionHalfHeight + 2.0, *Tag);
                if (!Knight)
                {
                    return false;
                }
                Configure(*Knight);
                ++Count;
            }
            Output.Logf(TEXT("Imported %d authored %s placements (thing type %d)."), Count, *Profile.Name, Type);
            if (!bMounted && FPaths::GetBaseFilename(WadPath).Equals(TEXT("L60"), ESearchCase::IgnoreCase)
                && !InstallL60Generators(World, Actor, WadPath, Profile, Configure, Output))
            {
                Output.Log(TEXT("Could not import the authored L60 knight generators."));
                return false;
            }
        }
        return !bAssetError;
    }

    bool InstallL22Journal(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        return InstallQuestPickup(World, Actor, Map, NativeWadRules::JugurthaJournal, TEXT("JugurthasJournal"),
            TEXT("BOOK/BOOK.CPC"), TEXT("L22 Jugurtha Journal"), Output);
    }

    bool InstallQuestPickup(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, int32 Type,
        const TCHAR* Asset, const TCHAR* Picture, const TCHAR* Label, FOutputDevice& Output, int32 CheckpointRevision = 0)
    {
        const int32 Index = Map.Things.IndexOfByPredicate([Type](const FWadThing& Thing) { return Thing.Type == Type; });
        if (!Map.Things.IsValidIndex(Index))
        {
            Output.Logf(TEXT("The source pickup for %s was not found."), Label);
            return false;
        }
        const FWadThing& Thing = Map.Things[Index];
        const int32 Sector = Actor.FindWadSector(FVector2D(Thing.X, Thing.Y));
        if (!Map.Sectors.IsValidIndex(Sector))
        {
            return false;
        }
        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        auto* Image = SpriteMaterial(Asset, Picture);
        if (!Plane || !Image)
        {
            return false;
        }
        auto* Pickup = ImportSliceThing<ANativeWadPickup>(World, Map, Index, Map.Sectors[Sector].FloorHeight + 12.0, Label);
        if (!Pickup)
        {
            return false;
        }
        Pickup->Configure(Plane, Image, Type, CheckpointRevision);
        Output.Logf(TEXT("Imported %s from Thing %d in sector %d."), Label, Index, Sector);
        return true;
    }

    bool InstallPuzzleSwitch(ANativeWadLevel& Actor, int32 SwitchSide, FOutputDevice& Output)
    {
        if (!Actor.Sides.IsValidIndex(SwitchSide))
        {
            return false;
        }
        const UStaticMeshComponent* Switch = Actor.Sides[SwitchSide].Bands[uint8(EWadWallBand::Lower)];
        Parents[uint8(EWadWallBlend::Opaque)] = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/AAReview/M_AANativeOpaque.M_AANativeOpaque"));
        if (!Switch || !Switch->GetMaterial(0) || !Parents[uint8(EWadWallBlend::Opaque)] || !Texture(TEXT("SWIT1U")))
        {
            Output.Logf(TEXT("The switch at source sidedef %d could not be installed."), SwitchSide);
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

    UMaterial* SpriteMaterial(const FString& Name, const FString& PicturePath, FIntPoint* OutSize = nullptr)
    {
        FIntPoint Size;
        TArray<FColor> Pixels;
        if (!LoadWadSprite(PicturePath, Size, Pixels))
        {
            return nullptr;
        }
        if (OutSize)
        {
            *OutSize = Size;
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
        const FWadThing& Thing = Map.Things[ThingId];
        return ImportTaggedActor<T>(World, FVector(Thing.X, Thing.Y, CenterZ), FRotator(0, Thing.Angle * 45.0, 0), Name);
    }

    template<class T>
    static T* ImportTaggedActor(UWorld& World, const FVector& Position, const FRotator& Rotation, const TCHAR* Name)
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
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        T* Actor = World.SpawnActor<T>(Position, Rotation, Params);
        if (Actor)
        {
            Actor->Tags.Add(Tag);
            Actor->SetActorLabel(Name);
        }
        return Actor;
    }

    static bool InstallActivities(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const TCHAR* ScriptName, TConstArrayView<int32> EntryActivities, FOutputDevice& Output, TConstArrayView<int32> UnavailableSounds = {})
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
        if (!ImportWadSounds(FPaths::GetPath(ScriptPath) / TEXT("SOUNDS.RES"), Activities, Actor.ActivitySounds, Output, UnavailableSounds))
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
        bool bDistanceField, TArray<FKConvexElem> Hulls = {}, int32 FrontAxis = INDEX_NONE)
    {
        UStaticMesh* Mesh = FindOrCreateAsset<UStaticMesh>(TEXT("Meshes"), Name);
        if (!Mesh || Geometry.Description.Triangles().Num() == 0)
        {
            return nullptr;
        }
        Mesh->GetStaticMaterials().Reset();
        Mesh->GetStaticMaterials().Emplace(Material, SurfaceSlot, SurfaceSlot);
        if (FrontAxis != INDEX_NONE)
        {
            Mesh->GetSectionInfoMap().Set(0, 0, FMeshSectionInfo(0));
            Mesh->GetOriginalSectionInfoMap().Set(0, 0, FMeshSectionInfo(0));
            UMaterial* Shell = LoadOrCreateLightingShell();
            if (!Shell || !SplitLightingShell(*Mesh, Geometry.Description, FrontAxis, *Shell))
            {
                return nullptr;
            }
            if (Shell->GetOutermost()->IsDirty())
            {
                AssetsToSave.AddUnique(Shell);
            }
        }
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
                Parents[Blend], bOpaque, {}, bOpaque ? 1 : INDEX_NONE);
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
        UStaticMesh* Mesh = BuildMesh(TEXT("SM_") + Name, Geometry, SurfaceMaterial, !bSky, MoveTemp(Hulls), 2);
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

bool RefreshNativeWadSurfaces(const FString& MapPackage, FOutputDevice& Output)
{
    if (!MapPackage.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(MapPackage)
        || !FPackageName::DoesPackageExist(MapPackage))
    {
        Output.Log(TEXT("Usage: AA.RefreshNativeWadSurfaces </Game/MapPackage>"));
        return false;
    }
    const FString MeshRoot = MapPackage + TEXT("_Assets/Meshes");
    const FString OpaqueWall = TEXT("SM_Wall_Opaque");
    if (!FPackageName::DoesPackageExist(MeshRoot / OpaqueWall))
    {
        Output.Logf(TEXT("%s has no generated native WAD wall mesh."), *MapPackage);
        return false;
    }
    TArray<FString> MeshFiles;
    const FString Extension = FPackageName::GetAssetPackageExtension();
    const FString Pattern = FPackageName::LongPackageNameToFilename(MeshRoot) / (TEXT("SM_Sector_*") + Extension);
    IFileManager::Get().FindFiles(MeshFiles, *Pattern, true, false);
    if (MeshFiles.IsEmpty())
    {
        return false;
    }
    MeshFiles.Add(OpaqueWall + Extension);

    UMaterial* Shell = LoadOrCreateLightingShell();
    if (!Shell || (Shell->GetOutermost()->IsDirty() && !SaveAsset(*Shell, *Extension)))
    {
        return false;
    }
    TArray<UStaticMesh*> ChangedMeshes;
    for (const FString& File : MeshFiles)
    {
        const FString Name = FPaths::GetBaseFilename(File);
        const FString ObjectPath = MeshRoot / (Name + TEXT(".") + Name);
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
        if (!Mesh)
        {
            return false;
        }
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        const auto& Materials = Mesh->GetStaticMaterials();
        if (Materials.IsValidIndex(1) && Materials[1].MaterialSlotName == ShellSlot && Materials[1].MaterialInterface == Shell)
        {
            continue;
        }
        FMeshDescription* Geometry = Mesh->GetMeshDescription(0);
        if (!Geometry || !SplitLightingShell(*Mesh, *Geometry, Name == OpaqueWall ? 1 : 2, *Shell))
        {
            Output.Logf(TEXT("Could not split the generated lighting shell for %s."), *ObjectPath);
            return false;
        }
        Mesh->CommitMeshDescription(0);
        Mesh->Build(false);
        Mesh->MarkPackageDirty();
        ChangedMeshes.Add(Mesh);
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    for (UStaticMesh* Mesh : ChangedMeshes)
    {
        if (!SaveAsset(*Mesh, *Extension))
        {
            return false;
        }
    }
    Output.Logf(TEXT("Refreshed %d of %d native WAD surface meshes for %s."),
        ChangedMeshes.Num(), MeshFiles.Num(), *MapPackage);
    return true;
}

namespace
{
    bool ReadTownQuest(const FString& Path, FWadTownQuest& Quest)
    {
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
        {
            return false;
        }
        TMap<FString, FString> Fields;
        FString Section;
        for (const FString& Line : Lines)
        {
            if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
            {
                Section = Line.Mid(1, Line.Len() - 2).ToLower();
                continue;
            }
            FString Key, Value;
            if (!Line.Split(TEXT("="), &Key, &Value) || Key.TrimStart().StartsWith(TEXT("#")))
            {
                continue;
            }
            // The original INI reader joins repeated keys and uses underscores to preserve spaces.
            if (Value.StartsWith(TEXT(" ")))
            {
                Value.RemoveAt(0);
            }
            Value.ReplaceInline(TEXT("_"), TEXT(" "));
            Value.ReplaceInline(TEXT("\\r"), TEXT("\n"));
            Value.ReplaceInline(TEXT("\\n"), TEXT("\n"));
            Value.ReplaceInline(TEXT("\\t"), TEXT("\t"));
            Fields.FindOrAdd(Section + TEXT(".") + Key.TrimStartAndEnd().ToLower()) += Value;
        }
        Quest.Title = Fields.FindRef(TEXT("main.title"));
        Quest.Goal = Fields.FindRef(TEXT("main.goal"));
        Quest.Description = Fields.FindRef(TEXT("main.description"));
        Quest.NotesHeading = Fields.FindRef(TEXT("main.description2"));
        Quest.Dialogue = Fields.FindRef(TEXT("main.dialogue"));
        Quest.Success = Fields.FindRef(TEXT("success.description"));
        Quest.Failure = Fields.FindRef(TEXT("failure.description"));
        Quest.TurnInMessage = Fields.FindRef(TEXT("main.keepobjtext"));
        Quest.GoldMessage = Fields.FindRef(TEXT("main.rewardgoldtext"));
        Quest.QuestItem = FCString::Atoi(*Fields.FindRef(TEXT("main.questobj")));
        Quest.Gold = FCString::Atoi(*Fields.FindRef(TEXT("main.gold")));
        Quest.Experience = FCString::Atoi(*Fields.FindRef(TEXT("main.experience")));
        Quest.bKeepItem = !Fields.FindRef(TEXT("main.keepobj")).Equals(TEXT("FALSE"), ESearchCase::IgnoreCase);
        const int32 Count = FCString::Atoi(*Fields.FindRef(TEXT("main.nummaps")));
        for (int32 Index = 1; Index <= Count; ++Index)
        {
            const FString Map = FString::Printf(TEXT("map%d."), Index);
            const int32 Number = FCString::Atoi(*Fields.FindRef(Map + TEXT("mapnumber")));
            if (Number <= 0)
            {
                return false;
            }
            Quest.Maps.Add(Number);
            Quest.MapNotes.Add(Fields.FindRef(Map + TEXT("description")));
        }
        return !Quest.Title.IsEmpty() && !Quest.Maps.IsEmpty()
            && Quest.Maps[0] == FCString::Atoi(*Fields.FindRef(TEXT("main.firstmap")));
    }

    UTexture2D* SaveInterfaceTexture(FAAWadNativeImport& Import, const TCHAR* Folder, const FString& Name,
        FIntPoint Size, const TArray<FColor>& Pixels)
    {
        UTexture2D* Texture = Import.FindOrCreateAsset<UTexture2D>(Folder, TEXT("T_") + Name);
        if (Texture)
        {
            Texture->Source.Init(Size.X, Size.Y, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
            Texture->CompressionSettings = TC_EditorIcon;
            Texture->MipGenSettings = TMGS_NoMipmaps;
            Texture->Filter = TF_Nearest;
            Texture->AddressX = TA_Clamp;
            Texture->AddressY = TA_Clamp;
            Texture->NeverStream = true;
            Texture->SRGB = true;
            Texture->PostEditChange();
            Import.QueueSave(*Texture);
        }
        return Texture;
    }

    bool ImportInterfaceFont(FAAWadNativeImport& Import, const TCHAR* Folder, const FString& Source, const TCHAR* Name, FWadTownFont& Out)
    {
        constexpr int32 Header = 5;
        constexpr int32 Glyphs = 256;
        constexpr int32 Columns = 16;
        constexpr int32 Width = Columns * 8;
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *(Source / Name + TEXT(".FNT"))) || Bytes.Num() < Header + Glyphs
            || Bytes[0] != 'F' || Bytes[1] != 'o' || Bytes[2] != 'n' || Bytes[3] != 0)
        {
            return false;
        }
        Out.Height = Bytes[4];
        if (Out.Height <= 0 || Bytes.Num() != Header + Glyphs * (Out.Height + 1))
        {
            return false;
        }
        Out.Widths.Reset();
        Out.Widths.Append(Bytes.GetData() + Header, Glyphs);
        TArray<FColor> Pixels;
        Pixels.Init(FColor(255, 255, 255, 0), Width * Columns * Out.Height);
        for (int32 Glyph = 0; Glyph < Glyphs; ++Glyph)
        {
            const int32 Start = (Glyph % Columns) * 8 + (Glyph / Columns) * Out.Height * Width;
            for (int32 Row = 0; Row < Out.Height; ++Row)
            {
                const uint8 Bits = Bytes[Header + Glyphs + Glyph * Out.Height + Row];
                for (int32 Col = 0; Col < 8; ++Col)
                {
                    Pixels[Start + Row * Width + Col].A = (Bits & (0x80 >> Col)) ? 255 : 0;
                }
            }
        }
        Out.Atlas = SaveInterfaceTexture(Import, Folder, Name, FIntPoint(Width, Columns * Out.Height), Pixels);
        return Out.Atlas != nullptr;
    }

    UNativeWadTownData* ImportTownContent(FAAWadNativeImport& Import, FOutputDevice& Output)
    {
        const FString Original = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor");
        UNativeWadTownData* Content = Import.FindOrCreateAsset<UNativeWadTownData>(TEXT("Town"), TEXT("TownData"));
        if (!Content || !LoadWadInterfacePalette(Content->Palette))
        {
            return nullptr;
        }
        Content->Quests.Reset();
        for (int32 Index = 0; ; ++Index)
        {
            const FString File = Original / FString::Printf(TEXT("Exe/MAPDESC/QUEST%d.INI"), Index);
            if (!FPaths::FileExists(File))
            {
                break;
            }
            if (!ReadTownQuest(File, Content->Quests.AddDefaulted_GetRef()))
            {
                Output.Logf(TEXT("Could not read the town quest %s."), *File);
                return nullptr;
            }
        }
        const TCHAR* Pictures[] = {
            TEXT("UI/3DUI/MAINBACK"), TEXT("UI/TOWN/TWNBACK2"), TEXT("UI/TOWN/QUEST"), TEXT("UI/TOWN/INN"),
            TEXT("UI/TOWN/STORE"), TEXT("UI/TOWN/BANK"), TEXT("UI/TOWN/LAST"), TEXT("UI/TOWN/NEXT"),
            TEXT("UI/TOWN/BLOCK"), TEXT("UI/TOWN/SB2"), TEXT("UI/COMMON/UPARROW"), TEXT("UI/COMMON/DNARROW"),
            TEXT("UI/PROMPT/PMPTBULL"), TEXT("UI/PROMPT/BULLSB"), TEXT("UI/PROMPT/DLGBACK"), TEXT("UI/PROMPT/DLGSB"),
            TEXT("UI/PROMPT/BULLUP"), TEXT("UI/PROMPT/BULLDN"), TEXT("UI/PROMPT/OK"), TEXT("UI/PROMPT/ACCEPT"), TEXT("UI/PROMPT/CANCEL"),
        };
        Content->Pictures.Reset();
        FIntPoint Size;
        TArray<FColor> Pixels;
        const auto AddPicture = [&](const FString& Name)
        {
            UTexture2D* Texture = SaveInterfaceTexture(Import, TEXT("Town"), Name, Size, Pixels);
            if (Texture)
            {
                Content->Pictures.Add(FName(*Name), Texture);
            }
            return Texture != nullptr;
        };
        for (const TCHAR* Picture : Pictures)
        {
            if (!LoadWadInterfacePicture(Picture, Size, Pixels) || !AddPicture(FPaths::GetCleanFilename(Picture)))
            {
                Output.Logf(TEXT("Could not import the town graphic %s."), Picture);
                return nullptr;
            }
        }
        const TPair<int32, const TCHAR*> QuestItems[] = {
            {957, TEXT("BOOK/BOOK.CPC")}, {958, TEXT("NECKLACE/NECKLACE.CPC")}, {954, TEXT("ORB5/ORB5.CPC")},
            {955, TEXT("ORB6/ORB6.CPC")}, {380, TEXT("SCRL8/SCRL8.CPC")}, {918, TEXT("WAND18/WAND18.CPC")}, {956, TEXT("ORB7/ORB7.CPC")},
        };
        for (const auto& [Type, Sprite] : QuestItems)
        {
            if (!LoadWadSprite(Sprite, Size, Pixels) || !AddPicture(FString::Printf(TEXT("QuestItem%d"), Type)))
            {
                Output.Logf(TEXT("Could not import the town quest item %s."), Sprite);
                return nullptr;
            }
        }
        const FString Fonts = Original / TEXT("Resources/ArtFinal/FONTS");
        if (Content->Quests.IsEmpty() || !ImportInterfaceFont(Import, TEXT("Town"), Fonts, TEXT("MEDIUM"), Content->Medium)
            || !ImportInterfaceFont(Import, TEXT("Town"), Fonts, TEXT("TINY"), Content->Tiny))
        {
            Output.Log(TEXT("Town needs the authored quest data and original fonts."));
            return nullptr;
        }
        Import.QueueSave(*Content);
        Output.Logf(TEXT("Imported %d quests, %d original town graphics and both bitmap fonts."),
            Content->Quests.Num(), Content->Pictures.Num());
        return Content;
    }
}

bool SaveNativeWadHud(FOutputDevice& Output)
{
    constexpr const TCHAR* Folder = TEXT("Graphics");
    FAAWadNativeImport Import;
    Import.AssetRoot = TEXT("/Game/WadInterface");
    auto* Data = Import.FindOrCreateAsset<UNativeWadHudData>(Folder, TEXT("ClassicHud"));
    const FString Fonts = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/FONTS");
    if (!Data || !LoadWadInterfacePalette(Data->Palette)
        || !ImportInterfaceFont(Import, Folder, Fonts, TEXT("TINY"), Data->Tiny))
    {
        return false;
    }
    TArray<FString> Pictures = {
        TEXT("UI/3DUI/MAINBACK"), TEXT("UI/3DUI/INVBAR3"), TEXT("UI/3DUI/STABAR"), TEXT("UI/3DUI/SMPORT04"),
        TEXT("UI/3DUI/RUNBUT12"), TEXT("UI/3DUI/RUNBUT13"), TEXT("UI/3DUI/RUDBUT12"), TEXT("UI/3DUI/RUDBUT13"),
        TEXT("UI/3DUI/JNLBAR"), TEXT("UI/3DUI/RWND"), TEXT("UI/3DUI/FFWD"),
        TEXT("UI/COMMON/LTARROW"), TEXT("UI/COMMON/RTARROW"), TEXT("UI/COMMON/CLOSEWIN")
    };
    for (int32 Frame = 0; Frame < 8; ++Frame)
    {
        Pictures.Add(FString::Printf(TEXT("UI/3DUI/POTION%d"), Frame));
    }
    Data->Pictures.Reset();
    FIntPoint Size;
    TArray<FColor> Pixels;
    for (const FString& Picture : Pictures)
    {
        if (!LoadWadInterfacePicture(Picture, Size, Pixels))
        {
            Output.Logf(TEXT("Could not read classic HUD art %s."), *Picture);
            return false;
        }
        const FString Name = FPaths::GetCleanFilename(Picture);
        if (Name == TEXT("STABAR"))
        {
            if (Size != FIntPoint(111, 152))
            {
                Output.Log(TEXT("The classic statistics art has unexpected dimensions."));
                return false;
            }
            // BAN00003.FRM replaces the demonstration text inside these fields.
            const struct { int32 X; int32 Y; int32 Width; } Fields[] = {
                {5, 16, 99}, {90, 26, 14}, {90, 36, 14}, {90, 46, 14}, {90, 56, 14},
                {90, 66, 14}, {90, 76, 14}, {90, 86, 14}, {31, 102, 73}, {31, 112, 73},
                {54, 128, 50}, {54, 138, 50}
            };
            for (const auto& Field : Fields)
            {
                for (int32 Row = Field.Y + 1; Row < Field.Y + 7; ++Row)
                {
                    FColor* Start = Pixels.GetData() + Row * Size.X + Field.X + 1;
                    const FColor Background = *Start;
                    for (int32 Column = 0; Column < Field.Width - 1; ++Column)
                    {
                        Start[Column] = Background;
                    }
                }
            }
        }
        UTexture2D* Texture = SaveInterfaceTexture(Import, Folder, Name, Size, Pixels);
        if (!Texture)
        {
            return false;
        }
        Data->Pictures.Add(FName(*Name), Texture);
    }
    // Source object definitions give weights in tenths of a kilogram.
    const struct { int32 Type; int32 Weight; const TCHAR* Sprite; } Items[] = {
        {NativeWadRules::IronDagger, 5, TEXT("DAGGER/DAGGER.CPC")},
        {NativeWadRules::Berry, 1, TEXT("BERI/BERI.CPC")},
        {NativeWadRules::ChurchKey, 1, TEXT("KEY2/KEY2.CPC")},
        {NativeWadRules::FeatherfallPotion, 2, TEXT("POTION17/POTION17.CPC")},
        {NativeWadRules::JugurthaJournal, 1, TEXT("BOOK/BOOK.CPC")},
        {958, 1, TEXT("NECKLACE/NECKLACE.CPC")},
    };
    Data->Items.Reset();
    Data->ItemWeights.Reset();
    for (const auto& Item : Items)
    {
        if (!LoadWadSprite(Item.Sprite, Size, Pixels))
        {
            Output.Logf(TEXT("Could not read classic HUD item art %s."), Item.Sprite);
            return false;
        }
        UTexture2D* Texture = SaveInterfaceTexture(Import, Folder, FString::Printf(TEXT("Item%d"), Item.Type), Size, Pixels);
        if (!Texture)
        {
            return false;
        }
        Data->Items.Add(Item.Type, Texture);
        Data->ItemWeights.Add(Item.Type, Item.Weight);
    }
    Import.QueueSave(*Data);
    FAssetCompilingManager::Get().FinishAllCompilation();
    for (UObject* Asset : Import.AssetsToSave)
    {
        if (!SaveAsset(*Asset, *FPackageName::GetAssetPackageExtension()))
        {
            return false;
        }
    }
    Output.Logf(TEXT("Saved classic WAD HUD with %d pictures, %d item icons and FontTiny."),
        Data->Pictures.Num(), Data->Items.Num());
    return true;
}

bool SaveNativeWadTown(FOutputDevice& Output)
{
    const FString MapPackage = NativeWadTown::Package;
    const bool bNew = !FPackageName::DoesPackageExist(MapPackage);
    if (FindPackage(nullptr, *MapPackage))
    {
        Output.Logf(TEXT("Unload the town package before creating %s."), *MapPackage);
        return false;
    }
    UWorld* World = bNew
        ? UWorld::CreateWorld(EWorldType::Editor, false, FName(*FPackageName::GetShortName(MapPackage)), CreatePackage(*MapPackage))
        : LoadObject<UWorld>(nullptr, NativeWadTown::ObjectPath);
    if (!World)
    {
        return false;
    }
    World->SetFlags(RF_Public | RF_Standalone);
    World->GetWorldSettings()->DefaultGameMode = ANativeWadTownGameMode::StaticClass();
    FAAWadNativeImport Import;
    Import.AssetRoot = MapPackage + TEXT("_Assets");
    UNativeWadTownData* Data = ImportTownContent(Import, Output);
    const TActorIterator<ANativeWadTownContent> Existing(World);
    ANativeWadTownContent* Content = Existing ? *Existing : World->SpawnActor<ANativeWadTownContent>();
    bool bSaved = Data && Content && (!bNew || World->SpawnActor<APlayerStart>());
    if (bSaved)
    {
        Content->Data = Data;
        FAssetCompilingManager::Get().FinishAllCompilation();
        for (UObject* Asset : Import.AssetsToSave)
        {
            bSaved &= SaveAsset(*Asset, *FPackageName::GetAssetPackageExtension());
        }
        bSaved = bSaved && SaveAsset(*World, *FPackageName::GetMapPackageExtension());
    }
    if (bSaved)
    {
        if (bNew)
        {
            FAssetRegistryModule::AssetCreated(World);
        }
        Output.Logf(TEXT("Saved native WAD town at %s."), *MapPackage);
    }
    if (bNew)
    {
        World->DestroyWorld(false);
    }
    return bSaved;
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
