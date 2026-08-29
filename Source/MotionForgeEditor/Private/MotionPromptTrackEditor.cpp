#include "MotionPromptTrackEditor.h"

#include "MotionDef.h"
#include "MotionForge.h"
#include "MotionForgeSubsystem.h"
#include "MotionPromptSequence.h"
#include "MovieSceneMotionPromptTrack.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "MVVM/Views/ViewUtilities.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "Framework/Application/SlateApplication.h"
#include "SequencerToolMenuContext.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

namespace MotionPromptEditorPrivate
{
	/**
	 * Edit one beat's text in a window with room for it.
	 *
	 * **Modeless, and that is not a preference.** A modal window blocks the game thread for as long as
	 * it is open, which freezes the whole editor - including the MCP server, whose tool calls run on
	 * that thread. Left open while reading the timeline behind it, the first version produced an editor
	 * that accepted connections and answered nothing, with no window a close request could reach: an
	 * orphaned process holding the module lock. A window you are meant to leave open cannot be modal.
	 *
	 * The cost is lifetime: the section can be deleted while this is up, so it holds a weak pointer and
	 * re-checks on commit rather than capturing the object.
	 *
	 * The full-stop warning is live rather than reported at generation time, which is the whole reason
	 * this is a window and not a text field. On a segmenting provider a full stop **is** a beat break,
	 * so a decimal typed here quietly splits this beat in two at the far end; saying so while the
	 * cursor is still in the sentence is the only moment it costs nothing to fix.
	 */
	static void EditBeatText(TWeakObjectPtr<UMovieSceneMotionPromptSection> WeakSection, double Seconds)
	{
		UMovieSceneMotionPromptSection* Section = WeakSection.Get();
		if (Section == nullptr)
		{
			return;
		}

		// The text box is built **before** the window, so everything below can capture it by value as
		// a weak pointer instead of by reference into this stack frame.
		//
		// Two bugs came out of not doing that. A `[&Box]` capture survived while the window was modal,
		// because AddModalWindow kept this frame alive - and crashed on the first paint once it was
		// modeless, reading a TSharedPtr that had been destroyed on return. And `SAssignNew(Box, ...)`
		// inside the widget tree meant `TWeakPtr(Box)` taken in a sibling slot could be evaluated
		// before the assignment ever ran: operands of an overloaded `+` are not sequenced, so the
		// Apply handler would have held a null box and silently written nothing.
		TSharedRef<SMultiLineEditableTextBox> Box = SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Section->BeatText))
			.AutoWrapText(true)
			.HintText(LOCTEXT("BeatWindowHint",
				"Name the pose this beat acts on. A beat inherits the body from the beat before it, "
				"but not the words."));

		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("EditBeatTitle", "Edit Beat"))
			.SizingRule(ESizingRule::UserSized)
			.ClientSize(FVector2D(560.f, 260.f))
			.SupportsMaximize(false)
			.SupportsMinimize(false);

		// Weak, and weak in both directions: the content holds these lambdas, so a shared reference to
		// the window from inside one is a cycle that leaks the whole window.
		const TWeakPtr<SMultiLineEditableTextBox> WeakBox = Box;
		const TWeakPtr<SWindow> WeakWindow = Window;

		const auto WarningText = [WeakBox]() -> FText
		{
			TSharedPtr<SMultiLineEditableTextBox> LiveBox = WeakBox.Pin();
			if (!LiveBox.IsValid())
			{
				return FText::GetEmpty();
			}

			// Everything but a single trailing full stop, which is harmless - the join trims it.
			FString Text = LiveBox->GetText().ToString().TrimStartAndEnd();
			while (Text.EndsWith(TEXT(".")))
			{
				Text.LeftChopInline(1);
				Text.TrimEndInline();
			}

			if (Text.IsEmpty())
			{
				return LOCTEXT("BeatEmptyWarn",
					"An empty beat is refused at generation time. Write what it does, or delete it.");
			}

			if (Text.Contains(TEXT(".")))
			{
				return LOCTEXT("BeatStopWarn",
					"This contains a full stop. A provider that segments divides on full stops and "
					"nothing else - a decimal point included - so this becomes two beats at the far end "
					"while the timeline still shows one. Write numbers in words, or split the beat.");
			}

			return FText::GetEmpty();
		};

		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(12.f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(STextBlock)
					.Text(FText::Format(
						LOCTEXT("BeatDuration",
							"{0} seconds. One sentence, no full stop - they are added when the beats are "
							"joined."),
						FText::AsNumber(Seconds)))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.f)
				[
					Box
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda(WarningText)
					.ColorAndOpacity(FLinearColor(1.f, 0.72f, 0.25f))
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.f, 0.f, 6.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("BeatAccept", "Apply"))
						.OnClicked_Lambda([WeakSection, WeakBox, WeakWindow]()
						{
							// Re-checked rather than captured. The window is modeless, so the section
							// can be deleted from the timeline while it is open - and a beat window
							// left up while somebody rearranges the track is a normal thing to do.
							UMovieSceneMotionPromptSection* Live = WeakSection.Get();
							TSharedPtr<SMultiLineEditableTextBox> LiveBox = WeakBox.Pin();

							if (Live && LiveBox.IsValid())
							{
								const FScopedTransaction Transaction(
									LOCTEXT("EditBeatText", "Edit Motion Prompt Beat"));
								Live->Modify();
								Live->BeatText = LiveBox->GetText().ToString().TrimStartAndEnd();
							}

							if (TSharedPtr<SWindow> LiveWindow = WeakWindow.Pin())
							{
								LiveWindow->RequestDestroyWindow();
							}

							return FReply::Handled();
						})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("BeatCancel", "Cancel"))
						.OnClicked_Lambda([WeakWindow]()
						{
							if (TSharedPtr<SWindow> LiveWindow = WeakWindow.Pin())
							{
								LiveWindow->RequestDestroyWindow();
							}

							return FReply::Handled();
						})
					]
				]
			]);

		// Added, not run. AddWindow returns immediately and the editor keeps ticking behind it, which
		// is the whole point - see the note above about what the modal version did.
		FSlateApplication::Get().AddWindow(Window);
	}
}

