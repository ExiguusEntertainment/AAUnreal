#include "SectorMeshBuilderActor.h"
#include "DoomWadLoader.h"
#include "DoomWadWalls.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "KismetProceduralMeshLibrary.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

ASectorMeshBuilderActor::ASectorMeshBuilderActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(SceneRoot);

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> VertexColorMatFinder(
        TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
    if (VertexColorMatFinder.Succeeded())
    {
        if (!FloorMaterial) FloorMaterial = VertexColorMatFinder.Object;
        if (!CeilingMaterial) CeilingMaterial = VertexColorMatFinder.Object;
        if (!WallMaterial) WallMaterial = VertexColorMatFinder.Object;
    }
}

namespace
{
    constexpr const TCHAR* OpaqueSurfaceMaterialPath = TEXT("/Game/AAReview/M_AAWallOpaque.M_AAWallOpaque");
    const FName SurfaceTextureParameter(TEXT("SurfaceTexture"));
    const FName SurfaceOffsetParameter(TEXT("SurfaceOffset"));
    constexpr int32 AAPictureResourceHeaderSize = 14;
    constexpr int32 AAPictureResourceEntrySize = 39;
    struct FPictureResEntry
    {
        int32 FileOffset = 0;
        int32 Size = 0;
    };

    struct FPictureResourceCache
    {
        bool bInitialized = false;
        bool bInitSucceeded = false;
        TMap<FString, FPictureResEntry> Entries;
        TArray<FColor> Palette;
        TMap<FString, TWeakObjectPtr<UTexture2D>> TextureCache;
    };

    static FPictureResourceCache GPictureResourceCache;

    static FString NormalizeTextureName(const FString& TextureName)
    {
        return TextureName.TrimStartAndEnd().ToUpper().Left(8);
    }

    static bool IsUsableTextureName(const FString& TextureName)
    {
        const FString Normalized = NormalizeTextureName(TextureName);
        return !Normalized.IsEmpty() && Normalized != TEXT("-");
    }

