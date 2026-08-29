// What the poses in this stretch of timeline pin.

#pragma once

#include "CoreMinimal.h"
#include "MotionControl.h"
#include "MovieSceneNameableTrack.h"
#include "MovieSceneSection.h"
#include "MovieSceneMotionConstraintTrack.generated.h"

/**
 * A stretch of timeline over which keyed poses constrain one particular thing.
 *
 * The section's range is when, and its `Type` is what. A rig key inside it becomes a constraint of
 * that type; a rig key outside every section falls back to the sequence-wide type on the Motion
 * Prompt track.
 *
 * That fallback is the point. Most clips want one answer for the whole thing and should not have to
 * draw a section to say so - this track exists for the clip that wants **the hand pinned in the
 * middle and the whole body at the ends**, which is the shape that actually comes up and which one
 * enum for the sequence cannot express.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Motion Constraint Span"))
class MOTIONFORGE_API UMovieSceneMotionConstraintSection : public UMovieSceneSection
{
	GENERATED_BODY()

public:

	UMovieSceneMotionConstraintSection(const FObjectInitializer& ObjectInitializer);

	/**
	 * What poses in this span pin.
	 *
	 * An end effector leaves everything it does not name free for the model to invent, which is
	 * usually what you want in the middle of a clip. Full Body suits the anchors at either end.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constraint")
	EMotionConstraintType Type = EMotionConstraintType::FullBody;
};

/**
 * Where constraint types change along the clip.
 *
 * Deliberately **not** the thing that holds the poses - the Control Rig does that, and a second place
 * to put a pose is a second place for it to be wrong. This track carries only the answer to *what is
 * being pinned, and when*, which is the one piece the rig cannot express.
 *
 * Sections may overlap or leave gaps; neither is an error. A gap means the sequence-wide default, and
 * where two spans overlap the first one found wins - stated so that it is a rule rather than a
 * surprise, though authoring two spans over one moment is asking a question with no good answer.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Motion Constraint Track"))
class MOTIONFORGE_API UMovieSceneMotionConstraintTrack : public UMovieSceneNameableTrack
{
	GENERATED_BODY()

public:

	UMovieSceneMotionConstraintTrack(const FObjectInitializer& ObjectInitializer);

	/** Add a span of one type. */
	UMovieSceneMotionConstraintSection* AddSpan(
		EMotionConstraintType Type, const TRange<FFrameNumber>& Range);

	/**
	 * The type in force at a moment, or nothing when no span covers it.
	 *
	 * Callers supply the fallback rather than this track knowing it, because the fallback lives on the
	 * prompt track and belongs to the definition - see `UMovieSceneMotionPromptTrack::ConstraintType`.
	 */
	bool FindTypeAt(FFrameNumber Tick, EMotionConstraintType& OutType) const;

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
	virtual bool SupportsMultipleRows() const override { return false; }

#if WITH_EDITORONLY_DATA
	virtual FText GetDefaultDisplayName() const override;
#endif

private:

	UPROPERTY()
	TArray<TObjectPtr<UMovieSceneSection>> Sections;
};