FText FMotionPromptSectionInterface::GetSectionTitle() const
{
	const UMovieSceneMotionPromptSection* Section =
		Cast<UMovieSceneMotionPromptSection>(WeakSection.Get());

	if (Section == nullptr || Section->BeatText.IsEmpty())
	{
		// Named rather than blank. An empty beat is refused at generation time with a message that
		// says which one, and a section that shows nothing looks like a drawing fault instead.
		return LOCTEXT("EmptyBeat", "(no text)");
	}

	return FText::FromString(Section->BeatText);
}

FText FMotionPromptSectionInterface::GetSectionToolTip() const
{
	const UMovieSceneMotionPromptSection* Section =
		Cast<UMovieSceneMotionPromptSection>(WeakSection.Get());

	if (Section == nullptr || !Section->HasStartFrame() || !Section->HasEndFrame())
	{
		return GetSectionTitle();
	}

	const UMovieScene* MovieScene = Section->GetTypedOuter<UMovieScene>();
	if (MovieScene == nullptr)
	{
		return GetSectionTitle();
	}

	const FFrameRate Tick = MovieScene->GetTickResolution();
	const double Seconds = Tick.AsSeconds(
		FFrameTime(Section->GetExclusiveEndFrame() - Section->GetInclusiveStartFrame()));

	return FText::Format(
		LOCTEXT("BeatToolTip", "{0}\n\n{1}s. Beats are defined by their start times, so drag the "
			"next beat's left edge to change this one's length."),
		GetSectionTitle(),
		FText::AsNumber(Seconds));
}

void FMotionPromptSectionInterface::BuildSectionContextMenu(
	FMenuBuilder& MenuBuilder, const FGuid& ObjectBinding)
{
	UMovieSceneMotionPromptSection* Section =
		Cast<UMovieSceneMotionPromptSection>(WeakSection.Get());

	if (Section == nullptr)
	{
		return;
	}

	MenuBuilder.BeginSection(TEXT("MotionPromptBeat"), LOCTEXT("BeatSection", "Beat"));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("EditBeatEntry", "Edit Beat Prompt..."),
		LOCTEXT("EditBeatEntryTooltip",
			"Open this beat's text in a window, with room to read it and a warning if it contains a "
			"full stop. Double-clicking the beat does the same."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &FMotionPromptSectionInterface::EditTextInWindow)));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("SplitBeat", "Split Beat at Playhead"),
		LOCTEXT("SplitBeatTooltip",
			"Cut this beat in two where the playhead is, keeping the clip's total length. Both halves "
			"start with this beat's text - rewrite the second one."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &FMotionPromptSectionInterface::SplitAtPlayhead)));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("DeleteBeat", "Delete Beat and Close the Gap"),
		LOCTEXT("DeleteBeatTooltip",
			"Remove this beat and give its time to the beat before it, so the beats stay tiled. "
			"Deleting the first beat gives its time to the one after instead."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &FMotionPromptSectionInterface::DeleteAndHeal)));

	MenuBuilder.EndSection();
}

