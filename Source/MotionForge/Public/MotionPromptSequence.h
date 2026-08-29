// Reading a prompt off a timeline, and writing one onto it.

#pragma once

#include "CoreMinimal.h"
#include "MotionPromptSequence.generated.h"

class ULevelSequence;
class UMotionDef;
class UMovieSceneMotionPromptTrack;

/**
 * How a prompt track stands relative to the definition it names.
 *
 * **Divergence is normal and correct**, so none of these is an error. A sequence wins the moment it is
 * assigned and editing it is the entire point - `Ahead` is the working state, not a warning.
 */
UENUM(BlueprintType)
enum class EMotionPromptSync : uint8
{
	/** The track names no definition. Its beats reach nothing. */
	NoDefinition		UMETA(DisplayName = "No Definition"),

	/**
	 * The track names a definition that does not name it back.
	 *
	 * Two sequences can name the same definition and only one can be its Constraint Sequence, so this
	 * is a plain fact rather than a conflict: these beats are not what that definition generates from.
	 */
	NotLinkedBack		UMETA(DisplayName = "Not Linked Back"),

	/** The beats equal the definition's own prompt and durations. Usually just after a pull. */
	Matches				UMETA(DisplayName = "In Sync"),

	/** The sequence has been edited since. **This is what generates.** Bake to write it back. */
	Ahead				UMETA(DisplayName = "Ahead"),

	/** The track has beats that cannot be read - see the problems on the read. */
	Unreadable			UMETA(DisplayName = "Unreadable")
};

/** One beat, as the generator will receive it. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionPromptBeat
{
	GENERATED_BODY()

	/** The sentence, without its full stop. Full stops are added when the beats are joined. */
	UPROPERTY(BlueprintReadOnly, Category = "Beat")
	FString Text;

	/** Where this beat begins in the clip. The first beat is normally zero. */
	UPROPERTY(BlueprintReadOnly, Category = "Beat")
	float StartSeconds = 0.f;

	/** How long it lasts. This is the number that reaches the provider as `BeatSeconds`. */
	UPROPERTY(BlueprintReadOnly, Category = "Beat")
	float Seconds = 0.f;

	/**
	 * The same two numbers on the generator's frame grid, which is where they are actually spent.
	 *
	 * Reported rather than derived by the reader, because the rounding is the point: the generator
	 * computes `int(duration * fps)` and **truncates**, so a boundary that lands a thousandth of a
	 * second short of a frame loses that frame with nothing said. Snapping on read is what makes the
	 * seconds above exactly representable, and printing the frames is how you check that it did.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Beat")
	int32 StartFrame = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Beat")
	int32 Frames = 0;
};

/** What a prompt track says, and what is wrong with it. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionPromptRead
{
	GENERATED_BODY()

	/** False when the definition carries no prompt sequence, in which case its own fields are the truth. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	bool bFromSequence = false;

	/** The sequence the beats were read from, empty when they were not. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	FString SequencePath;

	/** The beats joined into one prompt, which is what is sent. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	FString Prompt;

	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	TArray<FMotionPromptBeat> Beats;

	/** The sum of the beats, which is the clip's length. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	float TotalSeconds = 0.f;

	/** The rate the boundaries were snapped to - the provider's, never the timeline's display rate. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	int32 FrameRate = 0;

	/**
	 * What the reader found and did not fix.
	 *
	 * Reported rather than normalised, deliberately. A tool that silently closes a gap between two
	 * beats produces a clip that does not match the timeline the artist is looking at, and the next
	 * question - "why is this beat shorter than I drew it" - has no answer anywhere. Every entry here
	 * names a section and says what it would cost.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	TArray<FString> Problems;
};

/**
 * The prompt track as data: building one from a definition, and reading one back.
 *
 * Deliberately free of Sequencer. Everything here is `UMovieScene` data - sections, ranges and
 * strings - so the whole of it works with no editor module, no track editor, and nothing open. That
 * is what lets the behaviour be built and proven before the affordance to drag a section exists, and
 * it is why a failure in the track editor cannot take the pipeline down with it.
 *
 * **The sequence wins while it is present.** Same rule as `FMotionControl::ConstraintSequence`, and
 * the same sequence: with a prompt track on it, the track is the truth for the prompt and its beats,
 * and the definition's own `Prompt` and `BeatSeconds` are ignored rather than merged. Two sources of
 * truth for one clip is how a definition ends up asking for something nobody can find. Baking writes
 * the track back onto the asset, for when the sequence is later removed.
 */
