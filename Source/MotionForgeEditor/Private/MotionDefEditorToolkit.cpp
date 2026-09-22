// Copyright Blackcode SA. All rights reserved.

#include "MotionDefEditorToolkit.h"

#include "MotionForgeEditorModule.h"
#include "SMotionGeneratePanel.h"
#include "SMotionHome.h"
#include "SMotionTakesPanel.h"

#include "MotionDef.h"
#include "MotionForgeSubsystem.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "IDetailsView.h"
#include "LevelSequence.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

const FName FMotionDefEditorToolkit::ToolkitName(TEXT("MotionDefEditor"));
const FName FMotionDefEditorToolkit::TakesTabId(TEXT("MotionDefEditor_Takes"));
const FName FMotionDefEditorToolkit::RecordTabId(TEXT("MotionDefEditor_Record"));
const FName FMotionDefEditorToolkit::GenerateTabId(TEXT("MotionDefEditor_Generate"));
const FName FMotionDefEditorToolkit::SettingsTabId(TEXT("MotionDefEditor_Settings"));

namespace
{
	/**
	 * The Settings tab is every authoring field; pipeline state and bookkeeping are drawn properly by
	 * the takes panel, and each provider's settings by the Generate card.
	 */
	bool IsAuthoringProperty(const FPropertyAndParent& PropertyAndParent)
	{
		const FString Category = PropertyAndParent.Property.GetMetaData(TEXT("Category"));
		return Category != TEXT("State") && Category != TEXT("Result")
			&& PropertyAndParent.Property.GetFName() != GET_MEMBER_NAME_CHECKED(UMotionDef, Pipelines);
	}
}

// -------------------------------------------------------------------------------------------------

void FMotionDefEditorToolkit::Initialise(
	EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host, UMotionDef* InDef)
{
	Definition = InDef;

	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));

	FDetailsViewArgs Args;
	Args.bAllowSearch = true;
	Args.bHideSelectionTip = true;
	Args.bShowOptions = false;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	DetailsView = PropertyModule.CreateDetailView(Args);
	DetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateStatic(&IsAuthoringProperty));
	DetailsView->SetObject(InDef);

	// Built before the tabs spawn: the Record tab reads the takes panel's selection.
	TakesPanel = SNew(SMotionTakesPanel).Definition(InDef);
	GeneratePanel = SNew(SMotionGeneratePanel).Definition(InDef);

	// Renamed from v1 on purpose: Unreal remembers a layout by its name, and a v1 layout restored over
	// this one would put the old tabs back.
	const TSharedRef<FTabManager::FLayout> Layout =
		FTabManager::NewLayout("MotionDefEditor_v2")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewSplitter()
				->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.62f)
					->AddTab(TakesTabId, ETabState::OpenedTab)
					->AddTab(RecordTabId, ETabState::OpenedTab)
					->SetForegroundTab(TakesTabId)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.38f)
					->AddTab(GenerateTabId, ETabState::OpenedTab)
					->AddTab(SettingsTabId, ETabState::OpenedTab)
					->SetForegroundTab(GenerateTabId)
				)
			)
		);

	// Before InitAssetEditor, or the toolbar is built without it and the buttons never appear.
	ExtendToolbar();

	InitAssetEditor(
		Mode,
		Host,
		ToolkitName,
		Layout,
		/*bCreateDefaultStandaloneMenu*/ true,
		/*bCreateDefaultToolbar*/ true,
		InDef);
}

void FMotionDefEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(
		LOCTEXT("WorkspaceMenu", "Motion Definition"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	const TSharedRef<FWorkspaceItem> Category = WorkspaceMenuCategory.ToSharedRef();

	InTabManager->RegisterTabSpawner(TakesTabId, FOnSpawnTab::CreateSP(this, &FMotionDefEditorToolkit::SpawnTakesTab))
		.SetDisplayName(LOCTEXT("TakesTab", "Takes"))
		.SetGroup(Category)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimSequence"));

	InTabManager->RegisterTabSpawner(RecordTabId, FOnSpawnTab::CreateSP(this, &FMotionDefEditorToolkit::SpawnRecordTab))
		.SetDisplayName(LOCTEXT("RecordTab", "Record"))
		.SetGroup(Category)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Info"));

	InTabManager->RegisterTabSpawner(GenerateTabId, FOnSpawnTab::CreateSP(this, &FMotionDefEditorToolkit::SpawnGenerateTab))
		.SetDisplayName(LOCTEXT("GenerateTab", "Generate"))
		.SetGroup(Category)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Play"));

	InTabManager->RegisterTabSpawner(SettingsTabId, FOnSpawnTab::CreateSP(this, &FMotionDefEditorToolkit::SpawnSettingsTab))
		.SetDisplayName(LOCTEXT("SettingsTab", "Settings"))
		.SetGroup(Category)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FMotionDefEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(TakesTabId);
	InTabManager->UnregisterTabSpawner(RecordTabId);
	InTabManager->UnregisterTabSpawner(GenerateTabId);
	InTabManager->UnregisterTabSpawner(SettingsTabId);
}

TSharedRef<SDockTab> FMotionDefEditorToolkit::SpawnTakesTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab).Label(LOCTEXT("TakesTab", "Takes"))[ TakesPanel.ToSharedRef() ];
}

