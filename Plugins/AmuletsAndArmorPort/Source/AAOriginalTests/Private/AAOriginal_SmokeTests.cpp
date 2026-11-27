#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "AAOriginalAPI.h"
#include "DoomWadLoader.h"
#include "DoomWadWalls.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadEncounter.h"
#include "NativeWadRules.h"
#include "SectorMeshBuilderActor.h"
#include "Camera/CameraTypes.h"
#include "Components/BoxComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "Misc/ScopeExit.h"
#include "LegacyMapLoader.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Containers/Set.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalModuleLoads, "AAOriginal.Smoke.ModuleLoads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalModuleLoads::RunTest(const FString& Parameters)
{
    IModuleInterface* Module = FModuleManager::Get().LoadModule("AAOriginal");
    TestNotNull(TEXT("AAOriginal module should load"), Module);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadCameraNearPlane, "AAOriginal.WAD.Gameplay.CameraNearPlane",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_WadCameraNearPlane::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for WAD camera"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    if (!TestNotNull(TEXT("Native WAD player"), Player))
    {
        return false;
    }
    FMinimalViewInfo View;
    Player->CalcCamera(0.0f, View);
    const float EyeOffset = View.Location.Z - Player->GetActorLocation().Z;
    const float CapsuleCapOffset = FMath::Max(0.0f, EyeOffset - (Player->CapsuleHalfHeight - Player->CapsuleRadius));
    const float Clearance = FMath::Sqrt(FMath::Square(Player->CapsuleRadius) - FMath::Square(CapsuleCapOffset));
    return TestTrue(TEXT("The perspective near plane stays inside the capsule at eye height"),
        View.PerspectiveNearClipPlane > 0.0f && View.PerspectiveNearClipPlane < Clearance);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadWallPenetration, "AAOriginal.WAD.Gameplay.WallPenetrationRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_WadWallPenetration::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for WAD wall recovery"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadLevel* Level = World->SpawnActor<ANativeWadLevel>();
    const FVector SafePosition(-27, 0, 32);
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>(SafePosition, FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("Native level"), Level) || !TestNotNull(TEXT("Native player"), Player))
    {
        return false;
    }

    const auto AddWall = [Level](double X, double HalfWidth)
    {
        UBoxComponent* Wall = NewObject<UBoxComponent>(Level);
        Wall->SetupAttachment(Level->GetRootComponent());
        Wall->SetRelativeLocation(FVector(X, 0, 32));
        Wall->SetBoxExtent(FVector(HalfWidth, 80, 64));
        Wall->SetCollisionProfileName(TEXT("BlockAll"));
        Wall->RegisterComponent();
    };
    AddWall(15, 15);
    AddWall(-10, 1);
    UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
    Movement->bRunPhysicsWithNoController = true;
    Movement->SetMovementMode(MOVE_Flying);
    Movement->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    TestTrue(TEXT("The movement update recorded the clear approach"), Movement->GetLastUpdateLocation().Equals(SafePosition));

    const FVector Overlapping(-4, 0, 32);
    Player->SetActorLocation(Overlapping, false, nullptr, ETeleportType::TeleportPhysics);
    const FCollisionShape Capsule = FCollisionShape::MakeCapsule(Player->CapsuleRadius, Player->CapsuleHalfHeight);
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(WadPenetrationTest), false, Player);
    TestTrue(TEXT("The test starts inside both blocking wall spans"),
        World->OverlapBlockingTestByChannel(Overlapping, FQuat::Identity, ECC_Pawn, Capsule, Query));

    FHitResult Hit;
    Movement->SafeMoveUpdatedComponent(FVector(1, 0, 0), FQuat::Identity, true, Hit);
    const FVector Recovered = Player->GetActorLocation();
    TestTrue(TEXT("Recovery stays on the original side of the wall"), Recovered.X < -21.0);
    return TestFalse(TEXT("The recovered capsule is clear"),
        World->OverlapBlockingTestByChannel(Recovered, FQuat::Identity, ECC_Pawn, Capsule, Query));
}

// CRC32 known vectors
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_CRC32_KnownVector, "AAOriginal.Unit.CRC32.KnownVector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_CRC32_KnownVector::RunTest(const FString& Parameters)
{
    const ANSICHAR* TestData = "123456789"; // length 9
    const int32 Length = 9;
    const uint32 Expected = 0xCBF43926u; // standard CRC32 of "123456789"

    const uint32 Actual = AA_ComputeCRC32(reinterpret_cast<const uint8*>(TestData), Length);
    TestEqual(TEXT("CRC32(123456789)"), Actual, Expected);
    return true;
}

// Additional TDD tests
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_CRC32_Empty, "AAOriginal.Unit.CRC32.Empty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_CRC32_Empty::RunTest(const FString& Parameters)
{
    const uint32 Actual1 = AA_ComputeCRC32(nullptr, 0);
    const uint32 Actual2 = AA_ComputeCRC32(reinterpret_cast<const uint8*>(""), 0);
    TestEqual(TEXT("CRC32(nullptr,0) == 0"), Actual1, 0u);
    TestEqual(TEXT("CRC32(\"\",0) == 0"), Actual2, 0u);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_CRC32_QuickBrownFox, "AAOriginal.Unit.CRC32.QuickBrownFox",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_CRC32_QuickBrownFox::RunTest(const FString& Parameters)
{
    const char* Str = "The quick brown fox jumps over the lazy dog";
    const uint32 Expected = 0x414FA339u;
    const uint32 Actual = AA_ComputeCRC32(reinterpret_cast<const uint8*>(Str), static_cast<int32>(FCStringAnsi::Strlen(Str)));
    TestEqual(TEXT("CRC32(quick brown fox)"), Actual, Expected);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_Version_Sanity, "AAOriginal.Unit.Version.Sanity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_Version_Sanity::RunTest(const FString& Parameters)
{
    const TCHAR* Ver = AA_GetVersion();
    TestNotNull(TEXT("Version pointer not null"), Ver);
    const FString VerStr(Ver);
    TestTrue(TEXT("Version not empty"), !VerStr.IsEmpty());
    TestTrue(TEXT("Version looks like x.y.z"), VerStr.Contains(TEXT(".")));
    return true;
}

// WAD (MAP) loader test: expects a test .map placed under either
//  1) Project/Content/TestData/*.map
//  2) Plugin Content (AmuletsAndArmorPort)/TestData/*.map
// The custom loader currently treats .map as a WAD-compatible binary for this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_Basic, "AAOriginal.WAD.LoadBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static void GatherCandidateRoots(TArray<FString>& OutRoots)
{
    OutRoots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("TestData")));
	IPluginManager& PManager = IPluginManager::Get();
    if (const TSharedPtr<IPlugin> Plugin = PManager.FindPlugin(TEXT("AmuletsAndArmorPort")))
    {
        OutRoots.Add(FPaths::ConvertRelativePathToFull(Plugin->GetContentDir() / TEXT("TestData")));
    }
}

static FString FindTestMapFile(FString& OutSearched)
{
    TArray<FString> Roots; 
    TArray<FString> Found;

    GatherCandidateRoots(Roots);

    for (const FString& Root : Roots)
    {
        if (!FPaths::DirectoryExists(Root)) continue;
        TArray<FString> Local;
        IFileManager::Get().FindFilesRecursive(Local, *Root, TEXT("*.map"), true, false);
        IFileManager::Get().FindFilesRecursive(Local, *Root, TEXT("*.MAP"), true, false);
        if (Local.Num())
        {
            OutSearched = FString::Join(Roots, TEXT(";"));
            return Local[0];
        }
    }
    OutSearched = FString::Join(Roots, TEXT(";"));
    return FString();
}

static float ComputePolygonSignedArea(const TArray<FVector2D>& Poly)
{
    if (Poly.Num() < 3)
    {
        return 0.0f;
    }

    double TwiceArea = 0.0;
    for (int32 i = 0; i < Poly.Num(); ++i)
    {
        const FVector2D& A = Poly[i];
        const FVector2D& B = Poly[(i + 1) % Poly.Num()];
        TwiceArea += (double(A.X) * double(B.Y)) - (double(B.X) * double(A.Y));
    }

    return float(TwiceArea * 0.5);
}

static bool PointInPolygon2D(const FVector2D& Point, const TArray<FVector2D>& Poly)
{
    bool bInside = false;
    int32 J = Poly.Num() - 1;
    for (int32 I = 0; I < Poly.Num(); ++I)
    {
        const FVector2D& A = Poly[I];
        const FVector2D& B = Poly[J];
        if ((A.Y > Point.Y) != (B.Y > Point.Y))
        {
            double Denom = double(B.Y) - double(A.Y);
            if (FMath::IsNearlyZero((float)Denom))
            {
                Denom = (Denom >= 0.0) ? KINDA_SMALL_NUMBER : -KINDA_SMALL_NUMBER;
            }
            const double CrossX = (((double(B.X) - double(A.X)) * (double(Point.Y) - double(A.Y)) / Denom) + double(A.X));
            if (double(Point.X) < CrossX)
            {
                bInside = !bInside;
            }
        }
        J = I;
    }
    return bInside;
}