FReply FMotionPromptSectionInterface::OnSectionDoubleClicked(
	const FGeometry& SectionGeometry, const FPointerEvent& MouseEvent)
{
	EditTextInWindow();
	return FReply::Handled();
}

void FMotionPromptSectionInterface::EditTextInWindow()
{
	UMovieSceneMotionPromptSection* Section =
		Cast<UMovieSceneMotionPromptSection>(WeakSection.Get());

	if (Section == nullptr)
	{
		return;
	}

	double Seconds = 0.0;
	if (const UMovieScene* MovieScene = Section->GetTypedOuter<UMovieScene>())
	{
		if (Section->HasStartFrame() && Section->HasEndFrame())
		{
			Seconds = MovieScene->GetTickResolution().AsSeconds(
				FFrameTime(Section->GetExclusiveEndFrame() - Section->GetInclusiveStartFrame()));
		}
	}

	// Returns as soon as the window is up. The write happens in the window's own Apply, against a
	// weak pointer, because by then this call is long gone.
	MotionPromptEditorPrivate::EditBeatText(
		TWeakObjectPtr<UMovieSceneMotionPromptSection>(Section), Seconds);
}

void FMotionPromptSectionInterface::SplitAtPlayhead()
{
	UMovieSceneMotionPromptSection* Section =
		Cast<UMovieSceneMotionPromptSection>(WeakSection.Get());

	TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();

	if (Section == nullptr || !Sequencer.IsValid()
		|| !Section->HasStartFrame() || !Section->HasEndFrame())
	{
		return;
	}

	UMovieSceneMotionPromptTrack* Track = Section->GetTypedOuter<UMovieSceneMotionPromptTrack>();
	if (Track == nullptr)
	{
		return;
	}

	const FFrameNumber At = Sequencer->GetLocalTime().Time.FrameNumber;

	// Strictly inside, or there is nothing to split - a cut on a boundary produces a zero-length beat,
	// which the reader refuses later with a message about two beats starting at the same time.
	if (At <= Section->GetInclusiveStartFrame() || At >= Section->GetExclusiveEndFrame())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("SplitBeatTransaction", "Split Motion Prompt Beat"));
	Track->Modify();
	Section->Modify();

	const FFrameNumber End = Section->GetExclusiveEndFrame();

	// The second half carries the same text rather than none. An empty beat is refused at generation
	// time, and a split is nearly always "this sentence was really two" - so the copy is the better
	// starting point, and it is one edit away from right either way.
	Section->SetRange(TRange<FFrameNumber>(Section->GetInclusiveStartFrame(), At));
	Track->AddBeat(Section->BeatText, TRange<FFrameNumber>(At, End));

	Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemAdded);
}

