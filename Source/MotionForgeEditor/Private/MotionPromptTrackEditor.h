// Showing a beat on the timeline, and letting it be dragged.

#pragma once

#include "CoreMinimal.h"
#include "ISequencerSection.h"
#include "MovieSceneTrackEditor.h"

/**
 * One beat, drawn.
 *
 * The whole affordance is the title: the section says what it asks the character to do, and its width
 * says for how long. That is the argument for the track in one glance - a prompt whose beats are
 * visible is a prompt whose division cannot ambush you.
 *
 * Dragging comes free from `FSequencerSection`, which moves and resizes the section range, and the
 * range **is** the duration. Nothing here has to know that.
 */
class FMotionPromptSectionInterface
	: public FSequencerSection
	// Needed for FExecuteAction::CreateSP on the context-menu entries below. FSequencerSection is not
	// shared-from-this itself, and the failure is a wall of template errors inside the delegate header
	// rather than anything naming this class.
	, public TSharedFromThis<FMotionPromptSectionInterface>
{
public:

	FMotionPromptSectionInterface(UMovieSceneSection& InSection, TWeakPtr<ISequencer> InSequencer)
		: FSequencerSection(InSection)
		, WeakSequencer(InSequencer)
	{}

	virtual FText GetSectionTitle() const override;
	virtual FText GetSectionToolTip() const override;

	/**
	 * Right-click a beat: write it, split it, delete it.
	 *
	 * The Details panel can edit the text too, but a beat is a sentence you rewrite twenty times while
	 * looking at the timeline, and going to another panel for each edit is what stops people iterating.
	 */
	virtual void BuildSectionContextMenu(FMenuBuilder& MenuBuilder, const FGuid& ObjectBinding) override;

	/** Double-click a beat to write it. The obvious gesture, so it should be the one that works. */
	virtual FReply OnSectionDoubleClicked(
		const FGeometry& SectionGeometry, const FPointerEvent& MouseEvent) override;

private:

	/**
	 * Open the beat's text in a window of its own.
	 *
	 * A beat is a whole sentence and often two lines of one. Editing it in a menu strip means typing
	 * into a slot narrower than the text, with no room to see what you wrote - and the full-stop trap
	 * is invisible until generation. A window has room for both.
	 */
	void EditTextInWindow();

	/** Cut this beat in two at the playhead, keeping the total unchanged. */
	void SplitAtPlayhead();

	/**
	 * Remove this beat and give its time to the one before it.
	 *
	 * Beats tile, so deletion has an obvious right answer and leaving a hole is not it. The first beat
	 * is the exception - there is nothing in front of it, so its time goes to the beat after instead.
	 */
	void DeleteAndHeal();

	TWeakPtr<ISequencer> WeakSequencer;
};

/** Makes `UMovieSceneMotionPromptTrack` appear in Sequencer. It would otherwise not be drawn at all. */
class FMotionPromptTrackEditor : public FMovieSceneTrackEditor
{
public:

	static TSharedRef<ISequencerTrackEditor> CreateTrackEditor(TSharedRef<ISequencer> InSequencer);

	/**
	 * Put **Generate** on Sequencer's own toolbar, and only when the open sequence has a prompt track.
	 *
	 * The toolbar rather than the track's row because the row is a few dozen pixels of a narrow
	 * outliner column - a button there is invisible until you know where to look, which is the report
	 * that prompted this. Generate is the action pressed most often and it belongs where the transport
	 * controls are.
	 *
	 * Conditional because Sequencer is used for far more than motion prompts, and a button that does
	 * nothing on every other sequence is worse than no button.
	 */
	static void RegisterSequencerToolbar();

	explicit FMotionPromptTrackEditor(TSharedRef<ISequencer> InSequencer)
		: FMovieSceneTrackEditor(InSequencer)
	{}