    static FString GetArtFinalPath(const FString& LeafName)
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal"), LeafName));
    }

    static bool LoadGamePalette(TArray<FColor>& OutPalette)
    {
        TArray<uint8> RawBytes;
        if (!FFileHelper::LoadFileToArray(RawBytes, *GetArtFinalPath(TEXT("GAME.PAL"))) || RawBytes.Num() < (256 * 3))
        {
            return false;
        }

        OutPalette.Reset(256);
        for (int32 PaletteIndex = 0; PaletteIndex < 256; ++PaletteIndex)
        {
            const uint8 R = uint8(((int32)RawBytes[(PaletteIndex * 3) + 0] & 0x3F) * 255 / 63);
            const uint8 G = uint8(((int32)RawBytes[(PaletteIndex * 3) + 1] & 0x3F) * 255 / 63);
            const uint8 B = uint8(((int32)RawBytes[(PaletteIndex * 3) + 2] & 0x3F) * 255 / 63);
            OutPalette.Add(FColor(R, G, B, 255));
        }
        return true;
    }

    static bool LoadPictureResourceIndex(TMap<FString, FPictureResEntry>& OutEntries)
    {
        TArray<uint8> ResourceBytes;
        if (!FFileHelper::LoadFileToArray(ResourceBytes, *GetArtFinalPath(TEXT("PICS.RES"))) || ResourceBytes.Num() < AAPictureResourceHeaderSize)
        {
            return false;
        }

        const uint32 UniqueId = *(const uint32*)(ResourceBytes.GetData() + 0);
        const uint32 IndexOffset = *(const uint32*)(ResourceBytes.GetData() + 4);
        const uint32 IndexSize = *(const uint32*)(ResourceBytes.GetData() + 8);
        const uint16 NumEntries = *(const uint16*)(ResourceBytes.GetData() + 12);
        if (UniqueId != 0x21736552u || IndexOffset + IndexSize > (uint32)ResourceBytes.Num() || IndexSize != (uint32)NumEntries * AAPictureResourceEntrySize)
        {
            return false;
        }

        OutEntries.Reset();
        OutEntries.Reserve(NumEntries);
        for (uint16 EntryIndex = 0; EntryIndex < NumEntries; ++EntryIndex)
        {
            const uint8* EntryPtr = ResourceBytes.GetData() + IndexOffset + (EntryIndex * AAPictureResourceEntrySize);
            FString ResourceName = FString(ANSI_TO_TCHAR(reinterpret_cast<const char*>(EntryPtr + 4))).Left(14).TrimEnd();
            ResourceName = NormalizeTextureName(ResourceName);
            if (ResourceName.IsEmpty())
            {
                continue;
            }

            FPictureResEntry& Entry = OutEntries.FindOrAdd(ResourceName);
            Entry.FileOffset = *(const int32*)(EntryPtr + 18);
            Entry.Size = *(const int32*)(EntryPtr + 22);
        }
        return OutEntries.Num() > 0;
    }

    static bool EnsurePictureResourceCache()
    {
        if (GPictureResourceCache.bInitialized)
        {
            return GPictureResourceCache.bInitSucceeded;
        }

        GPictureResourceCache.bInitialized = true;
        GPictureResourceCache.bInitSucceeded =
            LoadGamePalette(GPictureResourceCache.Palette) &&
            LoadPictureResourceIndex(GPictureResourceCache.Entries);
        return GPictureResourceCache.bInitSucceeded;
    }

    static UTexture2D* LoadSurfaceTexture(const FString& TextureName)
    {
        const FString Normalized = NormalizeTextureName(TextureName);
        if (!IsUsableTextureName(Normalized) || !EnsurePictureResourceCache())
        {
            return nullptr;
        }

        if (const TWeakObjectPtr<UTexture2D>* ExistingTexture = GPictureResourceCache.TextureCache.Find(Normalized);
            ExistingTexture && ExistingTexture->IsValid())
        {
            return ExistingTexture->Get();
        }

        const FPictureResEntry* Entry = GPictureResourceCache.Entries.Find(Normalized);
        if (!Entry)
        {
            return nullptr;
        }

        TArray<uint8> ResourceBytes;
        if (!FFileHelper::LoadFileToArray(ResourceBytes, *GetArtFinalPath(TEXT("PICS.RES"))))
        {
            return nullptr;
        }
        if (Entry->FileOffset < 0 || Entry->Size < 4 || int64(Entry->FileOffset) + Entry->Size > ResourceBytes.Num())
        {
            return nullptr;
        }

        const uint8* DataPtr = ResourceBytes.GetData() + Entry->FileOffset;
        // The map renderer stores height first, then width, followed by one column at a time.
        const int32 Height = DataPtr[0] | (int32(DataPtr[1]) << 8);
        const int32 Width = DataPtr[2] | (int32(DataPtr[3]) << 8);
        const int64 PixelCount = int64(Width) * Height;
        if (Width <= 0 || Height <= 0 || PixelCount > Entry->Size - 4)
        {
            return nullptr;
        }

        TArray<FColor> Pixels;
        Pixels.SetNumUninitialized(int32(PixelCount));
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                const uint8 PaletteIndex = DataPtr[4 + X * Height + Y];
                FColor& Pixel = Pixels[Y * Width + X];
                Pixel = GPictureResourceCache.Palette[PaletteIndex];
                Pixel.A = PaletteIndex == 0 ? 0 : 255;
            }
        }

        UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (!Texture)
        {
            return nullptr;
        }

        Texture->MipGenSettings = TMGS_NoMipmaps;
        Texture->NeverStream = true;
        Texture->Filter = TF_Nearest;
        Texture->AddressX = TA_Wrap;
        Texture->AddressY = TA_Wrap;
        Texture->SRGB = true;

        FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
        void* DestData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(DestData, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
        Mip.BulkData.Unlock();
        Texture->UpdateResource();

        GPictureResourceCache.TextureCache.Add(Normalized, Texture);
        return Texture;
    }

    static FVector2D TextureDimensions(const UTexture2D* Texture)
    {
        return Texture ? FVector2D(Texture->GetSizeX(), Texture->GetSizeY()) : FVector2D(64.0, 64.0);
    }

    static const UTexture2D* GetBoundSurfaceTexture(UMaterialInstanceDynamic* Material)
    {
        return Material ? Cast<UTexture2D>(Material->K2_GetTextureParameterValue(SurfaceTextureParameter)) : nullptr;
    }

    static void ApplySurfaceOffset(UMaterialInstanceDynamic& Material, FVector2D Texels)
    {
        const FVector2D Size = TextureDimensions(GetBoundSurfaceTexture(&Material));
        Material.SetVectorParameterValue(SurfaceOffsetParameter, FLinearColor(
            float(FMath::Fmod(Texels.X, Size.X) / Size.X), float(FMath::Fmod(Texels.Y, Size.Y) / Size.Y), 0.0f, 0.0f));
    }

    static UMaterialInstanceDynamic* MakePlaneMaterial(UObject* Owner, const FString& TextureName)
    {
        UTexture2D* Texture = LoadSurfaceTexture(TextureName);
        UMaterialInterface* Parent = Texture ? LoadObject<UMaterialInterface>(nullptr, OpaqueSurfaceMaterialPath) : nullptr;
        if (!Parent)
        {
            return nullptr;
        }
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, Owner);
        Material->SetTextureParameterValue(SurfaceTextureParameter, Texture);
        return Material;
    }

}

