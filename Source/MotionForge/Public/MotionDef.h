// One motion: what to ask for, what came back, and what it became.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MotionForgeTypes.h"
#include "MotionDef.generated.h"

class UMotionCharacter;
class UMotionPipeline;
class UIKRetargeter;
class UAnimSequence;

/**
 * The editable, regenerable unit of the pipeline - one asset per motion.
 *
 * Edit the prompt, generate again. Candidates accumulate rather than being replaced, because on
 * providers with no seed a take that is discarded can never be recreated; the MotionId is the only
 * route back to it.
 *
 * This asset deliberately stops at an imported UAnimSequence. Montages, curves, notifies and any
 * gameplay wiring belong to adapters built on top of this plugin.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Motion Definition"))
class MOTIONFORGE_API UMotionDef : public UDataAsset
{
	GENERATED_BODY()

public:

	// ---------------------------------------------------------------------------------------------
	// Authoring - edit these, then generate
	// ---------------------------------------------------------------------------------------------

	// Tooltips below are what a person reads in the Settings tab; the reasoning for maintainers stays in
	// `//` comments where no tooltip can pick it up (PANEL_RULES 21).

	/**
	 * What the motion should be. Start with "A person", describe one or two actions, and say how the
	 * motion ends. On a provider that splits prompts, every full stop starts a new beat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "1 Prompt", meta = (MultiLine = true))
	FString Prompt;

	// The whole clip however many beats the prompt describes. A provider that splits the prompt shares
	// this out between the beats rather than spending it on each (KIMODO_FRAME_DOUBLING.md). Explicit
	// beat seconds, where the provider splits, replace it.
	/**
	 * Seconds for the whole clip. Each provider has its own range, and the Generate card says when this
	 * is clamped. On a provider that splits prompts, the beats share it unless beat seconds are set.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "1 Prompt", meta = (ClampMin = 1, ClampMax = 60, Units = "Seconds"))
	int32 Length = 5;

	// One by default, deliberately: on pay-as-you-go every take bills when it is submitted.
	/**
	 * How many takes one Generate makes. Free on a local runner, where more is better; billed per take
	 * on a paid provider, where the cost line says the price before you press Generate.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Generate", meta = (ClampMin = 1, ClampMax = 16))
	int32 Variants = 1;

	/**
	 * Seconds of the imported clip to keep, as start and end. Zero keeps the whole clip. Useful where
	 * a model pads a short action out to its minimum length.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "4 Import")
	FVector2D TrimWindow = FVector2D::ZeroVector;

	/** Who the motion is for. Each provider needs a character prepared for it; the Character card lists the ones that suit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "2 Character")
	TSoftObjectPtr<UMotionCharacter> Character;

	// Per clip rather than per character: it is a property of the motion, and changing it needs nothing
	// rebuilt. It must retarget between the same two rigs as the character's.
	/**
	 * A different IK Retargeter for this one clip, instead of the character's. Most often for a hand
	 * that must meet an object and wants IK goals on the hands. Empty uses the character's.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "4 Import", AdvancedDisplay)
	TSoftObjectPtr<UIKRetargeter> RetargeterOverride;

	/**
	 * Which provider generates this motion. None follows the project default, and the window says
	 * which one that is. Switching keeps each provider's own settings and picks a character that
	 * suits the new provider.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Generate",
		meta = (GetOptions = "/Script/MotionForge.MotionForgeSettings.GetProviderOptions"))
	FName ProviderId;

	// Everything below is authored content that is not a provider setting: poses pinned at moments, the
	// timeline the prompt may live on, and per-beat seconds. The sampler fields inside FMotionControl
	// (seed, steps, guidance, post-process, splitting) are no longer read from here - they live in
	// the provider's pipeline, and PostLoad moves any older values across.
	/**
	 * Constraint poses, the prompt timeline, and seconds per beat. Edit these on the prompt timeline or
	 * in the Kimodo Direct card; the provider's own settings live in its pipeline.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "5 Direct", AdvancedDisplay)
	FMotionControl Control;

	/**
	 * Each provider's own settings, one instance per provider this definition has used.
	 *
	 * Kept rather than replaced on a switch, so Kimodo's seed and steps are still there after a round
	 * trip through Uthana. Edited in the Generate card; agents use Set Motion Pipeline Option.
	 */
	UPROPERTY(Instanced, BlueprintReadOnly, Category = "3 Generate")
	TArray<TObjectPtr<UMotionPipeline>> Pipelines;

	/**
	 * The character last used with each provider, so switching back picks it again.
	 */
	UPROPERTY()
	TMap<FName, TSoftObjectPtr<UMotionCharacter>> CharacterByProvider;

	/** The number the next take gets. Takes are numbered across every generation, never restarting. */
	UPROPERTY()
	int32 NextTakeNumber = 1;

	// Kept so definitions saved before providers declared their own settings still load. Read once by
	// MigrateLegacySettings and never written back - the _DEPRECATED suffix loads the old name and
	// saves nothing. Remove once every definition has been resaved.
	UPROPERTY()
	FString ModelId_DEPRECATED;

	UPROPERTY()
	bool bRewritePrompt_DEPRECATED = true;

	// ---------------------------------------------------------------------------------------------
	// State - written by the pipeline, read by everyone
	// ---------------------------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	EMotionDefStatus Status = EMotionDefStatus::Draft;

	/** Every take ever generated for this definition. Never pruned automatically. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	TArray<FMotionCandidate> Candidates;

	/** The chosen take. Set by review, or by automatic mode picking the first success. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString SelectedMotionId;

	/** Why the last operation failed. Cleared when one succeeds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString LastError;

	/** Batch this definition is currently part of, if any. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString ActiveBatchId;

	// ---------------------------------------------------------------------------------------------
	// Result
	// ---------------------------------------------------------------------------------------------

	/** The imported animation. This is what the plugin exists to produce. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	TSoftObjectPtr<UAnimSequence> ImportedSequence;

	/**
	 * The take the imported animation was made from.
	 *
	 * Not the same fact as Selected Motion Id, and not derivable from the status: generating more
	 * takes, or choosing one whose import then fails, leaves the clip in the game exactly as it was.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FString ImportedMotionId;

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	/** The candidate matching SelectedMotionId, or null when nothing is chosen. */
	const FMotionCandidate* FindSelectedCandidate() const;

	/** The candidate with this motion id, or null. */
	const FMotionCandidate* FindCandidate(const FString& MotionId) const;
	FMotionCandidate* FindCandidateMutable(const FString& MotionId);

	/** True when jobs are in flight and a second submit would be a duplicate charge. */
	UFUNCTION(BlueprintPure, Category = "State")
	bool IsBusy() const;

	/** Candidates that finished successfully and could be downloaded. */
	UFUNCTION(BlueprintPure, Category = "State")
	int32 CountUsableCandidates() const;

	/** Seconds that would be fetched if every usable candidate were downloaded. */
	UFUNCTION(BlueprintPure, Category = "State")
	int32 EstimateDownloadSeconds(bool bSelectedOnly) const;

	/**
	 * Apply an authoring spec, leaving pipeline state untouched. Only the fields the spec fills in are
	 * changed, so an update can set one thing without restating the rest.
	 *
	 * @param OutProblems Settings that were refused - an unknown option, a model the provider does not
	 *        offer. Everything else in the spec is still applied.
	 */
	void ApplySpec(const FMotionDefSpec& Spec, TArray<FString>* OutProblems = nullptr);

	/**
	 * Fill in the project's defaults for whatever is still unset - the provider, and a character when
	 * the default one suits that provider. Never overwrites a field that has been chosen.
	 *
	 * Called on every newly created definition, whether an agent made it through CreateMotionDef or a
	 * person made one in the Content Browser, so both arrive configured the same way.
	 */
	void ApplyProjectDefaults();

	/** Move to a new status, recording an error when moving to Failed. */
	void SetStatus(EMotionDefStatus NewStatus, const FString& Error = FString());

	// ---------------------------------------------------------------------------------------------
	// Pipelines
	// ---------------------------------------------------------------------------------------------

	/** The provider this definition generates on: its own, or the project default. */
	FName GetResolvedProviderId() const;

	/** This definition's settings for a provider, or null when it has never used that provider. */
	UMotionPipeline* FindPipeline(FName InProviderId) const;

	/**
	 * This definition's settings for a provider, created with the provider's defaults on first use.
	 * Null when the provider declares no settings or is not installed.
	 */
	UMotionPipeline* GetOrCreatePipeline(FName InProviderId);

	/**
	 * The settings a request is built from: this definition's instance when it has one, the
	 * provider's defaults when it has not. Never creates anything, so it is safe from a const read.
	 */
	const UMotionPipeline* GetPipelineForRead(FName InProviderId) const;

	/**
	 * Move settings saved before providers declared their own into the pipelines. Safe to call at any
	 * time; does nothing once done, and nothing while the provider plugins are not yet loaded.
	 *
	 * @return true when anything moved, meaning the asset should be saved.
	 */
	bool MigrateLegacySettings();

	/** The seconds a provider-agnostic reader should show as this clip's length: the beats' sum where they set it. */
	float GetAuthoredLengthSeconds() const;

	/** Settings moved into pipelines when this was loaded, and not saved since. Migrate Definitions saves it. */
	bool bMigratedOnLoad = false;

	// ---------------------------------------------------------------------------------------------
	// UObject
	// ---------------------------------------------------------------------------------------------

	virtual void PostLoad() override;
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;

#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;

	/** The provider as it was before an edit, so the character it used can be remembered for it. */
	FName ProviderBeforeEdit;
#endif

	/** Asset registry tag names, read by the library so listing definitions loads none of them. */
	static const FName TagStatus;
	static const FName TagProvider;
	static const FName TagTakeCount;
	static const FName TagClip;
	static const FName TagPrompt;
	static const FName TagCharacter;
};
