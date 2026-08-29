// The one thing a prompt track needs an editor module for: appearing in Sequencer.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Everything MotionForge puts on screen: the definition window, the character checklist, the prompt
 * track's affordance in Sequencer, and the library.
 *
 * A custom `UMovieSceneTrack` is invisible in Sequencer until something registers a track editor for
 * it - that was the original reason this module existed. Everything these surfaces *do* still lives
 * in MotionForge and works with this module absent: building a sequence, reading the beats back,
 * generating from them and baking them onto the asset are all driven over MCP with no UI in the
 * picture. This half is the looking and the dragging.
 */
class FMotionForgeEditorModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** The library's nomad tab, so the console command and the menu entry name the same thing. */
	static const FName LibraryTabName;

private:

	/** Deferred until UToolMenus exists - extending a menu before it does registers against nothing. */
	static void RegisterLibraryMenu();

	/** Handed back by ISequencerModule so the registration can be undone on unload. */
	FDelegateHandle PromptTrackEditorHandle;

	/** The same, for the track that says what a stretch of timeline constrains. */
	FDelegateHandle ConstraintTrackEditorHandle;

	FDelegateHandle ToolMenusHandle;
};