UTexture2D* LoadWadSurfaceTexture(const FString& TextureName)
{
    return LoadSurfaceTexture(TextureName);
}

void ASectorMeshBuilderActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
}

void ASectorMeshBuilderActor::ClearChildren()
{
    if (!bClearPrevious) return;

    for (TObjectPtr<USceneComponent>& Root : FloorRoots)
    {
        if (Root)
        {
            Root->DestroyComponent();
        }
    }

    for (TObjectPtr<USceneComponent>& Root : CeilRoots)
    {
        if (Root)
        {
            Root->DestroyComponent();
        }
    }

    TArray<UActorComponent*> Comps;
    GetComponents(Comps);
    for (UActorComponent* C : Comps)
    {
        if (C && C->IsA(UProceduralMeshComponent::StaticClass()))
        {
            C->DestroyComponent();
        }
    }

    FloorRoots.Reset();
    CeilRoots.Reset();
    FloorPMCs.Reset();
    CeilPMCs.Reset();
}

static void AppendMeshToPMC(UProceduralMeshComponent* PMC, const TArray<FVector>& Verts, const TArray<int32>& Indices,
                            const TArray<FVector>& Normals, const TArray<FVector2D>& UV0, const TArray<FColor>& Colors, const TArray<FProcMeshTangent>& Tangents)
{
    int32 Section = PMC->GetNumSections();
    PMC->CreateMeshSection(Section, Verts, Indices, Normals, UV0, Colors, Tangents, true);
}

static void BuildFootprintMesh(UProceduralMeshComponent* PMC, const FDoomWadSectorFootprint& Footprint,
                              bool bCeiling, float UnitsToUU, const FColor& VertexColor)
{
    TArray<FVector> Vertices;
    TArray<FVector2D> UVs;
    TArray<FColor> Colors;
    Vertices.Reserve(Footprint.Vertices.Num());
    UVs.Reserve(Footprint.Vertices.Num());
    const FVector2D TextureSize = TextureDimensions(GetBoundSurfaceTexture(Cast<UMaterialInstanceDynamic>(PMC->GetMaterial(0))));
    Colors.Init(VertexColor, Footprint.Vertices.Num());
    for (const FVector2D& Point : Footprint.Vertices)
    {
        Vertices.Emplace(Point.X * UnitsToUU, Point.Y * UnitsToUU, 0.0);
        UVs.Emplace(Point.X / TextureSize.X, Point.Y / TextureSize.Y);
    }

    TArray<int32> Indices = Footprint.Triangles;
    // Unreal mesh winding faces the floor up and the ceiling down.
    if (!bCeiling)
    {
        for (int32 Index = 0; Index < Indices.Num(); Index += 3)
        {
            Swap(Indices[Index + 1], Indices[Index + 2]);
        }
    }

    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;
    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, Indices, UVs, Normals, Tangents);
    AppendMeshToPMC(PMC, Vertices, Indices, Normals, UVs, Colors, Tangents);
}

namespace
{
    constexpr int32 WallLinesPerBatch = 64;
    constexpr uint64 SharedWallSection = MAX_uint64;

    uint64 WallSurfaceKey(int32 SideIndex, EWadWallBand Band)
    {
        return (uint64(SideIndex) << 2) | uint64(Band);
    }

    struct FWallMeshSection
    {
        FString TextureName;
        EWadWallBlend Blend = EWadWallBlend::Opaque;
        uint64 ScrollKey = SharedWallSection;
        UTexture2D* Texture = nullptr;
        TArray<FVector> Vertices;
        TArray<int32> Indices;
        TArray<FVector> Normals;
        TArray<FVector2D> UVs;
        TArray<FColor> Colors;
        TArray<FProcMeshTangent> Tangents;
    };

    const TCHAR* WallComponentTag(EWadWallBlend Blend)
    {
        switch (Blend)
        {
        case EWadWallBlend::Masked: return TEXT("AA.Wall.Masked");
        case EWadWallBlend::Translucent: return TEXT("AA.Wall.Translucent");
        default: return TEXT("AA.Wall.Opaque");
        }
    }

