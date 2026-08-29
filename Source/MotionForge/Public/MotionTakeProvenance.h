#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "MotionTakeProvenance.generated.h"

class UAnimSequence;
class UMotionDef;

/**
 * What made this clip, written onto the clip.
 *
 * **Not on the definition, and that is the entire point.** A definition can be edited, renamed,
 * regenerated or deleted, and the animation outlives all of it. Every question that cost real time in
 * this project was one the *asset* should have been able to answer about itself:
 *
 * - "is this Uthana at 60fps, which is correct, or Kimodo at 60fps, which means it plays at double
 *   speed?" - four innocent clips were accused of a defect and needlessly regenerated on a metered
 *   provider because nothing distinguished them by inspection.
 * - "did the Blender round trip run on this one?" - the answer was "it depends which computer you
 *   imported it on", and finding that out took most of a day.
 * - "which of these two assets with the same name in different folders am I looking at?"
 *
 * Attached as `UAssetUserData`, so it needs no subclass of `UAnimSequence`, survives cooking, shows in
 * the asset's Details panel under *Asset User Data*, and is readable from Blueprint, Python and the
 * toolsets.
 *
 * **Nothing here is used to make decisions.** It is a record. The moment the pipeline starts *reading*
 * provenance to decide what to do, a clip somebody hand-copied becomes a bug report.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionBeatRecord
{
	GENERATED_BODY()

	/** The sentence this beat was generated from, as the provider split it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Beat")
	FString Text;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Beat")
	float Seconds = 0.f;
};

/**
 * One thing that was done to the clip after it was generated.
 *
 * **A generated clip and a generated-then-corrected clip are not the same artefact**, and nothing
 * about the animation itself says which one you are holding. A body-penetration fix, a Blender round
 * trip, a retarget: each changes what the data is, and each has to be answerable later by the asset.
 *
 * Appended, never rewritten, because these are events. Two fixes are two entries, and running the
 * same fix twice is worth seeing.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionProcessingRecord
{
	GENERATED_BODY()

	/**
	 * What was done, as a stable id: `blender.normalise`, `physics.depenetrate`.
	 *
	 * A name rather than an enum, so a step somebody else writes records itself on the same terms as
	 * ours. Use the constants below for the ones we ship, and something dotted and durable otherwise.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FName StepId;

	/** What did it, with a version where it has one. `Blender 4.2`, `MotionForge depenetrate v1`. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FString Tool;

	/** One line a person reads: "12 frames corrected, deepest 3.4cm at the left forearm". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing", meta = (MultiLine = true))
	FString Summary;

	/**
	 * What the step was told to do.
	 *
	 * Free-form because every step's settings are its own. Enough to argue about a result later - a
	 * tolerance, a mode, an iteration count - without needing the definition that ran it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	TMap<FName, FString> Settings;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FDateTime At;

	/** The pipeline run that did it, when it came from one. Empty when a person ran it by hand. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FString RunId;
};

UCLASS(BlueprintType)
class MOTIONFORGE_API UMotionTakeProvenance : public UAssetUserData
{
	GENERATED_BODY()

public:

	// --- who made it -----------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	FString ProviderId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	FString ModelId;

	/**
	 * Where the runner was, and whether it was this machine.
	 *
	 * A hosted runner and a local one are different computers and can differ in output - which is
	 * exactly how the Blender-round-trip defect hid, so it is worth recording where a clip was made.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	FString RunnerUrl;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	bool bRunnerWasLocal = true;

	// --- what was asked for ----------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Recipe", meta = (MultiLine = true))
	FString Prompt;

	/**
	 * The beats and their durations, when the provider segments a prompt.
	 *
	 * Empty on a provider that does not, and empty when the beats were shared out evenly rather than
	 * stated - in which case Prompt and Duration are enough to reconstruct them.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Recipe")
	TArray<FMotionBeatRecord> Beats;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Recipe")
	int32 Seed = -1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Recipe")
	int32 DiffusionSteps = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Recipe")
	int32 RequestedLengthSeconds = 0;

	// --- what came back --------------------------------------------------------------------------

	/**
	 * The provider's own frame rate, and the clip's frames and length.
	 *
	 * Kept together because **the three of them are a check**: `FrameCount / DurationSeconds` must
	 * equal `NativeFrameRate`. 240 keys in four seconds is a 60fps clip, and if the model runs at 30
	 * then that clip holds twice the motion and plays at double speed - while passing every other
	 * test, including a duration that is exactly what was asked for. Three library clips shipped that
	 * way for a week. See `LooksMisRated`.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	int32 NativeFrameRate = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	int32 FrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	float DurationSeconds = 0.f;

	/**
	 * Whether the Blender round trip ran on this clip.
	 *
	 * Recorded because it reorients the rig, and because whether it ran used to depend on nothing more
	 * than whether the computer doing the import happened to have Blender installed.
	 *
	 * **A summary of the Processing list below, not a second source of truth.** It is written from
	 * that list and never independently, because two records of one fact drift and the drift is
	 * invisible. Kept because assets already hold it and it reads well in the Details panel.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	bool bWasNormalized = false;

	/** True when this clip was retargeted from the provider's own rig rather than built on ours. Same rule. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	bool bWasRetargeted = false;

	// --- what was done to it afterwards ------------------------------------------------------------

	/**
	 * Everything done to this clip after it came back, oldest first.
	 *
	 * Generation is not in here - that is everything above. This is the part that was missing: a clip
	 * whose body penetration was measured and corrected is a different artefact from one that was not,
	 * and until this existed the asset could not say which it was.
	 *
	 * Cleared by a re-stamp, and that is correct: a clip re-imported from the provider is new data, so
	 * a fix applied to what was there before no longer describes it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	TArray<FMotionProcessingRecord> Processing;

	/** Whether this step has ever been applied. */
	bool WasProcessedBy(FName StepId) const
	{
		return Processing.ContainsByPredicate(
			[StepId](const FMotionProcessingRecord& Record) { return Record.StepId == StepId; });
	}

	/** The most recent application of a step, or nullptr. The last one is the one that decided the data. */
	const FMotionProcessingRecord* FindLastProcessing(FName StepId) const
	{
		for (int32 Index = Processing.Num() - 1; Index >= 0; --Index)
		{
			if (Processing[Index].StepId == StepId)
			{
				return &Processing[Index];
			}
		}

		return nullptr;
	}

	/** Fills in the Processing list for a clip stamped before it existed. */
	virtual void PostLoad() override;

	// --- where to find it again ------------------------------------------------------------------

	/** The provider-side handle. Losing it loses the take. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Origin")
	FString MotionId;

	/** The definition that asked for it - a lead, not a guarantee, since definitions get renamed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Origin")
	FString DefinitionPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Origin")
	FDateTime GeneratedAt;

	/**
	 * The engine version that imported it.
	 *
	 * Cheap, and it dates a clip against a known-bad window - which is how three double-speed clips
	 * were eventually identified as a group rather than one at a time.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Origin")
	FString EngineVersion;

	/** frames ÷ duration, or 0 when there is nothing to divide. */
	float DerivedFrameRate() const
	{
		return DurationSeconds > KINDA_SMALL_NUMBER ? FrameCount / DurationSeconds : 0.f;
	}

	/**
	 * True when the clip's own numbers disagree with the rate it claims.
	 *
	 * The whole reason the three fields above are recorded together. Tolerant by a frame, because a
	 * builder that writes `(NumKeys - 1) / fps` as the length is off by exactly that much and is not
	 * wrong.
	 */
	bool LooksMisRated() const
	{
		if (NativeFrameRate <= 0 || FrameCount <= 1)
		{
			return false;
		}

		return FMath::Abs(DerivedFrameRate() - NativeFrameRate) > 1.5f;
	}
};