struct MOTIONFORGE_API FMotionPromptSequence
{
	/** The prompt track on a sequence, or null. A sequence may legitimately have none. */
	static UMovieSceneMotionPromptTrack* FindTrack(const ULevelSequence* Sequence);

	/**
	 * Read the beats off a sequence.
	 *
	 * **Sections are defined by their start times.** Each start marks the end of the beat before it,
	 * so the beats tile by construction and there is no way to author a clip with a hole in it. The
	 * last beat ends where its own section ends, because nothing follows to say otherwise.
	 *
	 * A section whose own end disagrees with the start of the next one is therefore not an error and
	 * not silently repaired - it is reported, with the duration that will actually be used. Dragging a
	 * section's right edge is a common way to try to lengthen a beat, and doing it that way does
	 * nothing at all except leave a visible gap, which is worth being told once.
	 *
	 * @param FrameRate The generator's native rate, from `FMotionProviderCaps`. Never the sequence's
	 *        display rate - the rate on the timeline is a viewing preference, and a clip whose beats
	 *        moved because somebody switched the timeline to 60fps would be a miserable thing to debug.
	 * @return false with OutError set when there is no track, or nothing readable on it.
	 */
	static bool Read(
		const ULevelSequence* Sequence,
		int32 FrameRate,
		FMotionPromptRead& Out,
		FString& OutError);

	/**
	 * The beats a definition will actually be generated from.
	 *
	 * Reads the prompt track on the definition's `Control.ConstraintSequence` when there is one, and
	 * falls back to the definition's own `Prompt` and `Control.BeatSeconds` when there is not - so a
	 * caller never has to ask which route produced the answer.
	 *
	 * @return false only when a track is present and cannot be read. A definition with no sequence is
	 *         a success with `bFromSequence` false, because that is the ordinary case.
	 */
	static bool Resolve(
		const UMotionDef* Definition,
		int32 FrameRate,
		FMotionPromptRead& Out,
		FString& OutError);

	/**
	 * Replace a sequence's prompt track with the definition's prompt, one section per beat.
	 *
	 * The definition's prompt is cut the way a segmenting provider cuts it - at every full stop - so
	 * what appears on the timeline is what the generator was always going to do with it. A prompt that
	 * divides into four beats when its author meant three shows that on the first read, which is the
	 * entire argument for the track.
	 *
	 * Durations come from `Control.BeatSeconds` when it has one entry per beat, and otherwise from an
	 * even split of `Length` across them, landing on whole frames with the remainder going to the
	 * earliest beats - the same division the runner would have made.
	 */
	static bool Populate(
		ULevelSequence* Sequence,
		const UMotionDef* Definition,
		int32 FrameRate,
		FString& OutError);

	/**
	 * Join beats into the prompt that is sent.
	 *
	 * ". " between them and a full stop at the end, because on a segmenting provider a full stop is a
	 * beat break and nothing else. Each beat's own trailing full stops are trimmed first, so a text
	 * that already ends in one does not buy an empty segment.
	 */
	static FString JoinBeats(const TArray<FMotionPromptBeat>& Beats);

	/**
	 * How many beats a provider that segments on full stops would find in this prompt.
	 *
	 * Mirrors the division exactly, including the two properties of it that surprise people: an empty
	 * fragment is dropped, so a trailing full stop buys nothing, and **a decimal point divides a
	 * prompt** - "hold for 2.5 seconds" is two beats. This is a check, never a correction; the
	 * provider is the authority on its own splitting and one that does not segment ignores all of it.
	 */
	static int32 CountProviderBeats(const FString& Prompt);

	// ---------------------------------------------------------------------------------------------
	// The link between a sequence and the definition that reads it
	// ---------------------------------------------------------------------------------------------