    const TCHAR* WallMaterialPath(EWadWallBlend Blend)
    {
        switch (Blend)
        {
        case EWadWallBlend::Masked: return TEXT("/Game/AAReview/M_AAWallMasked.M_AAWallMasked");
        case EWadWallBlend::Translucent: return TEXT("/Game/AAReview/M_AAWallTranslucent.M_AAWallTranslucent");
        default: return OpaqueSurfaceMaterialPath;
        }
    }

    void AppendWallQuad(TArray<FVector>& Vertices, TArray<int32>& Indices, const FVector2D& From, const FVector2D& To,
        double Bottom, double Top, double UnitsToUU, bool bTwoSidedCollision)
    {
        const int32 First = Vertices.Num();
        Vertices.Emplace(From.X * UnitsToUU, From.Y * UnitsToUU, Bottom * UnitsToUU);
        Vertices.Emplace(From.X * UnitsToUU, From.Y * UnitsToUU, Top * UnitsToUU);
        Vertices.Emplace(To.X * UnitsToUU, To.Y * UnitsToUU, Top * UnitsToUU);
        Vertices.Emplace(To.X * UnitsToUU, To.Y * UnitsToUU, Bottom * UnitsToUU);
        Indices.Append({First, First + 1, First + 2, First, First + 2, First + 3});
        if (bTwoSidedCollision)
        {
            Indices.Append({First, First + 2, First + 1, First, First + 3, First + 2});
        }
    }
}

void ASectorMeshBuilderActor::BuildWalls(const FDoomWadMapData& Map)
{
    WallMap = Map;
    WallBatches.SetNum(FMath::DivideAndRoundUp(Map.Lines.Num(), WallLinesPerBatch));
    WallBatchesBySector.SetNum(Map.Sectors.Num());
    WallBatchesBySide.SetNum(Map.Sides.Num());
    for (int32 Index = 0; Index < Map.Lines.Num(); ++Index)
    {
        const FWadLineDef& Line = Map.Lines[Index];
        for (int32 Side : Line.Side)
        {
            if (WallBatchesBySide.IsValidIndex(Side))
            {
                WallBatchesBySide[Side].AddUnique(Index / WallLinesPerBatch);
            }
            const int32 SectorIndex = Map.GetSectorForSide(Side);
            if (WallBatchesBySector.IsValidIndex(SectorIndex))
            {
                WallBatchesBySector[SectorIndex].AddUnique(Index / WallLinesPerBatch);
            }
        }
    }
    for (int32 Index = 0; Index < WallBatches.Num(); ++Index)
    {
        BuildWallBatch(Index);
    }
}

