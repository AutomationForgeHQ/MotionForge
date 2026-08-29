// A prompt, laid out in time: one section per beat.

#pragma once

#include "CoreMinimal.h"
#include "MotionControl.h"
#include "MovieSceneNameableTrack.h"
#include "MovieSceneSection.h"
#include "MovieSceneMotionPromptTrack.generated.h"

class UMotionDef;

/**
 * One beat of a prompt, occupying the stretch of timeline it will be generated over.
 *
 * The section's **length is the beat's duration**, so dragging the boundary between two sections is
 * exactly editing `BeatSeconds = [2, 7, 3]` - which is the whole reason this exists. A prompt with
 * three sentences and a total length already *is* a timeline; a segmenting model cuts it into pieces
 * and gives each one a duration whether or not anybody said so. Writing that down in a text box is
 * how a full stop inside "hold for 2.5 seconds" silently becomes an extra beat too short to perform.
 *
 * Nothing here evaluates. A prompt track drives nothing in the level and produces no animation - it
 * is authoring data that happens to live on a timeline, read at generation time and at no other.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Motion Prompt Beat"))
class MOTIONFORGE_API UMovieSceneMotionPromptSection : public UMovieSceneSection
{
	GENERATED_BODY()

public:

	UMovieSceneMotionPromptSection(const FObjectInitializer& ObjectInitializer);

	/**
	 * What this beat asks for, as one sentence with no full stop of its own.
	 *
	 * The full stops are added when the sections are joined into a prompt, because on a segmenting
	 * provider **a full stop is a beat break and nothing else**. A full stop typed in here therefore
	 * splits this section into two beats at the far end while the timeline still shows one, which is
	 * precisely the ambush the track exists to prevent - so reading the track reports it rather than
	 * quietly repairing the text.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beat", meta = (MultiLine = true))
	FString BeatText;
};

/**
 * The beats of one generated clip, in the order they are performed.
 *
 * A root track rather than one hanging off a character binding: the prompt describes the whole clip,
 * not one actor's contribution to it, and the sequence's binding exists so poses can be authored -
 * see `FMotionControl::ConstraintSequence`, which is the same sequence.
 *
 * **Constraint keys and prompt beats index the same timeline**, which is a genuinely lucky alignment
 * rather than a design: Kimodo's own multi-prompt path crops each constraint to its beat's window and
 * re-bases it, so an existing constraint track sits alongside this one and needs no translation at
 * all. One sequence carries both.
 *
 * Sections tile by construction - see `FMotionPromptSequence::Read` for what "by construction" costs
 * and what it reports when the artist drags one out of line.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Motion Prompt Track"))
class MOTIONFORGE_API UMovieSceneMotionPromptTrack : public UMovieSceneNameableTrack
{
	GENERATED_BODY()

public:

	UMovieSceneMotionPromptTrack(const FObjectInitializer& ObjectInitializer);

	/**
	 * The Motion Definition these beats are the prompt for.
	 *
	 * **Editable, and it writes through.** Setting it points that definition's
	 * `Control → Constraint Sequence` at this sequence, so the two cannot disagree and there is never
	 * anything to reconcile. That is what makes the sync status honest rather than decorative.
	 *
	 * It is also the setup gesture: assign a definition here, press **Pull**, and the beats and a
	 * character on the definition's skeleton appear. Sequence first, definition second - which is the
	 * order somebody with Sequencer already open actually works in.
	 *
	 * Assigning a definition that already points at *another* sequence repoints it, and says so in the
	 * log naming the sequence left behind. That is allowed on purpose: "use this sequence instead" is
	 * an ordinary thing to want.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Prompt")
	TSoftObjectPtr<UMotionDef> Definition;

	/**
	 * What the poses keyed in this sequence pin - the whole body, one hand, where the character stands.
	 *
	 * The same value as the definition's `Control → Constraint Sequence Type`, surfaced here because
	 * this is where you are when you decide it, and because a setting you cannot see from the timeline
	 * is one you find out about by generating. Writes through in both directions, like `Definition`.
	 *
	 * Full Body suits the usual shape - a rest pose at each end and the beat in the middle. Choose an
	 * end effector when the sequence exists to place a hand: everything the model is **not** told stays
	 * free for it to invent, which is the part you still want it doing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Prompt")
	EMotionConstraintType ConstraintType = EMotionConstraintType::FullBody;

	/** Add a beat covering the given range. The range is the beat's duration. */
	UMovieSceneMotionPromptSection* AddBeat(const FString& Text, const TRange<FFrameNumber>& Range);

	/**
	 * The beats in start-time order, which is the order they are performed in.
	 *
	 * Sections are stored in the order they were added, and an artist who authors the last beat first
	 * would otherwise get a prompt that reads backwards - with nothing anywhere saying so.
	 */
	TArray<UMovieSceneMotionPromptSection*> GetBeatsInOrder() const;

	//~ UMovieSceneTrack
	virtual void AddSection(UMovieSceneSection& Section) override;
	virtual void RemoveSection(UMovieSceneSection& Section) override;
	virtual void RemoveSectionAt(int32 SectionIndex) override;
	virtual void RemoveAllAnimationData() override;
	virtual bool HasSection(const UMovieSceneSection& Section) const override;
	virtual bool IsEmpty() const override;
	virtual const TArray<UMovieSceneSection*>& GetAllSections() const override;
	virtual UMovieSceneSection* CreateNewSection() override;
	virtual bool SupportsType(TSubclassOf<UMovieSceneSection> SectionClass) const override;

	/**
	 * One row, always.
	 *
	 * Beats are a sequence, not a set of layers. Two rows would let two beats claim the same instant
	 * and there is no meaning to give that - the prompt is read top to bottom in time.
	 */
	virtual bool SupportsMultipleRows() const override { return false; }

#if WITH_EDITORONLY_DATA
	virtual FText GetDefaultDisplayName() const override;
#endif

	/**
	 * Loads `Definition`, so everything downstream can use `Get()` without faulting assets in.
	 *
	 * A soft pointer to an unloaded asset reads **null**, not "unset" - and nothing distinguishes the
	 * two at the point of reading. The status on the track row is computed on every paint, so it
	 * cannot afford to load anything; left to `Get()` alone it reported "no def" for a track whose
	 * Details panel plainly showed a definition, and disabled Generate with the wrong reason.
	 *
	 * Loading it once, here, is the cheap end of that trade: a Motion Definition is a small data asset,
	 * and a Level Sequence carrying a prompt track is one somebody has just opened to work on.
	 */
	virtual void PostLoad() override;

#if WITH_EDITOR
	/** Writes `Definition` through to that definition's Constraint Sequence when it is changed here. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:

	UPROPERTY()
	TArray<TObjectPtr<UMovieSceneSection>> Sections;
};