	/**
	 * Point a definition at this track's sequence, and the track at the definition.
	 *
	 * Both sides in one call, so they cannot disagree. Passing null clears the track's reference and
	 * leaves the definition alone - a definition that stops being named here has not necessarily
	 * stopped wanting this sequence, and silently unsetting somebody's Constraint Sequence because
	 * they cleared a label would be a poor trade.
	 *
	 * Repointing a definition that already names a *different* sequence is allowed and logged with the
	 * name of the sequence left behind.
	 */
	static bool LinkDefinition(
		class UMovieSceneMotionPromptTrack* Track, UMotionDef* Definition, FString& OutError);

	/**
	 * Lay the definition's own prompt out on this track, and put a character in the sequence.
	 *
	 * The setup gesture from the sequence side: assign a definition, pull, and there are beats to drag
	 * and a body to pose. Replaces the track's beats outright rather than merging - a pull is "show me
	 * what the asset says", and half of one prompt joined to half of another is not that.
	 */
	static bool PullFromDefinition(
		class UMovieSceneMotionPromptTrack* Track, int32 FrameRate, FString& OutError);

	/**
	 * Bind a character on the definition's skeleton into the sequence, if it has none.
	 *
	 * Spawned rather than possessed, and with its spawn track: a possessable binds to whatever actor
	 * is in the level that happens to be open, and a spawnable without a spawn track never spawns -
	 * which reads as the wrong character rather than as no character when constraints are harvested.
	 */
	static bool EnsureCharacter(
		class ULevelSequence* Sequence, const UMotionDef* Definition, FString& OutError);

	/**
	 * Put the generated take's pose at one moment onto the sequence's Control Rig, as keys.
	 *
	 * The gesture the loop was missing. Scrub to where the clip looks right, copy that pose onto the
	 * rig, then drag the rig's keys to where the clip goes wrong - and those moments become
	 * constraints on the next generation.
	 *
	 * **The engine does the bone-to-control solve.** `LoadAnimSequenceIntoControlRigSectionWithRange`
	 * reads a frame range out of an animation and lands it on the rig's controls, so nothing here has
	 * to know that `upperarm_r_fk_ctrl` drives `upperarm_r`, or which way an IK/FK switch is thrown.
	 * Mapping controls by naming convention would work on this rig and quietly mis-pose the next one.
	 *
	 * @param SourceTick Where to read the pose from, in the sequence's tick resolution.
	 * @param TargetTick Where to put it. The same moment, normally - move the key afterwards.
	 */
	static bool CopyTakePoseToRig(
		ULevelSequence* Sequence,
		FFrameNumber SourceTick,
		FFrameNumber TargetTick,
		FString& OutError);

	/**
	 * Put the definition's authored constraint poses onto the sequence's Control Rig, as keys.
	 *
	 * The direction that was missing. Beats travel both ways - Pull and Bake - but constraints only
	 * travelled sequence to asset, so a definition that already had keys showed none on its timeline,
	 * and building a sequence from it silently dropped them out of sight. Worse, the moment anything
	 * *was* keyed on the rig the sequence won outright and the authored keys stopped being sent at all.
	 *
	 * With this they arrive as rig keys like any other, so old and new sit on one timeline and go out
	 * together.
	 *
	 * Each pose becomes a one-frame animation and is loaded onto the controls by the same engine call
	 * that copies a pose out of a take, so nothing here maps bones to controls - and a key driven by a
	 * pose asset arrives the same way, for free.
	 *
	 * @return the number of keys placed. Zero with no error means the definition had none to place.
	 */
	static int32 PushConstraintsToRig(
		ULevelSequence* Sequence,
		const UMotionDef* Definition,
		int32 FrameRate,
		FString& OutError);

	/**
	 * How this track stands relative to the definition it names, and a sentence saying so.
	 *
	 * Cheap enough to call from a Slate attribute: it compares beat count and durations rather than
	 * re-harvesting anything. `OutDetail` is written for a human reading a tooltip.
	 */
	static EMotionPromptSync GetSyncState(
		const class UMovieSceneMotionPromptTrack* Track, FString& OutDetail);
};