void ASectorMeshBuilderActor::BuildWallBatch(int32 BatchIndex)
{
    TArray<TWeakObjectPtr<UProceduralMeshComponent>>& BatchMeshes = WallBatches[BatchIndex];
    for (const TWeakObjectPtr<UProceduralMeshComponent>& Mesh : BatchMeshes)
    {
        if (Mesh.IsValid())
        {
            Mesh->DestroyComponent();
        }
    }
    BatchMeshes.Reset();

    TArray<FWallMeshSection> Sections;
    TArray<FVector> CollisionVertices;
    TArray<int32> CollisionIndices;
    TArray<FVector> CreatureVertices;
    TArray<int32> CreatureIndices;

    const int32 StartLine = BatchIndex * WallLinesPerBatch;
    const int32 EndLine = FMath::Min(StartLine + WallLinesPerBatch, WallMap.Lines.Num());
    for (int32 LineIndex = StartLine; LineIndex < EndLine; ++LineIndex)
    {
        FWadWallLine Line;
        if (!BuildWadWallLine(WallMap, LineIndex, Line, SectorHeights)
            || (SectorFilter >= 0 && Line.Sectors.X != SectorFilter && Line.Sectors.Y != SectorFilter))
        {
            continue;
        }

        for (const FWadWallSurface& Surface : Line.Surfaces)
        {
            if (SectorFilter >= 0 && Surface.SectorIndex != SectorFilter)
            {
                continue;
            }
            FString Upper;
            FString Lower;
            FString Middle;
            FVector2D Offset;
            UDoomWadLoader::GetWadSideTextureNames(WallMap.Sides[Surface.SideIndex], Upper, Lower, Middle, Offset);
            const FString& TextureName = Surface.Band == EWadWallBand::Upper ? Upper
                : Surface.Band == EWadWallBand::Lower ? Lower : Middle;

            const uint64 SurfaceKey = WallSurfaceKey(Surface.SideIndex, Surface.Band);
            const uint64 ScrollKey = WallTextureScrolls.Contains(SurfaceKey) ? SurfaceKey : SharedWallSection;
            FWallMeshSection* Section = Sections.FindByPredicate([&](const FWallMeshSection& Candidate)
            {
                return Candidate.Blend == Surface.Blend && Candidate.TextureName == TextureName && Candidate.ScrollKey == ScrollKey;
            });
            if (!Section)
            {
                Section = &Sections.AddDefaulted_GetRef();
                Section->Blend = Surface.Blend;
                Section->TextureName = TextureName;
                Section->ScrollKey = ScrollKey;
                Section->Texture = LoadSurfaceTexture(TextureName);
            }

            const FVector2D& From = Surface.bBack ? Line.To : Line.From;
            const FVector2D& To = Surface.bBack ? Line.From : Line.To;
            AppendWallQuad(Section->Vertices, Section->Indices, From, To, Surface.Bottom, Surface.Top, BuiltUnitsToUU, false);

            const FVector2D Direction = (To - From).GetSafeNormal();
            const FVector Normal(Direction.Y, -Direction.X, 0.0);
            const FProcMeshTangent Tangent(FVector(Direction.X, Direction.Y, 0.0), false);
            const FVector2D Size = TextureDimensions(Section->Texture);
            const double U0 = Offset.X / Size.X;
            const double U1 = (Offset.X + (To - From).Size()) / Size.X;
            const double VBottom = (Offset.Y + Surface.VOrigin - Surface.Bottom) / Size.Y;
            const double VTop = (Offset.Y + Surface.VOrigin - Surface.Top) / Size.Y;
            Section->UVs.Append({FVector2D(U0, VBottom), FVector2D(U0, VTop),
                FVector2D(U1, VTop), FVector2D(U1, VBottom)});
            for (int32 Corner = 0; Corner < 4; ++Corner)
            {
                Section->Normals.Add(Normal);
                Section->Colors.Add(WallVertexColor);
                Section->Tangents.Add(Tangent);
            }
        }

        for (const FWadWallCollision& Span : Line.Collision)
        {
            TArray<FVector>& Vertices = Span.bCreatureOnly ? CreatureVertices : CollisionVertices;
            TArray<int32>& Indices = Span.bCreatureOnly ? CreatureIndices : CollisionIndices;
            AppendWallQuad(Vertices, Indices, Line.From, Line.To, Span.Bottom, Span.Top, BuiltUnitsToUU, true);
        }
    }

    for (EWadWallBlend Blend : {EWadWallBlend::Opaque, EWadWallBlend::Masked, EWadWallBlend::Translucent})
    {
        if (!Sections.ContainsByPredicate([Blend](const FWallMeshSection& Section) { return Section.Blend == Blend; }))
        {
            continue;
        }
        UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, WallMaterialPath(Blend));
        if (!Parent && Blend != EWadWallBlend::Opaque)
        {
            UE_LOG(LogTemp, Warning, TEXT("Missing wall material %s. Its walls keep their collision."), WallMaterialPath(Blend));
            continue;
        }

        UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(this);
        Mesh->ComponentTags.Add(FName(WallComponentTag(Blend)));
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetupAttachment(SceneRoot);
        for (const FWallMeshSection& Section : Sections)
        {
            if (Section.Blend != Blend)
            {
                continue;
            }
            const int32 SectionIndex = Mesh->GetNumSections();
            Mesh->CreateMeshSection(SectionIndex, Section.Vertices, Section.Indices, Section.Normals, Section.UVs,
                Section.Colors, Section.Tangents, false);
            if (Parent)
            {
                const FString Key = FString::Printf(TEXT("%d:%s:%llu"), int32(Blend),
                    *Section.TextureName, Section.ScrollKey);
                TObjectPtr<UMaterialInterface>& Material = WallMaterials.FindOrAdd(Key);
                if (!Material)
                {
                    UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, this);
                    if (Section.Texture)
                    {
                        Instance->SetTextureParameterValue(SurfaceTextureParameter, Section.Texture);
                    }
                    else
                    {
                        Instance->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor(WallVertexColor));
                    }
                    Material = Instance;
                }
                if (FWallTextureScroll* Scroll = WallTextureScrolls.Find(Section.ScrollKey))
                {
                    if (UMaterialInstanceDynamic* Instance = Cast<UMaterialInstanceDynamic>(Material.Get()))
                    {
                        ApplySurfaceOffset(*Instance, Scroll->Offset);
                        Scroll->Materials.AddUnique(Instance);
                    }
                }
                Mesh->SetMaterial(SectionIndex, Material);
            }
            else
            {
                Mesh->SetMaterial(SectionIndex, WallMaterial);
            }
        }
        Mesh->RegisterComponent();
        BatchMeshes.Add(Mesh);
    }

    const auto BuildCollision = [this, &BatchMeshes](const TCHAR* Tag, const TArray<FVector>& Vertices, const TArray<int32>& Indices, bool bCreatureOnly)
    {
        if (Indices.IsEmpty())
        {
            return;
        }
        UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(this);
        Mesh->ComponentTags.Add(FName(Tag));
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetVisibility(false);
        Mesh->SetHiddenInGame(true);
        Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
        if (bCreatureOnly)
        {
            // A creature collision channel can opt into these spans without blocking the player.
            Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
        Mesh->SetupAttachment(SceneRoot);
        Mesh->CreateMeshSection(0, Vertices, Indices, {}, {}, {}, {}, true);
        Mesh->RegisterComponent();
        BatchMeshes.Add(Mesh);
    };
    BuildCollision(TEXT("AA.Wall.Collision"), CollisionVertices, CollisionIndices, false);
    BuildCollision(TEXT("AA.Wall.CreatureOnly"), CreatureVertices, CreatureIndices, true);
}

