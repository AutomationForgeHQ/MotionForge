// Project Settings > Automation Forge > MotionForge.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MotionForgeTypes.h"
#include "MotionForgeSettings.generated.h"

class UMotionCharacter;

/**
 * Everything the pipeline needs that is not per-motion, and that the whole team shares.
 *
 * What belongs here is what the project decides: where output goes, which provider is the default,
 * what a definition falls back to. Signing in does not — a key is one person's, on one machine, so
 * it lives in Editor Preferences ▸ Automation Forge ▸ MotionForge and in the OS credential vault.
 * See UMotionForgeEditorSettings.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "MotionForge"))
class MOTIONFORGE_API UMotionForgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	UMotionForgeSettings();

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	// Its own category rather than the Plugins bucket. The family is seven settings pages, and under
	// Plugins they scatter through the alphabet among the engine's own - findable only by somebody
	// who already knows every name to look for.
	virtual FName GetCategoryName() const override { return TEXT("Automation Forge"); }

	static const UMotionForgeSettings* Get();

	// ---------------------------------------------------------------------------------------------
	// Provider
	// ---------------------------------------------------------------------------------------------

	/**
	 * Every provider registered right now, for the pickers.
	 *
	 * **Not an enum, deliberately.** Providers arrive as separate plugins - Kimodo is one, and a
	 * third party could add another - so a fixed list in this module would mean MotionForge naming
	 * its own add-ons, which is the dependency the whole family is built to avoid, and would make a
	 * new provider impossible without editing this file.
	 *
	 * A list read from the registry gives the same thing that matters to a person: a dropdown of
	 * what is actually installed, with nothing to type and nothing to spell wrong. It just stays
	 * open at the far end.
	 */
	UFUNCTION()
	static TArray<FString> GetProviderOptions();

	// Billing, the model id and the unattended mode used to live here. Billing was Uthana's plan applied
	// to every metered provider (a rented Kimodo pod was priced per generated second); it is each
	// provider's own now. A model id is a provider's private vocabulary; each provider owns its
	// default. And a mode that turned every Generate button into an unattended purchase from a page
	// nobody looks at was a trap - agents and pipelines ask for Automatic by name instead.
	// Orphaned ini lines for the removed keys are harmless.

	/**
	 * The provider new motion definitions use. Definitions that name none follow it, and the
	 * definition window says so. When only one provider is installed, it is the default.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider",
		meta = (GetOptions = "GetProviderOptions"))
	FName DefaultProviderId = NAME_None;

	/**
	 * A character new definitions get, when it suits their provider. A definition on a provider this
	 * character is not prepared for gets one that is, or asks for one.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	TSoftObjectPtr<UMotionCharacter> DefaultCharacter;

	/** Seconds between asking a provider how its jobs are doing. */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 1, ClampMax = 120, Units = "Seconds"))
	int32 PollIntervalSeconds = 5;

	/**
	 * After this long a job is marked late and asked about less often. It is not given up on: the
	 * provider may still finish it, and a paid one bills it either way. Cancel stops the waiting.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 30, Units = "Seconds"))
	int32 JobTimeoutSeconds = 900;

	// ---------------------------------------------------------------------------------------------
	// Output
	// ---------------------------------------------------------------------------------------------

	/**
	 * The content folder everything is created under: definitions, takes, characters, rigs, prompt
	 * timelines and montages, each in its own subfolder. Must be under a mounted root such as /Game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	FString OutputContentPath = TEXT("/Game/_Generated/Motion");

	// Sorted by kind, at the point of generation.
	//
	// A tool that writes every asset it makes into one folder produces something nobody can read
	// after the second run - characters, recipes, results and rigs in one list, with throwaway test
	// fixtures indistinguishable from real content. Tidying that up by hand does not hold, because
	// the next generation puts it all back; the sort has to live here, where the assets are created.
	FString GetDefinitionsPath() const { return OutputContentPath / TEXT("Definitions"); }
	FString GetTakesPath()       const { return OutputContentPath / TEXT("Takes"); }
	FString GetCharactersPath()  const { return OutputContentPath / TEXT("Characters"); }
	FString GetRigsPath()        const { return OutputContentPath / TEXT("Rigs"); }
	FString GetSequencesPath()   const { return OutputContentPath / TEXT("Sequences"); }

	/**
	 * Montages and their recipes.
	 *
	 * Owned here rather than by whoever builds them, because the Narrative add-ons build dialogue
	 * montages too and two plugins deciding separately where montages live is how half a library ends
	 * up somewhere nobody looks.
	 */
	FString GetMontagesPath() const { return OutputContentPath / TEXT("Montages"); }

	/**
	 * Intermediates, kept apart from the results.
	 *
	 * A `_Source` clip is the generator's own rig, not the game's - useful for diagnosis and never
	 * the thing to play. Filed separately so a content browser full of `Takes` shows only clips that
	 * are actually usable.
	 */
	FString GetSourceTakesPath() const { return OutputContentPath / TEXT("Takes") / TEXT("Source"); }

	/** Every path above, resolved, as one value a caller can read or report. */
	FMotionOutputPaths GetOutputPaths() const;

	/**
	 * Point the pipeline at a different content root, and persist it.
	 *
	 * Refuses anything that is not a valid content path under a mounted root, because the failure it
	 * prevents is silent: an unmounted or malformed path produces packages that are created in memory,
	 * never saved, and reported as successes.
	 *
	 * Moves nothing. Assets already written stay where they are - this decides where the *next* ones
	 * go, and anything the pipeline reads back by path (a provider rig, a curve preset) must still be
	 * findable where it actually is.
	 *
	 * @return The resolved structure. On refusal, `Problem` says why and the paths describe what is
	 *         still configured rather than what was asked for.
	 */
	static FMotionOutputPaths SetOutputRoot(const FString& ContentPath);

	// Raw downloads are kept: on a provider with no seed the file is the only copy there will ever be.
	/** Where fetched takes are kept on disk, relative to the project. Keep them: a take without a seed cannot be made again. */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	FString StagingDirectory = TEXT("Saved/MotionForge");

	// ---------------------------------------------------------------------------------------------
	// Import
	// ---------------------------------------------------------------------------------------------

	// Lived under Blender for a long time while also deciding every Kimodo import. It is an import
	// setting for every provider.
	/** Remove the root's travel so clips play on the spot. Turn off for motion that should move through the world, such as a walk to a mark. */
	UPROPERTY(config, EditAnywhere, Category = "Import")
	bool bZeroRootTranslation = true;

	/** Add a root bone when the provider's rig has none, so the engine can drive root motion later. */
	UPROPERTY(config, EditAnywhere, Category = "Import")
	bool bEnsureRootBone = true;

	// ---------------------------------------------------------------------------------------------
	// Legacy: Blender normalisation
	// ---------------------------------------------------------------------------------------------

	// Off, deliberately. A Blender FBX round trip reorients the rig and re-times the clip, and every
	// bone still matches by name, so it looks like a rig mismatch; it has cost most of a day twice. It
	// is a separate switch from the executable path because a capability being available must never
	// be the same fact as it being wanted - a machine that happened to have Blender once turned it on.
	/**
	 * Run fetched FBX clips through Blender before import. Off: the round trip rotates and re-times
	 * clips, and stays off until that is solved. Leave it off.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Legacy: Blender normalisation")
	bool bNormalizeImportedClips = false;

	/** Blender, for anything that needs it. Setting it turns nothing on by itself. */
	UPROPERTY(config, EditAnywhere, Category = "Legacy: Blender normalisation", meta = (FilePathFilter = "exe"))
	FFilePath BlenderExecutable;

	/** Seconds to allow one normalisation before stopping it. */
	UPROPERTY(config, EditAnywhere, Category = "Legacy: Blender normalisation", meta = (ClampMin = 10, Units = "Seconds"))
	int32 BlenderTimeoutSeconds = 120;

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	/** Absolute path of the staging directory, created on demand. */
	FString GetAbsoluteStagingDirectory() const;

	/** True when a Blender path is set and the file is actually there. */
	bool IsBlenderConfigured(FString& OutReason) const;

	/**
	 * Keep the intermediate clip built on the provider's rig, alongside the retargeted result.
	 *
	 * Only ever produced on the retarget path, where a clip lands on the generator's own skeleton as
	 * `AS_<Name>_Source` and is then moved onto the game's as `AS_<Name>`. Nothing plays the
	 * intermediate and nothing references it at runtime.
	 *
	 * Off, because it is derivable twice over: the downloaded `.mfmo` in the staging directory
	 * rebuilds it without touching the provider, and a seeded generator reproduces the whole clip
	 * from prompt, model and seed. Keeping it doubles the asset count in the output folder and
	 * leaves anyone else on the project guessing which of two similarly-named animations to use.
	 *
	 * Worth turning on while iterating on a rig or a bone map: re-retargeting from the source is
	 * seconds, where regenerating is minutes and possibly money.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Retargeting")
	bool bKeepSourceClips = false;
};