void FMotionPromptSectionInterface::DeleteAndHeal()
{
	UMovieSceneMotionPromptSection* Section =
		Cast<UMovieSceneMotionPromptSection>(WeakSection.Get());

	TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();

	if (Section == nullptr || !Sequencer.IsValid())
	{
		return;
	}

	UMovieSceneMotionPromptTrack* Track = Section->GetTypedOuter<UMovieSceneMotionPromptTrack>();
	if (Track == nullptr)
	{
		return;
	}

	const TArray<UMovieSceneMotionPromptSection*> Beats = Track->GetBeatsInOrder();
	const int32 Index = Beats.IndexOfByKey(Section);

	if (Index == INDEX_NONE || !Section->HasStartFrame() || !Section->HasEndFrame())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("DeleteBeatTransaction", "Delete Motion Prompt Beat"));
	Track->Modify();

	// Beats tile, so the time has to go somewhere. Backwards by preference - extending the beat before
	// leaves every later boundary where the artist put it, where extending the one after would shift
	// the whole rest of the clip.
	if (Index > 0)
	{
		UMovieSceneMotionPromptSection* Previous = Beats[Index - 1];
		Previous->Modify();
		Previous->SetRange(
			TRange<FFrameNumber>(Previous->GetInclusiveStartFrame(), Section->GetExclusiveEndFrame()));
	}
	else if (Beats.Num() > 1)
	{
		// Nothing in front of the first beat, so the clip would otherwise start late - which is a
		// problem the reader reports and cannot fix.
		UMovieSceneMotionPromptSection* Next = Beats[1];
		Next->Modify();
		Next->SetRange(
			TRange<FFrameNumber>(Section->GetInclusiveStartFrame(), Next->GetExclusiveEndFrame()));
	}

	Track->RemoveSection(*Section);

	Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemRemoved);
}

TSharedRef<ISequencerTrackEditor> FMotionPromptTrackEditor::CreateTrackEditor(
	TSharedRef<ISequencer> InSequencer)
{
	return MakeShared<FMotionPromptTrackEditor>(InSequencer);
}

FText FMotionPromptTrackEditor::GetDisplayName() const
{
	return LOCTEXT("MotionPromptTrackEditor", "Motion Prompt Track");
}

bool FMotionPromptTrackEditor::SupportsType(TSubclassOf<UMovieSceneTrack> TrackClass) const
{
	return TrackClass == UMovieSceneMotionPromptTrack::StaticClass();
}

bool FMotionPromptTrackEditor::SupportsSequence(UMovieSceneSequence* InSequence) const
{
	// Level Sequences only. A prompt track is authored alongside the constraint poses of the same
	// clip, and those are harvested by evaluating a Level Sequence in the editor world.
	return InSequence != nullptr && InSequence->IsA<ULevelSequence>();
}

TSharedRef<ISequencerSection> FMotionPromptTrackEditor::MakeSectionInterface(
	UMovieSceneSection& SectionObject,
	UMovieSceneTrack& Track,
	FGuid ObjectBinding)
{
	return MakeShared<FMotionPromptSectionInterface>(SectionObject, GetSequencer());
}

void FMotionPromptTrackEditor::BuildAddTrackMenu(FMenuBuilder& MenuBuilder)
{
	MenuBuilder.AddMenuEntry(
		LOCTEXT("AddMotionPromptTrack", "Motion Prompt Track"),
		LOCTEXT("AddMotionPromptTrackTooltip",
			"Adds a track whose sections are the beats of a generated motion's prompt. Each section "
			"carries one beat's text and its length is that beat's duration. Point a Motion "
			"Definition's Constraint Sequence at this sequence and the track becomes its prompt."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FMotionPromptTrackEditor::HandleAddTrack)));
}

void FMotionPromptTrackEditor::HandleAddTrack()
{
	UMovieScene* MovieScene = GetFocusedMovieScene();
	if (MovieScene == nullptr || MovieScene->IsReadOnly())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddMotionPromptTrackTransaction", "Add Motion Prompt Track"));
	MovieScene->Modify();

	UMovieSceneMotionPromptTrack* Track = MovieScene->AddTrack<UMovieSceneMotionPromptTrack>();
	if (Track == nullptr)
	{
		return;
	}

	// One beat to start with, rather than an empty track. An empty prompt track offers nothing to
	// click, and the first thing anybody wants to do with it is write a beat.
	HandleAddBeat(Track);

	if (GetSequencer().IsValid())
	{
		GetSequencer()->OnAddTrack(Track, FGuid());
	}
}

