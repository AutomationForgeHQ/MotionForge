// Showing what a stretch of timeline constrains, and letting it be changed.

#pragma once

#include "CoreMinimal.h"
#include "ISequencerSection.h"
#include "MovieSceneTrackEditor.h"

/** One span, drawn with the name of what it pins. */
class FMotionConstraintSectionInterface
	: public FSequencerSection
	, public TSharedFromThis<FMotionConstraintSectionInterface>
{
public:

	explicit FMotionConstraintSectionInterface(UMovieSceneSection& InSection)
		: FSequencerSection(InSection)
	{}

	virtual FText GetSectionTitle() const override;
	virtual FText GetSectionToolTip() const override;

	/** Right-click a span to change what it pins. Every type, in one list. */
	virtual void BuildSectionContextMenu(FMenuBuilder& MenuBuilder, const FGuid& ObjectBinding) override;
};

/**
 * Makes `UMovieSceneMotionConstraintTrack` appear in Sequencer.
 *
 * The track is only ever a handful of spans, so it needs no editing beyond "add one" and "change what
 * this one pins" - the poses live on the Control Rig and the times come from the rig's keys.
 */
class FMotionConstraintTrackEditor : public FMovieSceneTrackEditor
{
public:

	static TSharedRef<ISequencerTrackEditor> CreateTrackEditor(TSharedRef<ISequencer> InSequencer);

	explicit FMotionConstraintTrackEditor(TSharedRef<ISequencer> InSequencer)
		: FMovieSceneTrackEditor(InSequencer)
	{}

	virtual FText GetDisplayName() const override;
	virtual bool SupportsType(TSubclassOf<UMovieSceneTrack> TrackClass) const override;
	virtual bool SupportsSequence(UMovieSceneSequence* InSequence) const override;

	virtual TSharedRef<ISequencerSection> MakeSectionInterface(
		UMovieSceneSection& SectionObject,
		UMovieSceneTrack& Track,
		FGuid ObjectBinding) override;

	virtual void BuildAddTrackMenu(FMenuBuilder& MenuBuilder) override;
	virtual void BuildTrackContextMenu(FMenuBuilder& MenuBuilder, UMovieSceneTrack* Track) override;

	virtual TSharedPtr<SWidget> BuildOutlinerEditWidget(
		const FGuid& ObjectBinding,
		UMovieSceneTrack* Track,
		const FBuildEditWidgetParams& Params) override;

private:

	void HandleAddTrack();

	/** Append a two-second span, after the last one. */
	FReply HandleAddSpan(UMovieSceneTrack* Track);
};
