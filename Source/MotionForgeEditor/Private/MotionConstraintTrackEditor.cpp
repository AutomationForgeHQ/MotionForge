#include "MotionConstraintTrackEditor.h"

#include "MotionForge.h"
#include "MovieSceneMotionConstraintTrack.h"
#include "MovieSceneMotionPromptTrack.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "MVVM/Views/ViewUtilities.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

namespace MotionConstraintEditorPrivate
{
	static FText NameOf(EMotionConstraintType Type)
	{
		return StaticEnum<EMotionConstraintType>()->GetDisplayNameTextByValue(
			static_cast<int64>(Type));
	}
}

FText FMotionConstraintSectionInterface::GetSectionTitle() const
{
	const UMovieSceneMotionConstraintSection* Section =
		Cast<UMovieSceneMotionConstraintSection>(WeakSection.Get());

	return Section ? MotionConstraintEditorPrivate::NameOf(Section->Type) : FText::GetEmpty();
}

FText FMotionConstraintSectionInterface::GetSectionToolTip() const
{
	return FText::Format(
		LOCTEXT("SpanToolTip",
			"Poses keyed on the Control Rig within this span constrain: {0}.\n\n"
			"Outside every span, the sequence-wide type on the Motion Prompt track applies."),
		GetSectionTitle());
}

void FMotionConstraintSectionInterface::BuildSectionContextMenu(
	FMenuBuilder& MenuBuilder, const FGuid& ObjectBinding)
{
	UMovieSceneMotionConstraintSection* Section =
		Cast<UMovieSceneMotionConstraintSection>(WeakSection.Get());

	if (Section == nullptr)
	{
		return;
	}

	MenuBuilder.BeginSection(TEXT("MotionConstraintSpan"), LOCTEXT("SpanSection", "Constrains"));

	const UEnum* Types = StaticEnum<EMotionConstraintType>();

	for (int32 Index = 0; Index < Types->NumEnums() - 1; ++Index)
	{
		const EMotionConstraintType Type =
			static_cast<EMotionConstraintType>(Types->GetValueByIndex(Index));

		MenuBuilder.AddMenuEntry(
			Types->GetDisplayNameTextByIndex(Index),
			Types->GetToolTipTextByIndex(Index),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakSection = WeakSection, Type]()
				{
					if (UMovieSceneMotionConstraintSection* Live =
							Cast<UMovieSceneMotionConstraintSection>(WeakSection.Get()))
					{
						const FScopedTransaction Transaction(
							LOCTEXT("SetSpanType", "Set What This Span Constrains"));
						Live->Modify();
						Live->Type = Type;
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakSection = WeakSection, Type]()
				{
					const UMovieSceneMotionConstraintSection* Live =
						Cast<UMovieSceneMotionConstraintSection>(WeakSection.Get());
					return Live && Live->Type == Type;
				})),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	}

	MenuBuilder.EndSection();
}

TSharedRef<ISequencerTrackEditor> FMotionConstraintTrackEditor::CreateTrackEditor(
	TSharedRef<ISequencer> InSequencer)
{
	return MakeShared<FMotionConstraintTrackEditor>(InSequencer);
}

FText FMotionConstraintTrackEditor::GetDisplayName() const
{
	return LOCTEXT("MotionConstraintTrackEditor", "Motion Constraint Track");
}

bool FMotionConstraintTrackEditor::SupportsType(TSubclassOf<UMovieSceneTrack> TrackClass) const
{
	return TrackClass == UMovieSceneMotionConstraintTrack::StaticClass();
}

bool FMotionConstraintTrackEditor::SupportsSequence(UMovieSceneSequence* InSequence) const
{
	return InSequence != nullptr && InSequence->IsA<ULevelSequence>();
}

TSharedRef<ISequencerSection> FMotionConstraintTrackEditor::MakeSectionInterface(
	UMovieSceneSection& SectionObject,
	UMovieSceneTrack& Track,
	FGuid ObjectBinding)
{
	return MakeShared<FMotionConstraintSectionInterface>(SectionObject);
}