bool FMotionPromptTrackEditor::CanGenerate(UMovieSceneTrack* Track, FText& OutReason)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	if (PromptTrack == nullptr)
	{
		OutReason = LOCTEXT("NoTrack", "No prompt track.");
		return false;
	}

	// Disabled with the reason, never absent. A control that vanishes when it cannot run teaches
	// nobody what is missing, and "why is there no Generate button" has no answer anywhere.
	UMotionDef* Definition = PromptTrack->Definition.Get();
	if (Definition == nullptr)
	{
		OutReason = PromptTrack->Definition.IsNull()
			? LOCTEXT("GenNoDef", "This track names no Motion Definition. Set one in Details, then Pull.")
			: LOCTEXT("GenDefNotLoaded", "The Motion Definition is not loaded yet.");
		return false;
	}

	if (Definition->IsBusy())
	{
		OutReason = FText::Format(
			LOCTEXT("GenBusy", "'{0}' is already generating. Watch the Output Log under LogMotionForge."),
			FText::FromString(Definition->GetName()));
		return false;
	}

	if (PromptTrack->GetBeatsInOrder().Num() == 0)
	{
		OutReason = LOCTEXT("GenNoBeats", "No beats on this track, so there is nothing to generate.");
		return false;
	}

	FString Detail;
	if (FMotionPromptSequence::GetSyncState(PromptTrack, Detail) == EMotionPromptSync::NotLinkedBack)
	{
		OutReason = FText::FromString(Detail);
		return false;
	}

	OutReason = FText::Format(
		LOCTEXT("GenReady",
			"Generate '{0}' from these beats. The take lands on the animation row below, replacing the "
			"one there. Free and about fifteen seconds on a local provider."),
		FText::FromString(Definition->GetName()));

	return true;
}

TSharedPtr<SWidget> FMotionPromptTrackEditor::BuildOutlinerEditWidget(
	const FGuid& ObjectBinding,
	UMovieSceneTrack* Track,
	const FBuildEditWidgetParams& Params)
{
	TWeakObjectPtr<UMovieSceneTrack> WeakTrack(Track);

	// Status first, because it is the thing that answers "will pressing this do what I think".
	const auto StatusText = [WeakTrack]() -> FText
	{
		FString Detail;
		const EMotionPromptSync State = FMotionPromptSequence::GetSyncState(
			Cast<UMovieSceneMotionPromptTrack>(WeakTrack.Get()), Detail);

		switch (State)
		{
		// Short enough to fit the fixed slot below; the tooltip carries the sentence.
		case EMotionPromptSync::NoDefinition:  return LOCTEXT("SyncNone", "no def");
		case EMotionPromptSync::NotLinkedBack: return LOCTEXT("SyncOrphan", "orphan");
		case EMotionPromptSync::Matches:       return LOCTEXT("SyncMatch", "in sync");
		case EMotionPromptSync::Ahead:         return LOCTEXT("SyncAhead", "ahead");
		default:                               return LOCTEXT("SyncUnread", "no beats");
		}
	};

	const auto StatusTip = [WeakTrack]() -> FText
	{
		FString Detail;
		FMotionPromptSequence::GetSyncState(
			Cast<UMovieSceneMotionPromptTrack>(WeakTrack.Get()), Detail);
		return FText::FromString(Detail);
	};

	// Fixed width, so a long status can never push the buttons off the row. That is exactly what
	// happened the first time: the status went from "in sync" to "sequence wins" and Pull vanished,
	// leaving no visible way to do the thing the status was describing.
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SBox)
			.WidthOverride(58.f)
			.HAlign(HAlign_Right)
			[
				SNew(STextBlock)
				.Text_Lambda(StatusText)
				.ToolTipText_Lambda(StatusTip)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			UE::Sequencer::MakeAddButton(
				LOCTEXT("AddBeat", "Beat"),
				FOnClicked::CreateSP(this, &FMotionPromptTrackEditor::HandleAddBeat, Track),
				Params.ViewModel)
		];
}

