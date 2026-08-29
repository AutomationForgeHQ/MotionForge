// One motion: what to ask for, what came back, and what it became.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MotionForgeTypes.h"
#include "MotionDef.generated.h"

class UMotionCharacter;
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

	/**
	 * What the motion should be.
	 *
	 * Models with a minimum clip length spend the whole duration whether or not you tell them how, so
	 * an underspecified prompt comes back padded and lifeless. Describe beats, give each its own
	 * tempo, and say how the motion ends.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (MultiLine = true))
	FString Prompt;

	/**
	 * Seconds to generate, for the whole clip. Clamped to the provider's supported range at submit
	 * time.
	 *
	 * The whole clip however many beats the prompt describes - a provider that segments a prompt
	 * shares this out between them rather than spending it on each. That is worth stating because
	 * the other reading is not absurd, and a provider quietly holding it costs you a clip of the
	 * wrong length with nothing in the log. Kimodo did exactly that until 2026-08-15; see
	 * Plugins/MotionForgeKimodo/KIMODO_FRAME_DOUBLING.md.
	 *
	 * So on a segmenting provider, more beats means less time each. Budget it: three beats worth
	 * four seconds apiece is a Length of twelve, not four.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	int32 Length = 5;

	/** How many takes to generate. Free wherever generation is unmetered - be generous. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = 1, ClampMax = 16))
	int32 Variants = 4;

	/**
	 * Let the provider rewrite the prompt before generating.
	 *
	 * Some models depend on it; others flatten deliberate phrasing such as tempo adverbs. Worth
	 * testing both ways rather than assuming.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	bool bRewritePrompt = true;

	/**
	 * Seconds to keep, as [start, end]. Leave at zero to keep the whole clip.
	 *
	 * Applied during normalisation. Most clips need topping and tailing because the model pads a
	 * short action out to its minimum duration.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	FVector2D TrimWindow = FVector2D::ZeroVector;

	/** Who to generate for. Falls back to the settings default when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	TSoftObjectPtr<UMotionCharacter> Character;

	/**
	 * Retarget this clip with a different IK Retargeter than the character's usual one.
	 *
	 * Empty means use the character's. Set it when one clip needs retargeting differently from the
	 * rest of the library - most often when a hand has to meet a specific object and wants IK goals
	 * on the hands, which are wrong as a default because they turn a natural reach on a longer arm
	 * into a lockout on a shorter one.
	 *
	 * The override must retarget between the same two rigs as the character's, or the clip will not
	 * match anything else in the library. Choosing per clip rather than per character is deliberate:
	 * it is a property of the motion, it lives with the asset that describes that motion, and it
	 * does not require rebuilding anything to change.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion|Advanced")
	TSoftObjectPtr<UIKRetargeter> RetargeterOverride;

	/** Which provider to use. Falls back to the settings default when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion|Advanced")
	FName ProviderId;

	/** Provider model identifier. Falls back to the provider's default when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion|Advanced")
	FString ModelId;

	/**
	 * Seed, sampler settings and kinematic constraints.
	 *
	 * On a provider that seeds, filling this in is what turns a definition into a complete recipe -
	 * the clip becomes reproducible from the asset alone and the raw file stops being precious.
	 * Providers that cannot honour a field ignore it and say so in the log.
	 *
	 * Variants walk the seed rather than repeating it, so a fixed seed with four variants gives four
	 * different but reproducible takes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion|Advanced")
	FMotionControl Control;

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

	/** Apply an authoring spec, leaving pipeline state untouched. */
	void ApplySpec(const FMotionDefSpec& Spec);

	/** Move to a new status, recording an error when moving to Failed. */
	void SetStatus(EMotionDefStatus NewStatus, const FString& Error = FString());
};
