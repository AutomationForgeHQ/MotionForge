#include "MotionForgeEditorModule.h"

#include "MotionConstraintTrackEditor.h"
#include "MotionPromptTrackEditor.h"

#include "ISequencerModule.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

IMPLEMENT_MODULE(FMotionForgeEditorModule, MotionForgeEditor)

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

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}
