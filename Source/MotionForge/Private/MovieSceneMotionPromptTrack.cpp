#include "MovieSceneMotionPromptTrack.h"

#include "MotionDef.h"
#include "MotionForge.h"
#include "MotionPromptSequence.h"

#include "MovieScene.h"

#define LOCTEXT_NAMESPACE "MotionForge"

// -------------------------------------------------------------------------------------------------
// Section
// -------------------------------------------------------------------------------------------------

UMovieSceneMotionPromptSection::UMovieSceneMotionPromptSection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// A beat has a start and an end or it has no duration to state, and duration is the only thing
	// this section carries besides its text. An infinite range would read as "this beat lasts the
	// whole clip", which is never what was meant and would come back as a beat list of length one.
	bSupportsInfiniteRange = false;
}

// -------------------------------------------------------------------------------------------------
// Track
// -------------------------------------------------------------------------------------------------

UMovieSceneMotionPromptTrack::UMovieSceneMotionPromptTrack(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TrackTint = FColor(88, 60, 120, 96);

	// No default section. A track that creates one on being added would put a beat covering all of
	// time onto every new sequence, and a one-beat prompt reading "" is worse than an empty track:
	// it generates, and it generates nothing anybody asked for.
	bSupportsDefaultSections = false;

	// Nothing on this track evaluates - it produces no entities, no template, and no evaluation tree
	// entries. Leaving the eval options at their defaults is harmless because there is nothing for
	// them to switch on, but a track that says so explicitly is cheaper to reason about than one that
	// leaves a reader wondering what it does at runtime. The answer is nothing, ever.
	EvalOptions.bCanEvaluateNearestSection = false;
	EvalOptions.bEvaluateInPreroll = false;
	EvalOptions.bEvaluateInPostroll = false;
}

UMovieSceneMotionPromptSection* UMovieSceneMotionPromptTrack::AddBeat(
	const FString& Text, const TRange<FFrameNumber>& Range)
{
	UMovieSceneMotionPromptSection* Section =
		NewObject<UMovieSceneMotionPromptSection>(this, NAME_None, RF_Transactional);

	Section->BeatText = Text;
	Section->SetRange(Range);
	Section->SetRowIndex(0);

	AddSection(*Section);
	return Section;
}

TArray<UMovieSceneMotionPromptSection*> UMovieSceneMotionPromptTrack::GetBeatsInOrder() const
{
	TArray<UMovieSceneMotionPromptSection*> Beats;
	Beats.Reserve(Sections.Num());

	for (UMovieSceneSection* Section : Sections)
	{
		if (UMovieSceneMotionPromptSection* Beat = Cast<UMovieSceneMotionPromptSection>(Section))
		{
			// A section with no lower bound has no place in the order, and sorting on it would put it
			// somewhere arbitrary. Reading reports it; ordering simply leaves it out of the comparison
			// by treating it as starting at zero, which is where an unbounded section visually begins.
			Beats.Add(Beat);
		}
	}

	Beats.Sort([](const UMovieSceneMotionPromptSection& A, const UMovieSceneMotionPromptSection& B)
	{
		const FFrameNumber StartA = A.HasStartFrame() ? A.GetInclusiveStartFrame() : FFrameNumber(0);
		const FFrameNumber StartB = B.HasStartFrame() ? B.GetInclusiveStartFrame() : FFrameNumber(0);
		return StartA < StartB;
	});

	return Beats;
}

void UMovieSceneMotionPromptTrack::AddSection(UMovieSceneSection& Section)
{
	Sections.AddUnique(&Section);
}

void UMovieSceneMotionPromptTrack::RemoveSection(UMovieSceneSection& Section)
{
	Sections.Remove(&Section);
}

void UMovieSceneMotionPromptTrack::RemoveSectionAt(int32 SectionIndex)
{
	Sections.RemoveAt(SectionIndex);
}

void UMovieSceneMotionPromptTrack::RemoveAllAnimationData()
{
	Sections.Empty();
}

bool UMovieSceneMotionPromptTrack::HasSection(const UMovieSceneSection& Section) const
{
	return Sections.Contains(&Section);
}

bool UMovieSceneMotionPromptTrack::IsEmpty() const
{
	return Sections.Num() == 0;
}

const TArray<UMovieSceneSection*>& UMovieSceneMotionPromptTrack::GetAllSections() const
{
	return Sections;
}

UMovieSceneSection* UMovieSceneMotionPromptTrack::CreateNewSection()
{
	return NewObject<UMovieSceneMotionPromptSection>(this, NAME_None, RF_Transactional);
}

bool UMovieSceneMotionPromptTrack::SupportsType(TSubclassOf<UMovieSceneSection> SectionClass) const
{
	return SectionClass == UMovieSceneMotionPromptSection::StaticClass();
}

void UMovieSceneMotionPromptTrack::PostLoad()
{
	Super::PostLoad();

	// See the header. This is the one place that can afford to load it, and every reader after this
	// point gets a straight answer from Get() instead of "null, but which kind of null".
	Definition.LoadSynchronous();
}

#if WITH_EDITORONLY_DATA
FText UMovieSceneMotionPromptTrack::GetDefaultDisplayName() const
{
	return LOCTEXT("MotionPromptTrackName", "Motion Prompt");
}
#endif

#if WITH_EDITOR
void UMovieSceneMotionPromptTrack::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Changed = PropertyChangedEvent.GetPropertyName();

	if (Changed == GET_MEMBER_NAME_CHECKED(UMovieSceneMotionPromptTrack, Definition))
	{
		// Write through, so the label and the definition's own Constraint Sequence cannot disagree.
		// Done here rather than only on the Pull button because the Details panel is where somebody
		// will set this, and a field that has to be followed by pressing something else to take
		// effect is a field that gets set and forgotten.
		FString Error;
		if (!FMotionPromptSequence::LinkDefinition(this, Definition.LoadSynchronous(), Error))
		{
			UE_LOG(LogMotionForge, Warning, TEXT("%s"), *Error);
		}
	}
	else if (Changed == GET_MEMBER_NAME_CHECKED(UMovieSceneMotionPromptTrack, ConstraintType))
	{
		// Same rule, same reason. This is a duplicate of a field on the asset rather than a second
		// source of truth, so it has to write through the instant it changes or it becomes one.
		if (UMotionDef* Def = Definition.LoadSynchronous())
		{
			Def->Control.ConstraintSequenceType = ConstraintType;
			Def->MarkPackageDirty();

			UE_LOG(LogMotionForge, Log, TEXT("'%s' now constrains %s."),
				*Def->GetName(), *StaticEnum<EMotionConstraintType>()->GetNameStringByValue(
					static_cast<int64>(ConstraintType)));
		}
	}
}
#endif

#undef LOCTEXT_NAMESPACE