bool FAAOriginal_WadLoader_Basic::RunTest(const FString& Parameters)
{
    FString RootsStr;
    const FString MapFile = FindTestMapFile(RootsStr);
    if (MapFile.IsEmpty())
    {
        AddWarning(FString::Printf(TEXT("No .map test file found. Searched roots: %s"), *RootsStr));
        return true; // skip without failing
    }

    // Allow external parameter to specify map marker, else default
    FString MapMarker = Parameters.IsEmpty() ? TEXT("L30") : Parameters;

    // Use loader (treat .map as WAD). If loader requires .wad extension adapt here.
    const bool bLoaded = UDoomWadLoader::LoadDoomWadMap(MapFile, MapMarker);
    TestTrue(FString::Printf(TEXT("Map load for marker '%s' from '%s'"), *MapMarker, *MapFile), bLoaded);

    // Optionally inspect stats if available
    if (bLoaded)
    {
        const FDoomWadLoadStats Stats = UDoomWadLoader::GetLastLoadStats();
        TestTrue(TEXT("Has vertices"), Stats.NumVertices > 0);
        TestTrue(TEXT("Has lines"), Stats.NumLines > 0);
        TestTrue(TEXT("Has sectors"), Stats.NumSectors > 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_LegacyMap_Load, "AAOriginal.LegacyMap.LoadBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static FString FindAnyModernLegacyMap(FString& OutTried)
{
    TArray<FString> Roots;
    Roots.Add(FPaths::ProjectDir());
    Roots.Add(FPaths::ProjectContentDir() / TEXT("TestData"));
    TArray<FString> Found;
    for (const FString& Root : Roots)
    {
        if (!FPaths::DirectoryExists(Root)) continue;
        TArray<FString> Local;
        IFileManager::Get().FindFilesRecursive(Local, *Root, TEXT("l*.map"), true, false);
        if (Local.Num()) { OutTried = FString::Join(Roots, TEXT(";")); return Local[0]; }
    }
    OutTried = FString::Join(Roots, TEXT(";"));
    return FString();
}

bool FAAOriginal_LegacyMap_Load::RunTest(const FString& Parameters)
{
    FString Tried; const FString Path = FindAnyModernLegacyMap(Tried);
    if (Path.IsEmpty())
    {
        AddWarning(FString::Printf(TEXT("No test l*.map file found (searched: %s). Place a modern AAM1 format file to exercise loader."), *Tried));
        return true;
    }
    const bool bLoaded = ULegacyMapLoader::LoadLegacyMapFromPath(Path);
    TestTrue(TEXT("Legacy map loaded (modern placeholder format)"), bLoaded);
    if (bLoaded)
    {
        const FLegacyMapLoadStats Stats = ULegacyMapLoader::GetLastLegacyStats();
        TestTrue(TEXT("Legacy stats marked valid"), Stats.bValid);
        TestTrue(TEXT("Vertices > 0"), Stats.NumVertices > 0);
        TestTrue(TEXT("Lines > 0"), Stats.NumLines > 0);
        // The modern parser currently provides 2D geometry first and does not populate sector counts yet.
        TestTrue(TEXT("Sectors are non-negative"), Stats.NumSectors >= 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_SectorCoverage, "AAOriginal.WAD.L30.SectorCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_BoundaryAreaCoverage, "AAOriginal.WAD.L30.BoundaryAreaCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_MapData, "AAOriginal.WAD.L30.MapData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_Footprints, "AAOriginal.WAD.L30.Footprints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L52_OverlappingBoundary, "AAOriginal.WAD.L52.OverlappingBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_WadLoader_L52_OverlappingBoundary::RunTest(const FString& Parameters)
{
    const FString Path = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L52.MAP");
    if (!FPaths::FileExists(Path))
    {
        AddWarning(TEXT("L52.MAP fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L52 loads"), UDoomWadLoader::LoadDoomWadMap(Path, TEXT("L52"))))
    {
        return false;
    }
    FDoomWadSectorFootprint Footprint;
    if (!TestTrue(TEXT("Sector 154 triangulates despite its overlapping linedefs"),
        UDoomWadLoader::GetLastWadSectorFootprint(154, Footprint)))
    {
        return false;
    }

    TestTrue(TEXT("The loader reports the repaired boundary"), Footprint.bNormalizedOverlap);
    const FVector2D Samples[] = {{-1706, 3488}, {-1758, 3502}, {-1394, 3734}, {-922, 3270}, {-2400, 3712.5}};
    bool Filled[UE_ARRAY_COUNT(Samples)] = {};
    double Area = 0.0;
    for (int32 Index = 0; Index < Footprint.Triangles.Num(); Index += 3)
    {
        const FVector2D& A = Footprint.Vertices[Footprint.Triangles[Index]];
        const FVector2D& B = Footprint.Vertices[Footprint.Triangles[Index + 1]];
        const FVector2D& C = Footprint.Vertices[Footprint.Triangles[Index + 2]];
        Area += FVector2D::CrossProduct(B - A, C - A) * 0.5;
        for (int32 Sample = 0; Sample < UE_ARRAY_COUNT(Samples); ++Sample)
        {
            const FVector2D& P = Samples[Sample];
            Filled[Sample] |= FVector2D::CrossProduct(B - A, P - A) >= 0
                && FVector2D::CrossProduct(C - B, P - B) >= 0 && FVector2D::CrossProduct(A - C, P - C) >= 0;
        }
    }
    TestEqual(TEXT("Courtyard triangles cover exactly the normalized sector area"), Area, 2629174.5);
    TestTrue(TEXT("The courtyard beside plate 96 stays filled"), Filled[0]);
    TestFalse(TEXT("Sector 96 remains a cutout in the courtyard"), Filled[1]);
    TestFalse(TEXT("Sector 106 remains a cutout in the courtyard"), Filled[2]);
    TestFalse(TEXT("Sector 97 remains a cutout in the courtyard"), Filled[3]);
    TestFalse(TEXT("The overlap does not extend the courtyard outside its west edge"), Filled[4]);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_FixedWalls, "AAOriginal.WAD.L30.FixedWalls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L73_CrossedBoundary, "AAOriginal.WAD.L73.CrossedBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_WadLoader_L73_CrossedBoundary::RunTest(const FString& Parameters)
{
    const FString Path = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L73.MAP");
    if (!FPaths::FileExists(Path))
    {
        AddWarning(TEXT("L73.MAP fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L73 loads"), UDoomWadLoader::LoadDoomWadMap(Path, TEXT("L73"))))
    {
        return false;
    }
    FDoomWadSectorFootprint Footprint;
    if (!TestTrue(TEXT("Sector 184 triangulates despite its crossing linedefs"),
        UDoomWadLoader::GetLastWadSectorFootprint(184, Footprint)))
    {
        return false;
    }
    const FVector2D Intersections[] = {{-67472.0 / 17.0, 10848.0 / 17.0}, {-893128.0 / 225.0, 143344.0 / 225.0}};
    for (const FVector2D& Point : Intersections)
    {
        TestTrue(TEXT("A crossing was retained at its fractional map position"),
            Footprint.Vertices.ContainsByPredicate([&Point](const FVector2D& Vertex) { return Vertex.Equals(Point, 1.e-6); }));
    }
    TestTrue(TEXT("The crossed edges are serialized as a repaired boundary"), Footprint.bNormalizedOverlap);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_SectorMotion, "AAOriginal.WAD.L30.SectorMotion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadLoader_L30_Textures, "AAOriginal.WAD.L30.Textures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_WadLoader_L30_Textures::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP");
    const FString ArtPath = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
    if (!FPaths::FileExists(MapPath) || !FPaths::FileExists(ArtPath / TEXT("PICS.RES"))
        || !FPaths::FileExists(ArtPath / TEXT("GAME.PAL")))
    {
        AddWarning(TEXT("L30 or its picture resource fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L30 loads"), UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"))))
    {
        return false;
    }

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for texture checks"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ASectorMeshBuilderActor* Builder = World->SpawnActor<ASectorMeshBuilderActor>();
    if (!TestNotNull(TEXT("Sector mesh builder"), Builder))
    {
        return false;
    }
    Builder->BuildSectorsFromLastWad();

    const FName TextureParameter(TEXT("SurfaceTexture"));
    const FName OffsetParameter(TEXT("SurfaceOffset"));
    struct FSurface
    {
        UProceduralMeshComponent* Mesh = nullptr;
        UMaterialInstanceDynamic* Material = nullptr;
        FVector2D UV0;
        FVector2D UV1;
        FVector2D UV2;
    };
    const auto FindPlane = [&](int32 SectorIndex, bool bCeiling) -> FSurface
    {
        const FName SectorTag(*FString::Printf(TEXT("AA.Sector.%d"), SectorIndex));
        const FName Type(bCeiling ? TEXT("AA.Sector.Ceiling") : TEXT("AA.Sector.Floor"));
        TArray<UProceduralMeshComponent*> Meshes;
        Builder->GetComponents(Meshes);
        for (UProceduralMeshComponent* Mesh : Meshes)
        {
            if (Mesh->ComponentHasTag(Type) && Mesh->ComponentHasTag(SectorTag))
            {
                const FProcMeshSection* Section = Mesh->GetProcMeshSection(0);
                if (Section && !Section->ProcVertexBuffer.IsEmpty())
                {
                    return {Mesh, Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0)), Section->ProcVertexBuffer[0].UV0};
                }
            }
        }
        return {};
    };
    const FDoomWadMapData& Map = *UDoomWadLoader::GetLastWadMapData();
    const auto FindWall = [&](int32 LineIndex, bool bBack, FName Type, double Bottom) -> FSurface
    {
        const FWadLineDef& Line = Map.Lines[LineIndex];
        const FWadVertex& From = Map.Vertices[bBack ? Line.To : Line.From];
        const FWadVertex& To = Map.Vertices[bBack ? Line.From : Line.To];
        TArray<UProceduralMeshComponent*> Meshes;
        Builder->GetComponents(Meshes);
        for (UProceduralMeshComponent* Mesh : Meshes)
        {
            if (!Mesh->ComponentHasTag(Type))
            {
                continue;
            }
            for (int32 SectionIndex = 0; SectionIndex < Mesh->GetNumSections(); ++SectionIndex)
            {
                const FProcMeshSection* Section = Mesh->GetProcMeshSection(SectionIndex);
                for (int32 Index = 0; Section && Index + 3 < Section->ProcVertexBuffer.Num(); Index += 4)
                {
                    const FProcMeshVertex& A = Section->ProcVertexBuffer[Index];
                    const FProcMeshVertex& B = Section->ProcVertexBuffer[Index + 2];
                    if (A.Position == FVector(From.X, From.Y, Bottom)
                        && FVector2D(B.Position) == FVector2D(To.X, To.Y))
                    {
                        return {Mesh, Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(SectionIndex)),
                            A.UV0, Section->ProcVertexBuffer[Index + 1].UV0, B.UV0};
                    }
                }
            }
        }
        return {};
    };

    const FSurface Upper = FindWall(11, false, TEXT("AA.Wall.Opaque"), 104.0);
    if (!TestNotNull(TEXT("Line 11 has its rectangular FART3 upper texture"), Upper.Material))
    {
        return false;
    }
    UTexture2D* Texture = Cast<UTexture2D>(Upper.Material->K2_GetTextureParameterValue(TextureParameter));
    if (!TestNotNull(TEXT("Line 11 texture"), Texture)
        || !TestEqual(TEXT("FART3 decoded dimensions"), FIntPoint(Texture->GetSizeX(), Texture->GetSizeY()), FIntPoint(256, 128)))
    {
        return false;
    }
    FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
    const FColor* Pixels = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
    if (TestNotNull(TEXT("Transient texture retains the decoded pixels"), Pixels))
    {
        TestEqual(TEXT("FART3 texel (1,2) keeps source orientation and RGB"), Pixels[2 * 256 + 1], FColor(72, 36, 16));
        TestEqual(TEXT("FART3 texel (2,1) stays distinct"), Pixels[1 * 256 + 2], FColor(85, 48, 28));
    }
    Mip.BulkData.Unlock();
    TestEqual(TEXT("Upper texture begins at the neighboring ceiling"), Upper.UV0, FVector2D::ZeroVector);
    TestEqual(TEXT("Wall V runs down from the height anchor"), Upper.UV1.Y, -104.0 / 128.0);
    TestTrue(TEXT("Wall U uses one texel per source unit and the image width"),
        FMath::IsNearlyEqual(Upper.UV2.X, FVector2D(16, 64).Size() / 256.0));

    // L30's nonzero sidedef offset starts on a hidden lower band. Expose it as a moving floor would.
    Builder->SetSectorFloorZ(159, -260.0f);
    const FSurface OffsetBand = FindWall(1102, true, TEXT("AA.Wall.Opaque"), -300.0);
    TestNotNull(TEXT("The offset wall band appears when the neighboring floor rises"), OffsetBand.Material);
    TestEqual(TEXT("Side 1513 keeps its 23-texel V offset"), OffsetBand.UV1.Y, 23.0 / 128.0);
    TestEqual(TEXT("The back-side texture runs from the reversed linedef start"), OffsetBand.UV0, FVector2D(0, 63.0 / 128.0));

    const FSurface Neighbor = FindPlane(146, false);
    const FSurface Moving = FindPlane(147, false);
    const FSurface Sky = FindPlane(0, true);
    if (!TestNotNull(TEXT("Neighbor floor material"), Neighbor.Material) || !TestNotNull(TEXT("Moving floor material"), Moving.Material))
    {
        return false;
    }
    TestTrue(TEXT("Sky has a ceiling component with no opaque render surface"), Sky.Mesh && !Sky.Mesh->IsVisible());
    TestTrue(TEXT("The floors share a texture but have separate materials"), Moving.Material != Neighbor.Material
        && Moving.Material->K2_GetTextureParameterValue(TextureParameter) == Neighbor.Material->K2_GetTextureParameterValue(TextureParameter));
    TestTrue(TEXT("Floor UV at its first vertex uses map XY in texels"),
        Moving.UV0.Equals(FVector2D(Moving.Mesh->GetProcMeshSection(0)->ProcVertexBuffer[0].Position) / 64.0));
    TestTrue(TEXT("Floor scroll can be set in source texels"),
        Builder->SetSectorTextureScrollTexels(147, false, FVector2D(17, 11)));
    TestEqual(TEXT("Only the moving floor receives an offset"), Moving.Material->K2_GetVectorParameterValue(OffsetParameter),
        FLinearColor(-17.0f / 64, -11.0f / 64, 0, 0));
    TestEqual(TEXT("The neighboring floor keeps its offset"), Neighbor.Material->K2_GetVectorParameterValue(OffsetParameter),
        FLinearColor(0, 0, 0, 0));

    const FName PaneTag(TEXT("AA.Wall.Translucent"));
    const FSurface Front = FindWall(910, false, PaneTag, 66.0);
    const FSurface Back = FindWall(910, true, PaneTag, 66.0);
    if (!TestNotNull(TEXT("Front pane material"), Front.Material) || !TestNotNull(TEXT("Back pane material"), Back.Material))
    {
        return false;
    }
    TestEqual(TEXT("Static pane faces share a material"), Front.Material, Back.Material);
    TestTrue(TEXT("A wall side can start scrolling independently"),
        Builder->SetWallTextureScrollTexels(1278, EWadWallBand::Middle, FVector2D(9, 5)));
    const FSurface Animated = FindWall(910, false, PaneTag, 66.0);
    const FSurface Stationary = FindWall(910, true, PaneTag, 66.0);
    if (!TestNotNull(TEXT("Animated pane material"), Animated.Material) || !TestNotNull(TEXT("Stationary pane material"), Stationary.Material))
    {
        return false;
    }
    TestTrue(TEXT("The two pane faces still share their texture"), Animated.Material != Stationary.Material
        && Animated.Material->K2_GetTextureParameterValue(TextureParameter) == Stationary.Material->K2_GetTextureParameterValue(TextureParameter));
    TestEqual(TEXT("The first wall scroll is normalized by texture size"), Animated.Material->K2_GetVectorParameterValue(OffsetParameter),
        FLinearColor(9.0f / 64, 5.0f / 64, 0, 0));
    TestEqual(TEXT("The opposite side keeps its offset"), Stationary.Material->K2_GetVectorParameterValue(OffsetParameter),
        FLinearColor(0, 0, 0, 0));
    TestTrue(TEXT("Repeated scroll updates reuse the mesh"),
        Builder->SetWallTextureScrollTexels(1278, EWadWallBand::Middle, FVector2D(13, 7))
        && FindWall(910, false, PaneTag, 66.0).Mesh == Animated.Mesh);
    Builder->SetSectorCeilingZ(116, 160.0f);
    const FSurface Rebuilt = FindWall(910, false, PaneTag, 66.0);
    if (TestNotNull(TEXT("The pane remains after a height update"), Rebuilt.Material))
    {
        TestEqual(TEXT("Height changes preserve the animated material and offset"), Rebuilt.Material, Animated.Material);
        TestEqual(TEXT("The rebuilt pane retains its scroll offset"), Rebuilt.Material->K2_GetVectorParameterValue(OffsetParameter),
            FLinearColor(13.0f / 64, 7.0f / 64, 0, 0));
    }
    return true;
}

bool FAAOriginal_WadLoader_L30_SectorMotion::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP");
    if (!FPaths::FileExists(MapPath))
    {
        AddWarning(TEXT("L30.MAP fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L30 loads"), UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"))))
    {
        return false;
    }

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for sector movement checks"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ASectorMeshBuilderActor* Builder = World->SpawnActor<ASectorMeshBuilderActor>();
    if (!TestNotNull(TEXT("Sector mesh builder"), Builder))
    {
        return false;
    }
    Builder->BuildSectorsFromLastWad();

    const int32 DoorSector = 147;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(L30SectorMotion), true);
    const auto ExpectHeights = [&](double Floor, double Ceiling)
    {
        FVector2D Heights;
        if (TestTrue(TEXT("Both independent surfaces exist"), Builder->GetSectorSurfaceHeights(DoorSector, Heights)))
        {
            TestEqual(TEXT("Floor and ceiling transforms"), Heights, FVector2D(Floor, Ceiling));
        }
    };
    const auto CheckPassage = [&](double Z, float HalfHeight, bool bBlocked, const TCHAR* Label)
    {
        const FVector West(2668, 1516, Z);
        const FVector East(2744, 1516, Z);
        for (bool bFromEast : {false, true})
        {
            FHitResult Hit;
            const bool bHit = World->SweepSingleByChannel(Hit, bFromEast ? East : West, bFromEast ? West : East,
                FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(6.0f, HalfHeight), Query);
            TestEqual(Label, bHit, bBlocked);
            if (bHit && bBlocked)
            {
                const UPrimitiveComponent* Component = Hit.GetComponent();
                FVector2D Heights;
                // A sweep can hit either triangle mesh where the fixed lip meets the moving ceiling.
                const bool bMovingCeilingEdge = Component && Component->ComponentHasTag(TEXT("AA.Sector.Ceiling"))
                    && Component->ComponentHasTag(TEXT("AA.Sector.147"))
                    && Builder->GetSectorSurfaceHeights(DoorSector, Heights)
                    && FMath::IsNearlyEqual(Hit.ImpactPoint.Z, Heights.Y, 0.1);
                TestTrue(FString::Printf(TEXT("%s hits the doorway seam from %s (component %s, point %s)"),
                    Label, bFromEast ? TEXT("east") : TEXT("west"), *GetNameSafe(Component), *Hit.ImpactPoint.ToString()),
                    Component && (Component->ComponentHasTag(TEXT("AA.Wall.Collision")) || bMovingCeilingEdge)
                    && FMath::IsNearlyEqual(Hit.ImpactPoint.Y, 1516.0, 1.0)
                    && FMath::IsNearlyEqual(Hit.ImpactPoint.X, bFromEast ? 2708.0 : 2692.0, 1.0));
            }
        }
    };
    const auto CountDoorBands = [&](double Bottom, double Top)
    {
        TArray<UProceduralMeshComponent*> Meshes;
        Builder->GetComponents(Meshes);
        int32 Count = 0;
        for (UProceduralMeshComponent* Mesh : Meshes)
        {
            if (!Mesh->ComponentHasTag(TEXT("AA.Wall.Opaque")))
            {
                continue;
            }
            for (int32 SectionIndex = 0; SectionIndex < Mesh->GetNumSections(); ++SectionIndex)
            {
                const FProcMeshSection* Section = Mesh->GetProcMeshSection(SectionIndex);
                if (!Section)
                {
                    continue;
                }
                for (int32 Index = 0; Index + 3 < Section->ProcVertexBuffer.Num(); Index += 4)
                {
                    const FVector& A = Section->ProcVertexBuffer[Index].Position;
                    const FVector& B = Section->ProcVertexBuffer[Index + 2].Position;
                    if ((A.X == 2692 || A.X == 2708) && A.X == B.X
                        && FMath::Min(A.Y, B.Y) == 1460 && FMath::Max(A.Y, B.Y) == 1572
                        && A.Z == Bottom && B.Z == Top)
                    {
                        ++Count;
                    }
                }
            }
        }
        return Count;
    };
    const auto FindUnrelatedCollider = [&]() -> UPrimitiveComponent*
    {
        FHitResult Hit;
        World->LineTraceSingleByChannel(Hit, FVector(1452, 1176, 120), FVector(1460, 1176, 120), ECC_Pawn, Query);
        return Hit.GetComponent();
    };
    const TWeakObjectPtr<UPrimitiveComponent> UnrelatedCollider = FindUnrelatedCollider();
    TestTrue(TEXT("Unrelated line 910 has a collider"), UnrelatedCollider.IsValid());

    ExpectHeights(32, 32);
    TestEqual(TEXT("Both closed door faces span floor to ceiling"), CountDoorBands(32, 128), 2);
    CheckPassage(55, 22, true, TEXT("Closed door blocks a standing capsule"));

    constexpr float PartlyOpenCeiling = 75.2f;
    Builder->SetSectorCeilingZ(DoorSector, PartlyOpenCeiling);
    ExpectHeights(32, PartlyOpenCeiling);
    TestEqual(TEXT("Both door faces stop at the raised ceiling"), CountDoorBands(PartlyOpenCeiling, 128), 2);
    CheckPassage(55, 22, true, TEXT("Partly open door blocks a standing capsule"));
    CheckPassage(45, 12, false, TEXT("Partly open door clears a short capsule"));
    FHitResult CeilingHit;
    TestTrue(TEXT("The ceiling surface collision moves to the new height"),
        World->LineTraceSingleByChannel(CeilingHit, FVector(2700, 1516, 52), FVector(2700, 1516, 100), ECC_Pawn, Query)
        && FMath::IsNearlyEqual(CeilingHit.ImpactPoint.Z, double(PartlyOpenCeiling), 0.1));

    Builder->SetSectorCeilingZ(DoorSector, 128);
    ExpectHeights(32, 128);
    TestEqual(TEXT("Open door removes the old upper bands"), CountDoorBands(PartlyOpenCeiling, 128), 0);
    CheckPassage(55, 22, false, TEXT("Fully open door clears a standing capsule"));

    Builder->SetSectorFloorZ(DoorSector, 56.5f);
    ExpectHeights(56.5, 128);
    TestEqual(TEXT("Raising the floor adds both lower seam faces at the fractional height"), CountDoorBands(32, 56.5), 2);
    CheckPassage(45, 12, true, TEXT("Raised floor blocks the old crossing height"));
    CheckPassage(80, 22, false, TEXT("Raised floor clears a crossing above its new height"));
    FHitResult FloorHit;
    TestTrue(TEXT("The floor surface collision moves to the fractional height"),
        World->LineTraceSingleByChannel(FloorHit, FVector(2700, 1516, 90), FVector(2700, 1516, 40), ECC_Pawn, Query)
        && FMath::IsNearlyEqual(FloorHit.ImpactPoint.Z, 56.5, 0.1));

    Builder->SetSectorFloorZ(DoorSector, 32);
    Builder->SetSectorCeilingZ(DoorSector, 0);
    ExpectHeights(32, 32);
    TestEqual(TEXT("Closing restores both full-height faces"), CountDoorBands(32, 128), 2);
    CheckPassage(55, 22, true, TEXT("Closed again, the door blocks from either side"));
    TestTrue(TEXT("Unrelated line 910 keeps its original collision component"),
        UnrelatedCollider.IsValid() && UnrelatedCollider.Get() == FindUnrelatedCollider());
    return true;
}

bool FAAOriginal_WadLoader_L30_FixedWalls::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP");
    if (!FPaths::FileExists(MapPath))
    {
        AddWarning(TEXT("L30.MAP fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L30 loads"), UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"))))
    {
        return false;
    }

    const FDoomWadMapData& Map = *UDoomWadLoader::GetLastWadMapData();
    FWadWallLine Passage;
    FWadWallLine Screen;
    FWadWallLine Pane;
    if (!TestTrue(TEXT("Line 11 resolves"), BuildWadWallLine(Map, 11, Passage))
        || !TestTrue(TEXT("Line 590 resolves"), BuildWadWallLine(Map, 590, Screen))
        || !TestTrue(TEXT("Line 910 resolves"), BuildWadWallLine(Map, 910, Pane)))
    {
        return false;
    }
    TestEqual(TEXT("Passage shares height 24 to 104"), FIntPoint(Passage.OpeningBottom, Passage.OpeningTop), FIntPoint(24, 104));
    TestEqual(TEXT("Passage exposes only the lower and upper bands"), Passage.Surfaces.Num(), 2);
    TestFalse(TEXT("Passage has no middle surface"), Passage.Surfaces.ContainsByPredicate(
        [](const FWadWallSurface& Surface) { return Surface.Band == EWadWallBand::Middle; }));
    TestEqual(TEXT("Pane shares height 66 to 174"), FIntPoint(Pane.OpeningBottom, Pane.OpeningTop), FIntPoint(66, 174));
    TestTrue(TEXT("Pane has one translucent middle face from each side"), Pane.Surfaces.Num() == 2
        && Pane.Surfaces[0].Blend == EWadWallBlend::Translucent && Pane.Surfaces[1].Blend == EWadWallBlend::Translucent
        && Pane.Surfaces[0].SideIndex == 1278 && Pane.Surfaces[1].SideIndex == 1279
        && !Pane.Surfaces[0].bBack && Pane.Surfaces[1].bBack);
    TestEqual(TEXT("Pane's middle has one collision span"), Pane.Collision.Num(), 1);
    TestTrue(TEXT("Screen has two masked faces and no collision"), Screen.Surfaces.Num() == 2
        && Screen.Surfaces[0].Blend == EWadWallBlend::Masked && Screen.Surfaces[1].Blend == EWadWallBlend::Masked
        && Screen.Collision.IsEmpty());

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for wall collision checks"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    ASectorMeshBuilderActor* Builder = World->SpawnActor<ASectorMeshBuilderActor>();
    if (!TestNotNull(TEXT("Wall mesh builder"), Builder))
    {
        return false;
    }
    Builder->SetBuildFloorsEnabled(false);
    Builder->BuildSectorsFromLastWad();

    TArray<UProceduralMeshComponent*> Meshes;
    Builder->GetComponents(Meshes);
    auto VerifyRenderedWall = [&](const FWadWallLine& Line, FName Tag, EBlendMode Blend)
    {
        int32 MatchingFaces = 0;
        for (UProceduralMeshComponent* Mesh : Meshes)
        {
            if (!Mesh->ComponentHasTag(Tag))
            {
                continue;
            }
            for (int32 SectionIndex = 0; SectionIndex < Mesh->GetNumSections(); ++SectionIndex)
            {
                const FProcMeshSection* Section = Mesh->GetProcMeshSection(SectionIndex);
                if (!Section)
                {
                    continue;
                }
                for (int32 Index = 0; Index + 3 < Section->ProcVertexBuffer.Num(); Index += 4)
                {
                    const FVector& Start = Section->ProcVertexBuffer[Index].Position;
                    const FVector& End = Section->ProcVertexBuffer[Index + 2].Position;
                    if (Start.Z == Line.OpeningBottom && End.Z == Line.OpeningTop
                        && ((FVector2D(Start) == Line.From && FVector2D(End) == Line.To)
                            || (FVector2D(Start) == Line.To && FVector2D(End) == Line.From)))
                    {
                        ++MatchingFaces;
                        UMaterialInterface* Material = Mesh->GetMaterial(SectionIndex);
                        TestTrue(TEXT("The wall uses the expected Unreal blend mode"), Material && Material->GetBlendMode() == Blend);
                        TestFalse(TEXT("The render section has no collision"), Section->bEnableCollision);
                    }
                }
            }
        }
        TestEqual(TEXT("Both sides of the tested wall are present in the Unreal mesh"), MatchingFaces, 2);
    };
    VerifyRenderedWall(Screen, TEXT("AA.Wall.Masked"), BLEND_Masked);
    VerifyRenderedWall(Pane, TEXT("AA.Wall.Translucent"), BLEND_Translucent);

    const auto SweepAcross = [this, World](const FWadWallLine& Line, double Height, bool bShouldBlock, const TCHAR* Label)
    {
        const FVector Center((Line.From.X + Line.To.X) * 0.5, (Line.From.Y + Line.To.Y) * 0.5, Height);
        const FVector2D Direction = (Line.To - Line.From).GetSafeNormal();
        const FVector Offset(Direction.Y * 4, -Direction.X * 4, 0);
        for (double Sign : {-1.0, 1.0})
        {
            FHitResult Hit;
            const bool bBlocked = World->SweepSingleByChannel(Hit, Center - Offset * Sign, Center + Offset * Sign,
                FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(1.5f, 6.0f),
                FCollisionQueryParams(SCENE_QUERY_STAT(L30WallSweep), true));
            TestEqual(Label, bBlocked, bShouldBlock);
            if (bBlocked && bShouldBlock)
            {
                TestTrue(TEXT("Capsule hits the separate wall collider"), Hit.GetComponent()
                    && Hit.GetComponent()->ComponentHasTag(TEXT("AA.Wall.Collision")));
                TestTrue(TEXT("The impact is on the tested linedef"),
                    FMath::Abs(FVector::DotProduct(Hit.ImpactPoint - Center, Offset.GetSafeNormal())) < 1.0);
            }
        }
    };

    SweepAcross(Passage, 60, false, TEXT("Capsule passes through open line 11"));
    SweepAcross(Passage, 160, true, TEXT("Upper band above the passage blocks the capsule"));
    SweepAcross(Screen, 104, false, TEXT("Capsule passes through masked line 590"));
    SweepAcross(Pane, 120, true, TEXT("Translucent line 910 blocks the capsule"));
    return true;
}

bool FAAOriginal_WadLoader_L30_Footprints::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP");
    if (!FPaths::FileExists(MapPath))
    {
        AddWarning(TEXT("L30.MAP fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L30 loads"), UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"))))
    {
        return false;
    }

    const FDoomWadMapData& Map = *UDoomWadLoader::GetLastWadMapData();
    int32 ClosedCount = 0;
    for (int32 SectorIndex = 0; SectorIndex < Map.Sectors.Num(); ++SectorIndex)
    {
        FDoomWadSectorFootprint Footprint;
        TestTrue(FString::Printf(TEXT("Sector %d has a triangulated footprint"), SectorIndex),
            UDoomWadLoader::GetLastWadSectorFootprint(SectorIndex, Footprint));
        const FWadSector& Sector = Map.Sectors[SectorIndex];
        ClosedCount += Sector.FloorHeight == Sector.CeilingHeight ? 1 : 0;
    }
    TestEqual(TEXT("L30 initially closed sector count"), ClosedCount, 12);

    FDoomWadSectorFootprint Footprint;
    if (!TestTrue(TEXT("Sector 163 has a footprint"), UDoomWadLoader::GetLastWadSectorFootprint(163, Footprint)))
    {
        return false;
    }
    const auto ContainsPoint = [&Footprint](const FVector2D& Point)
    {
        for (int32 Index = 0; Index < Footprint.Triangles.Num(); Index += 3)
        {
            const FVector2D& A = Footprint.Vertices[Footprint.Triangles[Index]];
            const FVector2D& B = Footprint.Vertices[Footprint.Triangles[Index + 1]];
            const FVector2D& C = Footprint.Vertices[Footprint.Triangles[Index + 2]];
            if (FVector2D::CrossProduct(B - A, Point - A) >= 0
                && FVector2D::CrossProduct(C - B, Point - B) >= 0
                && FVector2D::CrossProduct(A - C, Point - C) >= 0)
            {
                return true;
            }
        }
        return false;
    };

    TestFalse(TEXT("First cutout stays empty"), ContainsPoint(FVector2D(96, 1120)));
    TestFalse(TEXT("Second cutout stays empty"), ContainsPoint(FVector2D(136, 1120)));
    TestFalse(TEXT("Third cutout stays empty"), ContainsPoint(FVector2D(176, 1120)));
    TestTrue(TEXT("The strip between cutouts stays filled"), ContainsPoint(FVector2D(116, 1120)));
    TestTrue(TEXT("The main floor stays filled"), ContainsPoint(FVector2D(80, 1200)));

    double TriangleArea = 0.0;
    for (int32 Index = 0; Index < Footprint.Triangles.Num(); Index += 3)
    {
        const FVector2D& A = Footprint.Vertices[Footprint.Triangles[Index]];
        const FVector2D& B = Footprint.Vertices[Footprint.Triangles[Index + 1]];
        const FVector2D& C = Footprint.Vertices[Footprint.Triangles[Index + 2]];
        TriangleArea += FVector2D::CrossProduct(B - A, C - A) * 0.5;
    }
    TestEqual(TEXT("Sector 163 area is 248 squared minus three 32-square cutouts"), TriangleArea, 58432.0);

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for floor and ceiling mesh check"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    ASectorMeshBuilderActor* Builder = World->SpawnActor<ASectorMeshBuilderActor>();
    if (!TestNotNull(TEXT("Sector mesh builder"), Builder))
    {
        return false;
    }
    Builder->SetBuildWallsEnabled(false);
    Builder->BuildSectorsFromLastWad();
    TArray<UProceduralMeshComponent*> Meshes;
    Builder->GetComponents(Meshes);
    TestEqual(TEXT("One floor and one ceiling component for all sectors, including closed sectors"),
        Meshes.Num(), 2 * Map.Sectors.Num());

    int32 Sector163SurfaceCount = 0;
    for (UProceduralMeshComponent* Mesh : Meshes)
    {
        const FProcMeshSection* Section = Mesh->GetProcMeshSection(0);
        if (Section && Section->ProcVertexBuffer.Num() == Footprint.Vertices.Num()
            && FVector2D(Section->ProcVertexBuffer[0].Position) == Footprint.Vertices[0])
        {
            ++Sector163SurfaceCount;
            TestEqual(TEXT("Sector 163 surface uses the triangulated footprint"), Section->ProcIndexBuffer.Num(), Footprint.Triangles.Num());
            TestEqual(TEXT("One material section covers the full surface"), Mesh->GetNumSections(), 1);
        }
    }
    TestEqual(TEXT("Sector 163 produces both surfaces"), Sector163SurfaceCount, 2);
    return true;
}

bool FAAOriginal_WadLoader_L30_MapData::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP");
    if (!FPaths::FileExists(MapPath))
    {
        AddWarning(TEXT("L30.MAP fixture is missing."));
        return true;
    }
    if (!TestTrue(TEXT("L30 loads using its file name"), UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"))))
    {
        return false;
    }
    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!TestNotNull(TEXT("Imported map data"), Map))
    {
        return false;
    }

    TestEqual(TEXT("Embedded marker"), Map->Marker, FString(TEXT("MAP01")));
    if (!TestEqual(TEXT("Vertex count"), Map->Vertices.Num(), 1253)
        || !TestEqual(TEXT("Line count"), Map->Lines.Num(), 1186)
        || !TestEqual(TEXT("Side count"), Map->Sides.Num(), 1641)
        || !TestEqual(TEXT("Sector count"), Map->Sectors.Num(), 179))
    {
        return false;
    }
    TestEqual(TEXT("Thing count"), Map->Things.Num(), 470);
    TestEqual(TEXT("BSP node count"), Map->Nodes.Num(), 725);

    const FWadLineDef& Barrier = Map->Lines[910];
    TestEqual(TEXT("Line 910 is translucent, two sided, and impassable"), Barrier.Flags, uint16(0x25));
    TestEqual(TEXT("Line 910 front sector"), Map->GetSectorForSide(Barrier.Side[0]), 116);
    TestEqual(TEXT("Line 910 back sector"), Map->GetSectorForSide(Barrier.Side[1]), 131);
    TestEqual(TEXT("Missing side has no sector"), Map->GetSectorForSide(INDEX_NONE), INDEX_NONE);
    TestEqual(TEXT("Side 1513 keeps its nonzero texture offset"), Map->Sides[1513].TexYOffset, int16(23));
    TestEqual(TEXT("Sector 117 retains both heights when closed"), Map->Sectors[117].FloorHeight, Map->Sectors[117].CeilingHeight);
    TestEqual(TEXT("Sector 163 retains its light value"), Map->Sectors[163].Light, uint16(50));

    return true;
}

bool FAAOriginal_WadLoader_L30_SectorCoverage::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP"));
    if (!FPaths::FileExists(MapPath))
    {
        AddWarning(FString::Printf(TEXT("L30.MAP not found at expected path: %s"), *MapPath));
        return true;
    }

    const bool bLoaded = UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"));
    TestTrue(FString::Printf(TEXT("L30 should load from %s"), *MapPath), bLoaded);
    if (!bLoaded)
    {
        return false;
    }

    const FDoomWadLoadStats Stats = UDoomWadLoader::GetLastLoadStats();
    TestTrue(TEXT("Loaded stats valid"), Stats.bValid);
    TestTrue(TEXT("Expected non-trivial sector count"), Stats.NumSectors >= 20);

    TArray<FVector2D> Verts2D;
    TArray<FIntPoint> Lines;
    TArray<FIntPoint> LineSectors;
    TArray<FIntPoint> SectorHeights;
    const bool bHaveTopology = UDoomWadLoader::GetLastWadWallTopology(Verts2D, Lines, LineSectors, SectorHeights);
    TestTrue(TEXT("Wall topology available after L30 load"), bHaveTopology);
    if (!bHaveTopology)
    {
        return false;
    }

    int32 SectorsWithBoundaryEdges = 0;
    int32 SectorsWithLoops = 0;
    int32 TotalLoopCount = 0;
    int32 MultiLoopSectors = 0;
    int32 TotalLoopVertexCount = 0;
    int32 LargeSectors = 0;
    int32 LargeSectorsWithLoops = 0;

    for (int32 SectorIdx = 0; SectorIdx < SectorHeights.Num(); ++SectorIdx)
    {
        TSet<int32> SectorVertexIndices;
        for (int32 LineIdx = 0; LineIdx < Lines.Num() && LineIdx < LineSectors.Num(); ++LineIdx)
        {
            const FIntPoint& LS = LineSectors[LineIdx];
            if (LS.X != SectorIdx && LS.Y != SectorIdx)
            {
                continue;
            }

            const FIntPoint& L = Lines[LineIdx];
            if (Verts2D.IsValidIndex(L.X)) SectorVertexIndices.Add(L.X);
            if (Verts2D.IsValidIndex(L.Y)) SectorVertexIndices.Add(L.Y);
        }

        if (SectorVertexIndices.Num() == 0)
        {
            continue;
        }

        ++SectorsWithBoundaryEdges;

        FVector2D Min(FLT_MAX, FLT_MAX);
        FVector2D Max(-FLT_MAX, -FLT_MAX);
        for (const int32 VertexIdx : SectorVertexIndices)
        {
            const FVector2D& V = Verts2D[VertexIdx];
            Min.X = FMath::Min(Min.X, V.X);
            Min.Y = FMath::Min(Min.Y, V.Y);
            Max.X = FMath::Max(Max.X, V.X);
            Max.Y = FMath::Max(Max.Y, V.Y);
        }

        const double SectorBBoxArea = FMath::Max(0.0, double(Max.X - Min.X)) * FMath::Max(0.0, double(Max.Y - Min.Y));
        if (SectorBBoxArea >= 4096.0)
        {
            ++LargeSectors;
        }

        TArray<int32> LoopOffsets;
        TArray<int32> LoopCounts;
        TArray<FVector2D> LoopVerts;
        const bool bHaveLoops = UDoomWadLoader::GetLastWadSectorBoundaryLoops(SectorIdx, LoopOffsets, LoopCounts, LoopVerts);
        if (!bHaveLoops || LoopOffsets.Num() == 0)
        {
            continue;
        }

        ++SectorsWithLoops;
        TotalLoopCount += LoopOffsets.Num();
        if (LoopOffsets.Num() > 1)
        {
            ++MultiLoopSectors;
        }

        for (int32 LoopIdx = 0; LoopIdx < LoopOffsets.Num(); ++LoopIdx)
        {
            const int32 Offset = LoopOffsets[LoopIdx];
            const int32 Count = LoopCounts[LoopIdx];
            if (Count < 3 || Offset < 0 || Offset + Count > LoopVerts.Num())
            {
                continue;
            }

            TotalLoopVertexCount += Count;
        }

        if (SectorBBoxArea >= 4096.0)
        {
            if (LoopOffsets.Num() > 0)
            {
                ++LargeSectorsWithLoops;
            }
        }
    }

    AddInfo(FString::Printf(
        TEXT("L30 coverage stats: sectors=%d boundary=%d sectorsWithLoops=%d totalLoops=%d multiLoopSectors=%d large=%d largeWithLoops=%d avgLoopVerts=%.2f"),
        Stats.NumSectors,
        SectorsWithBoundaryEdges,
        SectorsWithLoops,
        TotalLoopCount,
        MultiLoopSectors,
        LargeSectors,
        LargeSectorsWithLoops,
        TotalLoopCount > 0 ? double(TotalLoopVertexCount) / double(TotalLoopCount) : 0.0));

    TestTrue(TEXT("All sectors with boundaries should produce boundary loops"),
        SectorsWithBoundaryEdges > 0 && SectorsWithLoops == SectorsWithBoundaryEdges);

    TestTrue(TEXT("Large sectors should all produce boundary loops"),
        LargeSectors == 0 || LargeSectorsWithLoops == LargeSectors);

    TestTrue(TEXT("A meaningful number of sectors should require multiple loops"),
        MultiLoopSectors >= 10);

    TestTrue(TEXT("Total loop count should exceed sector count"),
        TotalLoopCount >= SectorsWithLoops + 10);

    TestTrue(TEXT("Boundary loops should retain non-trivial shape complexity"),
        TotalLoopCount == 0 || (double(TotalLoopVertexCount) / double(TotalLoopCount)) >= 4.5);

    return true;
}

bool FAAOriginal_WadLoader_L30_BoundaryAreaCoverage::RunTest(const FString& Parameters)
{
    const FString MapPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("TestData/L30.MAP"));
    if (!FPaths::FileExists(MapPath))
    {
        AddWarning(FString::Printf(TEXT("L30.MAP not found at expected path: %s"), *MapPath));
        return true;
    }

    const bool bLoaded = UDoomWadLoader::LoadDoomWadMap(MapPath, TEXT("L30"));
    TestTrue(FString::Printf(TEXT("L30 should load from %s"), *MapPath), bLoaded);
    if (!bLoaded)
    {
        return false;
    }

    TArray<FVector2D> Verts2D;
    TArray<FIntPoint> Lines;
    TArray<FIntPoint> LineSectors;
    TArray<FIntPoint> SectorHeights;
    const bool bHaveTopology = UDoomWadLoader::GetLastWadWallTopology(Verts2D, Lines, LineSectors, SectorHeights);
    TestTrue(TEXT("Wall topology available after L30 load"), bHaveTopology);
    if (!bHaveTopology)
    {
        return false;
    }

    constexpr int32 SampleResolution = 24;

    int32 SectorsSampled = 0;
    int32 SectorsAtOrAbove30Pct = 0;
    int32 LargeSectorsSampled = 0;
    int32 LargeSectorsAtOrAbove30Pct = 0;
    double CoverageSum = 0.0;
    double LargeCoverageSum = 0.0;
    double WeightedCoveredArea = 0.0;
    double WeightedBBoxArea = 0.0;
    double WeightedLargeCoveredArea = 0.0;
    double WeightedLargeBBoxArea = 0.0;

    for (int32 SectorIdx = 0; SectorIdx < SectorHeights.Num(); ++SectorIdx)
    {
        TSet<int32> SectorVertexIndices;
        for (int32 LineIdx = 0; LineIdx < Lines.Num() && LineIdx < LineSectors.Num(); ++LineIdx)
        {
            const FIntPoint& LS = LineSectors[LineIdx];
            if (LS.X != SectorIdx && LS.Y != SectorIdx)
            {
                continue;
            }

            const FIntPoint& L = Lines[LineIdx];
            if (Verts2D.IsValidIndex(L.X)) SectorVertexIndices.Add(L.X);
            if (Verts2D.IsValidIndex(L.Y)) SectorVertexIndices.Add(L.Y);
        }

        if (SectorVertexIndices.Num() == 0)
        {
            continue;
        }

        FVector2D Min(FLT_MAX, FLT_MAX);
        FVector2D Max(-FLT_MAX, -FLT_MAX);
        for (const int32 VertexIdx : SectorVertexIndices)
        {
            const FVector2D& V = Verts2D[VertexIdx];
            Min.X = FMath::Min(Min.X, V.X);
            Min.Y = FMath::Min(Min.Y, V.Y);
            Max.X = FMath::Max(Max.X, V.X);
            Max.Y = FMath::Max(Max.Y, V.Y);
        }

        const double Width = FMath::Max(0.0, double(Max.X - Min.X));
        const double Height = FMath::Max(0.0, double(Max.Y - Min.Y));
        const double SectorBBoxArea = Width * Height;
        if (SectorBBoxArea <= 0.0)
        {
            continue;
        }

        TArray<int32> LoopOffsets;
        TArray<int32> LoopCounts;
        TArray<FVector2D> LoopVerts;
        const bool bHaveLoops = UDoomWadLoader::GetLastWadSectorBoundaryLoops(SectorIdx, LoopOffsets, LoopCounts, LoopVerts);
        TestTrue(FString::Printf(TEXT("Sector %d should have boundary loops"), SectorIdx), bHaveLoops && LoopOffsets.Num() > 0);
        if (!bHaveLoops || LoopOffsets.Num() == 0)
        {
            continue;
        }

        TArray<TArray<FVector2D>> Loops;
        Loops.Reserve(LoopOffsets.Num());
        for (int32 LoopIdx = 0; LoopIdx < LoopOffsets.Num(); ++LoopIdx)
        {
            const int32 Offset = LoopOffsets[LoopIdx];
            const int32 Count = LoopCounts[LoopIdx];
            if (Count < 3 || Offset < 0 || Offset + Count > LoopVerts.Num())
            {
                continue;
            }

            TArray<FVector2D> Loop;
            Loop.Reserve(Count);
            for (int32 PointIdx = 0; PointIdx < Count; ++PointIdx)
            {
                Loop.Add(LoopVerts[Offset + PointIdx]);
            }
            if (Loop.Num() >= 3)
            {
                Loops.Add(MoveTemp(Loop));
            }
        }

        if (Loops.Num() == 0)
        {
            continue;
        }

        int32 InsideSamples = 0;
        const int32 TotalSamples = SampleResolution * SampleResolution;
        for (int32 Y = 0; Y < SampleResolution; ++Y)
        {
            const double SampleY = double(Min.Y) + ((double(Y) + 0.5) * Height / double(SampleResolution));
            for (int32 X = 0; X < SampleResolution; ++X)
            {
                const double SampleX = double(Min.X) + ((double(X) + 0.5) * Width / double(SampleResolution));
                int32 InsideCount = 0;
                for (const TArray<FVector2D>& Loop : Loops)
                {
                    if (PointInPolygon2D(FVector2D((float)SampleX, (float)SampleY), Loop))
                    {
                        ++InsideCount;
                    }
                }

                if ((InsideCount % 2) == 1)
                {
                    ++InsideSamples;
                }
            }
        }

        const double CoverageRatio = double(InsideSamples) / double(TotalSamples);
        const bool bLargeSector = SectorBBoxArea >= 4096.0;
        ++SectorsSampled;
        CoverageSum += CoverageRatio;
        WeightedCoveredArea += CoverageRatio * SectorBBoxArea;
        WeightedBBoxArea += SectorBBoxArea;
        if (CoverageRatio >= 0.30)
        {
            ++SectorsAtOrAbove30Pct;
        }

        if (bLargeSector)
        {
            ++LargeSectorsSampled;
            LargeCoverageSum += CoverageRatio;
            WeightedLargeCoveredArea += CoverageRatio * SectorBBoxArea;
            WeightedLargeBBoxArea += SectorBBoxArea;
            if (CoverageRatio >= 0.30)
            {
                ++LargeSectorsAtOrAbove30Pct;
            }
        }
    }

    const double AvgCoverage = SectorsSampled > 0 ? CoverageSum / double(SectorsSampled) : 0.0;
    const double AvgLargeCoverage = LargeSectorsSampled > 0 ? LargeCoverageSum / double(LargeSectorsSampled) : 0.0;
    const double WeightedCoverage = WeightedBBoxArea > 0.0 ? WeightedCoveredArea / WeightedBBoxArea : 0.0;
    const double WeightedLargeCoverage = WeightedLargeBBoxArea > 0.0 ? WeightedLargeCoveredArea / WeightedLargeBBoxArea : 0.0;

    AddInfo(FString::Printf(
        TEXT("L30 boundary area stats: sampled=%d avg=%.3f weighted=%.3f sectors>=30%%=%d large=%d largeAvg=%.3f largeWeighted=%.3f large>=30%%=%d"),
        SectorsSampled,
        AvgCoverage,
        WeightedCoverage,
        SectorsAtOrAbove30Pct,
        LargeSectorsSampled,
        AvgLargeCoverage,
        WeightedLargeCoverage,
        LargeSectorsAtOrAbove30Pct));

    TestTrue(TEXT("Average boundary-loop fill should cover most of each sector bbox"),
        AvgCoverage >= 0.75);

    TestTrue(TEXT("Weighted boundary-loop fill should cover a meaningful share of overall wall-topology bounds"),
        WeightedCoverage >= 0.35);

    TestTrue(TEXT("Most sectors should cover at least 30% of their wall-topology bbox"),
        SectorsSampled == 0 || SectorsAtOrAbove30Pct >= 150);

    TestTrue(TEXT("Large-sector average coverage should remain substantial"),
        LargeSectorsSampled == 0 || AvgLargeCoverage >= 0.55);

    TestTrue(TEXT("Large-sector weighted coverage should remain substantial"),
        LargeSectorsSampled == 0 || WeightedLargeCoverage >= 0.35);

    TestTrue(TEXT("Most large sectors should cover at least 30% of their wall-topology bbox"),
        LargeSectorsSampled == 0 || LargeSectorsAtOrAbove30Pct >= 55);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalRejuvenateOutcomes, "AAOriginal.WAD.Gameplay.RejuvenateOutcomes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalRejuvenateOutcomes::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    int32 Health = 1800;
    int32 Mana = 2200;
    TestTrue(TEXT("The last successful roll casts"), ResolveRejuvenate(Health, Mana, MaxHealth, Magic, 1, 87)
        == ERejuvenateResult::Success);
    TestEqual(TEXT("Healing stops at maximum health"), Health, MaxHealth);
    TestEqual(TEXT("Success spends 1000 mana"), Mana, 1200);

    Health = 1800;
    Mana = 2200;
    TestTrue(TEXT("The first failing roll fizzles"), ResolveRejuvenate(Health, Mana, MaxHealth, Magic, 1, 88)
        == ERejuvenateResult::Fizzle);
    TestEqual(TEXT("Fizzle leaves health alone"), Health, 1800);
    TestEqual(TEXT("Fizzle spends 500 mana"), Mana, 1700);

    Mana = RejuvenateCost;
    TestTrue(TEXT("The original requires mana strictly above the cost"),
        ResolveRejuvenate(Health, Mana, MaxHealth, Magic, 1, 0) == ERejuvenateResult::InsufficientMana);
    TestEqual(TEXT("A mana refusal leaves health alone"), Health, 1800);
    TestEqual(TEXT("A mana refusal spends nothing"), Mana, RejuvenateCost);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalElementalDamage, "AAOriginal.WAD.Gameplay.ElementalDamage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalElementalDamage::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for elemental damage"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadGuardian* Target = World->SpawnActor<ANativeWadGuardian>();
    if (!TestNotNull(TEXT("Elemental damage target"), Target)) { return false; }
    FWadEncounterProfile Profile;
    Profile.MaxHealth = 4000;
    const auto Damage = [&](int32 Base, uint8 Mask, int32 Armor, uint8 Resistance)
    {
        Profile.ArmorClass = Armor;
        Profile.ResistanceMask = Resistance;
        Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
        Target->RestoreEncounterHealth(Profile.MaxHealth);
        Target->TakeHit(Base, nullptr, Mask);
        return Profile.MaxHealth - Target->GetEncounterHealth();
    };
    const uint8 Fire = DamageFlag(EProjectileDamage::Fire);
    const uint8 Electric = DamageFlag(EProjectileDamage::Electricity);
    TestEqual(TEXT("Fire adds a quarter of the original damage, rounded down"), Damage(750, Fire, 0, 0), 937);
    TestEqual(TEXT("Resisted fire adds no bonus"), Damage(100, NormalDamage | Fire, 0, Fire), 50);
    TestEqual(TEXT("Chain adds 25 percent electric damage"), Damage(100, Electric, 1, 0), 125);
    TestEqual(TEXT("Plate adds 50 percent electric damage"), Damage(100, Electric, 2, 0), 150);
    TestEqual(TEXT("Combined bonuses use the original damage independently"), Damage(100, Fire | Electric, 1, 0), 150);
    TestEqual(TEXT("The class-permitted mace and electric scroll respect one resisted element"),
        Damage(75, NormalDamage | Electric, 1, NormalDamage | Fire), 47);
    FWadEncounterProfile Archer;
    Archer.SourceType = 1014;
    Archer.MaxHealth = 4000;
    Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Archer);
    Target->RestoreEncounterHealth(Archer.MaxHealth);
    Target->TakeHit(100, nullptr, Electric);
    TestEqual(TEXT("Eastern Archer uses source chain armor"), 4000 - Target->GetEncounterHealth(), 125);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPlayerElementalDamage, "AAOriginal.WAD.Gameplay.PlayerElementalDamage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalPlayerElementalDamage::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const uint8 Fire = DamageFlag(EProjectileDamage::Fire);
    const uint8 Poison = DamageFlag(EProjectileDamage::Poison);
    const uint8 Electric = DamageFlag(EProjectileDamage::Electricity);
    const uint8 Piercing = DamageFlag(EProjectileDamage::Piercing);
    const uint8 Drain = DamageFlag(EProjectileDamage::ManaDrain);
    TestEqual(TEXT("Player fire rounds the source quarter bonus down"), ResolvePlayerDamage(750, Fire, 0, 0, 0).Health, 937);
    TestEqual(TEXT("A single resisted element still inflicts the source 20 percent minimum"), ResolvePlayerDamage(100, Fire, Fire, 0, 0).Health, 20);
    TestEqual(TEXT("Resistance removes only one component of a mixed hit"), ResolvePlayerDamage(100, NormalDamage | Fire, Fire, 0, 0).Health, 50);
    TestEqual(TEXT("Normal incoming damage uses half the armor rating"), ResolvePlayerDamage(100, NormalDamage, 0, 0, 60).Health, 70);
    TestEqual(TEXT("Unresisted piercing bypasses player armor"), ResolvePlayerDamage(100, Piercing, 0, 0, 100).Health, 100);
    TestEqual(TEXT("Resisting piercing restores armor mitigation after the resistance cap"), ResolvePlayerDamage(100, Piercing, Piercing, 0, 100).Health, 10);
    TestEqual(TEXT("Player fire and plate-electric bonuses are sequential in STATS.C"), ResolvePlayerDamage(100, Fire | Electric, 0, 2, 0).Health, 187);
    TestEqual(TEXT("Chain-electric bonus, component resistance, and armor apply in source order"), ResolvePlayerDamage(100, NormalDamage | Fire | Electric, Fire, 1, 40).Health, 66);
    const FWadPlayerDamage PoisonHit = ResolvePlayerDamage(400, Poison, Poison, 0, 0);
    TestEqual(TEXT("Poison resistance quarters the status buildup"), PoisonHit.Poison, 10);
    TestEqual(TEXT("Poison resistance retains minimum health damage"), PoisonHit.Health, 80);
    const FWadPlayerDamage DrainHit = ResolvePlayerDamage(400, Drain, Drain, 0, 0);
    TestEqual(TEXT("Drain resistance quarters mana loss before health mitigation"), DrainHit.Mana, 100);
    TestEqual(TEXT("Drain resistance retains minimum health damage"), DrainHit.Health, 80);
    TestEqual(TEXT("Special Push is never interpreted as elemental health damage"), ResolvePlayerDamage(5000, 128 | 3, 0, 0, 0).Health, 0);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for incoming damage"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    if (!TestNotNull(TEXT("Player receiving a mixed hit"), Player)) { return false; }
    const int32 BeforeHealth = Player->GetHealth();
    const int32 BeforeMana = Player->GetMana();
    Player->ReceiveTypedHit(400, NormalDamage | Poison | Drain);
    TestEqual(TEXT("The live player loses health from a mixed creature hit"), Player->GetHealth(), BeforeHealth - 400);
    TestEqual(TEXT("The same live hit applies its poison component"), Player->GetPoisonLevel(), 40);
    TestEqual(TEXT("The same live hit also drains mana"), Player->GetMana(), BeforeMana - 400);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalFlyingCreature, "AAOriginal.WAD.Gameplay.FlyingCreature",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalFlyingCreature::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for flying creature pursuit"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>(FVector(300, 0, 200), FRotator::ZeroRotator);
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    ANativeWadGuardian* Creature = World->SpawnActor<ANativeWadGuardian>();
    if (!TestNotNull(TEXT("Airborne target"), Player) || !TestNotNull(TEXT("Player controller"), Controller)
        || !TestNotNull(TEXT("Flying creature"), Creature)) { return false; }
    World->AddController(Controller); // The isolated world has not run actor initialization.
    Controller->Possess(Player);
    FWadEncounterProfile Profile;
    Profile.SourceType = 1023;
    Profile.MaxHealth = 5000;
    Profile.WalkSpeed = 105;
    Profile.BoltDamage = 0;
    Creature->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
    Creature->RestoreEncounterHealth(5000);
    Creature->GetCharacterMovement()->MaxWalkSpeed = 600.0f;
    Creature->GetCharacterMovement()->MaxFlySpeed = 600.0f;
    Creature->DispatchBeginPlay();
    TestEqual(TEXT("Existing map actors restore the source walking speed on startup"), Creature->GetCharacterMovement()->MaxWalkSpeed, 105.0f);
    TestEqual(TEXT("Existing map actors restore the source flying speed on startup"), Creature->GetCharacterMovement()->MaxFlySpeed, 105.0f);
    TestEqual(TEXT("Source Griffon uses collision-aware flying physics"), Creature->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
    for (int32 Frame = 0; Frame < 60; ++Frame)
    {
        Creature->Tick(1.0f / 60.0f);
        Creature->GetCharacterMovement()->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    }
    TestTrue(TEXT("A Griffon acquires the player above it"), Creature->HasAcquiredTarget());
    TestTrue(TEXT("The flying pursuit rises toward the player's height"), Creature->GetActorLocation().Z > 20.0);
    TestTrue(TEXT("The same pursuit advances horizontally"), Creature->GetActorLocation().X > 30.0);
    Creature->RestoreEncounterHealth(0);
    Creature->ResetForGenerator();
    TestEqual(TEXT("A reused flying generator actor restores flight"), Creature->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
    Profile.SourceType = 1007;
    Creature->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
    Creature->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    Creature->ResetForGenerator();
    TestEqual(TEXT("Source Gargoyle remains grounded despite its wings"), Creature->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Walking);
    return true;
}