void ASectorMeshBuilderActor::BuildSectorsFromLastWad()
{
    UWorld* World = GetWorld(); if (!World) return;

    const FDoomWadMapData* Map = UDoomWadLoader::GetLastWadMapData();
    if (!Map)
    {
        UE_LOG(LogTemp, Warning, TEXT("No WAD topology loaded. Use AA.LoadWad first."));
        return;
    }

    if (!FMath::IsFinite(UnitsToUU) || UnitsToUU <= 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Sector map scale must be finite and positive."));
        return;
    }

    ClearChildren();
    BuiltUnitsToUU = UnitsToUU;
    WallMap = FDoomWadMapData();
    WallBatches.Reset();
    WallBatchesBySector.Reset();
    WallBatchesBySide.Reset();
    WallTextureScrolls.Reset();
    WallMaterials.Reset();
    SectorHeights.Reset(Map->Sectors.Num());
    for (const FWadSector& Sector : Map->Sectors)
    {
        SectorHeights.Add(FWadSectorHeightRange{double(Sector.FloorHeight), double(Sector.CeilingHeight)});
    }
    FloorRoots.SetNum(Map->Sectors.Num());
    CeilRoots.SetNum(Map->Sectors.Num());
    FloorPMCs.SetNum(Map->Sectors.Num());
    CeilPMCs.SetNum(Map->Sectors.Num());

    int32 TexturedFloorSections = 0;
    int32 FallbackFloorSections = 0;
    int32 TexturedCeilSections = 0;
    int32 FallbackCeilSections = 0;

    // Build both horizontal surfaces from one footprint.
    for (int32 SectorIdx = 0; bBuildFloorsAndCeilings && SectorIdx < Map->Sectors.Num(); ++SectorIdx)
    {
        if (SectorFilter >= 0 && SectorIdx != SectorFilter) continue;

        const int32 FloorZ = Map->Sectors[SectorIdx].FloorHeight;
        const int32 CeilZ = Map->Sectors[SectorIdx].CeilingHeight;
        if (CeilZ < FloorZ) continue;

        FString FloorTextureName;
        FString CeilingTextureName;
        UDoomWadLoader::GetLastWadSectorTextureNames(SectorIdx, FloorTextureName, CeilingTextureName);

        UProceduralMeshComponent* PMC_Floor = nullptr;
        UProceduralMeshComponent* PMC_Ceil = nullptr;

        USceneComponent* FloorRoot = nullptr;
        USceneComponent* CeilRoot = nullptr;
        if (bBuildFloorsAndCeilings)
        {
            FloorRoot = NewObject<USceneComponent>(this);
            FloorRoot->SetMobility(EComponentMobility::Movable);
            FloorRoot->AttachToComponent(SceneRoot, FAttachmentTransformRules::KeepRelativeTransform);
            FloorRoot->SetRelativeLocation(FVector(0.0f, 0.0f, (float)FloorZ * UnitsToUU));
            FloorRoot->RegisterComponent();
            FloorRoots[SectorIdx] = FloorRoot;

            if (bBuildCeilings)
            {
                CeilRoot = NewObject<USceneComponent>(this);
                CeilRoot->SetMobility(EComponentMobility::Movable);
                CeilRoot->AttachToComponent(SceneRoot, FAttachmentTransformRules::KeepRelativeTransform);
                CeilRoot->SetRelativeLocation(FVector(0.0f, 0.0f, (float)CeilZ * UnitsToUU));
                CeilRoot->RegisterComponent();
                CeilRoots[SectorIdx] = CeilRoot;
            }
        }

        auto EnsureFloorPMC = [&]() -> UProceduralMeshComponent*
        {
            if (!PMC_Floor)
            {
                USceneComponent* const AttachParent = FloorRoot ? FloorRoot : SceneRoot.Get();
                PMC_Floor = NewObject<UProceduralMeshComponent>(this);
                PMC_Floor->ComponentTags.Add(TEXT("AA.Sector.Floor"));
                PMC_Floor->ComponentTags.Add(FName(*FString::Printf(TEXT("AA.Sector.%d"), SectorIdx)));
                PMC_Floor->SetCanEverAffectNavigation(false);
                PMC_Floor->AttachToComponent(AttachParent, FAttachmentTransformRules::KeepRelativeTransform);
                PMC_Floor->RegisterComponent();
                if (UMaterialInterface* SurfaceMaterial = MakePlaneMaterial(PMC_Floor, FloorTextureName))
                {
                    PMC_Floor->SetMaterial(0, SurfaceMaterial);
                    ++TexturedFloorSections;
                }
                else if (FloorMaterial)
                {
                    PMC_Floor->SetMaterial(0, FloorMaterial);
                    ++FallbackFloorSections;
                }
                FloorPMCs[SectorIdx] = PMC_Floor;
            }
            return PMC_Floor;
        };

        auto EnsureCeilPMC = [&]() -> UProceduralMeshComponent*
        {
            if (!PMC_Ceil)
            {
                USceneComponent* const AttachParent = CeilRoot ? CeilRoot : SceneRoot.Get();
                PMC_Ceil = NewObject<UProceduralMeshComponent>(this);
                PMC_Ceil->ComponentTags.Add(TEXT("AA.Sector.Ceiling"));
                PMC_Ceil->ComponentTags.Add(FName(*FString::Printf(TEXT("AA.Sector.%d"), SectorIdx)));
                PMC_Ceil->SetCanEverAffectNavigation(false);
                PMC_Ceil->AttachToComponent(AttachParent, FAttachmentTransformRules::KeepRelativeTransform);
                PMC_Ceil->RegisterComponent();
                if (CeilingTextureName.StartsWith(TEXT("F_SKY")))
                {
                    // The original renderer draws the sky through this ceiling.
                    PMC_Ceil->SetVisibility(false);
                }
                else if (UMaterialInterface* SurfaceMaterial = MakePlaneMaterial(PMC_Ceil, CeilingTextureName))
                {
                    PMC_Ceil->SetMaterial(0, SurfaceMaterial);
                    ++TexturedCeilSections;
                }
                else if (CeilingMaterial)
                {
                    PMC_Ceil->SetMaterial(0, CeilingMaterial);
                    ++FallbackCeilSections;
                }
                CeilPMCs[SectorIdx] = PMC_Ceil;
            }
            return PMC_Ceil;
        };

        if (bBuildFloorsAndCeilings)
        {
            FDoomWadSectorFootprint Footprint;
            if (UDoomWadLoader::GetLastWadSectorFootprint(SectorIdx, Footprint))
            {
                BuildFootprintMesh(EnsureFloorPMC(), Footprint, false, UnitsToUU, FloorVertexColor);
                if (bBuildCeilings)
                {
                    BuildFootprintMesh(EnsureCeilPMC(), Footprint, true, UnitsToUU, CeilingVertexColor);
                }
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("Sector %d: could not triangulate the linedef boundaries."), SectorIdx);
            }
        }

    }

    if (bBuildWalls)
    {
        BuildWalls(*Map);
    }

    UE_LOG(LogTemp, Display, TEXT("Sector materials: floors textured=%d fallback=%d, ceilings textured=%d fallback=%d"),
        TexturedFloorSections, FallbackFloorSections, TexturedCeilSections, FallbackCeilSections);
    UE_LOG(LogTemp, Display, TEXT("Built per-sector meshes."));
}