void FMotionConstraintTrackEditor::BuildAddTrackMenu(FMenuBuilder& MenuBuilder)
{
	MenuBuilder.AddMenuEntry(
		LOCTEXT("AddConstraintTrack", "Motion Constraint Track"),
		LOCTEXT("AddConstraintTrackTooltip",
			"Adds a track that says what poses constrain, and when. Draw a span over the moments where "
			"only a hand matters, and the rig keys inside it become hand constraints while everything "
			"else stays whatever the Motion Prompt track's type says."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FMotionConstraintTrackEditor::HandleAddTrack)));
}

void FMotionConstraintTrackEditor::HandleAddTrack()
{
	UMovieScene* MovieScene = GetFocusedMovieScene();
	if (MovieScene == nullptr || MovieScene->IsReadOnly())
	{
		return;
	}

	const FScopedTransaction Transaction(
		LOCTEXT("AddConstraintTrackTransaction", "Add Motion Constraint Track"));
	MovieScene->Modify();

	UMovieSceneMotionConstraintTrack* Track = MovieScene->AddTrack<UMovieSceneMotionConstraintTrack>();
	if (Track == nullptr)
	{
		return;
	}

	HandleAddSpan(Track);

	if (GetSequencer().IsValid())
	{
		GetSequencer()->OnAddTrack(Track, FGuid());
	}
}

void FMotionConstraintTrackEditor::BuildTrackContextMenu(
	FMenuBuilder& MenuBuilder, UMovieSceneTrack* Track)
{
	if (!Track->IsA<UMovieSceneMotionConstraintTrack>())
	{
		return;
	}

	MenuBuilder.BeginSection(TEXT("MotionConstraint"), LOCTEXT("ConstraintSection", "Motion Constraints"));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("AddSpanEntry", "Add Span"),
		LOCTEXT("AddSpanEntryTooltip",
			"Append a two-second span after the last one. Drag it over the moments whose poses should "
			"pin something other than the sequence-wide default."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Track]() { HandleAddSpan(Track); })));

	MenuBuilder.EndSection();
}

TSharedPtr<SWidget> FMotionConstraintTrackEditor::BuildOutlinerEditWidget(
	const FGuid& ObjectBinding,
	UMovieSceneTrack* Track,
	const FBuildEditWidgetParams& Params)
{
	return UE::Sequencer::MakeAddButton(
		LOCTEXT("AddSpan", "Span"),
		FOnClicked::CreateSP(this, &FMotionConstraintTrackEditor::HandleAddSpan, Track),
		Params.ViewModel);
}

FReply FMotionConstraintTrackEditor::HandleAddSpan(UMovieSceneTrack* Track)
{
	UMovieSceneMotionConstraintTrack* ConstraintTrack =
		Cast<UMovieSceneMotionConstraintTrack>(Track);

	if (ConstraintTrack == nullptr)
	{
		return FReply::Handled();
	}

	UMovieScene* MovieScene = ConstraintTrack->GetTypedOuter<UMovieScene>();
	if (MovieScene == nullptr || MovieScene->IsReadOnly())
	{
		return FReply::Handled();
	}

	const FScopedTransaction Transaction(
		LOCTEXT("AddSpanTransaction", "Add Motion Constraint Span"));
	ConstraintTrack->Modify();

	const FFrameRate Tick = MovieScene->GetTickResolution();
	const FFrameNumber Length =
		FFrameRate::TransformTime(FFrameTime(FFrameNumber(2)), FFrameRate(1, 1), Tick).FrameNumber;

	// After the last one, for the same reason beats append: dropping a span at the playhead would
	// overlap whatever is already there, and overlapping spans is a question with no good answer.
	FFrameNumber Start = 0;
	for (const UMovieSceneSection* Section : ConstraintTrack->GetAllSections())
	{
		if (Section && Section->HasEndFrame())
		{
			Start = FMath::Max(Start, Section->GetExclusiveEndFrame());
		}
	}

	// A hand by default, because a span exists to say something *other* than the sequence-wide answer,
	// and that answer is almost always Full Body.
	ConstraintTrack->AddSpan(
		EMotionConstraintType::RightHand, TRange<FFrameNumber>(Start, Start + Length));

	if (GetSequencer().IsValid())
	{
		GetSequencer()->NotifyMovieSceneDataChanged(
			EMovieSceneDataChangeType::MovieSceneStructureItemAdded);
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