TSharedRef<SDockTab> FMotionDefEditorToolkit::SpawnRecordTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab).Label(LOCTEXT("RecordTab", "Record"))[ TakesPanel->GetRecordWidget() ];
}

TSharedRef<SDockTab> FMotionDefEditorToolkit::SpawnGenerateTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab).Label(LOCTEXT("GenerateTab", "Generate"))[ GeneratePanel.ToSharedRef() ];
}

TSharedRef<SDockTab> FMotionDefEditorToolkit::SpawnSettingsTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab).Label(LOCTEXT("SettingsTab", "Settings"))[ DetailsView.ToSharedRef() ];
}

FName FMotionDefEditorToolkit::GetToolkitFName() const          { return ToolkitName; }
FText FMotionDefEditorToolkit::GetBaseToolkitName() const       { return LOCTEXT("AppLabel", "Motion Definition"); }
FString FMotionDefEditorToolkit::GetWorldCentricTabPrefix() const { return LOCTEXT("TabPrefix", "Motion ").ToString(); }
FLinearColor FMotionDefEditorToolkit::GetWorldCentricTabColorScale() const { return FLinearColor(0.36f, 0.52f, 0.86f, 0.5f); }

// -------------------------------------------------------------------------------------------------
// The toolbar: navigation. The verbs are in the cards, beside what they act on.
// -------------------------------------------------------------------------------------------------

void FMotionDefEditorToolkit::ExtendToolbar()
{
	const TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	Extender->AddToolBarExtension(
		"Asset",
		EExtensionHook::After,
		GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateSP(this, &FMotionDefEditorToolkit::FillToolbar));

	AddToolbarExtender(Extender);
}

void FMotionDefEditorToolkit::FillToolbar(FToolBarBuilder& Builder)
{
	Builder.BeginSection(TEXT("MotionForgeNavigate"));

	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnShowAnimation),
			FCanExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::CanShowAnimation)),
		NAME_None,
		LOCTEXT("ShowAnimation", "Show Animation"),
		TAttribute<FText>::CreateSP(this, &FMotionDefEditorToolkit::ShowAnimationTooltip),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"));

	Builder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnOpenPromptSequence)),
		NAME_None,
		LOCTEXT("PromptTimeline", "Prompt Timeline"),
		LOCTEXT("PromptTimelineTip",
			"The prompt as beats on a Level Sequence, where their durations can be dragged, with the character "
			"and its Control Rig to pose. Made the first time. While it exists, the timeline is the prompt."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"));

	Builder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnOpenLibrary)),
		NAME_None,
		LOCTEXT("OpenLibrary", "Motion Library"),
		LOCTEXT("OpenLibraryTip", "Every definition in the project, the providers' setup, and where to start."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimSequence"));

	Builder.EndSection();
}

void FMotionDefEditorToolkit::OnOpenPromptSequence()
{
	UMotionDef* D = Def();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	if (!D || !Subsystem)
	{
		return;
	}

	// Already laid out: open what exists rather than re-laying it out, which would replace beats
	// somebody may have spent time retiming. The take row is refreshed every time.
	if (ULevelSequence* Existing = D->Control.ConstraintSequence.LoadSynchronous())
	{
		FString RefreshError;
		Subsystem->RefreshPromptSequenceTake(D->GetPathName(), RefreshError);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Existing);
		return;
	}

	FString Error;
	const FString Created = Subsystem->CreatePromptSequence(D->GetPathName(), FString(), Error);
	if (!Created.IsEmpty())
	{
		if (UObject* Sequence = LoadObject<UObject>(nullptr, *Created))
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
		}
	}
}

bool FMotionDefEditorToolkit::CanShowAnimation() const
{
	const UMotionDef* D = Def();
	if (!D || D->ImportedSequence.IsNull())
	{
		return false;
	}

	// Checked, not believed: a deleted clip leaves the soft pointer set.
	const FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	return Registry.Get().GetAssetByObjectPath(D->ImportedSequence.ToSoftObjectPath()).IsValid();
}

FText FMotionDefEditorToolkit::ShowAnimationTooltip() const
{
	const UMotionDef* D = Def();
	if (!D || D->ImportedSequence.IsNull())
	{
		return LOCTEXT("ShowAnimNone", "Nothing imported yet. Choose and import a take first.");
	}
	return CanShowAnimation()
		? LOCTEXT("ShowAnimTip", "Find the imported animation in the Content Browser.")
		: LOCTEXT("ShowAnimGone", "The imported animation was deleted. Choose and import a take to make it again.");
}

void FMotionDefEditorToolkit::OnShowAnimation()
{
	UMotionDef* D = Def();
	if (!D)
	{
		return;
	}

	if (UAnimSequence* Sequence = D->ImportedSequence.LoadSynchronous())
	{
		TArray<UObject*> Objects{ Sequence };
		GEditor->SyncBrowserToObjects(Objects);
	}
}

void FMotionDefEditorToolkit::OnOpenLibrary()
{
	SMotionHome::Open(EMotionHomePage::Library);
}

#undef LOCTEXT_NAMESPACE