bool ASectorMeshBuilderActor::GetSectorSurfaceHeights(int32 SectorIndex, FVector2D& OutHeights) const
{
    if (!FloorRoots.IsValidIndex(SectorIndex) || !CeilRoots.IsValidIndex(SectorIndex)
        || !FloorRoots[SectorIndex] || !CeilRoots[SectorIndex])
    {
        return false;
    }
    OutHeights = FVector2D(FloorRoots[SectorIndex]->GetRelativeLocation().Z,
                           CeilRoots[SectorIndex]->GetRelativeLocation().Z);
    return true;
}

void ASectorMeshBuilderActor::SetSectorFloorZ(int32 SectorIndex, float NewFloorZ_UU)
{
    SetSectorSurfaceZ(SectorIndex, NewFloorZ_UU, false);
}

void ASectorMeshBuilderActor::SetSectorCeilingZ(int32 SectorIndex, float NewCeilZ_UU)
{
    SetSectorSurfaceZ(SectorIndex, NewCeilZ_UU, true);
}

void ASectorMeshBuilderActor::SetSectorSurfaceZ(int32 SectorIndex, float NewZ_UU, bool bCeiling)
{
    const auto& Roots = bCeiling ? CeilRoots : FloorRoots;
    if (!Roots.IsValidIndex(SectorIndex) || !Roots[SectorIndex] || !SectorHeights.IsValidIndex(SectorIndex)
        || !FMath::IsFinite(NewZ_UU))
    {
        return;
    }

    FWadSectorHeightRange& Heights = SectorHeights[SectorIndex];
    const double Requested = NewZ_UU / BuiltUnitsToUU;
    const double NewHeight = bCeiling ? FMath::Max(Requested, Heights.Floor) : FMath::Min(Requested, Heights.Ceiling);
    double& Height = bCeiling ? Heights.Ceiling : Heights.Floor;
    if (Height == NewHeight)
    {
        return;
    }
    Height = NewHeight;

    USceneComponent* Root = Roots[SectorIndex].Get();
    FVector Location = Root->GetRelativeLocation();
    Location.Z = NewHeight * BuiltUnitsToUU;
    Root->SetRelativeLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);

    if (WallBatchesBySector.IsValidIndex(SectorIndex))
    {
        // Cook replacement collision before returning so movement and queries see the same opening.
        for (int32 BatchIndex : WallBatchesBySector[SectorIndex])
        {
            BuildWallBatch(BatchIndex);
        }
    }
}

