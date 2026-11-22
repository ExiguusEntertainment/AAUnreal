#include "NativeWadEditor.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Framework/Docking/TabManager.h"
#include "Materials/MaterialInterface.h"
#include "NativeWadLevel.h"
#include "PropertyCustomizationHelpers.h"
#include "ScopedTransaction.h"
#include "SSearchableComboBox.h"
#include "StaticMeshCompiler.h"
#include "ToolMenus.h"
#include "UnrealClient.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NativeWadEditor"

namespace
{
    const FName SurfaceTab(TEXT("AAWadSurfaces"));
    FDelegateHandle MenuHandle;
    FDelegateHandle PIEHandle;

    ANativeWadLevel* FindEditorLevel()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (World)
        {
            const TActorIterator<ANativeWadLevel> Level(World);
            return Level ? *Level : nullptr;
        }
        return nullptr;
    }

    class SWadSurfaces : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SWadSurfaces) {}
        SLATE_END_ARGS()

        void Construct(const FArguments&)
        {
            Reload();
            ChildSlot.Padding(10)[SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.0f)
                    [SAssignNew(Picker, SSearchableComboBox)
                        .OptionsSource(&Names)
                        .OnGenerateWidget_Lambda([](TSharedPtr<FString> Name)
                        { return SNew(STextBlock).Text(FText::FromString(*Name)); })
                        .OnSelectionChanged(this, &SWadSurfaces::SelectSurface)
                        [SNew(STextBlock).Text_Lambda([this]
                        { return Selection ? FText::FromString(*Selection) : LOCTEXT("Choose", "Choose a wall, floor, or ceiling"); })]]
                    + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
                    [SNew(SButton).Text(LOCTEXT("PickViewport", "Pick viewport center"))
                        .OnClicked(this, &SWadSurfaces::PickViewportSurface)]
                    + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
                    [SNew(SButton).Text(LOCTEXT("Refresh", "Refresh current map"))
                        .OnClicked_Lambda([this] { Reload(); return FReply::Handled(); })]]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [SNew(STextBlock).Text_Lambda([this] { return PickStatus; })
                    .Visibility_Lambda([this] { return PickStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [SNew(STextBlock).Text_Lambda([this]
                    {
                        if (const UStaticMeshComponent* Surface = Selected.Get())
                        {
                            return FText::Format(LOCTEXT("Location", "World position  {0}"), FText::FromString(Surface->GetComponentLocation().ToCompactString()));
                        }
                        return FText::GetEmpty();
                    })]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [SNew(SButton).Text(LOCTEXT("Focus", "Focus surface in viewport")).IsEnabled_Lambda([this] { return Selected.IsValid(); })
                    .OnClicked_Lambda([this]
                    {
                        if (const UStaticMeshComponent* Surface = Selected.Get(); Surface && GEditor)
                        {
                            GEditor->MoveViewportCamerasToBox(Surface->Bounds.GetBox(), false, 1.5f);
                        }
                        return FReply::Handled();
                    })]
                + SVerticalBox::Slot().AutoHeight()
                [SNew(STextBlock).Text(LOCTEXT("SurfaceMaterial", "Surface material"))]
                + SVerticalBox::Slot().AutoHeight()
                [SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UMaterialInterface::StaticClass())
                    .IsEnabled_Lambda([this] { return Selected.IsValid(); })
                    .ObjectPath_Lambda([this]
                    {
                        return Selected.IsValid() ? GetPathNameSafe(Selected->GetMaterial(0)) : FString();
                    })
                    .OnObjectChanged(this, &SWadSurfaces::ChangeMaterial)]];
        }

    private:
        void Reload()
        {
            Selected.Reset();
            Selection.Reset();
            PickStatus = FText::GetEmpty();
            Names.Reset();
            Surfaces.Reset();
            if (ANativeWadLevel* Level = FindEditorLevel())
            {
                TInlineComponentArray<UStaticMeshComponent*> Components(Level);
                for (UStaticMeshComponent* Component : Components)
                {
                    FString Name = Component->GetName();
                    if (!Component->GetStaticMesh() || (!Name.StartsWith(TEXT("Sector_")) && !Name.StartsWith(TEXT("Side_"))))
                    {
                        continue;
                    }
                    if (Name.StartsWith(TEXT("Side_")))
                    {
                        const TCHAR* Bands[] = {TEXT("Lower"), TEXT("Middle"), TEXT("Upper")};
                        const int32 Band = Name.Right(1)[0] - TCHAR('0');
                        if (Band >= 0 && Band < UE_ARRAY_COUNT(Bands))
                        {
                            Name.LeftChopInline(1);
                            Name += Bands[Band];
                        }
                    }
                    Name.ReplaceInline(TEXT("_"), TEXT(" "));
                    Surfaces.Add(Name, Component);
                    Names.Add(MakeShared<FString>(Name));
                }
                Names.Sort([](const TSharedPtr<FString>& A, const TSharedPtr<FString>& B) { return *A < *B; });
            }
            if (Picker)
            {
                Picker->ClearSelection();
                Picker->RefreshOptions();
            }
        }

        void SelectSurface(TSharedPtr<FString> Name, ESelectInfo::Type)
        {
            Selection = Name;
            Selected = Name ? Surfaces.FindRef(*Name) : TWeakObjectPtr<UStaticMeshComponent>();
            PickStatus = FText::GetEmpty();
        }

        FReply PickViewportSurface()
        {
            if (FViewport* Viewport = GEditor ? GEditor->GetActiveViewport() : nullptr)
            {
                const FIntPoint Center = Viewport->GetSizeXY() / 2;
                const HHitProxy* Proxy = Center.X > 0 && Center.Y > 0 ? Viewport->GetHitProxy(Center.X, Center.Y) : nullptr;
                if (Proxy && Proxy->IsA(HActor::StaticGetType()))
                {
                    const UPrimitiveComponent* Component = static_cast<const HActor*>(Proxy)->PrimComponent.Get();
                    const auto* Match = Names.FindByPredicate([this, Component](const TSharedPtr<FString>& Name)
                    { return Component && Surfaces.FindRef(*Name).Get() == Component; });
                    if (Match)
                    {
                        Picker->SetSelectedItem(*Match, ESelectInfo::Direct);
                        return FReply::Handled();
                    }
                }
            }
            PickStatus = LOCTEXT("NoViewportSurface", "Center a WAD surface in the level viewport and try again.");
            return FReply::Handled();
        }

        void ChangeMaterial(const FAssetData& Asset)
        {
            if (UStaticMeshComponent* Surface = Selected.Get(); Surface && Surface->GetOwner() == FindEditorLevel())
            {
                const FScopedTransaction Edit(LOCTEXT("EditMaterial", "Change WAD surface material"));
                Surface->Modify();
                Surface->SetMaterial(0, Cast<UMaterialInterface>(Asset.GetAsset()));
                Surface->MarkPackageDirty();
            }
        }

        TArray<TSharedPtr<FString>> Names;
        TMap<FString, TWeakObjectPtr<UStaticMeshComponent>> Surfaces;
        TWeakObjectPtr<UStaticMeshComponent> Selected;
        TSharedPtr<FString> Selection;
        FText PickStatus;
        TSharedPtr<SSearchableComboBox> Picker;
    };

    void PrepareWadMeshesForPIE(bool)
    {
        if (ANativeWadLevel* Level = FindEditorLevel())
        {
            TInlineComponentArray<UStaticMeshComponent*> Components(Level);
            TArray<UStaticMesh*> Pending;
            for (const UStaticMeshComponent* Component : Components)
            {
                if (UStaticMesh* Mesh = Component->GetStaticMesh(); Mesh && Mesh->IsCompiling())
                {
                    Pending.AddUnique(Mesh);
                }
            }
            if (!Pending.IsEmpty())
            {
                UE_LOG(LogTemp, Display, TEXT("Finishing %d compiling WAD meshes before PIE."), Pending.Num());
                FStaticMeshCompilingManager::Get().FinishCompilation(Pending);
            }
        }
    }
}