void FMotionPromptTrackEditor::BuildTrackContextMenu(FMenuBuilder& MenuBuilder, UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	if (PromptTrack == nullptr)
	{
		return;
	}

	FString Detail;
	const EMotionPromptSync State = FMotionPromptSequence::GetSyncState(PromptTrack, Detail);

	MenuBuilder.BeginSection(TEXT("MotionPrompt"), LOCTEXT("PromptSection", "Motion Prompt"));

	// The state, spelled out. The row can only afford a word, and the word is the half that raises
	// the question rather than the half that answers it.
	if (!Detail.IsEmpty())
	{
		MenuBuilder.AddWidget(
			SNew(SBox)
			.WidthOverride(420.f)
			.Padding(FMargin(12.f, 4.f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Detail))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			],
			FText::GetEmpty());
	}

	MenuBuilder.AddMenuEntry(
		LOCTEXT("AddBeatEntry", "Add Beat"),
		LOCTEXT("AddBeatEntryTooltip",
			"Append a two-second beat after the last one, growing the playback range to fit. Beats "
			"append rather than insert because they tile - a new one at the playhead would have to "
			"overlap or split what is already there."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Track]() { HandleAddBeat(Track); })));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("GenerateEntry", "Generate From These Beats"),
		TAttribute<FText>::CreateLambda([Track]()
		{
			FText Reason;
			CanGenerate(Track, Reason);
			return Reason;
		}),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([Track]() { Generate(Track); }),
			FCanExecuteAction::CreateLambda([Track]()
			{
				FText Reason;
				return CanGenerate(Track, Reason);
			})));

	MenuBuilder.AddSeparator();

	MenuBuilder.AddMenuEntry(
		LOCTEXT("PushConstraintsEntry", "Put Definition's Constraints on the Rig"),
		LOCTEXT("PushConstraintsEntryTooltip",
			"Take the poses already authored on the Motion Definition and key them onto this sequence's "
			"Control Rig, at their own frames. "
			"The direction that was missing: without it, a definition that already has constraints shows "
			"none on its timeline - and the moment anything is keyed on the rig, the sequence wins and "
			"those authored keys stop being sent at all. This puts them all on one timeline."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Track]() { HandlePushConstraints(Track); })));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyPoseEntry", "Copy Take's Pose to Rig at Playhead"),
		LOCTEXT("CopyPoseEntryTooltip",
			"Take the generated clip's pose at the playhead and key it onto this sequence's Control "
			"Rig, there. Scrub to where the take looks right, copy, then drag the rig's keys to where "
			"it goes wrong - those moments become constraints on the next generation.\n\n"
			"Needs a generated take and a Control Rig on the Motion Character."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Track]() { HandleCopyPoseToRig(Track); })));

	MenuBuilder.AddSeparator();

	MenuBuilder.AddMenuEntry(
		LOCTEXT("PullEntry", "Pull Prompt From Definition"),
		LOCTEXT("PullEntryTooltip",
			"Replace these beats with the definition's own prompt and durations, and put a character on "
			"its skeleton into the sequence to pose. This discards edits made here - it is how you go "
			"back to what the asset says."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Track]() { HandlePull(Track); })));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("BakeEntry", "Bake Sequence Into Definition"),
		LOCTEXT("BakeEntryTooltip",
			"Write everything on this sequence back onto the definition: the prompt, the beat durations, "
			"the length, and the constraint poses keyed on the rig. "
			"The sequence keeps winning afterwards and stays assigned - this is what survives deleting "
			"it, and it is how you make the asset agree after dragging a boundary."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([this, Track]() { HandleBake(Track); }),
			FCanExecuteAction::CreateLambda([State]()
			{
				// Nothing to write back from a track that is not the definition's source of truth.
				return State == EMotionPromptSync::Ahead || State == EMotionPromptSync::Matches;
			})));

	MenuBuilder.EndSection();
}

void FMotionPromptTrackEditor::HandleCopyPoseToRig(UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	if (PromptTrack == nullptr || !GetSequencer().IsValid())
	{
		return;
	}

	ULevelSequence* Sequence = PromptTrack->GetTypedOuter<ULevelSequence>();
	if (Sequence == nullptr)
	{
		return;
	}

	// Source and target are the same moment. Copying a pose onto the rig somewhere *else* is a second
	// decision, and the timeline is where you make it - drag the key afterwards.
	const FFrameNumber At = GetSequencer()->GetLocalTime().Time.FrameNumber;

	const FScopedTransaction Transaction(LOCTEXT("CopyPoseTransaction", "Copy Take Pose to Rig"));
	Sequence->Modify();

	FString Error;
	if (!FMotionPromptSequence::CopyTakePoseToRig(Sequence, At, At, Error))
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *Error);
		return;
	}

	GetSequencer()->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
}

