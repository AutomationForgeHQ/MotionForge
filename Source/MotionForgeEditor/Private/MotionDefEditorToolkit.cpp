// Copyright Blackcode SA. All rights reserved.

#include "MotionDefEditorToolkit.h"

#include "MotionDefDetails.h"
#include "SMotionDefTakes.h"

#include "MotionDef.h"
#include "MotionForgeSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IDetailsView.h"
#include "LevelSequence.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

const FName FMotionDefEditorToolkit::ToolkitName(TEXT("MotionDefEditor"));
const FName FMotionDefEditorToolkit::DetailsTabId(TEXT("MotionDefEditor_Details"));
const FName FMotionDefEditorToolkit::TakesTabId(TEXT("MotionDefEditor_Takes"));

namespace
{
	/**
	 * Everything under "State" is drawn by the takes panel, in a form a person can act on.
	 *
	 * Leaving it in the details view as well would show the same facts twice, once well and once as a
	 * read-only enum beside a collapsed array - and the collapsed array is exactly the surface this
	 * window exists to replace.
	 */
	bool IsAuthoringProperty(const FPropertyAndParent& PropertyAndParent)
	{
		return PropertyAndParent.Property.GetMetaData(TEXT("Category")) != TEXT("State");
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

	// Per-instance rather than registered globally: this shapes the definition window, and a details
	// panel somewhere else showing the same asset should still show it plainly.
	DetailsView->RegisterInstancedCustomPropertyLayout(
		UMotionDef::StaticClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMotionDefDetails::MakeInstance));

	DetailsView->SetObject(InDef);

	const TSharedRef<FTabManager::FLayout> Layout =
		FTabManager::NewLayout("MotionDefEditor_v1")
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
					->SetSizeCoefficient(0.55f)
					->AddTab(DetailsTabId, ETabState::OpenedTab)
					->SetHideTabWell(true)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.45f)
					->AddTab(TakesTabId, ETabState::OpenedTab)
					->SetHideTabWell(true)
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

	InTabManager->RegisterTabSpawner(DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FMotionDefEditorToolkit::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Definition"))
		.SetGroup(Category)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

	InTabManager->RegisterTabSpawner(TakesTabId,
		FOnSpawnTab::CreateSP(this, &FMotionDefEditorToolkit::SpawnTakesTab))
		.SetDisplayName(LOCTEXT("TakesTab", "Takes"))
		.SetGroup(Category)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FMotionDefEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(TakesTabId);
}

TSharedRef<SDockTab> FMotionDefEditorToolkit::SpawnDetailsTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTab", "Definition"))
		[
			DetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FMotionDefEditorToolkit::SpawnTakesTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("TakesTab", "Takes"))
		[
			SAssignNew(TakesPanel, SMotionDefTakes).Definition(Definition)
		];
}

FName FMotionDefEditorToolkit::GetToolkitFName() const          { return ToolkitName; }
FText FMotionDefEditorToolkit::GetBaseToolkitName() const       { return LOCTEXT("AppLabel", "Motion Definition"); }
FString FMotionDefEditorToolkit::GetWorldCentricTabPrefix() const { return LOCTEXT("TabPrefix", "Motion ").ToString(); }
FLinearColor FMotionDefEditorToolkit::GetWorldCentricTabColorScale() const { return FLinearColor(0.36f, 0.52f, 0.86f, 0.5f); }

// -------------------------------------------------------------------------------------------------
// The toolbar
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
	Builder.BeginSection(TEXT("MotionForge"));

	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnGenerate),
			FCanExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::CanGenerate)),
		NAME_None,
		LOCTEXT("Generate", "Generate"),
		TAttribute<FText>::CreateSP(this, &FMotionDefEditorToolkit::GenerateTooltip),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Play"));

	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnImportChosen),
			FCanExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::CanImportChosen)),
		NAME_None,
		TAttribute<FText>::CreateSP(this, &FMotionDefEditorToolkit::ImportLabel),
		TAttribute<FText>::CreateSP(this, &FMotionDefEditorToolkit::ImportTooltip),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"));

	Builder.EndSection();

	Builder.BeginSection(TEXT("MotionForgeNavigate"));

	Builder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnOpenPromptSequence)),
		NAME_None,
		LOCTEXT("PromptSequence", "Prompt Timeline"),
		LOCTEXT("PromptSequenceTip",
			"Lay the prompt out as beats on a Level Sequence, where their durations are visible and "
			"can be dragged. Creates the sequence the first time.\n\n"
			"While a prompt sequence exists it is the prompt - the text field below is left alone "
			"underneath it."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"));

	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::OnShowAnimation),
			FCanExecuteAction::CreateSP(this, &FMotionDefEditorToolkit::CanShowAnimation)),
		NAME_None,
		LOCTEXT("ShowAnimation", "Show Animation"),
		LOCTEXT("ShowAnimationTip", "Find the imported animation sequence in the Content Browser."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"));

	Builder.EndSection();
}

// -------------------------------------------------------------------------------------------------
// The verbs. Each is one subsystem call; none of them decides anything.
// -------------------------------------------------------------------------------------------------

