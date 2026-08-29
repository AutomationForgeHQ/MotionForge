#include "MovieSceneMotionConstraintTrack.h"

#include "MovieScene.h"

#define LOCTEXT_NAMESPACE "MotionForge"

UMovieSceneMotionConstraintSection::UMovieSceneMotionConstraintSection(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// A span says "between here and here", so it needs both ends. An infinite one would be the
	// sequence-wide default written out longhand, which already exists and needs no section.
	bSupportsInfiniteRange = false;
}

UMovieSceneMotionConstraintTrack::UMovieSceneMotionConstraintTrack(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TrackTint = FColor(120, 70, 45, 96);

	// No default section, for the same reason as the prompt track: one covering all of time on every
	// new track would silently override the sequence-wide type with whatever the default enum is.
	bSupportsDefaultSections = false;

	EvalOptions.bCanEvaluateNearestSection = false;
	EvalOptions.bEvaluateInPreroll = false;
	EvalOptions.bEvaluateInPostroll = false;
}

UMovieSceneMotionConstraintSection* UMovieSceneMotionConstraintTrack::AddSpan(
	EMotionConstraintType Type, const TRange<FFrameNumber>& Range)
{
	UMovieSceneMotionConstraintSection* Section =
		NewObject<UMovieSceneMotionConstraintSection>(this, NAME_None, RF_Transactional);

	Section->Type = Type;
	Section->SetRange(Range);
	Section->SetRowIndex(0);

	AddSection(*Section);
	return Section;
}

bool UMovieSceneMotionConstraintTrack::FindTypeAt(
	FFrameNumber Tick, EMotionConstraintType& OutType) const
{
	for (UMovieSceneSection* Section : Sections)
	{
		const UMovieSceneMotionConstraintSection* Span =
			Cast<UMovieSceneMotionConstraintSection>(Section);

		// Contains, rather than nearest. A key just outside a span is outside it - snapping to the
		// closest would make a span's edge mean something different from where it is drawn.
		if (Span && Span->GetRange().Contains(Tick))
		{
			OutType = Span->Type;
			return true;
		}
	}

	return false;
}

void UMovieSceneMotionConstraintTrack::AddSection(UMovieSceneSection& Section)
{
	Sections.AddUnique(&Section);
}

void UMovieSceneMotionConstraintTrack::RemoveSection(UMovieSceneSection& Section)
{
	Sections.Remove(&Section);
}

void UMovieSceneMotionConstraintTrack::RemoveSectionAt(int32 SectionIndex)
{
	Sections.RemoveAt(SectionIndex);
}

void UMovieSceneMotionConstraintTrack::RemoveAllAnimationData()
{
	Sections.Empty();
}

bool UMovieSceneMotionConstraintTrack::HasSection(const UMovieSceneSection& Section) const
{
	return Sections.Contains(&Section);
}

bool UMovieSceneMotionConstraintTrack::IsEmpty() const
{
	return Sections.Num() == 0;
}

const TArray<UMovieSceneSection*>& UMovieSceneMotionConstraintTrack::GetAllSections() const
{
	return Sections;
}

UMovieSceneSection* UMovieSceneMotionConstraintTrack::CreateNewSection()
{
	return NewObject<UMovieSceneMotionConstraintSection>(this, NAME_None, RF_Transactional);
}

bool UMovieSceneMotionConstraintTrack::SupportsType(TSubclassOf<UMovieSceneSection> SectionClass) const
{
	return SectionClass == UMovieSceneMotionConstraintSection::StaticClass();
}

#if WITH_EDITORONLY_DATA
FText UMovieSceneMotionConstraintTrack::GetDefaultDisplayName() const
{
	return LOCTEXT("MotionConstraintTrackName", "Motion Constraints");
}
#endif

#undef LOCTEXT_NAMESPACE