void FMotionPromptTrackEditor::HandlePushConstraints(UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();

	if (PromptTrack == nullptr || Forge == nullptr || !GetSequencer().IsValid())
	{
		return;
	}

	UMotionDef* Definition = PromptTrack->Definition.LoadSynchronous();
	ULevelSequence* Sequence = PromptTrack->GetTypedOuter<ULevelSequence>();

	if (Definition == nullptr || Sequence == nullptr)
	{
		UE_LOG(LogMotionForge, Warning,
			TEXT("This prompt track names no Motion Definition, so there are no constraints to put on "
				 "the rig."));
		return;
	}

	const int32 FrameRate = FMath::Max(1,
		Forge->GetProviderCaps(Definition->ProviderId).NativeFrameRate);

	const FScopedTransaction Transaction(
		LOCTEXT("PushConstraintsTransaction", "Put Constraints on the Rig"));
	Sequence->Modify();

	FString Error;
	const int32 Placed =
		FMotionPromptSequence::PushConstraintsToRig(Sequence, Definition, FrameRate, Error);

	if (!Error.IsEmpty())
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *Error);
	}
	else if (Placed == 0)
	{
		UE_LOG(LogMotionForge, Log,
			TEXT("'%s' has no authored constraint poses to put on the rig."), *Definition->GetName());
	}

	GetSequencer()->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
}

void FMotionPromptTrackEditor::HandleBake(UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();

	if (PromptTrack == nullptr || Forge == nullptr)
	{
		return;
	}

	UMotionDef* Definition = PromptTrack->Definition.LoadSynchronous();
	if (Definition == nullptr)
	{
		UE_LOG(LogMotionForge, Warning,
			TEXT("This prompt track names no Motion Definition, so there is nothing to bake into."));
		return;
	}

	FString Error;
	if (!Forge->BakePromptSequence(Definition->GetPathName(), Error))
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *Error);
	}
}

FReply FMotionPromptTrackEditor::HandlePull(UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	if (PromptTrack == nullptr)
	{
		return FReply::Handled();
	}

	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	UMotionDef* Definition = PromptTrack->Definition.LoadSynchronous();

	if (Forge == nullptr || Definition == nullptr)
	{
		UE_LOG(LogMotionForge, Warning,
			TEXT("This prompt track names no Motion Definition, so there is nothing to pull. Set one "
				 "in the track's Details first."));
		return FReply::Handled();
	}

	// The generator's rate, so the beats land on whole generated frames. Asked of the provider rather
	// than taken from the timeline, whose display rate is a viewing preference.
	const int32 FrameRate = FMath::Max(1,
		Forge->GetProviderCaps(Definition->ProviderId).NativeFrameRate);

	const FScopedTransaction Transaction(LOCTEXT("PullTransaction", "Pull Prompt From Definition"));
	PromptTrack->Modify();

	FString Error;
	if (!FMotionPromptSequence::PullFromDefinition(PromptTrack, FrameRate, Error))
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *Error);
		return FReply::Handled();
	}

	if (GetSequencer().IsValid())
	{
		GetSequencer()->NotifyMovieSceneDataChanged(
			EMovieSceneDataChangeType::MovieSceneStructureItemAdded);
	}

	return FReply::Handled();
}

void FMotionPromptTrackEditor::RegisterSequencerToolbar()
{
	// The same UToolMenu SSequencer builds its transport from, extended rather than replaced. Its
	// context carries the sequencer, which is what lets the entry key off *this* sequence having a
	// prompt track instead of guessing at whichever one was focused last.
	UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("Sequencer.MainToolBar"));
	if (Toolbar == nullptr)
	{
		return;
	}

	Toolbar->AddDynamicSection(
		TEXT("MotionForgePrompt"),
		FNewToolMenuDelegate::CreateStatic(&FMotionPromptTrackEditor::PopulateToolbarSection));
}