	virtual FText GetDisplayName() const override;
	virtual bool SupportsType(TSubclassOf<UMovieSceneTrack> TrackClass) const override;
	virtual bool SupportsSequence(UMovieSceneSequence* InSequence) const override;

	virtual TSharedRef<ISequencerSection> MakeSectionInterface(
		UMovieSceneSection& SectionObject,
		UMovieSceneTrack& Track,
		FGuid ObjectBinding) override;

	/**
	 * Puts "Motion Prompt Track" in Sequencer's own **+ Add** menu.
	 *
	 * The track's own affordance rather than a MotionForge menu. Create Prompt Sequence lays a whole
	 * definition out at once and is the usual route, but a track that can only ever arrive that way
	 * cannot be added to a sequence somebody already has - and "you have to delete this and start
	 * again" is not an answer.
	 */
	virtual void BuildAddTrackMenu(FMenuBuilder& MenuBuilder) override;

	/**
	 * Right-click the track: the two directions between sequence and asset, and what state they are in.
	 *
	 * They live here rather than on the row because the row has no room and a status long enough to be
	 * useful pushes buttons off it. A menu has all the room in the world, and these are deliberate
	 * actions rather than things you press while iterating.
	 */
	virtual void BuildTrackContextMenu(FMenuBuilder& MenuBuilder, UMovieSceneTrack* Track) override;

	/**
	 * The controls on the track's row: sync status, **Pull**, **Generate**, and **+ Beat**.
	 *
	 * In place, on the thing they act on. This is a human authoring workflow - an agent can assist,
	 * but somebody sits down and does it, and a capability reachable only through a tool call is not
	 * available to the person whose job it is.
	 */
	virtual TSharedPtr<SWidget> BuildOutlinerEditWidget(
		const FGuid& ObjectBinding,
		UMovieSceneTrack* Track,
		const FBuildEditWidgetParams& Params) override;

private:

	void HandleAddTrack();

	/** Lay the named definition's prompt out here, and put a character in to pose. */
	FReply HandlePull(UMovieSceneTrack* Track);

	/** Key the take's pose at the playhead onto the rig, so it can be dragged where it is needed. */
	void HandleCopyPoseToRig(UMovieSceneTrack* Track);

	/** Bring the definition's already-authored constraint poses onto the rig, at their own frames. */
	void HandlePushConstraints(UMovieSceneTrack* Track);

	/**
	 * Write these beats back onto the definition, as its prompt, durations and length.
	 *
	 * The other direction, and the one that was missing entirely: after dragging a boundary the
	 * sequence and the asset disagree, the sequence is what generates, and there was no way from the
	 * timeline to make the asset agree. The sequence keeps winning afterwards - this is what survives
	 * deleting it.
	 */
	void HandleBake(UMovieSceneTrack* Track);

	/**
	 * Generate from these beats, and drop the result on the animation row below.
	 *
	 * Non-blocking, and idempotent underneath - the subsystem refuses a definition already in flight,
	 * so a second click cannot submit twice. What this has to do is *look* busy, because a control
	 * that appears to do nothing gets pressed again.
	 *
	 * Static because three surfaces call it: the toolbar, the track's right-click, and nothing else
	 * should have to own a copy of what it means to generate from a track.
	 */
	static void Generate(UMovieSceneTrack* Track);

	/** Whether Generate can run, and the reason when it cannot. Drives both enabled and tooltip. */
	static bool CanGenerate(UMovieSceneTrack* Track, FText& OutReason);

	/** Builds the toolbar entry, and adds nothing at all when the open sequence has no prompt track. */
	static void PopulateToolbarSection(class UToolMenu* InMenu);

	/**
	 * Append a beat.
	 *
	 * Appended rather than inserted at the playhead, because beats **tile**: a new one dropped in the
	 * middle would either overlap the beat already there or split it, and neither is what "add a beat"
	 * means. The end of the last beat is the only place a new one can go without moving another.
	 */
	FReply HandleAddBeat(UMovieSceneTrack* Track);
};