void RegisterNativeWadEditorTools()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SurfaceTab, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
        { return SNew(SDockTab).TabRole(NomadTab)[SNew(SWadSurfaces)]; }))
        .SetDisplayName(LOCTEXT("Title", "Amulets & Armor Surfaces"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
    MenuHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]
    {
        FToolMenuOwnerScoped Owner(SurfaceTab);
        UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"))->FindOrAddSection(TEXT("LevelEditor"))
            .AddMenuEntry(SurfaceTab, LOCTEXT("Title", "Amulets & Armor Surfaces"),
                LOCTEXT("Tooltip", "Inspect the walls, floors, and ceilings of the current native WAD level."),
                FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([] { OpenNativeWadSurfaceInspector(); })));
    }));
    PIEHandle = FEditorDelegates::PreBeginPIE.AddStatic(PrepareWadMeshesForPIE);
}

void UnregisterNativeWadEditorTools()
{
    FEditorDelegates::PreBeginPIE.Remove(PIEHandle);
    UToolMenus::UnRegisterStartupCallback(MenuHandle);
    UToolMenus::UnregisterOwner(SurfaceTab);
    if (const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(SurfaceTab))
    {
        Tab->RequestCloseTab();
    }
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SurfaceTab);
}

bool OpenNativeWadSurfaceInspector()
{
    return GIsEditor && !IsRunningCommandlet() && FGlobalTabmanager::Get()->TryInvokeTab(SurfaceTab).IsValid();
}

#undef LOCTEXT_NAMESPACE
#endif
