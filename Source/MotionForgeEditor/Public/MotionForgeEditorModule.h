// The one thing a prompt track needs an editor module for: appearing in Sequencer.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Sequencer's affordance for the prompt track, and nothing else.
 *
 * A custom `UMovieSceneTrack` is invisible in Sequencer until something registers a track editor for
 * it - that is the entire reason this module exists. Everything the track *does* lives in MotionForge
 * and works with this module absent: building a sequence, reading the beats back, generating from
 * them and baking them onto the asset are all plain movie scene data, driven over MCP with no UI in
 * the picture. This half is the dragging.
 *
 * **No menus, no panels, no toolbar buttons.** Human UI for these plugins starts from UX flows rather
 * than from controls appearing next to features - see AUTOMATION_FORGE_SHELL_PLAN.md, which is on
 * hold for that conversation.
 */
class FMotionForgeEditorModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:

	/** Handed back by ISequencerModule so the registration can be undone on unload. */
	FDelegateHandle PromptTrackEditorHandle;

	/** The same, for the track that says what a stretch of timeline constrains. */
	FDelegateHandle ConstraintTrackEditorHandle;
};
