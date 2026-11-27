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
#include "NativeWadItemSource.h"
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
        constexpr int32 Lava = 1506;
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
            Sector.bLava = false;
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
            if (Thing.Type == Water || Thing.Type == Lava || Thing.Type == Depth
                || (Thing.Type >= FlowSlow && Thing.Type <= FlowFast))
            {
                const int32 Index = Actor.FindWadSector(Position);
                if (!Actor.Sectors.IsValidIndex(Index))
                {
                    Output.Logf(TEXT("Surface marker %d at %s is outside the map."), Thing.Type, *Position.ToString());
                    return false;
                }
                FAAWadNativeSector& Sector = Actor.Sectors[Index];
                if (Thing.Type == Water || Thing.Type == Lava)
                {
                    Sector.bWater = Thing.Type == Water;
                    Sector.bLava = Thing.Type == Lava;
                    Sector.FloorDepth = WaterDepth;
                    WaterSectors += Sector.bWater;
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
        const bool bRetreatMap = MapName == TEXT("L64") || MapName == TEXT("L65") || MapName == TEXT("L34")
            || MapName == TEXT("L152") || MapName == TEXT("L153");
        const bool bUndeadMap = MapName == TEXT("L231") || MapName == TEXT("L75") || MapName == TEXT("L10")
            || MapName == TEXT("L12");
        const bool bTempleMap = MapName == TEXT("L51") || MapName == TEXT("L52") || MapName == TEXT("L41") || MapName == TEXT("L44");
        const bool bLateCampaignMap = MapName == TEXT("L74") || MapName == TEXT("L73") || MapName == TEXT("L72")
            || MapName == TEXT("L32") || MapName == TEXT("L71") || MapName == TEXT("L70")
            || MapName == TEXT("L61") || MapName == TEXT("L40");
        const bool bExtendedScenery = bRetreatMap || bUndeadMap || bTempleMap || bLateCampaignMap;
        if ((MapName == TEXT("L30") && !InstallL30Slice(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L151") && (!InstallL151Slice(World, Actor, Map, WadPath, Output)
                || !InstallL151Key(World, Actor, Map, Output)))
            || (MapName == TEXT("L301") && !InstallL301Encounter(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L531") && !InstallL531Script(Actor, Map, WadPath, Output))
            || (MapName == TEXT("L31") && !InstallL31Script(Actor, Map, WadPath, Output))
            || (MapName == TEXT("L25") && !InstallL25Script(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L20") && !InstallL20Script(Actor, Map, WadPath, Output))
            || ((bChurchQuestMap || bExtendedScenery) && !InstallScenery(World, Actor, Map, bExtendedScenery, Output))
            || (bChurchQuestMap && !InstallChurchEncounters(World, Actor, Map, WadPath, Output))
            || (MapName == TEXT("L60") && !InstallL60Script(World, Actor, Map, WadPath, Output))
            || (bRetreatMap && (!InstallRetreatScript(Actor, Map, WadPath, MapName, Output)
                || !InstallRetreatEncounters(World, Actor, Map, Output)))
            || ((MapName == TEXT("L34") || MapName == TEXT("L152") || MapName == TEXT("L153"))
                && !InstallCampaignCreatures(World, Actor, Map, WadPath, Output))
            || (bUndeadMap && !InstallUndeadQuest(World, Actor, Map, WadPath, MapName, Output))
            || (bTempleMap && !InstallTempleMap(World, Actor, Map, WadPath, MapName, Output))
            || (bLateCampaignMap && !InstallLateCampaignMap(World, Actor, Map, WadPath, MapName, Output))
            || (MapName == TEXT("L152") && !InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey6, TEXT("ElmoreKey6"),
                    TEXT("KEY6/KEY6.CPC"), TEXT("L152 Key 6"), Output, FWadLevelCheckpoint::ElmoreKeysRevision))
            || (MapName == TEXT("L153") && (!InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey3, TEXT("ElmoreKey3"),
                    TEXT("KEY3/KEY3.CPC"), TEXT("L153 Key 3"), Output)
                || !InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey8, TEXT("ElmoreKey8"),
                    TEXT("KEY8/KEY8.CPC"), TEXT("L153 Key 8"), Output)
                || !InstallQuestPickup(World, Actor, Map, NativeWadRules::OrbOfDefense, TEXT("OrbOfDefense"),
                    TEXT("ORB5/ORB5.CPC"), TEXT("L153 Orb of Defense"), Output)))
            || (MapName == TEXT("L22") && (!InstallL22Script(Actor, Map, WadPath, Output)
                || !InstallL22Journal(World, Actor, Map, Output)))
            || !InstallAuthoredItems(World, Actor, Map, Output))
        {
            return false;
        }
        Actor.LevelLinks = MoveTemp(Links);
        AWorldSettings* Settings = World.GetWorldSettings();
        Settings->Modify();
        Settings->DefaultGameMode = ANativeWadGameMode::StaticClass();
        return true;
    }

    bool InstallAuthoredItems(UWorld& World, ANativeWadLevel& Level, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        Level.InitializeTopology();
        TSet<int32> Types;
        for (const FWadThing& Thing : Map.Things)
        {
            Types.Add(uint16(Thing.Type));
        }
        TMap<int32, FWadAuthoredItem> Items;
        if (!ReadWadAuthoredItems(Types, Items, Output))
        {
            return false;
        }
        if (Items.IsEmpty())
        {
            Output.Log(TEXT("This native WAD map has no authored inventory items."));
            return true;
        }
        auto* Catalog = LoadObject<UNativeWadHudData>(nullptr, UNativeWadHudData::ObjectPath);
        auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        if (!Catalog || !Plane)
        {
            Output.Log(TEXT("Import authored WAD items after AA.CreateClassicWadHUD."));
            return false;
        }

        TGuardValue<FString> AssetRootGuard(AssetRoot, TEXT("/Game/WadInterface/Inventory"));
        TMap<FName, ANativeWadPickup*> ExistingByTag;
        TMultiMap<FIntVector, ANativeWadPickup*> LegacyByPlacement;
        for (TActorIterator<ANativeWadPickup> It(&World); It; ++It)
        {
            if (It->Tags.IsEmpty() || It->GetCheckpointRevision() == FWadLevelCheckpoint::AuthoredItemsRevision)
            {
                if (!It->Tags.IsEmpty())
                {
                    ExistingByTag.Add(It->Tags[0], *It);
                }
                continue;
            }
            const FVector Location = It->GetActorLocation() - Level.GetActorLocation();
            LegacyByPlacement.Add({FMath::RoundToInt32(Location.X), FMath::RoundToInt32(Location.Y), It->GetItemType()}, *It);
        }
        TMap<FName, ANativeWadScenery*> SceneryByTag;
        for (TActorIterator<ANativeWadScenery> It(&World); It; ++It)
        {
            if (!It->Tags.IsEmpty())
            {
                SceneryByTag.Add(It->Tags[0], *It);
            }
        }

        TMap<FString, UMaterial*> ItemMaterials;
        TMap<int32, UTexture2D*> Icons;
        for (const auto& [Type, Item] : Items)
        {
            const FString Name = Item.AssetName();
            const FString MaterialName = TEXT("M_") + Name;
            const FString ImageName = TEXT("T_") + Name;
            const FString MaterialPackage = Item.WorldMaterialPackage();
            UMaterial*& Material = ItemMaterials.FindOrAdd(Item.Picture);
            if (!Material)
            {
                Material = FPackageName::DoesPackageExist(MaterialPackage)
                    ? LoadObject<UMaterial>(nullptr, *(MaterialPackage + TEXT(".") + MaterialName))
                    : SpriteMaterial(Name, Item.Picture);
            }
            const FString ImagePackage = AssetRoot / TEXT("Things") / ImageName;
            const FString ImagePath = ImagePackage + TEXT(".") + ImageName;
            UTexture2D* Image = FindObject<UTexture2D>(nullptr, *ImagePath);
            if (!Image && FPackageName::DoesPackageExist(ImagePackage))
            {
                Image = LoadObject<UTexture2D>(nullptr, *ImagePath);
            }
            if (!Image || !Material || Image->Source.GetSizeX() <= 0 || Image->Source.GetSizeY() <= 0)
            {
                Output.Logf(TEXT("Could not build the authored art for WAD item %d."), Type);
                return false;
            }
            Icons.Add(Type, Image);
            if (!Catalog->Items.Contains(Type))
            {
                Catalog->Items.Add(Type, Image);
            }
            Catalog->ItemWeights.Add(Type, Item.Weight);
            FWadInventoryDetails& Details = Catalog->ItemDetails.Add(Type, Item.Details);
            Details.WorldMaterial = Material;
            Details.SpriteSize = FIntPoint(int32(Image->Source.GetSizeX()), int32(Image->Source.GetSizeY()));
        }
        QueueSave(*Catalog);

        int32 Imported = 0;
        int32 Existing = 0;
        int32 Skipped = 0;
        for (int32 Id = 0; Id < Map.Things.Num(); ++Id)
        {
            const FWadThing& Thing = Map.Things[Id];
            const int32 Type = uint16(Thing.Type);
            const FWadAuthoredItem* Item = Items.Find(Type);
            if (!Item)
            {
                continue;
            }
            const FIntVector Placement(Thing.X, Thing.Y, Type);
            if (ANativeWadPickup* const* Found = LegacyByPlacement.Find(Placement))
            {
                LegacyByPlacement.RemoveSingle(Placement, *Found);
                ++Existing;
                continue;
            }
            const FVector2D XY(Thing.X, Thing.Y);
            int32 Sector = Level.FindWadSector(XY);
            if (Sector == INDEX_NONE)
            {
                Sector = FindSceneryWallSector(Map, XY);
            }
            FVector2D Heights;
            if (!Level.GetSectorSurfaceHeights(Sector, Heights))
            {
                Output.Logf(TEXT("WAD item Thing %d (type %d) has no containing sector."), Id, Type);
                ++Skipped;
                continue;
            }

            const UTexture2D* Image = Icons.FindChecked(Type);
            const FIntPoint Size(Image->Source.GetSizeX(), Image->Source.GetSizeY());
            const double HalfHeight = Size.Y * 0.5;
            const double Mount = Item->bHanging ? Heights.Y - HalfHeight : HalfHeight + FMath::Max(Heights.X,
                FMath::Min(Heights.X + (Item->bRaised ? 32.0 : 0.0), Heights.Y - Size.Y));
            const FString Label = FString::Printf(TEXT("WAD Item Thing %d"), Id);
            ANativeWadPickup* Pickup = ExistingByTag.FindRef(FName(*Label));
            if (!Pickup)
            {
                Pickup = ImportSliceThing<ANativeWadPickup>(World, Map, Id, Mount, *Label);
            }
            if (!Pickup)
            {
                return false;
            }
            Pickup->Modify();
            Pickup->SetActorLocation(Level.GetActorLocation() + FVector(XY, Mount));
            ANativeWadScenery* Scenery = SceneryByTag.FindRef(FName(*FString::Printf(TEXT("WAD Scenery Thing %d"), Id)));
            if (Scenery)
            {
                Scenery->Modify();
            }
            Pickup->ConfigureAuthored(Plane, ItemMaterials.FindChecked(Item->Picture), Type, FWadLevelCheckpoint::AuthoredItemsRevision,
                Size, Item->Radius, Item->bPassable, Scenery);
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(NativeWadRules::CrossbowAmmo); ++Index)
            {
                const auto& Ammo = NativeWadRules::CrossbowAmmo[Index];
                if (Ammo.Quiver == Type || Ammo.Bolt == Type)
                {
                    const int32 Quantity = Ammo.Bolt == Type ? 1
                        : Thing.Flags > 0 ? Thing.Flags : NativeWadRules::NormalQuiverBolts;
                    Pickup->SetAuthoredAmmo(Index, Quantity);
                    break;
                }
            }
            USceneComponent* Surface = Item->bHanging ? Level.Sectors[Sector].Ceiling.Get() : Level.Sectors[Sector].Floor.Get();
            if (!Surface || !Pickup->AttachToComponent(Surface, FAttachmentTransformRules::KeepWorldTransform))
            {
                Output.Logf(TEXT("Could not mount WAD item Thing %d to sector %d."), Id, Sector);
                return false;
            }
            ++Imported;
        }
        Output.Logf(TEXT("Prepared %d authored inventory placements, kept %d existing pickups, and skipped %d without a sector."),
            Imported, Existing, Skipped);
        return true;
    }

    bool InstallRetreatScript(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const FString& MapName, FOutputDevice& Output)
    {
        constexpr int32 L64Entries[] = {0, 1, 2, 3, 4, 201};
        constexpr int32 L65Entries[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 201, 202, 203, 204};
        constexpr int32 L34Entries[] = {0, 1, 2, 3, 4, 5, 200, 202, 203};
        constexpr int32 L152Entries[] = {0, 1, 2};
        constexpr int32 L153Entries[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
            20, 21, 22, 24, 25, 26, 27, 29};
        // Some script IDs are absent from the shipped SOUNDS.RES. Import any that a complete archive provides.
        constexpr int32 UnavailableSounds[] = {5, 15, 1508, 1510};
        const TConstArrayView<int32> Entries = MapName == TEXT("L64") ? TConstArrayView<int32>(L64Entries)
            : MapName == TEXT("L65") ? TConstArrayView<int32>(L65Entries)
            : MapName == TEXT("L152") ? TConstArrayView<int32>(L152Entries)
            : MapName == TEXT("L153") ? TConstArrayView<int32>(L153Entries) : TConstArrayView<int32>(L34Entries);
        return InstallCampaignScript(Actor, Map, WadPath, MapName, Entries, UnavailableSounds, Output);
    }

    bool InstallUndeadQuest(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const FString& MapName, FOutputDevice& Output)
    {
        constexpr int32 UnavailableSounds[] = {3501};
        if (!InstallCampaignActivitiesFromMap(Actor, Map, WadPath, MapName, UnavailableSounds, Output)
            || !InstallCampaignCreatures(World, Actor, Map, WadPath, Output))
        {
            return false;
        }
        if (MapName == TEXT("L10"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey6, TEXT("Key6"),
                TEXT("KEY6/KEY6.CPC"), TEXT("L10 Key 6"), Output, 0, true);
        }
        if (MapName == TEXT("L12"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::Key1, TEXT("Key1"),
                TEXT("KEY1/KEY1.CPC"), TEXT("L12 Key 1"), Output) && InstallWizardQuestDrop(World, Actor, Map, false, Output);
        }
        return true;
    }

    bool InstallTempleMap(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map,
        const FString& WadPath, const FString& MapName, FOutputDevice& Output)
    {
        if (!InstallCampaignActivitiesFromMap(Actor, Map, WadPath, MapName, {}, Output)
            || !InstallCampaignCreatures(World, Actor, Map, WadPath, Output))
        {
            return false;
        }
        if (MapName == TEXT("L41"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::Key9, TEXT("Key9"),
                TEXT("KEY9/KEY9.CPC"), TEXT("L41 Key 9"), Output)
                && InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey6, TEXT("Key6"),
                    TEXT("KEY6/KEY6.CPC"), TEXT("L41 Key 6"), Output);
        }
        if (MapName == TEXT("L44"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey3, TEXT("Key3"),
                TEXT("KEY3/KEY3.CPC"), TEXT("L44 Key 3"), Output)
                && InstallWizardQuestDrop(World, Actor, Map, true, Output);
        }
        return true;
    }

    bool InstallCampaignCreatures(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map,
        const FString& WadPath, FOutputDevice& Output)
    {
        struct FGeneratorSource
        {
            int32 Type, X, Y, Angle, Interval, Random, MaxActive, MaxLike, Active, Count;
        };
        TArray<FGeneratorSource> Source;
        TArray<FString> Lines;
        const FString GeneratorPath = FPaths::ChangeExtension(WadPath, TEXT("GEN"));
        if (!FFileHelper::LoadFileToStringArray(Lines, *GeneratorPath))
        {
            Output.Logf(TEXT("Could not read the original generator file %s."), *GeneratorPath);
            return false;
        }
        for (const FString& Line : Lines)
        {
            FString Record = Line;
            int32 Comment;
            if (Record.FindChar(TCHAR(';'), Comment))
            {
                Record.LeftInline(Comment);
            }
            TArray<FString> Words;
            Record.ParseIntoArrayWS(Words);
            // The source only recognizes records that begin with G, so a stray comment prefix is harmless.
            if (Words.IsEmpty() || Words[0] != TEXT("G"))
            {
                continue;
            }
            int32 Fields[10];
            if (Words.Num() != 11)
            {
                Output.Logf(TEXT("Malformed generator record in %s: %s"), *GeneratorPath, *Line);
                return false;
            }
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
            {
                if (!FDefaultValueHelper::ParseInt(Words[Index + 1], Fields[Index]))
                {
                    Output.Logf(TEXT("Invalid generator value in %s: %s"), *GeneratorPath, *Line);
                    return false;
                }
            }
            Source.Add({Fields[0], Fields[1], Fields[2], Fields[3], Fields[4], Fields[5],
                Fields[6], Fields[7], Fields[8], Fields[9]});
        }
        struct FCreatureSource
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
        };
        constexpr int32 Apollo = 1010;
        constexpr int32 Boulder = 1026;
        constexpr FCreatureSource Creatures[] = {
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
                3000, 500, 400, 60, 20, 12, 315, 1000, TEXT("L41"), 4020},
            {1014, TEXT("Eastern Archer"), TEXT("EVILARCH"), TEXT("EARCAT01.CPC"), TEXT("EARCDI02.CPC"),
                TEXT("EARCDI04.CPC"), TEXT("EARCAT01.CPC"), TEXT("ARROW4/1.CPC"), 4000, 500, 500, 60, 20, 10, 315, 600},
            {1015, TEXT("Mighty Hunter"), TEXT("GIANTARC"), TEXT("GODATK01.CPC"), TEXT("GODDIE01.CPC"),
                TEXT("GODDIE04.CPC"), TEXT("GODATK01.CPC"), TEXT("MISSILE7/1.CPC"), 18000, 500, 0, 0, 30, 10, 315, 1000},
            {1023, TEXT("Griffon"), TEXT("GRIFFON"), TEXT("GRFLY001.CPC"), TEXT("GRFDI001.CPC"),
                TEXT("GRFDI008.CPC"), TEXT("GRIFA001.CPC"), nullptr, 5000, 500, 350, 75, 30, 15, 315, 0},
            {1003, TEXT("Shadow"), TEXT("SHADOW"), TEXT("SHADOW1.CPC"), TEXT("SHAD_FH1.CPC"),
                TEXT("SHAD_DEA.CPC"), TEXT("SHAD_FA1.CPC"), nullptr, 4000, 500, 150, 55, 32, 20, 315, 0},
            {1007, TEXT("Gargoyle"), TEXT("BIFF"), TEXT("BIFFWF1.CPC"), TEXT("BIFFHTF.CPC"),
                TEXT("BIFFDI7.CPC"), TEXT("BIFFATF.CPC"), nullptr, 5000, 100, 400, 65, 32, 10, 315, 0, TEXT("L153")},
            {1020, TEXT("Lich"), TEXT("LICH"), TEXT("LICHW001.CPC"), TEXT("LICHD001.CPC"),
                TEXT("LICHD008.CPC"), TEXT("LICHA001.CPC"), TEXT("MISSILE2/1.CPC"), 8000, 0, 400, 60, 20, 10, 315, 300},
            {1036, TEXT("Elmore"), TEXT("ELMORE"), TEXT("ELWLK01.CPC"), TEXT("ELDIE01.CPC"),
                TEXT("ELDIE08.CPC"), TEXT("ELATK01.CPC"), nullptr, 7000, 0, 400, 55, 20, 10, 315, 0, TEXT("L152")},
        };
        const FString MapName = FPaths::GetBaseFilename(WadPath);
        Actor.CreatureGenerators.Reset();
        UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        for (const FCreatureSource& Creature : Creatures)
        {
            if (Creature.OnlyMap && !MapName.Equals(Creature.OnlyMap, ESearchCase::IgnoreCase))
            {
                continue;
            }
            const auto HasType = [&Creature](const auto& Entry) { return Entry.Type == Creature.Type; };
            if (!Map.Things.ContainsByPredicate(HasType) && !Source.ContainsByPredicate(HasType))
            {
                continue;
            }
            const FString Root(Creature.Resource);
            FIntPoint Size;
            auto* Standing = SpriteMaterial(Root, Root / Creature.Standing, &Size);
            auto* Hurt = SpriteMaterial(Root + TEXT("_Hurt"), Root / Creature.Hurt);
            auto* Downed = SpriteMaterial(Root + TEXT("_Down"), Root / Creature.Downed);
            auto* Attacking = SpriteMaterial(Root + TEXT("_Attack"), Root / Creature.Attacking);
            const FString BoltName = Creature.Type == 1025 ? TEXT("MagicDart") : Root + TEXT("_Bolt");
            auto* Bolt = Creature.Bolt ? SpriteMaterial(BoltName, Creature.Bolt) : nullptr;
            if (!Plane || !Standing || !Hurt || !Downed || !Attacking || (Creature.Bolt && !Bolt))
            {
                return false;
            }
            FWadEncounterProfile Profile;
            Profile.Name = Creature.Name;
            Profile.SourceType = Creature.Type;
            Profile.MaxHealth = Creature.Health;
            Profile.RetreatHealth = Creature.Retreat;
            Profile.MeleeDamage = Creature.Melee;
            Profile.MeleeDelayUpdates = 10;
            Profile.MeleeRange = FMath::Max(float(Creature.MeleeRange), Creature.Radius + ANativeWadPlayer::CapsuleRadius + 1.0f);
            Profile.CollisionRadius = Creature.Radius;
            Profile.CollisionHalfHeight = Size.Y * 0.5f;
            Profile.SpriteSize = FVector2D(Size);
            Profile.UpdateTicks = 7;
            Profile.WalkSpeed = Creature.Step * TicksPerSecond / 10.0f;
            Profile.VisionHalfAngle = Creature.Vision * 0.5f;
            Profile.BoltDamage = Creature.BoltDamage;
            Profile.MinBoltRange = Creature.Bolt ? 250.0f : 0.0f;
            Profile.BoltRange = Creature.Bolt ? 1000 : 0;
            Profile.HomingMissileType = Creature.HomingType;
            using NativeWadRules::DamageFlag;
            using NativeWadRules::EProjectileDamage;
            Profile.ApplySourceRules();
            if (Creature.Type == 1003)
            {
                Profile.RegenPerUpdate = 0;
                Profile.ResistanceMask = DamageFlag(EProjectileDamage::Poison);
            }
            if (Creature.Type == 1007)
            {
                Profile.RegenPerUpdate = 5;
                Profile.bResistsFire = true;
            }
            if (Creature.Type == 1020)
            {
                Profile.RegenPerUpdate = 3;
                Profile.BoltElement = uint8(EProjectileDamage::ManaDrain);
                Profile.ResistanceMask = DamageFlag(EProjectileDamage::ManaDrain) | DamageFlag(EProjectileDamage::Poison);
            }
            if (Creature.Type == 1036)
            {
                Profile.ArmorClass = 2;
                Profile.RegenPerUpdate = 2;
                Profile.ResistanceMask = uint8(0x7f & ~DamageFlag(EProjectileDamage::Electricity));
            }
            if (Creature.Type == 1002 || Creature.Type == 1006)
            {
                Profile.bKeepsBoltDistance = true;
                Profile.bResistsFire = Creature.Type == 1006;
                Profile.RegenPerUpdate = 2;
            }
            if (Creature.Type == Apollo)
            {
                Profile.bHomingPlasma = true;
                Profile.bKeepsBoltDistance = true;
                Profile.bResistsFire = true;
                Profile.bResistsAcidAndPoison = true;
                Profile.BoltRange = 2000;
                Profile.RegenPerUpdate = 0;
            }
            if (Creature.Type == 1011)
            {
                Profile.UpdateTicks = 4;
                Profile.RegenPerUpdate = 3;
                Profile.ArmorClass = 2;
            }
            if (Creature.Type == Boulder)
            {
                const TArray<FGeneratorSource> Rail = Source.FilterByPredicate(HasType);
                if (Rail.Num() != 2)
                {
                    Output.Log(TEXT("The boulder needs two authored generator positions."));
                    return false;
                }
                Profile.RailStart = FVector2D(Rail[0].X, Rail[0].Y);
                Profile.RailEnd = FVector2D(Rail[1].X, Rail[1].Y);
            }
            int32 Placed = 0;
            const double Offset = Profile.CollisionHalfHeight + 2.0;
            const auto Configure = [&](ANativeWadGuardian& Enemy)
            {
                Enemy.Configure(Plane, Standing, Downed, Attacking, Hurt, Bolt, Profile);
            };
            for (int32 Id = 0; Id < Map.Things.Num(); ++Id)
            {
                const FWadThing& Thing = Map.Things[Id];
                if (!HasType(Thing))
                {
                    continue;
                }
                const int32 Sector = Actor.FindWadSector(FVector2D(Thing.X, Thing.Y));
                if (!Map.Sectors.IsValidIndex(Sector))
                {
                    return false;
                }
                const FString Label = FString::Printf(TEXT("WAD Creature Thing %d"), Id);
                auto* Enemy = ImportSliceThing<ANativeWadGuardian>(World, Map, Id, Actor.GetWalkingHeight(Sector) + Offset, *Label);
                if (!Enemy)
                {
                    return false;
                }
                Configure(*Enemy);
                ++Placed;
            }
            Output.Logf(TEXT("Imported %d placed actors for %s (type %d)."), Placed, Creature.Name, Creature.Type);
            for (int32 Id = 0; Id < Source.Num(); ++Id)
            {
                const FGeneratorSource& Record = Source[Id];
                if (!HasType(Record))
                {
                    continue;
                }
                const FVector2D XY(Record.X, Record.Y);
                const int32 Sector = Actor.FindWadSector(XY);
                if (Sector == INDEX_NONE || Record.Interval <= 0 || Record.Random < 0 || (Record.Count <= 0 && Record.Count != -1)
                    || Record.Count > 40 || Record.MaxActive <= 0 || Record.MaxLike <= 0)
                {
                    return false;
                }
                FWadCreatureGenerator& Generator = Actor.CreatureGenerators.Add(Id);
                Generator.Sector = Sector;
                Generator.Interval = Record.Interval;
                Generator.RandomInterval = Record.Random;
                Generator.SourceType = Creature.Type;
                Generator.bInfinite = Record.Count == -1;
                Generator.SpawnXY = XY;
                Generator.MaxActive = FMath::Min(Record.MaxActive, Record.MaxLike);
                Generator.bStartsActive = Record.Active != 0;
                const FVector Position(XY, Actor.GetWalkingHeight(Sector) + Offset);
                const FRotator Rotation(0, Record.Angle * (360.0 / 65536.0), 0);
                // Unlimited generators reuse inactive or defeated actors. Live actors are never overwritten.
                const int32 PoolSize = Generator.bInfinite ? FMath::Max(40, Generator.MaxActive + 1) : Record.Count;
                for (int32 Index = 0; Index < PoolSize; ++Index)
                {
                    const FString Label = FString::Printf(TEXT("WAD Generator %d Creature %d"), Id, Index);
                    auto* Enemy = ImportTaggedActor<ANativeWadGuardian>(World, Position, Rotation, *Label);
                    if (!Enemy)
                    {
                        return false;
                    }
                    Enemy->SetActorLocationAndRotation(Position, Rotation);
                    Configure(*Enemy);
                    Enemy->SetSpawned(false);
                    Generator.Creatures.Add(Enemy);
                }
                Output.Logf(TEXT("Imported generator %d with %d %s releases (unlimited=%d, pool=%d)."),
                    Id, Record.Count, Creature.Name, Generator.bInfinite, PoolSize);
            }
        }
        for (int32 Id = 0; Id < Source.Num(); ++Id)
        {
            const FGeneratorSource& Record = Source[Id];
            if (MapName.Equals(TEXT("L75"), ESearchCase::IgnoreCase) && Record.Type == NativeWadRules::FlyingPotion)
            {
                const FVector2D XY(Record.X, Record.Y);
                const int32 Sector = Actor.FindWadSector(XY);
                FIntPoint Size;
                UMaterialInterface* Material = SpriteMaterial(TEXT("GeneratedFlyingPotion"), TEXT("POTION05/POTION05.CPC"), &Size);
                if (!Plane || !Material || !Actor.Sectors.IsValidIndex(Sector) || !Actor.Sectors[Sector].Floor
                    || Record.Count <= 0 || Record.Count > 40 || Record.Interval <= 0)
                {
                    Output.Logf(TEXT("Could not configure the flying potion generator %d."), Id);
                    return false;
                }
                FWadCreatureGenerator& Generator = Actor.CreatureGenerators.FindOrAdd(Id);
                Generator.Sector = Sector;
                Generator.Interval = Record.Interval;
                Generator.RandomInterval = Record.Random;
                Generator.SourceType = Record.Type;
                Generator.MaxActive = FMath::Min(Record.MaxActive, Record.MaxLike);
                Generator.bStartsActive = Record.Active != 0;
                for (int32 Index = 0; Index < Record.Count; ++Index)
                {
                    const FVector Position = Actor.GetActorLocation() + FVector(XY, Actor.GetWalkingHeight(Sector) + Size.Y * 0.5);
                    const FString Label = FString::Printf(TEXT("WAD Generator %d Pickup %d"), Id, Index);
                    auto* Pickup = ImportTaggedActor<ANativeWadPickup>(World, Position, FRotator::ZeroRotator, *Label);
                    if (!Pickup)
                    {
                        return false;
                    }
                    Pickup->ConfigureAuthored(Plane, Material, Record.Type, FWadLevelCheckpoint::AuthoredItemsRevision,
                        Size, 0, true, nullptr);
                    Pickup->AttachToComponent(Actor.Sectors[Sector].Floor, FAttachmentTransformRules::KeepWorldTransform);
                    Pickup->SetAvailable(false);
                    Generator.Pickups.Add(Pickup);
                }
                Output.Logf(TEXT("Imported generator %d with %d flying potion releases."), Id, Record.Count);
            }
            if (!Actor.CreatureGenerators.Contains(Id))
            {
                Output.Logf(TEXT("Deferred original generator %d (thing type %d)."), Id, Source[Id].Type);
            }
        }
        return true;
    }

    bool InstallLateCampaignMap(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map,
        const FString& WadPath, const FString& MapName, FOutputDevice& Output)
    {
        constexpr int32 UnavailableSounds[] = {3501};
        if (!InstallCampaignActivitiesFromMap(Actor, Map, WadPath, MapName, UnavailableSounds, Output)
            || !InstallCampaignCreatures(World, Actor, Map, WadPath, Output))
        {
            return false;
        }
        if (MapName == TEXT("L72"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::WandOfDisintegration, TEXT("WandOfDisintegration"),
                TEXT("WAND18/WAND18.CPC"), TEXT("L72 Wand of Disintegration"), Output);
        }
        if (MapName == TEXT("L61"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey8, TEXT("Key8"),
                TEXT("KEY8/KEY8.CPC"), TEXT("L61 Key 8"), Output);
        }
        if (MapName == TEXT("L40"))
        {
            return InstallQuestPickup(World, Actor, Map, NativeWadRules::Key4, TEXT("Key4"),
                TEXT("KEY4/KEY4.CPC"), TEXT("L40 Key 4"), Output)
                && InstallQuestPickup(World, Actor, Map, NativeWadRules::OrbOfDestruction, TEXT("OrbOfDestruction"),
                    TEXT("ORB7/ORB7.CPC"), TEXT("L40 Orb of Destruction"), Output);
        }
        return true;
    }

    bool InstallWizardQuestDrop(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map,
        bool bElymas, FOutputDevice& Output)
    {
        // L44 uses the extended type for the red variant of the orange wizard.
        const int32 CreatureType = bElymas ? 50158 : 1032;
        const TCHAR* Name = bElymas ? TEXT("Elymas") : TEXT("Exiguus");
        const int32 DropType = bElymas ? NativeWadRules::ElymasNotes : NativeWadRules::OrbOfTheUndead;
        const int32 Id = Map.Things.IndexOfByPredicate([CreatureType](const FWadThing& Thing)
        {
            return uint16(Thing.Type) == CreatureType;
        });
        if (!Map.Things.IsValidIndex(Id))
        {
            Output.Logf(TEXT("Could not find the authored %s Thing type %d."), Name, CreatureType);
            return false;
        }
        const FWadThing& Thing = Map.Things[Id];
        const FVector2D XY(Thing.X, Thing.Y);
        const int32 Sector = Actor.FindWadSector(XY);
        UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
        TArray<uint8> ColorTable;
        if (bElymas)
        {
            const FString ColorPath = FPaths::ProjectDir() /
                TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/COLORIZE");
            const FString File = FString::Printf(TEXT("COLORT%02d"), CreatureType >> 12);
            if (!FFileHelper::LoadFileToArray(ColorTable, *(ColorPath / File)) || ColorTable.Num() != 256)
            {
                Output.Logf(TEXT("Could not load Elymas's palette table %s."), *File);
                return false;
            }
        }
        FIntPoint Size;
        UMaterial* Standing = bElymas ? SpriteMaterial(TEXT("Elymas"), TEXT("WIZEVL/WIZEVL8.CPC"), &Size, ColorTable)
            : SpriteMaterial(TEXT("WIZORNG"), TEXT("WIZORNG/OWWALK01.CPC"), &Size);
        UMaterial* Hurt = bElymas ? SpriteMaterial(TEXT("Elymas_Hurt"), TEXT("WIZEVL/WIZHRT1.CPC"), nullptr, ColorTable)
            : SpriteMaterial(TEXT("WIZORNG_Hurt"), TEXT("WIZORNG/OWDIE01.CPC"));
        UMaterial* Downed = bElymas ? SpriteMaterial(TEXT("Elymas_Down"), TEXT("WIZEVL/WIZEVL7G.CPC"), nullptr, ColorTable)
            : SpriteMaterial(TEXT("WIZORNG_Down"), TEXT("WIZORNG/OWDIE08.CPC"));
        UMaterial* Attacking = bElymas ? SpriteMaterial(TEXT("Elymas_Attack"), TEXT("WIZEVL/WIZEVL1.CPC"), nullptr, ColorTable)
            : SpriteMaterial(TEXT("WIZORNG_Attack"), TEXT("WIZORNG/OWA01.CPC"));
        UMaterial* DropImage = bElymas ? SpriteMaterial(TEXT("ElymasNotes"), TEXT("SCRL8/SCRL8.CPC"))
            : SpriteMaterial(TEXT("OrbOfTheUndead"), TEXT("ORB6/ORB6.CPC"));
        UMaterial* Bolt = bElymas ? SpriteMaterial(TEXT("HomingFireball"), TEXT("HMISS6/1.CPC")) : nullptr;
        if (Sector == INDEX_NONE || !Plane || !Standing || !Hurt || !Downed || !Attacking || !DropImage || (bElymas && !Bolt))
        {
            Output.Logf(TEXT("Could not install %s's floor position, sprites, or quest drop."), Name);
            return false;
        }
        FWadEncounterProfile Profile;
        Profile.Name = Name;
        Profile.MaxHealth = 10000;
        Profile.RetreatHealth = bElymas ? 500 : 0;
        Profile.MeleeDamage = 400;
        Profile.MeleeDelayUpdates = 10;
        Profile.MeleeRange = 60.0f;
        Profile.BoltDamage = bElymas ? 1000 : 0;
        Profile.HomingMissileType = bElymas ? 4020 : 0;
        Profile.bResistsFire = bElymas;
        Profile.BoltRange = bElymas ? 5000 : 0;
        Profile.RangedDelayUpdates = bElymas ? 10 : 50;
        Profile.RegenPerUpdate = 0;
        Profile.CollisionRadius = 25.0f;
        Profile.CollisionHalfHeight = Size.Y * 0.5f;
        Profile.SpriteSize = FVector2D(Size);
        Profile.UpdateTicks = bElymas ? 8 : 7;
        Profile.WalkSpeed = 12 * TicksPerSecond / (Profile.UpdateTicks + (Profile.UpdateTicks >> 1));
        Profile.VisionHalfAngle = bElymas ? 90.f : 157.5f;
        const double Floor = Actor.GetWalkingHeight(Sector);
        ANativeWadGuardian* Wizard = ImportSliceThing<ANativeWadGuardian>(World, Map, Id,
            Floor + Profile.CollisionHalfHeight + 2.0, *FString::Printf(TEXT("WAD Creature %s"), Name));
        ANativeWadPickup* Drop = ImportTaggedActor<ANativeWadPickup>(World, FVector(XY, Floor + 12.0),
            FRotator::ZeroRotator, bElymas ? TEXT("WAD Elymas Notes Drop") : TEXT("WAD Exiguus Orb Drop"));
        if (!Wizard || !Drop)
        {
            return false;
        }
        Drop->Configure(Plane, DropImage, DropType);
        Wizard->Configure(Plane, Standing, Downed, Attacking, Hurt, Bolt, Profile);
        Wizard->SetDefeatPickup(Drop);
        if (!bElymas)
        {
            ANativeWadGuardian* Skeleton = nullptr;
            for (TActorIterator<ANativeWadGuardian> Enemy(&World); Enemy; ++Enemy)
            {
                if (Enemy->GetSourceType() == 1025 && !Enemy->IsHidden())
                {
                    Skeleton = *Enemy;
                    break;
                }
            }
            if (!Skeleton)
            {
                Output.Log(TEXT("Exiguus needs an imported Electric Skeleton to summon."));
                return false;
            }
            constexpr int32 ExiguusSummonGenerator = 1000;
            FWadCreatureGenerator& Generator = Actor.CreatureGenerators.FindOrAdd(ExiguusSummonGenerator);
            Generator.Summoner = Wizard;
            Generator.Sector = Sector;
            Generator.Interval = 5.0f;
            Generator.MaxActive = 50;
            Generator.SourceType = 1025;
            Generator.bStartsActive = true;
            for (int32 Index = 0; Index < Generator.MaxActive; ++Index)
            {
                const FString Label = FString::Printf(TEXT("WAD Exiguus Summon %d"), Index);
                auto* Minion = ImportTaggedActor<ANativeWadGuardian>(World, Wizard->GetActorLocation(), Wizard->GetActorRotation(), *Label);
                if (!Minion)
                {
                    return false;
                }
                Minion->ConfigureFrom(*Skeleton);
                Minion->SetSpawned(false);
                Generator.Creatures.Add(Minion);
            }
        }
        Output.Logf(TEXT("Imported %s from Thing %d in sector %d with quest drop %d."), Name, Id, Sector, DropType);
        return true;
    }

    bool InstallCampaignActivitiesFromMap(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const FString& MapName, TConstArrayView<int32> UnavailableSounds, FOutputDevice& Output)
    {
        TArray<int32> Entries{0};
        for (const auto& Sector : Map.Sectors)
        {
            Entries.AddUnique(Sector.Type);
        }
        for (const auto& Line : Map.Lines)
        {
            Entries.AddUnique(Line.Special);
        }
        Entries.Sort();
        return InstallCampaignScript(Actor, Map, WadPath, MapName, Entries, UnavailableSounds, Output);
    }

    bool InstallCampaignScript(ANativeWadLevel& Actor, const FDoomWadMapData& Map, const FString& WadPath,
        const FString& MapName, TConstArrayView<int32> Entries, TConstArrayView<int32> UnavailableSounds, FOutputDevice& Output)
    {
        const FString ScriptName = TEXT("S") + MapName.Mid(1) + TEXT(".SRP");
        if (!InstallActivities(Actor, Map, WadPath, *ScriptName, Entries, Output, UnavailableSounds))
        {
            return false;
        }
        Actor.ActivityWallMaterials.Reset();
        Actor.ActivityJournalPages.Reset();
        auto& OpaqueParent = Parents[uint8(EWadWallBlend::Opaque)];
        if (!OpaqueParent)
        {
            OpaqueParent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/AAReview/M_AANativeOpaque.M_AANativeOpaque"));
        }
        const auto InstallWallTexture = [&](int32 Side, const FString& Name)
        {
            if (!OpaqueParent || !Actor.FindActivitySideBand(Side) || !Texture(Name))
            {
                Output.Logf(TEXT("Could not install texture %s for scripted sidedef %d."), *Name, Side);
                return false;
            }
            Actor.ActivityWallMaterials.Add(FName(*Name), Material(Name, EWadWallBlend::Opaque));
            return true;
        };
        for (FWadScriptActivity& Activity : Actor.ImportedActivities)
        {
            for (FWadScriptCommand& Command : Activity.Commands)
            {
                if (Command.Operation == EWadScriptOperation::SetSwitch || Command.Operation == EWadScriptOperation::ChangeSideTexture)
                {
                    Command.Operation = EWadScriptOperation::ChangeSideTexture;
                    if (!InstallWallTexture(Command.Target, Command.Text))
                    {
                        return false;
                    }
                }
                else if (Command.Operation == EWadScriptOperation::ToggleWallSwitch)
                {
                    if (!InstallWallTexture(Command.Target, Command.Text)
                        || !InstallWallTexture(Command.Target, Command.SwitchDownTexture))
                    {
                        return false;
                    }
                }
                else if (Command.Operation == EWadScriptOperation::LaunchMissile && !Actor.ScriptMissileMaterials.Contains(Command.Target))
                {
                    const auto* Homing = NativeWadRules::FindHomingMissile(Command.Target);
                    const TCHAR* Resource = Homing ? Homing->Art
                        : Command.Target == NativeWadRules::AcidBall ? TEXT("MISSILE5/1.CPC")
                        : Command.Target == 4005 ? TEXT("MISSILE6/1.CPC") : Command.Target == 4006 ? TEXT("MISSILE7/1.CPC")
                        : Command.Target == 4012 ? TEXT("MISSIL13/1.CPC") : Command.Target == 4015 ? TEXT("MISSIL16/1.CPC")
                        : Command.Target == 4016 ? TEXT("EARTHBND/1.CPC")
                        : Command.Target == NativeWadRules::PoisonCrossbowBolt ? TEXT("ARROW2/1.CPC") : nullptr;
                    Actor.ScriptMissileMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
                    UMaterialInterface* Image = Resource ? SpriteMaterial(FString::Printf(TEXT("Missile%d"), Command.Target), Resource) : nullptr;
                    if (!Actor.ScriptMissileMesh || !Image)
                    {
                        Output.Logf(TEXT("Could not install art for scripted missile %d."), Command.Target);
                        return false;
                    }
                    Actor.ScriptMissileMaterials.Add(Command.Target, Image);
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
            TEXT("NECKLACE/NECKLACE.CPC"), TEXT("L60 Necklace of Andrew"), Output, FWadLevelCheckpoint::AndrewNecklaceRevision);
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

    static int32 FindSceneryWallSector(const FDoomWadMapData& Map, const FVector2D& Point)
    {
        int32 Sector = INDEX_NONE;
        for (const FWadLineDef& Line : Map.Lines)
        {
            const int32 Front = Map.GetSectorForSide(Line.Side[0]);
            const int32 Back = Map.GetSectorForSide(Line.Side[1]);
            if ((Front == INDEX_NONE) == (Back == INDEX_NONE)
                || !Map.Vertices.IsValidIndex(Line.From) || !Map.Vertices.IsValidIndex(Line.To))
            {
                continue;
            }
            const FWadVertex& Start = Map.Vertices[Line.From];
            const FWadVertex& End = Map.Vertices[Line.To];
            const FVector2D Edge(End.X - Start.X, End.Y - Start.Y);
            const FVector2D Offset = Point - FVector2D(Start.X, Start.Y);
            const double Along = FVector2D::DotProduct(Edge, Offset);
            if (Edge.IsZero() || Along < 0 || Along > Edge.SizeSquared() || FVector2D::CrossProduct(Edge, Offset) != 0)
            {
                continue;
            }
            const int32 WallSector = Front != INDEX_NONE ? Front : Back;
            if (Sector != INDEX_NONE && Sector != WallSector)
            {
                return INDEX_NONE;
            }
            Sector = WallSector;
        }
        return Sector;
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
            const FVector2D Position(Thing.X, Thing.Y);
            int32 Sector = Actor.FindWadSector(Position);
            if (Sector == INDEX_NONE)
            {
                Sector = FindSceneryWallSector(Map, Position);
                if (Sector != INDEX_NONE)
                {
                    Output.Logf(TEXT("Scenery Thing %d keeps its authored position on the wall of sector %d."), Id, Sector);
                }
            }
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

    bool InstallL151Key(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, FOutputDevice& Output)
    {
        Actor.InitializeTopology();
        return InstallQuestPickup(World, Actor, Map, NativeWadRules::ElmoreKey5, TEXT("ElmoreKey5"),
            TEXT("KEY5/KEY5.CPC"), TEXT("L151 Key 5"), Output, FWadLevelCheckpoint::ElmoreKeysRevision);
    }

    bool InstallQuestPickup(UWorld& World, ANativeWadLevel& Actor, const FDoomWadMapData& Map, int32 Type,
        const TCHAR* Asset, const TCHAR* Picture, const TCHAR* Label, FOutputDevice& Output,
        int32 CheckpointRevision = 0, bool bFollowFloor = false)
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
        if (bFollowFloor && !Pickup->AttachToComponent(Actor.Sectors[Sector].Floor, FAttachmentTransformRules::KeepWorldTransform))
        {
            Output.Logf(TEXT("Could not attach %s to its moving floor."), Label);
            return false;
        }
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

    UMaterial* SpriteMaterial(const FString& Name, const FString& PicturePath, FIntPoint* OutSize = nullptr,
        TConstArrayView<uint8> ColorTable = {})
    {
        FIntPoint Size;
        TArray<FColor> Pixels;
        if (!LoadWadSprite(PicturePath, Size, Pixels, ColorTable))
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
        constexpr int32 LiftSound = 1514;
        const int32 UnavailableSounds[] = {LiftSound};
        const FWadSoundSample ExistingLiftSound = Actor.ActivitySounds.FindRef(LiftSound);
        if (!Map.Sectors.IsValidIndex(31) || Map.Sectors[31].Type != 1 || !Map.Things.IsValidIndex(PotionThing)
            || Map.Things[MageThing].Type != 3002 || Map.Things[PotionThing].Type != NativeWadRules::FeatherfallPotion
            || !InstallActivities(Actor, Map, WadPath, TEXT("S151.SRP"), EntryActivities, Output, UnavailableSounds))
        {
            Output.Log(TEXT("Unable to install the L151 lift and encounter."));
            return false;
        }
        if (!Actor.ActivitySounds.Contains(LiftSound) && !ExistingLiftSound.UnsignedSamples.IsEmpty())
        {
            Actor.ActivitySounds.Add(LiftSound, ExistingLiftSound);
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
            if (Footprint.bNormalizedOverlap)
            {
                Sector.RepairedBoundaryPoints.Reserve(Footprint.BoundaryEdges.Num() * 2);
                for (const FIntPoint& Edge : Footprint.BoundaryEdges)
                {
                    Sector.RepairedBoundaryPoints.Add(Footprint.Vertices[Edge.X]);
                    Sector.RepairedBoundaryPoints.Add(Footprint.Vertices[Edge.Y]);
                }
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

bool SaveNativeWadGameplay(UWorld* World, const FString& WadPath, FOutputDevice& Output, bool bPickupsOnly)
{
    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!World || World->WorldType != EWorldType::Editor || !Map || !World->GetOutermost()->GetName().StartsWith(TEXT("/Game/")))
    {
        return false;
    }
    ANativeWadLevel* Actor = FAAWadNativeImport::FindMatchingLevel(*World, *Map);
    if (!Actor)
    {
        return false;
    }
    FAAWadNativeImport Import;
    Import.AssetRoot = World->GetOutermost()->GetName() + TEXT("_Assets");
    if (bPickupsOnly)
    {
        if (!FPaths::GetBaseFilename(WadPath).Equals(TEXT("L151"), ESearchCase::IgnoreCase))
        {
            Output.Log(TEXT("The Pickups update is supported for L151."));
            return false;
        }
        if (!Import.InstallL151Key(*World, *Actor, *Map, Output))
        {
            return false;
        }
    }
    else if (!Import.InstallGameplay(*World, *Actor, *Map, WadPath, Output))
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

bool SaveNativeWadItems(const FString& MapPackage, FOutputDevice& Output)
{
    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!Map || !MapPackage.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(MapPackage)
        || !FPackageName::DoesPackageExist(MapPackage))
    {
        Output.Log(TEXT("Authored item import needs a source WAD and an existing native map."));
        return false;
    }
    const FString ObjectPath = MapPackage + TEXT(".") + FPackageName::GetShortName(MapPackage);
    UWorld* World = LoadObject<UWorld>(nullptr, *ObjectPath);
    ANativeWadLevel* Level = World ? FAAWadNativeImport::FindMatchingLevel(*World, *Map) : nullptr;
    if (!Level || (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::Inactive))
    {
        Output.Logf(TEXT("No matching native editor level in %s."), *MapPackage);
        return false;
    }
    FAAWadNativeImport Import;
    if (!Import.InstallAuthoredItems(*World, *Level, *Map, Output))
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
    Output.Logf(TEXT("Saved authored inventory items in %s."), *MapPackage);
    return true;
}

bool SaveNativeWadItemRules(FOutputDevice& Output)
{
    auto* Catalog = LoadObject<UNativeWadHudData>(nullptr, UNativeWadHudData::ObjectPath);
    if (!Catalog)
    {
        return false;
    }
    if (!ReadWadStartingEquipment(Catalog->StartingEquipment, Output))
    {
        Output.Log(TEXT("Could not import all eleven original starting inventories."));
        return false;
    }
    TSet<int32> Types;
    Types.Add(NativeWadRules::StartingWaterGourd);
    Types.Add(234); // Original electric mace, also used by the ready/unready review.
    for (const int32 Type : {352, 353, 606, 607, 616, 617, 618})
    {
        Types.Add(Type); // Source protection scrolls and amulets are not all sold in Town.
    }
    for (const int32 Type : {900, 901, 902, 903, 904, 905, 906, 907, 908, 909, 910, 911, 912, 913, 914, 915, 916, 917, 918})
    {
        Types.Add(Type); // Include source projectile wands absent from ordinary shop stock.
    }
    for (const int32 Type : NativeWadRules::StartingFoods)
    {
        Types.Add(Type);
    }
    for (const FWadStartingEquipment& Entry : Catalog->StartingEquipment)
    {
        for (const int32 Type : Entry.Items)
        {
            Types.Add(Type);
        }
    }
    for (const auto& [Type, Icon] : Catalog->Items)
    {
        Types.Add(Type);
    }
    for (int32 Type = 100; Type <= 107; ++Type)
    {
        Types.Add(Type);
    }
    TMap<int32, int32> Stock;
    for (const bool bBank : {false, true})
    {
        if (!ReadWadShopStock(bBank, Stock, Output))
        {
            return false;
        }
        for (const auto& [Type, Tier] : Stock)
        {
            Types.Add(Type);
        }
    }
    TMap<int32, FWadAuthoredItem> Items;
    if (!ReadWadAuthoredItems(Types, Items, Output))
    {
        return false;
    }
    int32 Consumables = 0;
    int32 Weapons = 0;
    int32 Armor = 0;
    FAAWadNativeImport Import;
    Import.AssetRoot = TEXT("/Game/WadInterface/Inventory");
    auto LoadOrImportProjectile = [&Import](const FString& Name, const TCHAR* Art)
    {
        const FString Package = Import.AssetRoot / TEXT("Things/M_") + Name;
        const FString Object = Package + TEXT(".M_") + Name;
        return FPackageName::DoesPackageExist(Package)
            ? LoadObject<UMaterialInterface>(nullptr, *Object) : Import.SpriteMaterial(Name, Art);
    };
    Catalog->PlasmaProjectile = LoadOrImportProjectile(TEXT("PlasmaProjectile"), TEXT("PBALL/PBALL001.CPC"));
    if (!Catalog->PlasmaProjectile)
    {
        Output.Log(TEXT("Could not load the wand's plasma projectile art."));
        return false;
    }
    for (const NativeWadRules::FWadAmmo& Ammo : NativeWadRules::CrossbowAmmo)
    {
        const FString Name = FString::Printf(TEXT("Ammo%d"), Ammo.Missile);
        UMaterialInterface* Material = LoadOrImportProjectile(Name, Ammo.Art);
        if (!Material)
        {
            Output.Logf(TEXT("Could not import crossbow missile %d."), Ammo.Missile);
            return false;
        }
        Catalog->AmmoProjectiles.Add(Ammo.Missile, Material);
    }
    for (const NativeWadRules::FWadWandProjectile& Projectile : NativeWadRules::WandProjectiles)
    {
        if (Projectile.Type == 4017)
        {
            continue;
        }
        const FString Name = FString::Printf(TEXT("Wand%d"), Projectile.Type);
        UMaterialInterface* Material = LoadOrImportProjectile(Name, Projectile.Art);
        if (!Material)
        {
            Output.Logf(TEXT("Could not import wand missile %d."), Projectile.Type);
            return false;
        }
        Catalog->WandProjectileMaterials.Add(Projectile.Type, Material);
    }
    for (const auto& [Type, Item] : Items)
    {
        if (!Catalog->Items.Contains(Type))
        {
            const FString Name = Item.AssetName();
            const FString ImagePackage = Import.AssetRoot / TEXT("Things/T_") + Name;
            const FString ImagePath = ImagePackage + TEXT(".T_") + Name;
            UTexture2D* Image = FindObject<UTexture2D>(nullptr, *ImagePath);
            if (!Image && FPackageName::DoesPackageExist(ImagePackage))
            {
                Image = LoadObject<UTexture2D>(nullptr, *ImagePath);
            }
            if (!Image && Import.SpriteMaterial(Name, Item.Picture))
            {
                Image = FindObject<UTexture2D>(nullptr, *ImagePath);
            }
            if (!Image)
            {
                Output.Logf(TEXT("Could not import the shop item icon %d."), Type);
                return false;
            }
            Catalog->Items.Add(Type, Image);
            Catalog->ItemWeights.Add(Type, Item.Weight);
        }
        FWadInventoryDetails& Details = Catalog->ItemDetails.Add(Type, Item.Details);
        const FString MaterialPackage = Item.WorldMaterialPackage();
        const FString MaterialPath = MaterialPackage + TEXT(".") + FPackageName::GetShortName(MaterialPackage);
        if (NativeWadRules::CoinValueCopper(Type) > 0 && !FindObject<UMaterialInterface>(nullptr, *MaterialPath)
            && !FPackageName::DoesPackageExist(MaterialPackage) && !Import.SpriteMaterial(Item.AssetName(), Item.Picture))
        {
            Output.Logf(TEXT("Could not import the world art for coin %d."), Type);
            return false;
        }
        if (FindObject<UMaterialInterface>(nullptr, *MaterialPath) || FPackageName::DoesPackageExist(MaterialPackage))
        {
            Details.WorldMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MaterialPath));
        }
        const UTexture2D* Icon = Catalog->Items.FindRef(Type);
        if (Icon)
        {
            Details.SpriteSize = FIntPoint(int32(Icon->Source.GetSizeX()), int32(Icon->Source.GetSizeY()));
        }
        Consumables += Details.bUsable;
        Weapons += Details.HitFrame > 0;
        Armor += Details.ArmorSlot != INDEX_NONE && Details.ArmorValue > 0;
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    for (UObject* Asset : Import.AssetsToSave)
    {
        if (!SaveAsset(*Asset, *FPackageName::GetAssetPackageExtension()))
        {
            return false;
        }
    }
    Catalog->MarkPackageDirty();
    const FString SourceMaps = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe");
    TArray<FString> MapInfoFiles;
    IFileManager::Get().FindFiles(MapInfoFiles, *(SourceMaps / TEXT("L*.I")), true, false);
    Catalog->MusicByMap.Reset();
    for (const FString& File : MapInfoFiles)
    {
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *(SourceMaps / File)))
        {
            continue;
        }
        TArray<FString> Fields;
        Contents.ParseIntoArrayWS(Fields);
        if (Fields.IsEmpty())
        {
            continue;
        }
        const FString Track = FPaths::GetBaseFilename(Fields[0]).ToUpper();
        const FString Package = TEXT("/Game/WadInterface/Music/") + Track;
        if (FPackageName::DoesPackageExist(Package))
        {
            Catalog->MusicByMap.Add(FName(*FPaths::GetBaseFilename(File)),
                TSoftObjectPtr<USoundWave>(FSoftObjectPath(Package + TEXT(".") + Track)));
        }
    }
    Output.Logf(TEXT("Identified authored music for %d levels."), Catalog->MusicByMap.Num());
    if (!SaveAsset(*Catalog, *FPackageName::GetAssetPackageExtension()))
    {
        return false;
    }
    Output.Logf(TEXT("Saved WAD inventory rules for %d types, with %d consumables, %d melee weapons, and %d armor entries."),
        Items.Num(), Consumables, Weapons, Armor);
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
            TEXT("UI/STORE/STORBACK"), TEXT("UI/BANK/BANKBACK"), TEXT("UI/COMMON/LTARROW"), TEXT("UI/COMMON/RTARROW"),
            TEXT("UI/INN/INNBACK"), TEXT("UI/INN/ROOMBTN1"), TEXT("UI/INN/ROOMBTN2"), TEXT("UI/INN/ROOMBTN3"), TEXT("UI/INN/ROOMBTN4"),
            TEXT("UI/COMMON/CLOSEWIN"), TEXT("UI/3DUI/CLOSEDBA"), TEXT("UI/3DUI/EQPBAR"),
            TEXT("UI/3DUI/2EQBUTN"), TEXT("UI/3DUI/2INVBUTN"),
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
        if (!ReadWadShopStock(false, Content->StoreStock, Output) || !ReadWadShopStock(true, Content->BankStock, Output))
        {
            return nullptr;
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
        || !ImportInterfaceFont(Import, Folder, Fonts, TEXT("TINY"), Data->Tiny)
        || !ImportInterfaceFont(Import, Folder, Fonts, TEXT("MEDIUM"), Data->Medium))
    {
        return false;
    }
    TArray<FString> Pictures = {
        TEXT("UI/3DUI/MAINBACK"), TEXT("UI/3DUI/INVBAR3"), TEXT("UI/3DUI/STABAR"),
        TEXT("UI/3DUI/EQPBAR"), TEXT("UI/3DUI/2EQBUTN"), TEXT("UI/3DUI/2INVBUTN"),
        TEXT("UI/3DUI/JNLBAR"), TEXT("UI/3DUI/RWND"), TEXT("UI/3DUI/FFWD"),
        TEXT("UI/3DUI/AMMBAR"), TEXT("UI/3DUI/AMMSELUP"), TEXT("UI/3DUI/AMMSELDN"), TEXT("UI/3DUI/AMMGET"),
        TEXT("UI/3DUI/MONBAR2"), TEXT("UI/3DUI/PGET1"), TEXT("UI/3DUI/PGET5"),
        TEXT("UI/3DUI/GGET1"), TEXT("UI/3DUI/GGET5"), TEXT("UI/3DUI/SGET1"),
        TEXT("UI/3DUI/SGET5"), TEXT("UI/3DUI/CGET1"), TEXT("UI/3DUI/CGET5"),
        TEXT("UI/COMMON/LTARROW"), TEXT("UI/COMMON/RTARROW"), TEXT("UI/COMMON/CLOSEWIN")
    };
    for (int32 Frame = 0; Frame < 8; ++Frame)
    {
        Pictures.Add(FString::Printf(TEXT("UI/3DUI/POTION%d"), Frame));
    }
    for (int32 Class = 0; Class < UE_ARRAY_COUNT(NativeWadRules::CharacterClasses); ++Class)
    {
        Pictures.Add(FString::Printf(TEXT("UI/3DUI/SMPORT%02d"), Class));
    }
    for (int32 Picture = 0; Picture < 18; ++Picture)
    {
        Pictures.Add(FString::Printf(TEXT("UI/3DUI/RUNBUT%02d"), Picture));
        Pictures.Add(FString::Printf(TEXT("UI/3DUI/RUDBUT%02d"), Picture));
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
        if (Name == TEXT("STABAR") || Name == TEXT("EQPBAR") || Name == TEXT("MONBAR2"))
        {
            if (Size != FIntPoint(111, 152))
            {
                Output.Logf(TEXT("The classic HUD art %s has unexpected dimensions."), *Name);
                return false;
            }
            // The original forms replace the values printed on their artwork.
            struct FField { int32 X; int32 Y; int32 Width; };
            constexpr FField StatisticsFields[] = {
                {5, 16, 99}, {90, 26, 14}, {90, 36, 14}, {90, 46, 14}, {90, 56, 14},
                {90, 66, 14}, {90, 76, 14}, {90, 86, 14}, {31, 102, 73}, {31, 112, 73},
                {54, 128, 50}, {54, 138, 50}
            };
            constexpr FField EquipmentFields[] = {
                {32, 99, 17}, {32, 108, 17}, {32, 117, 17}, {32, 126, 17},
                {85, 99, 17}, {85, 108, 17}, {85, 117, 17}, {85, 126, 17},
            };
            constexpr FField FinanceFields[] = {
                {75, 31, 28}, {75, 43, 28}, {75, 55, 28}, {75, 67, 28},
                {51, 100, 52}, {51, 112, 52}, {51, 124, 52}, {51, 136, 52},
            };
            const TConstArrayView<FField> Fields = Name == TEXT("STABAR")
                ? MakeArrayView(StatisticsFields) : Name == TEXT("EQPBAR") ? MakeArrayView(EquipmentFields) : MakeArrayView(FinanceFields);
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
        {NativeWadRules::OrbOfDefense, 1, TEXT("ORB5/ORB5.CPC")},
        {NativeWadRules::OrbOfTheUndead, 1, TEXT("ORB6/ORB6.CPC")},
        {NativeWadRules::ElymasNotes, 1, TEXT("SCRL8/SCRL8.CPC")},
        {NativeWadRules::WandOfDisintegration, 5, TEXT("WAND18/WAND18.CPC")},
        {NativeWadRules::OrbOfDestruction, 1, TEXT("ORB7/ORB7.CPC")},
        {NativeWadRules::Key1, 1, TEXT("KEY1/KEY1.CPC")},
        {NativeWadRules::Key4, 1, TEXT("KEY4/KEY4.CPC")},
        {NativeWadRules::Key9, 1, TEXT("KEY9/KEY9.CPC")},
        {NativeWadRules::ElmoreKey3, 1, TEXT("KEY3/KEY3.CPC")},
        {NativeWadRules::ElmoreKey5, 1, TEXT("KEY5/KEY5.CPC")},
        {NativeWadRules::ElmoreKey6, 1, TEXT("KEY6/KEY6.CPC")},
        {NativeWadRules::ElmoreKey8, 1, TEXT("KEY8/KEY8.CPC")},
    };
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