/** One clip's provenance, flattened for reporting. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionProvenanceRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Provenance") FString AssetPath;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") FString ProviderId;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") FString ModelId;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") FString MotionId;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") int32 FrameCount = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") int32 NativeFrameRate = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") float DurationSeconds = 0.f;

	/** frames ÷ duration. Differs from NativeFrameRate only when something is wrong. */
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") float DerivedFrameRate = 0.f;

	/** **The finding.** True when this clip's own numbers say it plays at the wrong speed. */
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") bool bLooksMisRated = false;

	UPROPERTY(BlueprintReadOnly, Category = "Provenance") bool bWasNormalized = false;
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") bool bWasRetargeted = false;

	/**
	 * Everything done to the clip after generation, oldest first, as step ids.
	 *
	 * **The answer to "was this corrected, or is it raw?"** - which nothing in the animation itself
	 * can tell you, and which decides whether a clip is comparable with the one beside it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") TArray<FName> Processing;

	/** Empty when the clip predates provenance - which itself dates the asset. */
	UPROPERTY(BlueprintReadOnly, Category = "Provenance") FString GeneratedAt;
};

/** Writes provenance onto a finished clip. Failure is never fatal - a record is not the product. */
namespace FMotionProvenance
{
	/**
	 * The steps we ship, named once.
	 *
	 * Written into assets, so they are permanent: renaming one silently orphans every clip that
	 * recorded it. Somebody else's step uses its own dotted name and needs nothing from here.
	 */
	MOTIONFORGE_API extern const FName StepNormalise;
	MOTIONFORGE_API extern const FName StepRetarget;
	MOTIONFORGE_API extern const FName StepDepenetrate;

	/**
	 * Append one processing event to a clip's record.
	 *
	 * Appends rather than replaces, because these are things that happened. A clip with no record at
	 * all still gets one - with the generation half empty, which is what a clip that predates
	 * provenance looks like anyway - because losing the fact that it was corrected is worse than a
	 * half-filled record.
	 *
	 * @return false only when there is no sequence to write to.
	 */
	MOTIONFORGE_API bool RecordProcessing(UAnimSequence* Sequence, const FMotionProcessingRecord& Step);

	MOTIONFORGE_API void Stamp(
		UAnimSequence* Sequence,
		const UMotionDef* Definition,
		const FString& ProviderId,
		const FString& ModelId,
		const FString& MotionId,
		const FString& RunnerDescription,
		bool bRunnerWasLocal,
		int32 NativeFrameRate,
		bool bWasNormalized,
		bool bWasRetargeted);

	/** The record on a clip, or nullptr when it has none - which itself dates the asset. */
	MOTIONFORGE_API const UMotionTakeProvenance* Read(const UAnimSequence* Sequence);
}