void FMotionPromptTrackEditor::PopulateToolbarSection(UToolMenu* InMenu)
{
	USequencerToolMenuContext* Context = InMenu ? InMenu->FindContext<USequencerToolMenuContext>() : nullptr;
	TSharedPtr<ISequencer> Sequencer = Context ? Context->WeakSequencer.Pin() : nullptr;

	if (!Sequencer.IsValid())
	{
		return;
	}

	ULevelSequence* Sequence = Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence());
	UMovieSceneMotionPromptTrack* Track = FMotionPromptSequence::FindTrack(Sequence);

	// Nothing added at all, rather than a disabled button. Sequencer is used for far more than motion
	// prompts, and a permanently dead control on every cutscene is worse than no control.
	if (Track == nullptr)
	{
		return;
	}

	TWeakObjectPtr<UMovieSceneTrack> WeakTrack(Track);

	FToolMenuSection& Section = InMenu->AddSection(
		TEXT("MotionForgePromptSection"), LOCTEXT("ToolbarSection", "Motion Prompt"));

	Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		TEXT("MotionForgeGenerate"),
		FUIAction(
			FExecuteAction::CreateLambda([WeakTrack]()
			{
				FMotionPromptTrackEditor::Generate(WeakTrack.Get());
			}),
			FCanExecuteAction::CreateLambda([WeakTrack]()
			{
				FText Reason;
				return FMotionPromptTrackEditor::CanGenerate(WeakTrack.Get(), Reason);
			})),
		LOCTEXT("ToolbarGenerate", "Generate"),
		TAttribute<FText>::CreateLambda([WeakTrack]()
		{
			// The reason lives in the tooltip rather than beside the button, so a disabled control
			// still says what is missing.
			FText Reason;
			FMotionPromptTrackEditor::CanGenerate(WeakTrack.Get(), Reason);
			return Reason;
		}),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Sequencer.Actions"))));
}

void FMotionPromptTrackEditor::Generate(UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();

	FText Reason;
	if (PromptTrack == nullptr || Forge == nullptr || !CanGenerate(Track, Reason))
	{
		return;
	}

	UMotionDef* Definition = PromptTrack->Definition.LoadSynchronous();
	if (Definition == nullptr)
	{
		return;
	}

	// Automatic, so the take comes back and lands on the animation row without a second gesture. That
	// is the whole loop: change a beat, press this, watch the row below change.
	//
	// Non-blocking - it returns a batch id immediately and the import happens when the provider
	// answers. The button reads busy meanwhile because `IsBusy` is what CanGenerate checks, and the
	// subsystem refuses a definition already in flight, so a second press cannot pay twice.
	const FString BatchId = Forge->RunFullPipeline({ Definition->GetPathName() });

	if (BatchId.IsEmpty())
	{
		UE_LOG(LogMotionForge, Warning,
			TEXT("Nothing was eligible to generate for '%s'. The provider may not be running."),
			*Definition->GetName());
	}
}

FReply FMotionPromptTrackEditor::HandleAddBeat(UMovieSceneTrack* Track)
{
	UMovieSceneMotionPromptTrack* PromptTrack = Cast<UMovieSceneMotionPromptTrack>(Track);
	if (PromptTrack == nullptr)
	{
		return FReply::Handled();
	}

	UMovieScene* MovieScene = PromptTrack->GetTypedOuter<UMovieScene>();
	if (MovieScene == nullptr || MovieScene->IsReadOnly())
	{
		return FReply::Handled();
	}

	const FScopedTransaction Transaction(LOCTEXT("AddBeatTransaction", "Add Motion Prompt Beat"));
	PromptTrack->Modify();

	// Two seconds of the *display* rate, which on a prompt sequence is the generator's own rate - so
	// a fresh beat lands on whole generated frames rather than a fraction short of one, which the
	// generator would truncate away.
	const FFrameRate Tick = MovieScene->GetTickResolution();
	const FFrameNumber Length =
		FFrameRate::TransformTime(FFrameTime(FFrameNumber(2)), FFrameRate(1, 1), Tick).FrameNumber;

	FFrameNumber Start = 0;
	for (const UMovieSceneMotionPromptSection* Beat : PromptTrack->GetBeatsInOrder())
	{
		if (Beat->HasEndFrame())
		{
			Start = FMath::Max(Start, Beat->GetExclusiveEndFrame());
		}
	}

	PromptTrack->AddBeat(FString(), TRange<FFrameNumber>(Start, Start + Length));

	// The playback range follows the beats, because the beats are the clip. Left behind, the new beat
	// sits outside the range and is generated while looking like it is not part of the sequence.
	const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
	if (!Playback.HasUpperBound() || Playback.GetUpperBoundValue() < Start + Length)
	{
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), Start + Length));
	}

	if (GetSequencer().IsValid())
	{
		GetSequencer()->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemAdded);
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
