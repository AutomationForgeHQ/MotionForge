#include "MotionForgeEditorModule.h"

#include "MotionCharacterDetails.h"
#include "MotionConstraintTrackEditor.h"
#include "MotionPromptTrackEditor.h"
#include "SMotionHome.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "ISequencerModule.h"
#include "MotionCharacter.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

IMPLEMENT_MODULE(FMotionForgeEditorModule, MotionForgeEditor)

const FName FMotionForgeEditorModule::LibraryTabName(TEXT("MotionForgeLibrary"));

namespace
{
	/** Same shape as Kimodo's: a panel is worth nothing if you cannot find it from the keyboard. */
	FAutoConsoleCommand OpenLibraryCommand(
		TEXT("MotionForge.Library"),
		TEXT("Open the motion library: every definition in the project, what state each is in, and "
			 "what it produced."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			SMotionHome::Open(EMotionHomePage::Library);
		}));

	FAutoConsoleCommand OpenGetStartedCommand(
		TEXT("MotionForge.GetStarted"),
		TEXT("Open Get Started: set up a provider and a character, and make a first motion, step by step."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			SMotionHome::Open(EMotionHomePage::GetStarted);
		}));
}

void FMotionForgeEditorModule::StartupModule()
{
	ISequencerModule& Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));

	PromptTrackEditorHandle = Sequencer.RegisterTrackEditor(
		FOnCreateTrackEditor::CreateStatic(&FMotionPromptTrackEditor::CreateTrackEditor));

	ConstraintTrackEditorHandle = Sequencer.RegisterTrackEditor(
		FOnCreateTrackEditor::CreateStatic(&FMotionConstraintTrackEditor::CreateTrackEditor));

	// Deferred until tool menus exist. Extending "Sequencer.MainToolBar" before UToolMenus has
	// started up silently registers against nothing, and the button never appears with no error.
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(
		&FMotionPromptTrackEditor::RegisterSequencerToolbar));

	ToolMenusHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateStatic(&FMotionForgeEditorModule::RegisterLibraryMenu));

	// Registered globally rather than per window, because a Motion Character opens in the ordinary
	// asset editor and the checklist is the point of looking at one. It is the same customization
	// wherever the asset is shown, which is what somebody would expect.
	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));

	PropertyModule.RegisterCustomClassLayout(
		UMotionCharacter::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMotionCharacterDetails::MakeInstance));

	// The library. What the Content Browser cannot tell you about a folder of MD_ icons: which are
	// waiting on a decision, which failed, which have an animation to show for themselves - and what
	// generating a selection of them would cost.
	FGlobalTabmanager::Get()
		->RegisterNomadTabSpawner(LibraryTabName,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab)
					.TabRole(ETabRole::NomadTab)
					[
						SNew(SMotionHome)
					];
			}))
		.SetDisplayName(LOCTEXT("LibraryTabTitle", "MotionForge"))
		.SetTooltipText(LOCTEXT("LibraryTabTip",
			"Get started with a first motion, and every motion definition in the project with what state "
			"each is in and what it produced."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimSequence"))
		// Hidden from the auto-populated Tools list: the family's "Automation Forge" section is the
		// one entry.
		.SetMenuType(ETabSpawnerMenuType::Hidden)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
}

void FMotionForgeEditorModule::RegisterLibraryMenu()
{
	UToolMenu* Tools = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");

	// The section the hub, the Keys page and the Kimodo runner use, so the family groups together
	// whichever of them happens to be installed.
	FToolMenuSection& Section = Tools->FindOrAddSection("AutomationForge",
		LOCTEXT("ToolsSection", "Automation Forge"));

	// One entry, landing on whichever page fits: Get Started until the project has a motion to show,
	// the library after. The page switch is at the top of the tab.
	Section.AddMenuEntry(
		"MotionForgeLibrary",
		LOCTEXT("LibraryMenuLabel", "MotionForge"),
		LOCTEXT("LibraryMenuTip",
			"Make motion from a prompt. Get Started walks through the setup and a first motion; the library "
			"lists every definition, what state each is in, and what it produced."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimSequence"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			SMotionHome::Open(SMotionHome::DefaultPage());
		})));
}

void FMotionForgeEditorModule::ShutdownModule()
{
	// Guarded, because Sequencer may already have gone during editor shutdown and asking for it back
	// would load a module on its way out.
	if (PromptTrackEditorHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("Sequencer")))
	{
		ISequencerModule& Sequencer = FModuleManager::GetModuleChecked<ISequencerModule>(TEXT("Sequencer"));
		Sequencer.UnRegisterTrackEditor(PromptTrackEditorHandle);

		if (ConstraintTrackEditorHandle.IsValid())
		{
			Sequencer.UnRegisterTrackEditor(ConstraintTrackEditorHandle);
		}
	}

	PromptTrackEditorHandle.Reset();
	ConstraintTrackEditorHandle.Reset();

	if (FPropertyEditorModule* PropertyModule =
			FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		PropertyModule->UnregisterCustomClassLayout(UMotionCharacter::StaticClass()->GetFName());
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LibraryTabName);
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

#undef LOCTEXT_NAMESPACE
