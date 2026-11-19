#include "Modules/ModuleManager.h"
#include "CoreMinimal.h"
#include "Algo/AnyOf.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/Archive.h"
#include "Modules/ModuleManager.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "DoomWadLoader.h"
#include "LegacyMapLoader.h"
#include "NativeWadImport.h"
#include "NativeWadLevel.h"
#include "NativeWadReview.h"
#include "SectorMeshBuilderActor.h"
#include "Engine/SceneCapture2D.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif

class FAAOriginalModule : public IModuleInterface, public FSelfRegisteringExec
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}

    static FString ResolvePathOrProjectRelative(FString InPath)
    {
        if (InPath.IsEmpty()) return InPath;
        if (FPaths::FileExists(InPath) || FPaths::DirectoryExists(InPath))
        {
            return FPaths::ConvertRelativePathToFull(InPath);
        }
        const FString ProjectRel = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / InPath);
        if (FPaths::FileExists(ProjectRel) || FPaths::DirectoryExists(ProjectRel))
        {
            return ProjectRel;
        }
        return InPath; // unchanged; will fail later with a clear message
    }

    static bool TryAutoPickWadFromDirectory(const FString& DirPath, FString& OutWad)
    {
        if (!FPaths::DirectoryExists(DirPath)) return false;
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *DirPath, TEXT("*.wad"), true, false);
        IFileManager::Get().FindFilesRecursive(Files, *DirPath, TEXT("*.WAD"), true, false);
        if (Files.Num() > 0)
        {
            // Prefer common names if present
            int32 BestIdx = 0;
            for (int32 i = 0; i < Files.Num(); ++i)
            {
                const FString Name = FPaths::GetCleanFilename(Files[i]).ToLower();
                if (Name == TEXT("doom.wad") || Name == TEXT("doom2.wad") || Name == TEXT("amulets.wad"))
                {
                    BestIdx = i; break;
                }
            }
            OutWad = Files[BestIdx];
            return true;
        }
        return false;
    }

    static FString SanitizeMapMarker(const FString& In)
    {
        FString M = In;
        // Strip extension if user typed e.g. L30.MAP
        if (M.EndsWith(TEXT(".map"), ESearchCase::IgnoreCase))
        {
            M = FPaths::GetBaseFilename(M);
        }
        return M;
    }

    // New: allow .map files that are actually WADs
    static bool LooksLikeWadFile(const FString& Path)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 4)
        {
            return false;
        }
        const char* Sig = reinterpret_cast<const char*>(Bytes.GetData());
        return (FMemory::Memcmp(Sig, "IWAD", 4) == 0) || (FMemory::Memcmp(Sig, "PWAD", 4) == 0);
    }

    static void SetPixelSafe(TArray<FColor>& Pixels, int32 Width, int32 Height, int32 X, int32 Y, const FColor& Color)
    {
        if (X < 0 || Y < 0 || X >= Width || Y >= Height)
        {
            return;
        }
        Pixels[(Y * Width) + X] = Color;
    }

    static void DrawLine(TArray<FColor>& Pixels, int32 Width, int32 Height, int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& Color)
    {
        int32 DX = FMath::Abs(X1 - X0);
        int32 SX = X0 < X1 ? 1 : -1;
        int32 DY = -FMath::Abs(Y1 - Y0);
        int32 SY = Y0 < Y1 ? 1 : -1;
        int32 Err = DX + DY;

        while (true)
        {
            SetPixelSafe(Pixels, Width, Height, X0, Y0, Color);
            if (X0 == X1 && Y0 == Y1)
            {
                break;
            }

            const int32 E2 = 2 * Err;
            if (E2 >= DY)
            {
                Err += DY;
                X0 += SX;
            }
            if (E2 <= DX)
            {
                Err += DX;
                Y0 += SY;
            }
        }
    }

    static FColor MakeSectorColor(int32 SectorIndex)
    {
        const uint32 Seed = uint32(196613u * uint32(SectorIndex + 1));
        const uint8 R = uint8(96 + (Seed & 0x5F));
        const uint8 G = uint8(96 + ((Seed >> 7) & 0x5F));
        const uint8 B = uint8(96 + ((Seed >> 13) & 0x5F));
        return FColor(B, G, R, 255);
    }

    static bool WritePngToDisk(const FString& OutputPath, const TArray<FColor>& Pixels, int32 Width, int32 Height, FOutputDevice& Ar)
    {
        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> PngWriter = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!PngWriter.IsValid())
        {
            Ar.Log(TEXT("Failed to create PNG writer."));
            return false;
        }

        if (!PngWriter->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
        {
            Ar.Log(TEXT("Failed to encode image pixels."));
            return false;
        }

        const TArray64<uint8>& Compressed = PngWriter->GetCompressed(100);
        const bool bLooksAbsolute = !FPaths::IsRelative(OutputPath)
            || OutputPath.Contains(TEXT(":/"))
            || OutputPath.Contains(TEXT(":\\"));
        const FString ResolvedOutput = bLooksAbsolute
            ? OutputPath
            : FPaths::ConvertRelativePathToFull(OutputPath);
        const FString OutputDir = FPaths::GetPath(ResolvedOutput);
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (!PlatformFile.DirectoryExists(*OutputDir) && !PlatformFile.CreateDirectoryTree(*OutputDir))
        {
            Ar.Log(*FString::Printf(TEXT("Failed to create output directory: %s"), *OutputDir));
            return false;
        }

        TUniquePtr<FArchive> FileWriter(IFileManager::Get().CreateFileWriter(*ResolvedOutput));
        if (!FileWriter)
        {
            Ar.Log(*FString::Printf(TEXT("Failed to open PNG for write: %s"), *ResolvedOutput));
            return false;
        }

        FileWriter->Serialize((void*)Compressed.GetData(), Compressed.Num());
        FileWriter->Close();

        if (FileWriter->IsError() || !IFileManager::Get().FileExists(*ResolvedOutput))
        {
            Ar.Log(*FString::Printf(TEXT("Failed to write PNG: %s"), *ResolvedOutput));
            return false;
        }

        return true;
    }

    static bool ExportTopDownReviewImage(const FString& OutputPath, int32 ImageSize, int32 SectorFilter, FOutputDevice& Ar)
    {
        const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
        if (!Map || (SectorFilter != INDEX_NONE && !Map->Sectors.IsValidIndex(SectorFilter)))
        {
            Ar.Log(TEXT("Top-down review needs a loaded map and a valid sector ID."));
            return false;
        }
        if (ImageSize < 256 || ImageSize > 4096)
        {
            Ar.Log(TEXT("ImageSize must be between 256 and 4096."));
            return false;
        }

        TArray<FDoomWadSectorFootprint> Footprints;
        Footprints.SetNum(Map->Sectors.Num());
        FBox2D Bounds(ForceInit);
        int32 FilledSectorCount = 0;
        for (int32 SectorIndex = 0; SectorIndex < Footprints.Num(); ++SectorIndex)
        {
            if (SectorFilter != INDEX_NONE && SectorIndex != SectorFilter)
            {
                continue;
            }
            FDoomWadSectorFootprint& Footprint = Footprints[SectorIndex];
            if (!UDoomWadLoader::GetLastWadSectorFootprint(SectorIndex, Footprint))
            {
                Ar.Log(*FString::Printf(TEXT("Top-down review: no triangulated footprint for sector %d."), SectorIndex));
                continue;
            }
            ++FilledSectorCount;
            for (const FVector2D& Point : Footprint.Vertices)
            {
                Bounds += Point;
            }
        }
        if (!Bounds.bIsValid)
        {
            return false;
        }

        const FVector2D Center = Bounds.GetCenter();
        const FVector2D Size = Bounds.GetSize();
        const int32 Margin = FMath::Clamp(ImageSize / 20, 16, 128);
        const double Scale = double(ImageSize - 2 * Margin - 1) / FMath::Max(Size.X, Size.Y);
        const double PixelCenter = (ImageSize - 1) * 0.5;

        TArray<FColor> Pixels;
        Pixels.Init(FColor(248, 248, 248), ImageSize * ImageSize);
        int32 TriangleCount = 0;
        for (int32 SectorIndex = 0; SectorIndex < Footprints.Num(); ++SectorIndex)
        {
            FDoomWadSectorFootprint& Footprint = Footprints[SectorIndex];
            for (FVector2D& Point : Footprint.Vertices)
            {
                Point = FVector2D(PixelCenter + (Point.X - Center.X) * Scale,
                                  PixelCenter - (Point.Y - Center.Y) * Scale);
            }

            const FColor FillColor = MakeSectorColor(SectorIndex);
            for (int32 Index = 0; Index < Footprint.Triangles.Num(); Index += 3)
            {
                // The pixel Y axis points down. Swap B and C to keep a positive edge test.
                const FVector2D& A = Footprint.Vertices[Footprint.Triangles[Index]];
                const FVector2D& B = Footprint.Vertices[Footprint.Triangles[Index + 2]];
                const FVector2D& C = Footprint.Vertices[Footprint.Triangles[Index + 1]];
                const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(A.X, B.X, C.X)), 0, ImageSize - 1);
                const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(A.Y, B.Y, C.Y)), 0, ImageSize - 1);
                const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(A.X, B.X, C.X)), 0, ImageSize - 1);
                const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(A.Y, B.Y, C.Y)), 0, ImageSize - 1);
                for (int32 Y = MinY; Y <= MaxY; ++Y)
                {
                    for (int32 X = MinX; X <= MaxX; ++X)
                    {
                        const FVector2D P(double(X) + 0.5, double(Y) + 0.5);
                        if (FVector2D::CrossProduct(B - A, P - A) >= -UE_DOUBLE_SMALL_NUMBER
                            && FVector2D::CrossProduct(C - B, P - B) >= -UE_DOUBLE_SMALL_NUMBER
                            && FVector2D::CrossProduct(A - C, P - C) >= -UE_DOUBLE_SMALL_NUMBER)
                        {
                            Pixels[Y * ImageSize + X] = FillColor;
                        }
                    }
                }
                ++TriangleCount;
            }
        }

        for (const FDoomWadSectorFootprint& Footprint : Footprints)
        {
            for (const FIntPoint& Edge : Footprint.BoundaryEdges)
            {
                const FVector2D& A = Footprint.Vertices[Edge.X];
                const FVector2D& B = Footprint.Vertices[Edge.Y];
                DrawLine(Pixels, ImageSize, ImageSize,
                    FMath::RoundToInt(A.X), FMath::RoundToInt(A.Y),
                    FMath::RoundToInt(B.X), FMath::RoundToInt(B.Y), FColor(24, 24, 24));
            }
        }

        if (!WritePngToDisk(OutputPath, Pixels, ImageSize, ImageSize, Ar))
        {
            return false;
        }

        Ar.Log(*FString::Printf(
            TEXT("AA.ExportTopDownReview succeeded: sectorFilter=%d filledSectors=%d triangles=%d image=%dx%d out=%s"),
            SectorFilter, FilledSectorCount, TriangleCount, ImageSize, ImageSize,
            *FPaths::ConvertRelativePathToFull(OutputPath)));
        return true;
    }

    static bool ExportPerspectiveReviewImage(UWorld* World, const FString& WadPath, const FString& MapName,
        const FString& OutputPath, int32 ImageSize, FOutputDevice& Ar, int32 MotionSector = INDEX_NONE, float OpenCeiling = 0,
        bool bTextured = false, bool bRetryBlankOverview = true)
    {
        if (!World)
        {
            Ar.Log(TEXT("AA.ExportPerspectiveReview requires an active world."));
            return false;
        }
        if (ImageSize < 256 || ImageSize > 4096)
        {
            Ar.Log(TEXT("ImageSize must be between 256 and 4096."));
            return false;
        }
        if (!UDoomWadLoader::LoadDoomWadMap(WadPath, MapName))
        {
            Ar.Log(*FString::Printf(TEXT("AA.ExportPerspectiveReview failed to load map: %s (%s)"), *MapName, *WadPath));
            return false;
        }

        const bool bMotionReview = MotionSector != INDEX_NONE;
        const bool bScrollReview = bMotionReview && bTextured;
        FVector MotionFocus = FVector::ZeroVector;
        FVector MotionCamera = FVector::ZeroVector;
        double DoorDepth = 0.0;
        if (bMotionReview)
        {
            const FDoomWadMapData& Map = *UDoomWadLoader::GetLastWadMapData();
            if (!Map.Sectors.IsValidIndex(MotionSector)
                || Map.Sectors[MotionSector].FloorHeight != Map.Sectors[MotionSector].CeilingHeight
                || !FMath::IsFinite(OpenCeiling) || OpenCeiling <= Map.Sectors[MotionSector].CeilingHeight)
            {
                Ar.Log(TEXT("Motion review requires an initially closed sector and a higher open ceiling."));
                return false;
            }

            double Longest = 0.0;
            for (int32 Index = 0; Index < Map.Lines.Num(); ++Index)
            {
                const FWadLineDef& Raw = Map.Lines[Index];
                const int32 Front = Map.GetSectorForSide(Raw.Side[0]);
                const int32 Back = Map.GetSectorForSide(Raw.Side[1]);
                if (Front == INDEX_NONE || Back == INDEX_NONE || (Front == MotionSector) == (Back == MotionSector))
                {
                    continue;
                }
                FWadWallLine Line;
                if (!BuildWadWallLine(Map, Index, Line))
                {
                    continue;
                }
                const FVector2D Edge = Line.To - Line.From;
                const double Width = Edge.Size();
                if (Width <= Longest)
                {
                    continue;
                }
                Longest = Width;
                const double Direction = Front == MotionSector ? -1.0 : 1.0;
                const FVector TowardRoom(Edge.Y / Width * Direction, -Edge.X / Width * Direction, 0.0);
                const FVector2D Midpoint = (Line.From + Line.To) * 0.5;
                const double Z = (Map.Sectors[MotionSector].CeilingHeight + double(OpenCeiling)) * 0.5;
                MotionFocus = FVector(Midpoint.X, Midpoint.Y, Z);
                DoorDepth = FMath::Max(24.0, Width * 0.25);
                MotionCamera = MotionFocus + TowardRoom * DoorDepth;
                if (bScrollReview)
                {
                    MotionCamera.Z = Map.Sectors[MotionSector].FloorHeight + 46.0;
                    MotionFocus.Z = Map.Sectors[MotionSector].FloorHeight + 14.0;
                }
            }
            if (Longest == 0.0)
            {
                Ar.Log(TEXT("Motion review could not find a line shared with another sector."));
                return false;
            }
        }

        ASectorMeshBuilderActor* Builder = World->SpawnActor<ASectorMeshBuilderActor>(ASectorMeshBuilderActor::StaticClass(), FTransform::Identity);
        if (!Builder)
        {
            Ar.Log(TEXT("Failed to spawn SectorMeshBuilderActor."));
            return false;
        }
        Builder->SetBuildWallsEnabled(true);
        Builder->SetBuildFloorsEnabled(true);
        Builder->SetBuildCeilingsEnabled(bMotionReview);
        Builder->SetFloorVertexColor(FColor(184, 72, 72, 255));
        Builder->SetWallVertexColor(FColor(255, 236, 128, 255));
        Builder->BuildSectorsFromLastWad();

        UMaterialInterface* GeometryMaterial = bTextured ? nullptr : LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
        if (!bTextured && !GeometryMaterial)
        {
            Ar.Log(TEXT("Failed to load the geometry review material."));
            Builder->Destroy();
            return false;
        }

        TArray<UProceduralMeshComponent*> SolidReviewMeshes;
        TArray<UProceduralMeshComponent*> PaneReviewMeshes;
        const FName MotionSectorTag(*FString::Printf(TEXT("AA.Sector.%d"), MotionSector));
        const auto RefreshReviewMeshes = [&]()
        {
            SolidReviewMeshes.Reset();
            PaneReviewMeshes.Reset();
            TArray<UProceduralMeshComponent*> ReviewPMCs;
            Builder->GetComponents(ReviewPMCs);
            for (UProceduralMeshComponent* Mesh : ReviewPMCs)
            {
                if (!Mesh->IsVisible()
                    || (Mesh->ComponentHasTag(TEXT("AA.Sector.Ceiling")) && !Mesh->ComponentHasTag(MotionSectorTag)))
                {
                    continue;
                }
                const bool bPane = !bTextured && Mesh->ComponentHasTag(TEXT("AA.Wall.Translucent"));
                (bPane ? PaneReviewMeshes : SolidReviewMeshes).Add(Mesh);
                if (GeometryMaterial && !Mesh->ComponentHasTag(TEXT("AA.Wall.Masked")))
                {
                    for (int32 SectionIndex = 0; SectionIndex < Mesh->GetNumSections(); ++SectionIndex)
                    {
                        Mesh->SetMaterial(SectionIndex, GeometryMaterial);
                    }
                }
            }
        };
        RefreshReviewMeshes();
#if WITH_EDITOR
        FAssetCompilingManager::Get().FinishAllCompilation();
#endif

        const FBox Bounds = Builder->GetComponentsBoundingBox(true);
        if (!Bounds.IsValid)
        {
            Ar.Log(TEXT("Built mesh has invalid bounds."));
            Builder->Destroy();
            return false;
        }

        const FVector Center = Bounds.GetCenter();
        const FVector Extent = Bounds.GetExtent();
        const float RadiusXY = FMath::Max(FMath::Max(Extent.X, Extent.Y), 1.0f);
        const float Height = FMath::Max(Extent.Z, 64.0f);

        const FVector ViewDirection = FVector(-1.0f, -0.75f, 0.60f).GetSafeNormal();
        ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), Center, FRotator::ZeroRotator);
        if (!CaptureActor)
        {
            Ar.Log(TEXT("Failed to spawn SceneCapture2D."));
            Builder->Destroy();
            return false;
        }

        USceneCaptureComponent2D* CaptureComponent = CaptureActor->GetCaptureComponent2D();
        UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(CaptureActor);
        RenderTarget->RenderTargetFormat = RTF_RGBA32f;
        RenderTarget->ClearColor = FLinearColor::Black;
        const int32 TileImageSize = bMotionReview ? ImageSize : FMath::Max(ImageSize / 2, 256);
        RenderTarget->InitAutoFormat(TileImageSize, TileImageSize);
        RenderTarget->UpdateResourceImmediate(true);

        CaptureComponent->TextureTarget = RenderTarget;
        CaptureComponent->ProjectionType = ECameraProjectionMode::Perspective;
        CaptureComponent->FOVAngle = bScrollReview ? 100.0f : bMotionReview ? 132.0f : 42.0f;
        if (bTextured)
        {
            CaptureComponent->ShowFlags.SetEyeAdaptation(false);
        }
        CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        CaptureComponent->bCaptureEveryFrame = false;
        CaptureComponent->bCaptureOnMovement = false;

        const auto ShowMeshes = [CaptureComponent](const TArray<UProceduralMeshComponent*>& Meshes)
        {
            CaptureComponent->ClearShowOnlyComponents();
            for (UProceduralMeshComponent* Mesh : Meshes)
            {
                CaptureComponent->ShowOnlyComponent(Mesh);
            }
        };

        auto CaptureReviewPixels = [&](const FVector& FocusCenter, float FocusRadiusXY, TArray<FColor>& OutPixels) -> bool
        {
            const FVector CameraLocation = bMotionReview ? MotionCamera
                : FocusCenter + (ViewDirection * (FocusRadiusXY * 2.2f + Height * 0.65f)) + FVector(0.0f, 0.0f, Height * 0.75f);
            CaptureActor->SetActorLocation(CameraLocation);
            CaptureActor->SetActorRotation((FocusCenter - CameraLocation).Rotation());

            FTextureRenderTargetResource* Target = RenderTarget->GameThread_GetRenderTargetResource();
            if (!Target)
            {
                return false;
            }

            const auto CaptureBuffer = [&](ESceneCaptureSource Source, TArray<FLinearColor>& Buffer)
            {
                CaptureComponent->CaptureSource = Source;
                World->SendAllEndOfFrameUpdates();
                FlushRenderingCommands();
                CaptureComponent->CaptureScene();
                FlushRenderingCommands();
                return Target->ReadLinearColorPixels(Buffer) && Buffer.Num() == TileImageSize * TileImageSize;
            };
            if (bTextured)
            {
                ShowMeshes(SolidReviewMeshes);
                TArray<FLinearColor> Colors;
                if (!CaptureBuffer(ESceneCaptureSource::SCS_SceneColorHDRNoAlpha, Colors))
                {
                    return false;
                }
                OutPixels.SetNumUninitialized(Colors.Num());
                for (int32 Index = 0; Index < Colors.Num(); ++Index)
                {
                    Colors[Index].A = 1.0f;
                    OutPixels[Index] = Colors[Index].ToFColorSRGB();
                }
                return true;
            }

            // Use geometry buffers for the diagnostic colors independently of scene lights and materials.
            TArray<FLinearColor> Normals;
            TArray<FLinearColor> Depths;
            ShowMeshes(SolidReviewMeshes);
            if (!CaptureBuffer(ESceneCaptureSource::SCS_Normal, Normals)
                || !CaptureBuffer(ESceneCaptureSource::SCS_SceneDepth, Depths))
            {
                return false;
            }
            TArray<FLinearColor> PaneDepths;
            if (!PaneReviewMeshes.IsEmpty())
            {
                ShowMeshes(PaneReviewMeshes);
                if (!CaptureBuffer(ESceneCaptureSource::SCS_SceneDepth, PaneDepths))
                {
                    return false;
                }
            }

            const double MaxGeometryDepth = FVector::Distance(CameraLocation, Bounds.GetCenter()) + Extent.Size() + 1.0;
            const auto HasGeometryDepth = [MaxGeometryDepth](double Depth)
            {
                return Depth > 0.0 && Depth <= MaxGeometryDepth && FMath::IsFinite(Depth);
            };
            const FVector Light = FVector(-0.65, -0.45, 1.0).GetSafeNormal();
            OutPixels.SetNumUninitialized(Depths.Num());
            for (int32 Index = 0; Index < Depths.Num(); ++Index)
            {
                const double Depth = Depths[Index].R;
                FLinearColor Color(FColor(29, 34, 45));
                if (HasGeometryDepth(Depth))
                {
                    const FLinearColor& Sample = Normals[Index];
                    const FVector Normal = FVector(Sample.R, Sample.G, Sample.B).GetSafeNormal();
                    const bool bHorizontal = FMath::Abs(Normal.Z) > 0.7;
                    FLinearColor BaseColor = bHorizontal
                        ? FLinearColor(0.22f, 0.48f, 0.61f)
                        : FLinearColor(0.66f, 0.48f, 0.26f);
                    if (bMotionReview)
                    {
                        if (!bHorizontal && FMath::IsNearlyEqual(Depth, DoorDepth, 1.0))
                        {
                            BaseColor = FLinearColor(0.85f, 0.33f, 0.12f);
                        }
                        else if (Depth > DoorDepth + 1.0)
                        {
                            BaseColor *= 0.72f;
                        }
                    }
                    const float Shade = 0.45f + 0.55f * FMath::Abs(FVector::DotProduct(Normal, Light));
                    Color = BaseColor * Shade;
                }
                // Outline the panes in green while retaining the geometry visible behind them.
                if (PaneDepths.IsValidIndex(Index) && HasGeometryDepth(PaneDepths[Index].R)
                    && (!HasGeometryDepth(Depth) || PaneDepths[Index].R <= Depth))
                {
                    Color = FMath::Lerp(Color, FLinearColor(0.04f, 0.88f, 0.38f), 0.45f);
                }
                OutPixels[Index] = Color.ToFColorSRGB();
                OutPixels[Index].A = 255;
            }
            return true;
        };

        if (bScrollReview)
        {
            Builder->SetSectorCeilingZ(MotionSector, OpenCeiling);
            RefreshReviewMeshes();
            const TCHAR* FrameNames[] = {TEXT("before"), TEXT("after")};
            bool bSaved = true;
            for (int32 Frame = 0; Frame < UE_ARRAY_COUNT(FrameNames) && bSaved; ++Frame)
            {
                if (Frame == 1 && !Builder->SetSectorTextureScrollTexels(MotionSector, false, FVector2D(17.0, 11.0)))
                {
                    bSaved = false;
                    break;
                }
                TArray<FColor> Pixels;
                const FString Name = FString::Printf(TEXT("%s-%s.png"), *FPaths::GetBaseFilename(OutputPath), FrameNames[Frame]);
                const FString FramePath = FPaths::GetPath(OutputPath) / Name;
                bSaved = CaptureReviewPixels(MotionFocus, 0.0f, Pixels)
                    && WritePngToDisk(FramePath, Pixels, ImageSize, ImageSize, Ar);
                if (bSaved)
                {
                    Ar.Log(*FString::Printf(TEXT("AA.ExportTextureScrollReview saved %s: %s"), FrameNames[Frame], *FramePath));
                }
            }
            CaptureActor->Destroy();
            Builder->Destroy();
            return bSaved;
        }

        if (bMotionReview)
        {
            FVector2D Initial;
            bool bSaved = Builder->GetSectorSurfaceHeights(MotionSector, Initial);
            const TCHAR* StateNames[] = {TEXT("closed"), TEXT("partly-open"), TEXT("fully-open")};
            const double StateHeights[] = {Initial.Y, FMath::Lerp(Initial.Y, double(OpenCeiling), 0.45), OpenCeiling};
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(StateHeights) && bSaved; ++Index)
            {
                Builder->SetSectorCeilingZ(MotionSector, float(StateHeights[Index]));
                RefreshReviewMeshes();
                TArray<FColor> Pixels;
                const FString Filename = FString::Printf(TEXT("%s-%s.png"), *FPaths::GetBaseFilename(OutputPath), StateNames[Index]);
                const FString FramePath = FPaths::GetPath(OutputPath) / Filename;
                bSaved = CaptureReviewPixels(MotionFocus, 0.0f, Pixels)
                    && WritePngToDisk(FramePath, Pixels, ImageSize, ImageSize, Ar);
                if (bSaved)
                {
                    Ar.Log(*FString::Printf(TEXT("AA.ExportSectorMotionReview saved %s at ceiling Z %.2f: %s"),
                        StateNames[Index], StateHeights[Index], *FramePath));
                }
            }
            CaptureActor->Destroy();
            Builder->Destroy();
            return bSaved;
        }

        struct FReviewQuadrant
        {
            FString Label;
            FVector FocusCenter;
            float FocusRadiusXY = 0.0f;
            TArray<FColor> Pixels;
        };

        const float OffsetX = Extent.X * 0.42f;
        const float OffsetY = Extent.Y * 0.42f;
        const float FocusRadius = FMath::Max(FMath::Max(Extent.X, Extent.Y) * 0.58f, 256.0f);
        TArray<FReviewQuadrant> Quadrants;
        Quadrants.Reserve(4);
        Quadrants.Add({ TEXT("NW"), Center + FVector(-OffsetX,  OffsetY, 0.0f), FocusRadius });
        Quadrants.Add({ TEXT("NE"), Center + FVector( OffsetX,  OffsetY, 0.0f), FocusRadius });
        Quadrants.Add({ TEXT("SW"), Center + FVector(-OffsetX, -OffsetY, 0.0f), FocusRadius });
        Quadrants.Add({ TEXT("SE"), Center + FVector( OffsetX, -OffsetY, 0.0f), FocusRadius });

        for (FReviewQuadrant& Quadrant : Quadrants)
        {
            if (!CaptureReviewPixels(Quadrant.FocusCenter, Quadrant.FocusRadiusXY, Quadrant.Pixels))
            {
                Ar.Log(*FString::Printf(TEXT("Failed to capture perspective quadrant %s."), *Quadrant.Label));
                CaptureActor->Destroy();
                Builder->Destroy();
                return false;
            }
        }

        TArray<FColor> SheetPixels;
        SheetPixels.Init(FColor::Black, ImageSize * ImageSize);
        auto BlitTile = [&](const TArray<FColor>& TilePixels, int32 DestX, int32 DestY)
        {
            for (int32 Y = 0; Y < TileImageSize; ++Y)
            {
                for (int32 X = 0; X < TileImageSize; ++X)
                {
                    const int32 SheetX = DestX + X;
                    const int32 SheetY = DestY + Y;
                    if (SheetX < 0 || SheetY < 0 || SheetX >= ImageSize || SheetY >= ImageSize)
                    {
                        continue;
                    }
                    SheetPixels[(SheetY * ImageSize) + SheetX] = TilePixels[(Y * TileImageSize) + X];
                }
            }
        };

        if (Quadrants.Num() == 4)
        {
            BlitTile(Quadrants[0].Pixels, 0, 0);
            BlitTile(Quadrants[1].Pixels, TileImageSize, 0);
            BlitTile(Quadrants[2].Pixels, 0, TileImageSize);
            BlitTile(Quadrants[3].Pixels, TileImageSize, TileImageSize);
        }

        CaptureActor->Destroy();
        Builder->Destroy();

        // A fresh offscreen editor can return a blank first overview. Retry the capture once
        // with a new scene capture and builder before writing the image.
        if (bTextured && !Algo::AnyOf(SheetPixels, [](const FColor& Pixel)
            { return Pixel.R != 0 || Pixel.G != 0 || Pixel.B != 0; }))
        {
            if (bRetryBlankOverview)
            {
                Ar.Log(TEXT("Textured overview was blank. Retrying the capture."));
                return ExportPerspectiveReviewImage(World, WadPath, MapName, OutputPath, ImageSize, Ar,
                    INDEX_NONE, 0, true, false);
            }
            Ar.Log(TEXT("Textured overview was blank on both attempts."));
            return false;
        }

        const bool bSaved = WritePngToDisk(OutputPath, SheetPixels, ImageSize, ImageSize, Ar);
        if (bSaved)
        {
            Ar.Log(*FString::Printf(
                TEXT("AA.ExportPerspectiveReview succeeded: center=(%.1f,%.1f,%.1f) extent=(%.1f,%.1f,%.1f) tiles=%dx%d sheet=%dx%d out=%s"),
                Center.X, Center.Y, Center.Z,
                Extent.X, Extent.Y, Extent.Z,
                TileImageSize, TileImageSize,
                ImageSize, ImageSize,
                *FPaths::ConvertRelativePathToFull(OutputPath)));
        }

        return bSaved;
    }

    virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override
    {
        if (FParse::Command(&Cmd, TEXT("AA.LOADWAD")))
        {
            FString WadPath = FParse::Token(Cmd, false);
            FString MapName = FParse::Token(Cmd, false);
            if (WadPath.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.LoadWad <WadPathOrDir> <MapName>"));
                Ar.Log(TEXT("  Hint: If a directory is provided, the first .wad found will be used."));
                return true;
            }

            WadPath = ResolvePathOrProjectRelative(WadPath);

            // Allow passing a directory containing a WAD
            if (FPaths::DirectoryExists(WadPath))
            {
                FString Picked;
                if (!TryAutoPickWadFromDirectory(WadPath, Picked))
                {
                    Ar.Log(*FString::Printf(TEXT("No .wad files found under directory: %s"), *WadPath));
                    return true;
                }
                WadPath = Picked;
            }

            // Accept .map files if they are actual IWAD/PWAD containers
            if (FPaths::GetExtension(WadPath, true).Equals(TEXT(".map"), ESearchCase::IgnoreCase))
            {
                if (!LooksLikeWadFile(WadPath))
                {
                    Ar.Log(TEXT("AA.LoadWad expects a WAD (IWAD/PWAD). The provided .map is not a WAD container."));
                    Ar.Log(TEXT("Use: AA.LoadLegacyMap <PathToAAMap> for native A&A .MAP parsing."));
                    return true;
                }
                else
                {
                    Ar.Log(TEXT("Treating .map as WAD (IWAD/PWAD signature detected)."));
                }
            }

            if (!FPaths::FileExists(WadPath))
            {
                Ar.Log(*FString::Printf(TEXT("WAD file not found: %s"), *WadPath));
                return true; // handled
            }

            if (MapName.IsEmpty())
            {
                Ar.Log(TEXT("Missing <MapName>. Example: AA.LoadWad Content/TestData/doom2.wad MAP01"));
                return true;
            }

            MapName = SanitizeMapMarker(MapName);

            if (UDoomWadLoader::LoadDoomWadMap(WadPath, MapName))
            {
                Ar.Log(*FString::Printf(TEXT("AA.LoadWad succeeded: %s (%s)"), *MapName, *WadPath));
            }
            else
            {
                Ar.Log(*FString::Printf(TEXT("AA.LoadWad failed: %s (%s)"), *MapName, *WadPath));
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.LOADLEGACYMAP")))
        {
            FString MapPath = FParse::Token(Cmd, false);
            if (MapPath.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.LoadLegacyMap <PathToAAMap>"));
                return true;
            }
            MapPath = ResolvePathOrProjectRelative(MapPath);
            if (!FPaths::FileExists(MapPath))
            {
                Ar.Log(*FString::Printf(TEXT("Legacy map file not found: %s"), *MapPath));
                return true;
            }
            if (ULegacyMapLoader::LoadLegacyMapFromPath(MapPath))
            {
                const auto Stats = ULegacyMapLoader::GetLastLegacyStats();
                Ar.Log(*FString::Printf(TEXT("AA.LoadLegacyMap succeeded: V=%d L=%d Sides=%d Sec=%d (%s)"), Stats.NumVertices, Stats.NumLines, Stats.NumSides, Stats.NumSectors, *MapPath));
            }
            else
            {
                Ar.Log(*FString::Printf(TEXT("AA.LoadLegacyMap failed: %s"), *MapPath));
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.EXPORTMAPJSON")))
        {
            const FString Input = FParse::Token(Cmd, false);
            const FString Output = FParse::Token(Cmd, false);
            const FString Marker = FParse::Token(Cmd, false);
            if (Input.IsEmpty() || Output.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.ExportMapJson <MapOrWadPath> <OutputJson> [MapMarker]"));
                return true;
            }

            const FString InputPath = ResolvePathOrProjectRelative(Input);
            const FString OutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output);
            const FString MapName = Marker.IsEmpty() ? FPaths::GetBaseFilename(InputPath) : Marker;
            if (UDoomWadLoader::ExportDoomWadMapJson(InputPath, MapName, OutputPath))
            {
                Ar.Log(*FString::Printf(TEXT("AA.ExportMapJson succeeded: %s"), *OutputPath));
            }
            else
            {
                Ar.Log(*FString::Printf(TEXT("AA.ExportMapJson failed: %s"), *InputPath));
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.EXPORTTOPDOWNREVIEW")))
        {
            FString WadPath = FParse::Token(Cmd, false);
            FString MapName = FParse::Token(Cmd, false);
            FString OutputPath = FParse::Token(Cmd, false);
            FString SizeToken = FParse::Token(Cmd, false);
            const FString SectorToken = FParse::Token(Cmd, false);

            if (WadPath.IsEmpty() || MapName.IsEmpty() || OutputPath.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.ExportTopDownReview <WadPath> <MapName> <OutputPng> [ImageSize] [SectorId]"));
                return true;
            }

            WadPath = ResolvePathOrProjectRelative(WadPath);
            OutputPath = ResolvePathOrProjectRelative(OutputPath);
            MapName = SanitizeMapMarker(MapName);

            if (!FPaths::FileExists(WadPath))
            {
                Ar.Log(*FString::Printf(TEXT("WAD file not found: %s"), *WadPath));
                return true;
            }

            const int32 ImageSize = SizeToken.IsEmpty() ? 2048 : FMath::Max(256, FCString::Atoi(*SizeToken));
            int32 SectorFilter = INDEX_NONE;
            if (!SectorToken.IsEmpty() && (!LexTryParseString(SectorFilter, *SectorToken) || SectorFilter < 0))
            {
                Ar.Log(TEXT("SectorId must be a nonnegative integer."));
                return true;
            }
            if (!UDoomWadLoader::LoadDoomWadMap(WadPath, MapName))
            {
                Ar.Log(*FString::Printf(TEXT("AA.ExportTopDownReview failed to load map: %s (%s)"), *MapName, *WadPath));
                return true;
            }

            if (!ExportTopDownReviewImage(OutputPath, ImageSize, SectorFilter, Ar))
            {
                Ar.Log(TEXT("AA.ExportTopDownReview failed during image export."));
                return true;
            }

            return true;
        }
        else if (const bool bTextureScrollReview = FParse::Command(&Cmd, TEXT("AA.EXPORTTEXTURESCROLLREVIEW"));
            bTextureScrollReview || FParse::Command(&Cmd, TEXT("AA.EXPORTSECTORMOTIONREVIEW")))
        {
            const FString MapPath = ResolvePathOrProjectRelative(FParse::Token(Cmd, false));
            const FString MapName = SanitizeMapMarker(FParse::Token(Cmd, false));
            const FString Output = FParse::Token(Cmd, false);
            const FString SectorToken = FParse::Token(Cmd, false);
            const FString HeightToken = FParse::Token(Cmd, false);
            const FString SizeToken = FParse::Token(Cmd, false);
            int32 SectorIndex = INDEX_NONE;
            float OpenCeiling = 0;
            if (MapPath.IsEmpty() || MapName.IsEmpty() || Output.IsEmpty()
                || !LexTryParseString(SectorIndex, *SectorToken) || SectorIndex < 0
                || !LexTryParseString(OpenCeiling, *HeightToken))
            {
                Ar.Log(*FString::Printf(TEXT("Usage: %s <MapPath> <MapName> <OutputPng> <SectorId> <OpenCeilingZ> [ImageSize]"),
                    bTextureScrollReview ? TEXT("AA.ExportTextureScrollReview") : TEXT("AA.ExportSectorMotionReview")));
                return true;
            }
            const int32 ImageSize = SizeToken.IsEmpty() ? 750 : FCString::Atoi(*SizeToken);
            const FString OutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output);
            if (!ExportPerspectiveReviewImage(InWorld, MapPath, MapName, OutputPath, ImageSize, Ar, SectorIndex, OpenCeiling, bTextureScrollReview))
            {
                Ar.Log(bTextureScrollReview ? TEXT("AA.ExportTextureScrollReview failed.") : TEXT("AA.ExportSectorMotionReview failed."));
            }
            return true;
        }
#if WITH_EDITOR
        else if (FParse::Command(&Cmd, TEXT("AA.IMPORTNATIVEWAD")))
        {
            const FString WadPath = ResolvePathOrProjectRelative(FParse::Token(Cmd, false));
            const FString MapName = SanitizeMapMarker(FParse::Token(Cmd, false));
            const FString Package = FParse::Token(Cmd, false);
            if (WadPath.IsEmpty() || MapName.IsEmpty() || Package.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.ImportNativeWad <WadPath> <MapName> </Game/MapPackage>"));
                return true;
            }
            if (!UDoomWadLoader::LoadDoomWadMap(WadPath, MapName) || !SaveNativeWadLevel(Package, WadPath, Ar))
            {
                UE_LOG(LogTemp, Error, TEXT("AA.ImportNativeWad failed for %s."), *Package);
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.IMPORTWADDOORS")))
        {
            const FString WadPath = ResolvePathOrProjectRelative(FParse::Token(Cmd, false));
            const FString MapName = SanitizeMapMarker(FParse::Token(Cmd, false));
            const bool bSaved = !WadPath.IsEmpty() && !MapName.IsEmpty() && UDoomWadLoader::LoadDoomWadMap(WadPath, MapName)
                && SaveNativeWadDoors(InWorld, Ar);
            if (!bSaved)
            {
                UE_LOG(LogTemp, Error, TEXT("AA.ImportWadDoors failed. Usage: AA.ImportWadDoors <WadPath> <MapName>"));
            }
            UE_LOG(LogTemp, Display, TEXT("Native WAD door import finished"));
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.IMPORTWADGAMEPLAY")))
        {
            const FString WadPath = ResolvePathOrProjectRelative(FParse::Token(Cmd, false));
            const FString MapName = SanitizeMapMarker(FParse::Token(Cmd, false));
            if (WadPath.IsEmpty() || MapName.IsEmpty() || !UDoomWadLoader::LoadDoomWadMap(WadPath, MapName)
                || !SaveNativeWadGameplay(InWorld, WadPath, Ar))
            {
                UE_LOG(LogTemp, Error, TEXT("AA.ImportWadGameplay failed. Usage: AA.ImportWadGameplay <WadPath> <MapName>"));
            }
            UE_LOG(LogTemp, Display, TEXT("Native WAD gameplay import finished"));
            return true;
        }
#endif
        else if (FParse::Command(&Cmd, TEXT("AA.USEWADDOOR")))
        {
            const FString SectorToken = FParse::Token(Cmd, false);
            int32 Sector = INDEX_NONE;
            if (InWorld && InWorld->IsGameWorld() && LexTryParseString(Sector, *SectorToken) && Sector >= 0)
            {
                for (TActorIterator<ANativeWadLevel> Actor(InWorld); Actor; ++Actor)
                {
                    if (Actor->UseDoor(Sector))
                    {
                        Ar.Logf(TEXT("Activated WAD door sector %d."), Sector);
                        return true;
                    }
                }
            }
            Ar.Log(TEXT("AA.UseWadDoor <Sector> needs an unlocked native door in a running game."));
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.EXPORTNATIVEL30REVIEW")))
        {
            const FString Output = FParse::Token(Cmd, false);
            const FString SizeToken = FParse::Token(Cmd, false);
            const int32 ImageSize = SizeToken.IsEmpty() ? 900 : FCString::Atoi(*SizeToken);
            if (Output.IsEmpty() || !StartNativeL30Review(InWorld, FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output), ImageSize))
            {
                UE_LOG(LogTemp, Error, TEXT("AA.ExportNativeL30Review requires the saved L30 level and <OutputPng> [ImageSize]."));
                UE_LOG(LogTemp, Display, TEXT("Native L30 review finished"));
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.EXPORTNATIVEL30DOORREVIEW")))
        {
            const FString Output = FParse::Token(Cmd, false);
            const FString SizeToken = FParse::Token(Cmd, false);
            const int32 ImageSize = SizeToken.IsEmpty() ? 750 : FCString::Atoi(*SizeToken);
            if (Output.IsEmpty() || !StartNativeL30DoorReview(InWorld, FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Output), ImageSize))
            {
                UE_LOG(LogTemp, Error, TEXT("AA.ExportNativeL30DoorReview requires a running saved L30 and <OutputPng> [ImageSize]."));
                UE_LOG(LogTemp, Display, TEXT("Native L30 door review finished"));
            }
            return true;
        }
        else if (const bool bLighting = FParse::Command(&Cmd, TEXT("AA.EXPORTNATIVEWADLIGHTINGREVIEW"));
            bLighting || FParse::Command(&Cmd, TEXT("AA.EXPORTNATIVEL30PLAYERREVIEW")))
        {
            const FString File = FParse::Token(Cmd, false);
            const FString Size = FParse::Token(Cmd, false);
            const FString Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File);
            const int32 ImageSize = Size.IsEmpty() ? 720 : FCString::Atoi(*Size);
            const bool bStarted = !File.IsEmpty() && (bLighting ? StartNativeWadLightingReview(InWorld, Output, ImageSize)
                : StartNativeL30PlayerReview(InWorld, Output, ImageSize));
            if (!bStarted)
            {
                UE_LOG(LogTemp, Error, TEXT("The native review could not start on the opened game level."));
                UE_LOG(LogTemp, Display, TEXT("%s"), bLighting ? TEXT("Native WAD lighting review finished") : TEXT("Native L30 player review finished"));
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.EXPORTNATIVEWADPORTREVIEW")))
        {
            const FString Review = FParse::Token(Cmd, false).ToLower();
            const FString File = FParse::Token(Cmd, false);
            const FString Size = FParse::Token(Cmd, false);
            const FString Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File);
            const int32 ImageSize = Size.IsEmpty() ? 720 : FCString::Atoi(*Size);
            const bool bStarted = !File.IsEmpty() && (Review == TEXT("sky")
                ? StartNativeWadLightingReview(InWorld, Output, ImageSize, true)
                : (Review == TEXT("route") || Review == TEXT("link"))
                    && StartNativeL30WalkReview(InWorld, Output, ImageSize, Review == TEXT("link")));
            if (!bStarted)
            {
                UE_LOG(LogTemp, Error, TEXT("AA.ExportNativeWadPortReview <Route|Sky|Link> <OutputPng> [ImageSize] requires the corresponding native game level."));
                UE_LOG(LogTemp, Display, TEXT("Native WAD port review finished"));
            }
            return true;
        }
        else if (FParse::Command(&Cmd, TEXT("AA.EXPORTPERSPECTIVEREVIEW")))
        {
            FString WadPath = FParse::Token(Cmd, false);
            FString MapName = FParse::Token(Cmd, false);
            FString OutputPath = FParse::Token(Cmd, false);
            FString SizeToken = FParse::Token(Cmd, false);
            const bool bTextured = FParse::Command(&Cmd, TEXT("TEXTURED"));

            if (WadPath.IsEmpty() || MapName.IsEmpty() || OutputPath.IsEmpty())
            {
                Ar.Log(TEXT("Usage: AA.ExportPerspectiveReview <WadPath> <MapName> <OutputPng> [ImageSize] [Textured]"));
                return true;
            }

            WadPath = ResolvePathOrProjectRelative(WadPath);
            OutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutputPath);
            MapName = SanitizeMapMarker(MapName);

            if (!FPaths::FileExists(WadPath))
            {
                Ar.Log(*FString::Printf(TEXT("WAD file not found: %s"), *WadPath));
                return true;
            }

            const int32 ImageSize = SizeToken.IsEmpty() ? 1600 : FMath::Max(256, FCString::Atoi(*SizeToken));
            if (!ExportPerspectiveReviewImage(InWorld, WadPath, MapName, OutputPath, ImageSize, Ar, INDEX_NONE, 0, bTextured))
            {
                Ar.Log(TEXT("AA.ExportPerspectiveReview failed during capture."));
            }
            return true;
        }
        return false; // not our command
    }
};

IMPLEMENT_MODULE(FAAOriginalModule, AAOriginal)
