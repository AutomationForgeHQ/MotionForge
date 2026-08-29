// Project Settings > Plugins > MotionForge.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MotionForgeTypes.h"
#include "MotionForgeSettings.generated.h"

class UMotionCharacter;

/**
 * Everything the pipeline needs that is not per-motion, including signing in to a provider.
 *
 * The API key field below is deliberately unlike the rest of this class. It carries no `config`
 * specifier and is `Transient`, so it is never written to an ini; typing into it hands the value
 * straight to the OS credential vault and blanks the field again. What persists is the vault entry,
 * which lives outside the project directory and therefore cannot be copied, committed or zipped along
 * with the project.
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

#if WITH_EDITOR
	virtual void PostInitProperties() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---------------------------------------------------------------------------------------------
	// Credentials
	// ---------------------------------------------------------------------------------------------

	/** Which provider the fields below act on. */
	UPROPERTY(Transient, EditAnywhere, Category = "Credentials")
	FName CredentialProviderId = TEXT("Uthana");

	/**
	 * Paste an API key here to sign in.
	 *
	 * Stored in the OS credential vault the moment you commit the field, which is then cleared. The
	 * value is never saved to a config file and cannot be read back out through this panel.
	 */
	UPROPERTY(Transient, EditAnywhere, Category = "Credentials",
		meta = (PasswordField = true, DisplayName = "API Key"))
	FString ApiKeyEntry;

	/**
	 * Whether a key is available, and where it is coming from.
	 *
	 * Acting on it happens from the console, not from here:
	 *   MotionForge.TestConnection      one cheap authenticated call, result under LogMotionForge
	 *   MotionForge.CredentialStatus    re-read this line, e.g. after setting an environment variable
	 *   MotionForge.ClearKey            forget the stored key
	 *
	 * There are no buttons because there cannot be. UFUNCTION(CallInEditor) does not render on a
	 * UDeveloperSettings page - the details customization discards archetype objects before drawing
	 * them and a settings panel edits the CDO, which is one. Adding real buttons here needs an
	 * IDetailCustomization; until then the console is the honest surface rather than a control that
	 * silently does not exist.
	 */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Credentials", meta = (DisplayName = "Status"))
	FString CredentialStatus;

	/** Re-read CredentialStatus. Called on load and whenever the fields above change. */
	void RefreshStatus();

	// ---------------------------------------------------------------------------------------------
	// Provider
	// ---------------------------------------------------------------------------------------------

	/** Which provider new motion definitions use when they do not name one. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	FName DefaultProviderId = TEXT("Uthana");

	/** Model new definitions use when they do not name one. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	FString DefaultModelId = TEXT("text-to-motion-3.0");

	/** Character new definitions use when they do not name one. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	TSoftObjectPtr<UMotionCharacter> DefaultCharacter;

	/**
	 * What this account is billed on. Set it to match the plan, because the two invert the workflow.
	 *
	 * Pay-as-you-go bills every generated second, kept or discarded, and downloads are free - so ask
	 * for few variants and download all of them. A subscription bills downloads instead and
	 * generation is free - so generate generously and download only the keeper.
	 *
	 * Nothing can detect this; only the account knows.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	EMotionBillingModel BillingModel = EMotionBillingModel::PayPerGeneratedSecond;

	/**
	 * What one billed second costs, for turning estimates into money.
	 *
	 * Zero means estimates report seconds only. Uthana's text-to-motion-3.0 is $0.10 per generated
	 * second on pay-as-you-go.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 0.0))
	float RatePerBilledSecond = 0.f;

	/** Purely for display alongside an estimate. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	FString Currency = TEXT("USD");

	/** Seconds between job status polls. Providers document a recommended floor - respect it. */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 1, ClampMax = 120, Units = "Seconds"))
	int32 PollIntervalSeconds = 5;

	/** Give up on a job after this long. Stops a stuck batch polling forever. */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 30, Units = "Seconds"))
	int32 JobTimeoutSeconds = 900;

	// ---------------------------------------------------------------------------------------------
	// Pipeline
	// ---------------------------------------------------------------------------------------------

	/**
	 * How far a batch runs unattended.
	 *
	 * Human-in-the-loop is the default because generation is typically free while downloads are
	 * metered - stopping to choose is what keeps the bill down.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
	EMotionPipelineMode DefaultMode = EMotionPipelineMode::HumanInTheLoop;

	/** Content path new definitions and imported animations are created under. */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
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

	/**
	 * Where downloaded files land before import, relative to the project directory.
	 *
	 * Raw downloads are kept rather than deleted: on providers with no seed a generation cannot be
	 * reproduced, so the file on disk is the only copy that will ever exist.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
	FString StagingDirectory = TEXT("Saved/MotionForge");

	/**
	 * Frame rate requested from the provider, and the rate clips are sampled at on import.
	 *
	 * Match the provider's native rate. Uthana generates at 60, and asking it for less **re-times
	 * rather than resamples** - a four second clip fetched at 30 arrives as an 8.3 second one,
	 * imports without complaint, and is simply wrong. Nothing in the response says so; the only
	 * symptom is a duration that disagrees with the provider's own viewer.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline", meta = (ClampMin = 1, ClampMax = 240))
	int32 TargetFrameRate = 60;

	// ---------------------------------------------------------------------------------------------
	// Blender
	// ---------------------------------------------------------------------------------------------

	/**
	 * Run downloaded clips through a Blender round trip before importing. **Off, deliberately.**
	 *
	 * A Blender FBX round trip reorients the rig: the importer rebuilds bone orientations and the
	 * exporter applies an axis conversion, so a clip that was upright arrives rotated and twisted -
	 * and ten frames longer, because the re-export re-times it. Every bone still matches by name and
	 * nothing warns, which makes it look like a rig mismatch and sends you hunting in the wrong
	 * place. It has now cost most of a day twice.
	 *
	 * Importing the provider's file untouched, onto the provider's own skeleton, is correct. This
	 * stays off until the axis handling in `normalize_motion.py` is solved and verified against the
	 * provider's web viewer.
	 *
	 * **This is a separate switch from the executable path on purpose.** Normalisation used to be
	 * "off" only because nobody had set a Blender path, which is not a decision - it is a fact about
	 * one computer. Opening the project on a machine that happens to have Blender installed silently
	 * turned it back on, and every clip imported afterwards was rotated: same file, same code,
	 * different desk. A capability being *available* must never be the same fact as it being *wanted*.
	 *
	 * What is given up meanwhile: trimming, and stripping root translation. Uthana can do the second
	 * itself with `in_place` on the download, which is free and lossless - prefer that.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Blender")
	bool bNormalizeImportedClips = false;

	/**
	 * Blender executable, for whatever needs one.
	 *
	 * Safe to fill in and worth keeping filled in: on its own it does nothing, because the round trip
	 * is gated by `bNormalizeImportedClips` above. Another provider may need a Blender step that does
	 * not have this one's axis problem, and it should not have to re-answer "where is Blender".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Blender", meta = (FilePathFilter = "exe"))
	FFilePath BlenderExecutable;

	/** Seconds to allow one normalisation before killing it. */
	UPROPERTY(config, EditAnywhere, Category = "Blender", meta = (ClampMin = 10, Units = "Seconds"))
	int32 BlenderTimeoutSeconds = 120;

	/** Strip root translation so clips play in place. Turn off for motion that should travel. */
	UPROPERTY(config, EditAnywhere, Category = "Blender")
	bool bZeroRootTranslation = true;

	/** Add a root bone when the provider's rig has none, so the engine can drive root motion later. */
	UPROPERTY(config, EditAnywhere, Category = "Blender")
	bool bEnsureRootBone = true;

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