bool FMotionDefEditorToolkit::CanGenerate() const
{
	const UMotionDef* D = Def();
	if (!D || D->IsBusy())
	{
		return false;
	}

	// And only when it would actually work. Offering Generate to a definition with no character,
	// no key or a runner that is down is a button that cannot do what it says - the panel is
	// already saying what is missing, and the two must not disagree.
	//
	// Read from the panel's cache rather than asked here: the check touches the credential vault
	// and this runs every frame.
	return !TakesPanel.IsValid() || TakesPanel->Readiness().bCanGenerate;
}

void FMotionDefEditorToolkit::OnGenerate()
{
	UMotionDef* D = Def();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	if (D && Subsystem)
	{
		Subsystem->Generate({ D->GetPathName() });

		if (TakesPanel.IsValid())
		{
			TakesPanel->Refresh();
		}
	}
}

bool FMotionDefEditorToolkit::CanImportChosen() const
{
	const UMotionDef* D = Def();
	if (!D || D->IsBusy() || D->SelectedMotionId.IsEmpty())
	{
		return false;
	}

	const FMotionCandidate* Chosen = D->FindSelectedCandidate();
	return Chosen && Chosen->Status == EMotionJobStatus::Finished;
}

void FMotionDefEditorToolkit::OnImportChosen()
{
	UMotionDef* D = Def();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	if (D && Subsystem)
	{
		Subsystem->DownloadSelected({ D->GetPathName() });

		if (TakesPanel.IsValid())
		{
			TakesPanel->Refresh();
		}
	}
}

FText FMotionDefEditorToolkit::GenerateTooltip() const
{
	static const FText Normal = LOCTEXT("GenerateTip",
		"Submit this definition to its provider. A definition already generating is skipped "
		"rather than charged a second time.\n\n"
		"What it costs is on the takes panel, beside the takes it would add to.");

	if (TakesPanel.IsValid() && !TakesPanel->Readiness().bCanGenerate
		&& !TakesPanel->Readiness().Problem.IsEmpty())
	{
		return FText::Format(
			LOCTEXT("GenerateBlockedFmt", "Cannot generate yet.\n\n{0}"),
			FText::FromString(TakesPanel->Readiness().Problem));
	}

	return Normal;
}

FText FMotionDefEditorToolkit::ImportLabel() const
{
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	const UMotionDef* D = Def();

	if (Subsystem && D)
	{
		const FMotionProviderCaps Caps = Subsystem->GetProviderCaps(D->ProviderId);

		// On a subscription the download is the spend, and the label should say so before it is
		// pressed. On pay-as-you-go the money went at generation and this is free.
		if (Caps.bIsMetered && !Caps.bIsLocal)
		{
			return LOCTEXT("ImportPaid", "Download && Import");
		}
	}

	return LOCTEXT("Import", "Import Take");
}

FText FMotionDefEditorToolkit::ImportTooltip() const
{
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	const UMotionDef* D = Def();

	if (!Subsystem || !D)
	{
		return LOCTEXT("ImportTipPlain", "Fetch the chosen take and import it.");
	}

	const FMotionCostEstimate Estimate = Subsystem->EstimateCost({ D->GetPathName() }, /*bSelectedOnly*/ true);

	if (Estimate.DownloadSeconds == 0)
	{
		return LOCTEXT("ImportTipCached",
			"Fetch the chosen take, normalise it, and import it onto the character's skeleton.\n\n"
			"This take is already on disk, so fetching it again costs nothing.");
	}

	return FText::Format(
		LOCTEXT("ImportTipCostFmt",
			"Fetch the chosen take, normalise it, and import it onto the character's skeleton.\n\n"
			"{0} seconds would be downloaded."),
		FText::AsNumber(Estimate.DownloadSeconds));
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
	// somebody may have spent time retiming.
	if (ULevelSequence* Existing = D->Control.ConstraintSequence.LoadSynchronous())
	{
		// The animation row, though, is refreshed every time. A sequence built before the clip was
		// imported has an empty one, and a sequence built before a different take was chosen has a
		// stale one - and an empty row beside the beats that produced it is the one thing this
		// timeline exists to avoid.
		FString RefreshError;
		Subsystem->RefreshPromptSequenceTake(D->GetPathName(), RefreshError);

		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Existing);
		return;
	}

	FString Error;
	const FString Created = Subsystem->CreatePromptSequence(D->GetPathName(), FString(), Error);

	if (Created.IsEmpty())
	{
		FNotificationInfo Info(FText::Format(
			LOCTEXT("PromptSequenceFailedFmt", "Could not create a prompt sequence: {0}"),
			FText::FromString(Error)));
		Info.ExpireDuration = 6.f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	if (UObject* Sequence = LoadObject<UObject>(nullptr, *Created))
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
	}
}

bool FMotionDefEditorToolkit::CanShowAnimation() const
{
	const UMotionDef* D = Def();
	return D && !D->ImportedSequence.IsNull();
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

#undef LOCTEXT_NAMESPACE