bool ASectorMeshBuilderActor::SetSectorTextureScrollTexels(int32 SectorIndex, bool bCeiling, FVector2D OffsetTexels)
{
    const auto& Meshes = bCeiling ? CeilPMCs : FloorPMCs;
    if (OffsetTexels.ContainsNaN() || !Meshes.IsValidIndex(SectorIndex) || !Meshes[SectorIndex] || !Meshes[SectorIndex]->IsVisible())
    {
        return false;
    }
    UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Meshes[SectorIndex]->GetMaterial(0));
    if (!GetBoundSurfaceTexture(Material))
    {
        return false;
    }
    ApplySurfaceOffset(*Material, -OffsetTexels);
    return true;
}

bool ASectorMeshBuilderActor::SetWallTextureScrollTexels(int32 SideIndex, EWadWallBand Band, FVector2D OffsetTexels)
{
    if (OffsetTexels.ContainsNaN() || uint8(Band) > uint8(EWadWallBand::Upper)
        || !WallBatchesBySide.IsValidIndex(SideIndex) || WallBatchesBySide[SideIndex].IsEmpty())
    {
        return false;
    }
    const uint64 Key = WallSurfaceKey(SideIndex, Band);
    if (FWallTextureScroll* Scroll = WallTextureScrolls.Find(Key))
    {
        Scroll->Offset = OffsetTexels;
        for (const TWeakObjectPtr<UMaterialInstanceDynamic>& Material : Scroll->Materials)
        {
            if (Material.IsValid())
            {
                ApplySurfaceOffset(*Material.Get(), OffsetTexels);
            }
        }
    }
    else if (!OffsetTexels.IsZero())
    {
        WallTextureScrolls.FindOrAdd(Key).Offset = OffsetTexels;
        // Split this band out of its shared material section once. Later scroll
        // updates only set a material parameter and never rebuild the mesh.
        for (int32 BatchIndex : WallBatchesBySide[SideIndex])
        {
            BuildWallBatch(BatchIndex);
        }
    }
    return true;
}
