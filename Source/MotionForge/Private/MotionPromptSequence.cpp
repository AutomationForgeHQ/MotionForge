#include "MotionPromptSequence.h"

#include "MotionCharacter.h"
#include "MotionDef.h"
#include "MotionForge.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"
#include "MovieSceneMotionPromptTrack.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/ScopeExit.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Engine/SkeletalMesh.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "ObjectTools.h"
#include "MovieSceneBindingProxy.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "UObject/Package.h"

namespace MotionPromptPrivate
{
	/**
	 * The prompt as a segmenting provider will divide it.
	 *
	 * Mirrored from `kimodo_gen`'s own splitter rather than approximated, because "equivalent" is
	 * wrong here in three ways that all fail quietly:
	 *
	 * - it splits on a full stop and **nothing else**, so `!` and `?` do not divide a prompt however
	 *   much they read like sentence ends;
	 * - empty fragments are dropped, so a trailing full stop does not buy a segment;
	 * - **a decimal point divides a prompt.** "Hold for 2.5 seconds" is two beats.
	 *
	 * The last one is the trap the prompt track exists to make visible. It is not ours to correct -
	 * rewriting somebody's prompt to get a beat count is how the plugin's prompt and the model's
	 * prompt come to differ - so this counts, and callers report.
	 */
	static TArray<FString> SplitOnFullStops(const FString& Prompt)
	{
		TArray<FString> Raw;
		Prompt.ParseIntoArray(Raw, TEXT("."), /*InCullEmpty*/ false);

		TArray<FString> Beats;
		Beats.Reserve(Raw.Num());

		for (FString& Fragment : Raw)
		{
			FString Trimmed = Fragment.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				Beats.Add(MoveTemp(Trimmed));
			}
		}

		return Beats;
	}

	/** Whole frames per beat, remainder to the earliest - the division the runner would have made. */
	static TArray<int32> EvenFrameSplit(int32 TotalFrames, int32 BeatCount)
	{
		TArray<int32> Frames;
		if (BeatCount <= 0)
		{
			return Frames;
		}

		const int32 Base = TotalFrames / BeatCount;
		const int32 Remainder = TotalFrames % BeatCount;

		Frames.Reserve(BeatCount);
		for (int32 Index = 0; Index < BeatCount; ++Index)
		{
			Frames.Add(Base + (Index < Remainder ? 1 : 0));
		}

		return Frames;
	}

	/** A section's bound in seconds. Through the tick resolution, never the display rate. */
	static double SecondsAt(const UMovieScene& MovieScene, FFrameNumber Frame)
	{
		return MovieScene.GetTickResolution().AsSeconds(FFrameTime(Frame));
	}
}

// -------------------------------------------------------------------------------------------------
// Finding
// -------------------------------------------------------------------------------------------------

UMovieSceneMotionPromptTrack* FMotionPromptSequence::FindTrack(const ULevelSequence* Sequence)
{
	if (Sequence == nullptr)
	{
		return nullptr;
	}

	const UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene == nullptr)
	{
		return nullptr;
	}

	// A root track, not one under a binding: the prompt describes the clip, not one actor's part in
	// it. Searched rather than looked up by name so a track an artist renamed still counts.
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (UMovieSceneMotionPromptTrack* Prompt = Cast<UMovieSceneMotionPromptTrack>(Track))
		{
			return Prompt;
		}
	}

	return nullptr;
}

// -------------------------------------------------------------------------------------------------
// Reading
// -------------------------------------------------------------------------------------------------

bool FMotionPromptSequence::Read(
	const ULevelSequence* Sequence,
	int32 FrameRate,
	FMotionPromptRead& Out,
	FString& OutError)
{
	using namespace MotionPromptPrivate;

	Out = FMotionPromptRead();

	if (Sequence == nullptr)
	{
		OutError = TEXT("No Level Sequence to read a prompt from.");
		return false;
	}

	const UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene == nullptr)
	{
		OutError = FString::Printf(TEXT("'%s' has no movie scene in it."), *Sequence->GetName());
		return false;
	}

	if (FrameRate <= 0)
	{
		OutError = TEXT("The generator's frame rate is not known, so beat boundaries cannot be snapped.");
		return false;
	}

	UMovieSceneMotionPromptTrack* Track = FindTrack(Sequence);
	if (Track == nullptr)
	{
		OutError = FString::Printf(
			TEXT("'%s' has no Motion Prompt track on it. Build one with Create Prompt Sequence, or "
				 "clear Constraint Sequence to author the prompt on the definition instead."),
			*Sequence->GetName());
		return false;
	}

	const TArray<UMovieSceneMotionPromptSection*> Ordered = Track->GetBeatsInOrder();
	if (Ordered.Num() == 0)
	{
		OutError = FString::Printf(
			TEXT("The Motion Prompt track on '%s' has no beats on it, so there is nothing to generate."),
			*Sequence->GetName());
		return false;
	}

	Out.bFromSequence = true;
	Out.SequencePath = Sequence->GetPathName();
	Out.FrameRate = FrameRate;

	// Every boundary onto the generator's frame grid, in one pass, before any duration is worked out.
	//
	// Snapping the starts and then subtracting is not the same as subtracting and then snapping: the
	// second lets rounding error accumulate down the clip, so the last beat of a five-beat prompt
	// arrives a frame or two adrift of where the timeline says it ends.
	TArray<int32> StartFrames;
	TArray<int32> OwnEndFrames;
	StartFrames.Reserve(Ordered.Num());
	OwnEndFrames.Reserve(Ordered.Num());

	for (int32 Index = 0; Index < Ordered.Num(); ++Index)
	{
		const UMovieSceneMotionPromptSection* Section = Ordered[Index];

		if (!Section->HasStartFrame() || !Section->HasEndFrame())
		{
			OutError = FString::Printf(
				TEXT("Beat %d on '%s' has no start or no end, so it states no duration. Give it both "
					 "ends on the timeline."),
				Index + 1, *Sequence->GetName());
			return false;
		}

		StartFrames.Add(FMath::RoundToInt(
			SecondsAt(*MovieScene, Section->GetInclusiveStartFrame()) * FrameRate));
		OwnEndFrames.Add(FMath::RoundToInt(
			SecondsAt(*MovieScene, Section->GetExclusiveEndFrame()) * FrameRate));
	}

	// The first beat is where the clip begins. A gap in front of it is time the generator is never
	// told about, so the clip comes back shorter than the timeline and every later beat is early.
	if (StartFrames[0] != 0)
	{
		Out.Problems.Add(FString::Printf(
			TEXT("The first beat starts %.2fs in rather than at zero. The clip begins at the first "
				 "beat, so that leading time is not generated and everything after it happens earlier "
				 "than the timeline shows."),
			StartFrames[0] / static_cast<float>(FrameRate)));
	}

	for (int32 Index = 0; Index < Ordered.Num(); ++Index)
	{
		const UMovieSceneMotionPromptSection* Section = Ordered[Index];

		// The start of the next beat is the end of this one. That is what makes the beats tile, and
		// it is why dragging a section's right edge on its own achieves nothing.
		const bool bLast = (Index == Ordered.Num() - 1);
		const int32 EndFrame = bLast ? OwnEndFrames[Index] : StartFrames[Index + 1];

		if (!bLast && OwnEndFrames[Index] != EndFrame)
		{
			const float Difference = (OwnEndFrames[Index] - EndFrame) / static_cast<float>(FrameRate);

			Out.Problems.Add(FString::Printf(
				TEXT("Beat %d %s the one after it by %.2fs. Beats are defined by their start times, so "
					 "this beat lasts %.2fs whatever its own right edge says - drag beat %d's left edge "
					 "to change it."),
				Index + 1,
				Difference > 0.f ? TEXT("overlaps") : TEXT("stops short of"),
				FMath::Abs(Difference),
				(EndFrame - StartFrames[Index]) / static_cast<float>(FrameRate),
				Index + 2));
		}

		const int32 Frames = EndFrame - StartFrames[Index];

		if (Frames <= 0)
		{
			OutError = FString::Printf(
				TEXT("Beat %d on '%s' lasts %d frames. Two beats start at the same time, or one starts "
					 "before the beat in front of it."),
				Index + 1, *Sequence->GetName(), Frames);
			return false;
		}

		FMotionPromptBeat Beat;
		Beat.Text = Section->BeatText.TrimStartAndEnd();
		Beat.StartFrame = StartFrames[Index];
		Beat.Frames = Frames;
		Beat.StartSeconds = StartFrames[Index] / static_cast<float>(FrameRate);
		Beat.Seconds = Frames / static_cast<float>(FrameRate);

		if (Beat.Text.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Beat %d on '%s' has no text, so it asks for %.2fs of nothing in particular. "
					 "Write what it should do, or delete it."),
				Index + 1, *Sequence->GetName(), Beat.Seconds);
			return false;
		}

		// A full stop inside a beat is the ambush this whole track exists to prevent, seen one step
		// earlier than it used to be. The joined prompt would divide into more beats than there are
		// sections, and the durations would then pair up with the wrong text.
		if (Beat.Text.LeftChop(1).Contains(TEXT(".")))
		{
			Out.Problems.Add(FString::Printf(
				TEXT("Beat %d's text contains a full stop ('%s'). A provider that segments divides on "
					 "full stops and nothing else - a decimal point included - so this beat becomes two "
					 "at the far end while the timeline still shows one. Write the number in words, or "
					 "split it into two sections here."),
				Index + 1, *Beat.Text));
		}

		Out.TotalSeconds += Beat.Seconds;
		Out.Beats.Add(MoveTemp(Beat));
	}

	Out.Prompt = JoinBeats(Out.Beats);

	// The last check anybody would think to make by hand, made every time. The prompt is what the
	// provider divides; if it does not divide into the sections it was built from, the beat durations
	// pair with the wrong text and the failure looks like the model ignoring the prompt.
	const int32 ProviderBeats = CountProviderBeats(Out.Prompt);
	if (ProviderBeats != Out.Beats.Num())
	{
		Out.Problems.Add(FString::Printf(
			TEXT("These %d section(s) join into a prompt that a segmenting provider reads as %d beat(s), "
				 "so the durations would pair with the wrong text. Fix the beat texts flagged above."),
			Out.Beats.Num(), ProviderBeats));
	}

	return true;
}

bool FMotionPromptSequence::Resolve(
	const UMotionDef* Definition,
	int32 FrameRate,
	FMotionPromptRead& Out,
	FString& OutError)
{
	Out = FMotionPromptRead();

	if (Definition == nullptr)
	{
		OutError = TEXT("No motion definition to resolve a prompt for.");
		return false;
	}

	// LoadSynchronous, not Get. A soft pointer to an unloaded asset reads null, which here would mean
	// "no prompt track" and silently generate the definition's own wording instead of the artist's.
	const ULevelSequence* Sequence = Definition->Control.ConstraintSequence.LoadSynchronous();

	if (Sequence == nullptr || FindTrack(Sequence) == nullptr)
	{
		// The ordinary case, and not a failure: the definition's own fields are the prompt.
		Out.Prompt = Definition->Prompt;
		Out.FrameRate = FrameRate;

		float Total = 0.f;
		for (const float Seconds : Definition->Control.BeatSeconds)
		{
			FMotionPromptBeat Beat;
			Beat.Seconds = Seconds;
			Beat.StartSeconds = Total;
			Beat.StartFrame = FMath::RoundToInt(Total * FMath::Max(1, FrameRate));
			Beat.Frames = FMath::RoundToInt(Seconds * FMath::Max(1, FrameRate));
			Total += Seconds;

			// No text. The definition states durations and a prompt, not which sentence is which, and
			// pairing them here would be a guess recorded as a fact. Only the track knows.
			Out.Beats.Add(MoveTemp(Beat));
		}

		Out.TotalSeconds = Total;
		return true;
	}

	// A track is present, so it is the truth - and if it will not read, that is a refusal rather than
	// a fall back. Somebody pointed this definition at a sequence and meant it; generating from
	// different words because the sequence would not parse is a worse outcome than not generating.
	return Read(Sequence, FrameRate, Out, OutError);
}

// -------------------------------------------------------------------------------------------------
// Writing
// -------------------------------------------------------------------------------------------------

bool FMotionPromptSequence::Populate(
	ULevelSequence* Sequence,
	const UMotionDef* Definition,
	int32 FrameRate,
	FString& OutError)
{
	using namespace MotionPromptPrivate;

	if (Sequence == nullptr || Definition == nullptr)
	{
		OutError = TEXT("Nothing to populate a prompt track from.");
		return false;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene == nullptr)
	{
		OutError = FString::Printf(TEXT("'%s' has no movie scene in it."), *Sequence->GetName());
		return false;
	}

	if (FrameRate <= 0)
	{
		OutError = TEXT("The generator's frame rate is not known, so beats cannot be placed on frames.");
		return false;
	}

	const TArray<FString> Texts = SplitOnFullStops(Definition->Prompt);
	if (Texts.Num() == 0)
	{
		OutError = FString::Printf(
			TEXT("'%s' has an empty prompt, so there are no beats to lay out. Write what the motion "
				 "should be first."),
			*Definition->GetName());
		return false;
	}

	// Durations from the definition when it states them, and only when it states one per beat. A
	// partial list is a disagreement about how many beats there are, and picking a reading of it
	// would put the artist's durations on the wrong sentences.
	TArray<int32> FrameCounts;

	if (Definition->Control.BeatSeconds.Num() == Texts.Num())
	{
		FrameCounts.Reserve(Texts.Num());
		for (const float Seconds : Definition->Control.BeatSeconds)
		{
			FrameCounts.Add(FMath::Max(1, FMath::RoundToInt(Seconds * FrameRate)));
		}
	}
	else
	{
		if (Definition->Control.BeatSeconds.Num() > 0)
		{
			UE_LOG(LogMotionForge, Warning,
				TEXT("'%s' states %d beat duration(s) but its prompt divides into %d beat(s), so the "
					 "durations were shared out evenly instead. The timeline now shows the division the "
					 "generator would have made."),
				*Definition->GetName(), Definition->Control.BeatSeconds.Num(), Texts.Num());
		}

		FrameCounts = EvenFrameSplit(
			FMath::Max(Texts.Num(), Definition->Length * FrameRate), Texts.Num());
	}

	// Replace the beats, **reuse the track**. The definition is being laid out on a timeline; leaving
	// sections from a previous prompt behind would produce a clip that is part one prompt and part
	// another - but removing the track and adding a fresh one throws away everything else on it,
	// starting with which definition it names.
	//
	// That is not theoretical. It is what Pull did: link the definition onto the track, then replace
	// the track, and the reference went with it - the status read "no definition" the instant the
	// button was pressed. Worse, the caller was left holding a pointer to a track that no longer
	// belonged to the movie scene.
	UMovieSceneMotionPromptTrack* Track = FindTrack(Sequence);

	if (Track != nullptr)
	{
		Track->Modify();
		Track->RemoveAllAnimationData();
	}
	else
	{
		Track = MovieScene->AddTrack<UMovieSceneMotionPromptTrack>();
	}

	if (Track == nullptr)
	{
		OutError = FString::Printf(
			TEXT("Could not add a Motion Prompt track to '%s'."), *Sequence->GetName());
		return false;
	}

	const FFrameRate Tick = MovieScene->GetTickResolution();
	const FFrameRate Native(FrameRate, 1);

	// Counted in the generator's own frames and converted to ticks per boundary, rather than
	// accumulated in ticks. A tick resolution of 24000 divides evenly by 30 and not by everything, so
	// adding a converted duration each time round lets the error walk: the last beat of a five-beat
	// prompt ends a frame adrift of where the timeline says, and the generator truncates that away.
	int32 NativeCursor = 0;
	for (int32 Index = 0; Index < Texts.Num(); ++Index)
	{
		const FFrameNumber Start =
			FFrameRate::TransformTime(FFrameTime(FFrameNumber(NativeCursor)), Native, Tick).FrameNumber;

		NativeCursor += FrameCounts[Index];

		const FFrameNumber End =
			FFrameRate::TransformTime(FFrameTime(FFrameNumber(NativeCursor)), Native, Tick).FrameNumber;

		Track->AddBeat(Texts[Index], TRange<FFrameNumber>(Start, End));
	}

	const FFrameNumber TotalTicks =
		FFrameRate::TransformTime(FFrameTime(FFrameNumber(NativeCursor)), Native, Tick).FrameNumber;

	MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), TotalTicks));
	MovieScene->SetDisplayRate(Native);

	Sequence->MarkPackageDirty();

	UE_LOG(LogMotionForge, Log,
		TEXT("'%s': %d beat(s) laid out over %.2fs at %dfps."),
		*Sequence->GetName(), Texts.Num(), NativeCursor / static_cast<float>(FrameRate), FrameRate);

	return true;
}

// -------------------------------------------------------------------------------------------------
// Text
// -------------------------------------------------------------------------------------------------

FString FMotionPromptSequence::JoinBeats(const TArray<FMotionPromptBeat>& Beats)
{
	TArray<FString> Pieces;
	Pieces.Reserve(Beats.Num());

	for (const FMotionPromptBeat& Beat : Beats)
	{
		FString Text = Beat.Text.TrimStartAndEnd();

		// Trim the author's own full stops off the end before adding ours, so a beat typed with one
		// does not produce ".." - which splits into an empty fragment that the provider then drops,
		// leaving the beat count right by accident rather than by construction.
		while (Text.EndsWith(TEXT(".")))
		{
			Text.LeftChopInline(1);
			Text.TrimEndInline();
		}

		if (!Text.IsEmpty())
		{
			Pieces.Add(MoveTemp(Text));
		}
	}

	if (Pieces.Num() == 0)
	{
		return FString();
	}

	return FString::Join(Pieces, TEXT(". ")) + TEXT(".");
}

int32 FMotionPromptSequence::CountProviderBeats(const FString& Prompt)
{
	return MotionPromptPrivate::SplitOnFullStops(Prompt).Num();
}

// -------------------------------------------------------------------------------------------------
// The three operations, on the subsystem
// -------------------------------------------------------------------------------------------------

namespace MotionPromptPrivate
{
	/**
	 * The character a prompt sequence should be built around, and the mesh to put in it.
	 *
	 * No new field was added for this: the Motion Character's own `PreviewMesh` already names the mesh
	 * that represents this character on its skeleton - it is the one exported when pairing with a
	 * provider, so it is by definition the rig the clips are for.
	 *
	 * **The skeleton's preview mesh is the fallback, not the first choice**, and this project is
	 * exactly why. `SK_Mannequin_Narrative` previews as Epic's stock `SKM_Manny` while every Motion
	 * Character on it says `SKM_Quinn`, so asking the skeleton first puts the wrong body in the
	 * sequence - which poses correctly, harvests correctly, and is the wrong person.
	 */
	static bool ResolveCharacterMesh(
		const UMotionDef& Def,
		USkeleton*& OutSkeleton,
		USkeletalMesh*& OutMesh,
		FString& OutError,
		const UMotionCharacter** OutCharacter = nullptr)
	{
		const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

		const TSoftObjectPtr<UMotionCharacter> CharacterPtr =
			Def.Character.IsNull() ? Settings->DefaultCharacter : Def.Character;

		const UMotionCharacter* Character = CharacterPtr.LoadSynchronous();
		if (Character == nullptr)
		{
			OutError = FString::Printf(
				TEXT("'%s' has no Motion Character and there is no default in settings, so there is no "
					 "skeleton to build a sequence for."),
				*Def.GetName());
			return false;
		}

		OutSkeleton = Character->TargetSkeleton.LoadSynchronous();
		if (OutSkeleton == nullptr)
		{
			OutError = FString::Printf(
				TEXT("Motion Character '%s' has no Target Skeleton. Set it to the skeleton finished "
					 "clips end up on - the sequence binds a character on that skeleton so poses "
					 "authored in it can be harvested as constraints."),
				*Character->GetDisplayName());
			return false;
		}

		OutMesh = Character->PreviewMesh.LoadSynchronous();
		if (OutMesh == nullptr)
		{
			OutMesh = OutSkeleton->GetPreviewMesh();
		}

		if (OutCharacter)
		{
			*OutCharacter = Character;
		}

		return true;
	}
}

// -------------------------------------------------------------------------------------------------
// The link between a sequence and the definition that reads it
// -------------------------------------------------------------------------------------------------

bool FMotionPromptSequence::EnsureCharacter(
	ULevelSequence* Sequence, const UMotionDef* Definition, FString& OutError)
{
	using namespace MotionPromptPrivate;

	if (Sequence == nullptr || Definition == nullptr)
	{
		OutError = TEXT("Nothing to bind a character into.");
		return false;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene == nullptr)
	{
		OutError = FString::Printf(TEXT("'%s' has no movie scene in it."), *Sequence->GetName());
		return false;
	}

	// Already has a character. Not an error, and not something to add a second of - a sequence with two
	// in it makes the constraint harvest pick whichever it finds first.
	//
	// The rig is still checked below, because a sequence built before rigs existed has a character and
	// no way to pose it, and re-running Pull is the obvious thing somebody would try.
	const bool bHasCharacter =
		const_cast<const UMovieScene*>(MovieScene)->GetBindings().Num() > 0;

	USkeleton* Skeleton = nullptr;
	USkeletalMesh* Mesh = nullptr;
	const UMotionCharacter* Character = nullptr;
	if (!ResolveCharacterMesh(*Definition, Skeleton, Mesh, OutError, &Character))
	{
		return false;
	}

	if (Mesh == nullptr && !bHasCharacter)
	{
		OutError = FString::Printf(
			TEXT("'%s' has no preview mesh on '%s' and none on its Motion Character, so there is no "
				 "body to put in the sequence. The beats are complete without one; set a Preview Mesh "
				 "to author constraint poses in it."),
			*Definition->GetName(), *Skeleton->GetName());
		return false;
	}

	FGuid Spawned;

	if (bHasCharacter)
	{
		// Take the one already there. Its binding is what a rig track has to hang off.
		Spawned = const_cast<const UMovieScene*>(MovieScene)->GetBindings()[0].GetObjectGuid();
	}
	else
	{
	const FString Label = Mesh->GetName();

	// Spawned rather than possessed: a possessable binds to an actor in whatever level happens to be
	// open, so the sequence would resolve to nothing the moment somebody opened a different map - and
	// the constraint harvest would then report that nothing in it has the right skeleton.
	ASkeletalMeshActor* Template = NewObject<ASkeletalMeshActor>(
		MovieScene,
		ASkeletalMeshActor::StaticClass(),
		MakeUniqueObjectName(MovieScene, ASkeletalMeshActor::StaticClass(), FName(*Label)),
		RF_Transactional);

	Template->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);

	Spawned = MovieScene->AddSpawnable(Label, *Template);
	if (!Spawned.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Could not add '%s' to '%s'."), *Label, *Sequence->GetName());
		return false;
	}

	// A spawnable with no spawn track never spawns, and the failure is entirely silent: the binding is
	// there in the outliner with the right name, and the harvest reports that nothing in the sequence
	// has the right skeleton - which reads as the wrong character rather than as no character.
	// Sequencer adds this itself when a human makes a spawnable; building one in code has to as well.
	if (UMovieSceneSpawnTrack* SpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>(Spawned))
	{
		SpawnTrack->SetObjectId(Spawned);

		UMovieSceneSection* SpawnSection = SpawnTrack->CreateNewSection();
		SpawnSection->SetRange(TRange<FFrameNumber>::All());

		// Spawned for the whole clip, because every beat of it is time the character is meant to be
		// standing there being posed.
		if (UMovieSceneBoolSection* Bool = Cast<UMovieSceneBoolSection>(SpawnSection))
		{
			Bool->GetChannel().SetDefault(true);
		}

		SpawnTrack->AddSection(*SpawnSection);
	}
	}

	// A rig to pose with, disabled, holding nothing.
	//
	// This is the piece that makes constraint authoring an ordinary gesture rather than a console
	// command: key the rig where the take went wrong, and that moment is a constraint. It arrives
	// **muted** so the generated take plays underneath as a preview - the rig is for poses you choose,
	// not for re-performing what the model already did.
	//
	// **Never baked from the animation.** Baking a take onto this rig produces a key on every frame,
	// which the harvest turns into a constraint each: measured at 119 against a practical ceiling of
	// about twenty, which is not generation, it is the clip played back to itself. The rig comes from
	// the skeleton and stays empty until somebody keys it.
	if (Character->ControlRig.IsNull())
	{
		return true;
	}

	UClass* RigClass = Character->ControlRig.LoadSynchronous();
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

	if (RigClass == nullptr || World == nullptr)
	{
		UE_LOG(LogMotionForge, Warning,
			TEXT("'%s' names a Control Rig that would not load, so '%s' has no rig to pose with."),
			*Character->GetDisplayName(), *Sequence->GetName());
		return true;
	}

	FMovieSceneBindingProxy BindingProxy(Spawned, Sequence);

	if (UMovieSceneTrack* RigTrack = UControlRigSequencerEditorLibrary::FindOrCreateControlRigTrack(
			World, Sequence, RigClass, BindingProxy, /*bIsLayeredControlRig*/ false))
	{
		RigTrack->SetEvalDisabled(true);

		UE_LOG(LogMotionForge, Log,
			TEXT("'%s' has a '%s' track to pose with, disabled and empty. Key it where the take needs "
				 "holding and those moments become constraints."),
			*Sequence->GetName(), *RigClass->GetName());
	}

	Sequence->MarkPackageDirty();
	return true;
}

bool FMotionPromptSequence::LinkDefinition(
	UMovieSceneMotionPromptTrack* Track, UMotionDef* Definition, FString& OutError)
{
	if (Track == nullptr)
	{
		OutError = TEXT("No prompt track to link.");
		return false;
	}

	ULevelSequence* Sequence = Track->GetTypedOuter<ULevelSequence>();
	if (Sequence == nullptr)
	{
		OutError = TEXT("This prompt track is not inside a Level Sequence.");
		return false;
	}

	Track->Definition = Definition;
	Track->MarkPackageDirty();

	// Clearing the label does not clear the definition's own reference. A definition that stops being
	// named here has not necessarily stopped wanting this sequence, and silently unsetting somebody's
	// Constraint Sequence because they emptied a field would be a poor trade.
	if (Definition == nullptr)
	{
		return true;
	}

	// The track adopts the definition's constraint type rather than imposing its own. Linking is
	// "this definition reads these beats", not "these beats overwrite that definition" - and the
	// asset is the thing that existed first.
	Track->ConstraintType = Definition->Control.ConstraintSequenceType;

	const FString SequencePath = Sequence->GetPathName();
	const FString Existing = Definition->Control.ConstraintSequence.ToString();

	if (Existing == SequencePath)
	{
		return true;
	}

	if (!Existing.IsEmpty())
	{
		// Allowed on purpose. "Use this sequence instead" is an ordinary thing to want, and refusing
		// it would leave the only way out being to edit the definition by hand. Named, so the sequence
		// left behind is findable rather than merely orphaned.
		UE_LOG(LogMotionForge, Warning,
			TEXT("'%s' was reading '%s' and now reads '%s'. The old sequence still exists and is now "
				 "read by nothing."),
			*Definition->GetName(), *Existing, *SequencePath);
	}

	Definition->Control.ConstraintSequence = Sequence;
	Definition->MarkPackageDirty();

	UE_LOG(LogMotionForge, Log, TEXT("'%s' now reads '%s'."),
		*Definition->GetName(), *SequencePath);

	return true;
}

bool FMotionPromptSequence::PullFromDefinition(
	UMovieSceneMotionPromptTrack* Track, int32 FrameRate, FString& OutError)
{
	if (Track == nullptr)
	{
		OutError = TEXT("No prompt track to pull into.");
		return false;
	}

	UMotionDef* Definition = Track->Definition.LoadSynchronous();
	if (Definition == nullptr)
	{
		OutError = TEXT("This prompt track names no Motion Definition, so there is nothing to pull. "
						"Set one in the track's Details first.");
		return false;
	}

	ULevelSequence* Sequence = Track->GetTypedOuter<ULevelSequence>();
	if (Sequence == nullptr)
	{
		OutError = TEXT("This prompt track is not inside a Level Sequence.");
		return false;
	}

	// The link first, so a pull onto a track somebody assigned by hand also makes the definition read
	// it. Pulling without that would lay out beats the definition never sees.
	if (!LinkDefinition(Track, Definition, OutError))
	{
		return false;
	}

	if (!Populate(Sequence, Definition, FrameRate, OutError))
	{
		return false;
	}

	FString CharacterError;
	if (!EnsureCharacter(Sequence, Definition, CharacterError))
	{
		// The beats are the point and they are complete without a body. Reported, not fatal.
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *CharacterError);
	}

	// And the poses it already has, or they are invisible on the timeline they are supposed to be
	// authored on - which is worse than it sounds, because the moment anything else is keyed on the
	// rig the sequence wins outright and those authored keys stop being sent at all.
	FString ConstraintError;
	PushConstraintsToRig(Sequence, Definition, FrameRate, ConstraintError);

	if (!ConstraintError.IsEmpty())
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *ConstraintError);
	}

	return true;
}

namespace MotionPromptPrivate
{
	/**
	 * Land a range of an animation onto the sequence's Control Rig as keys.
	 *
	 * The one place that knows how a pose becomes control values, shared by both directions: a frame
	 * lifted out of a generated take, and a stored constraint key turned into a one-frame animation.
	 * The engine does the bone-to-control solve, so nothing here knows that `upperarm_r_fk_ctrl` drives
	 * `upperarm_r` - a convention that holds for one rig and mis-poses the next.
	 */
	static bool LoadPoseOntoRig(
		ULevelSequence* Sequence,
		UAnimSequence* Anim,
		FFrameNumber AnimStart,
		FFrameNumber AnimEnd,
		FFrameNumber TargetTick,
		FString& OutError)
	{
		UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
		if (MovieScene == nullptr || Anim == nullptr)
		{
			OutError = TEXT("Nothing to pose the rig from.");
			return false;
		}

		UMovieSceneTrack* RigTrack = nullptr;

		for (const FMovieSceneBinding& Binding : const_cast<const UMovieScene*>(MovieScene)->GetBindings())
		{
			for (UMovieSceneTrack* Candidate : Binding.GetTracks())
			{
				if (Candidate && Candidate->IsA<UMovieSceneControlRigParameterTrack>())
				{
					RigTrack = Candidate;
					break;
				}
			}
		}

		if (RigTrack == nullptr || RigTrack->GetAllSections().Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("'%s' has no Control Rig track to pose. Set a Control Rig on the Motion Character "
					 "and Pull, and one is added with the character."),
				*Sequence->GetName());
			return false;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World == nullptr)
		{
			OutError = TEXT("No editor world to evaluate the sequence in.");
			return false;
		}

		// A player, purely to get the spawnable on screen so there is a component to solve against.
		// Same trick the constraint harvest uses, and for the same reason: this has to work from a
		// tool, with nothing open.
		ALevelSequenceActor* Actor = nullptr;
		ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(
			World, Sequence, FMovieSceneSequencePlaybackSettings(), Actor);

		if (Player == nullptr)
		{
			OutError = FString::Printf(TEXT("Could not create a player for '%s'."), *Sequence->GetName());
			return false;
		}

		ON_SCOPE_EXIT
		{
			Player->Stop();
			if (Actor)
			{
				Actor->Destroy();
			}
		};

		Player->Play();
		Player->Pause();

		USkeletalMeshComponent* Posed = nullptr;

		for (const FMovieSceneBinding& Binding : const_cast<const UMovieScene*>(MovieScene)->GetBindings())
		{
			for (TWeakObjectPtr<> Bound : Player->FindBoundObjects(Binding.GetObjectGuid(), MovieSceneSequenceID::Root))
			{
				if (AActor* BoundActor = Cast<AActor>(Bound.Get()))
				{
					if (USkeletalMeshComponent* Component = BoundActor->FindComponentByClass<USkeletalMeshComponent>())
					{
						Posed = Component;
						break;
					}
				}
			}
		}

		if (Posed == nullptr)
		{
			OutError = FString::Printf(
				TEXT("Nothing in '%s' resolved to a character, so there is nothing to solve the pose "
					 "onto."),
				*Sequence->GetName());
			return false;
		}

		// Key reduction off. Reducing across one pose is either a no-op or the loss of the only thing
		// being asked for.
		const bool bLoaded = UControlRigSequencerEditorLibrary::LoadAnimSequenceIntoControlRigSectionWithRange(
			RigTrack->GetAllSections()[0],
			Anim,
			Posed,
			TargetTick,
			/*bUseCustomAnimRange*/ true,
			AnimStart,
			AnimEnd,
			EMovieSceneTimeUnit::TickResolution,
			/*bKeyReduce*/ false,
			/*Tolerance*/ 0.001f,
			EMovieSceneKeyInterpolation::SmartAuto,
			/*bResetControls*/ false,
			/*bOntoSelectedControls*/ false);

		if (!bLoaded)
		{
			OutError = FString::Printf(
				TEXT("Could not read '%s' onto the rig."), *Anim->GetName());
			return false;
		}

		return true;
	}

	/**
	 * One captured pose as a one-frame animation, so the engine can solve it onto a rig's controls.
	 *
	 * A constraint key stores **component-space** transforms per bone - that is the form every
	 * provider converts from, and the form a pose asset resolves into. An animation wants local ones,
	 * so each bone is divided back through its parent. The reference skeleton lists parents before
	 * children, so a single forward pass has every parent's global already computed.
	 *
	 * Transient and thrown away. It exists for the length of one call, purely as the currency the
	 * Control Rig loader accepts.
	 */
	static UAnimSequence* PoseAsOneFrameAnimation(
		const FMotionPoseSample& Pose, USkeleton& Skeleton)
	{
		const FReferenceSkeleton& Ref = Skeleton.GetReferenceSkeleton();

		TArray<FTransform> Global;
		Global.SetNum(Ref.GetNum());

		TArray<FName> Names;
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;

		for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
		{
			const FName BoneName = Ref.GetBoneName(Bone);
			const FTransform* Sampled = Pose.Bones.Find(BoneName);

			// A pose need not cover every bone - a hand-shape key holds a handful on purpose. Bones it
			// does not carry take the reference pose, which is what Unreal itself does.
			const int32 Parent = Ref.GetParentIndex(Bone);

			if (Sampled)
			{
				Global[Bone] = *Sampled;
			}
			else
			{
				const FTransform& Local = Ref.GetRefBonePose()[Bone];
				Global[Bone] = (Parent == INDEX_NONE) ? Local : Local * Global[Parent];
			}

			const FTransform Local = (Parent == INDEX_NONE)
				? Global[Bone]
				: Global[Bone] * Global[Parent].Inverse();

			Names.Add(BoneName);
			Positions.Add(Local.GetLocation());
			Rotations.Add(Local.GetRotation());
			Scales.Add(Local.GetScale3D());
		}

		UAnimSequence* Anim = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
		Anim->SetSkeleton(&Skeleton);

		IAnimationDataController& Controller = Anim->GetController();
		Controller.OpenBracket(
			NSLOCTEXT("MotionForge", "BuildConstraintPose", "Building a constraint pose"));

		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1));

		// Two frames, not one. A single-frame animation has zero length, and a zero-length range is
		// not something the Control Rig loader can read a pose out of - both keys hold the same pose,
		// so which end it samples does not matter.
		Controller.SetNumberOfFrames(FFrameNumber(1));

		for (int32 Bone = 0; Bone < Names.Num(); ++Bone)
		{
			Controller.AddBoneCurve(Names[Bone]);
			Controller.SetBoneTrackKeys(
				Names[Bone],
				{ Positions[Bone], Positions[Bone] },
				{ Rotations[Bone], Rotations[Bone] },
				{ Scales[Bone], Scales[Bone] });
		}

		Controller.NotifyPopulated();
		Controller.CloseBracket();

		return Anim;
	}
}

int32 FMotionPromptSequence::PushConstraintsToRig(
	ULevelSequence* Sequence,
	const UMotionDef* Definition,
	int32 FrameRate,
	FString& OutError)
{
	using namespace MotionPromptPrivate;

	if (Sequence == nullptr || Definition == nullptr || FrameRate <= 0)
	{
		OutError = TEXT("Nothing to push constraints from.");
		return 0;
	}

	USkeleton* Skeleton = nullptr;
	USkeletalMesh* Mesh = nullptr;
	if (!ResolveCharacterMesh(*Definition, Skeleton, Mesh, OutError))
	{
		return 0;
	}

	int32 Placed = 0;

	for (const FMotionConstraint& Constraint : Definition->Control.Constraints)
	{
		for (const FMotionConstraintKey& Key : Constraint.Keys)
		{
			// Resolved rather than read, so a key driven by a pose asset works exactly like a captured
			// one - the resolver is the single place that decides which of the two wins.
			FMotionPoseSample Pose;
			int32 Coverage = 0;
			FString ResolveError;

			if (!FMotionPoseResolve::Resolve(
					Key, Skeleton->GetReferenceSkeleton(), Constraint.Type, {},
					Pose, Coverage, ResolveError))
			{
				UE_LOG(LogMotionForge, Warning,
					TEXT("Constraint key at frame %d could not be read: %s"), Key.Frame, *ResolveError);
				continue;
			}

			if (!Pose.IsValid())
			{
				// A Root Path key pins where the body goes without saying anything about its shape, so
				// it has no pose to put on a rig. Skipped rather than warned about - it is not a fault.
				continue;
			}

			UAnimSequence* OneFrame = PoseAsOneFrameAnimation(Pose, *Skeleton);

			// Clip frames are the generator's, the timeline's are ticks. Through seconds, as everywhere
			// else, so neither rate can silently move the other.
			const FFrameNumber TargetTick = FFrameRate::TransformTime(
				FFrameTime(FFrameNumber(Key.Frame)),
				FFrameRate(FrameRate, 1),
				Sequence->GetMovieScene()->GetTickResolution()).FrameNumber;

			FString LoadError;
			if (LoadPoseOntoRig(Sequence, OneFrame, FFrameNumber(0), FFrameNumber(0), TargetTick, LoadError))
			{
				++Placed;
			}
			else
			{
				UE_LOG(LogMotionForge, Warning,
					TEXT("Constraint key at frame %d did not reach the rig: %s"), Key.Frame, *LoadError);
			}

			OneFrame->MarkAsGarbage();
		}
	}

	if (Placed > 0)
	{
		// Muted again. Loading keys onto a Control Rig section can activate the track, and a rig left
		// enabled overrides the generated take everywhere between its keys - so the preview row stops
		// showing what came back and starts showing the rig interpolating between two poses.
		//
		// Harmless to the constraints either way: the harvest wakes muted tracks for its own bake.
		for (const FMovieSceneBinding& Binding :
			 const_cast<const UMovieScene*>(Sequence->GetMovieScene())->GetBindings())
		{
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				if (Track && Track->IsA<UMovieSceneControlRigParameterTrack>())
				{
					Track->SetEvalDisabled(true);
				}
			}
		}

		Sequence->MarkPackageDirty();

		UE_LOG(LogMotionForge, Log,
			TEXT("Put %d authored constraint pose(s) from '%s' onto the rig in '%s'. They are rig keys "
				 "now, so edit them there - the sequence is what generates."),
			Placed, *Definition->GetName(), *Sequence->GetName());
	}

	return Placed;
}

bool FMotionPromptSequence::CopyTakePoseToRig(
	ULevelSequence* Sequence,
	FFrameNumber SourceTick,
	FFrameNumber TargetTick,
	FString& OutError)
{
	using namespace MotionPromptPrivate;

	UMovieSceneMotionPromptTrack* Track = FindTrack(Sequence);
	if (Track == nullptr)
	{
		OutError = TEXT("This sequence has no Motion Prompt track.");
		return false;
	}

	const UMotionDef* Definition = Track->Definition.LoadSynchronous();
	if (Definition == nullptr)
	{
		OutError = TEXT("This prompt track names no Motion Definition, so there is no take to copy from.");
		return false;
	}

	UAnimSequence* Take = Definition->ImportedSequence.LoadSynchronous();
	if (Take == nullptr)
	{
		OutError = FString::Printf(
			TEXT("'%s' has no imported animation yet. Generate first - this copies a pose out of the "
				 "take, so there has to be one."),
			*Definition->GetName());
		return false;
	}

	// Which frame of the take. Through seconds, so the sequence's display rate and the clip's own rate
	// stay independent - the take is 30fps because the generator is, not because the timeline is.
	const FFrameRate Tick = Sequence->GetMovieScene()->GetTickResolution();
	const double Seconds = Tick.AsSeconds(FFrameTime(SourceTick));
	const FFrameRate TakeRate = Take->GetSamplingFrameRate();

	const FFrameNumber TakeFrame(
		static_cast<int32>(FMath::RoundToInt(Seconds * TakeRate.AsDecimal())));

	if (!LoadPoseOntoRig(Sequence, Take, TakeFrame, TakeFrame, TargetTick, OutError))
	{
		return false;
	}

	Sequence->MarkPackageDirty();

	UE_LOG(LogMotionForge, Log,
		TEXT("Copied '%s' frame %d onto the rig in '%s' at %.2fs. Drag the keys to where the take "
			 "needs holding; those moments become constraints."),
		*Take->GetName(), TakeFrame.Value, *Sequence->GetName(),
		Tick.AsSeconds(FFrameTime(TargetTick)));

	return true;
}

EMotionPromptSync FMotionPromptSequence::GetSyncState(
	const UMovieSceneMotionPromptTrack* Track, FString& OutDetail)
{
	OutDetail.Reset();

	if (Track == nullptr)
	{
		return EMotionPromptSync::NoDefinition;
	}

	// Get, not LoadSynchronous. This is called from a Slate attribute on every draw, and faulting an
	// asset in from a paint pass is a good way to hitch the editor.
	const UMotionDef* Definition = Track->Definition.Get();

	if (Definition == nullptr)
	{
		if (Track->Definition.IsNull())
		{
			OutDetail = TEXT("This track names no Motion Definition, so its beats generate nothing. "
							 "Set one in Details, then Pull.");
			return EMotionPromptSync::NoDefinition;
		}

		OutDetail = TEXT("The Motion Definition is not loaded yet.");
		return EMotionPromptSync::NoDefinition;
	}

	const ULevelSequence* Sequence = Track->GetTypedOuter<ULevelSequence>();

	if (Sequence == nullptr
		|| Definition->Control.ConstraintSequence.ToString() != Sequence->GetPathName())
	{
		OutDetail = FString::Printf(
			TEXT("'%s' does not read this sequence - it reads '%s'. Two sequences can name the same "
				 "definition and only one can be its Constraint Sequence, so these beats generate "
				 "nothing. Pull, or set the definition again, to claim it."),
			*Definition->GetName(),
			Definition->Control.ConstraintSequence.IsNull()
				? TEXT("nothing")
				: *Definition->Control.ConstraintSequence.ToString());

		return EMotionPromptSync::NotLinkedBack;
	}

	const TArray<UMovieSceneMotionPromptSection*> Beats = Track->GetBeatsInOrder();

	if (Beats.Num() == 0)
	{
		OutDetail = FString::Printf(
			TEXT("No beats on this track, so '%s' has nothing to generate. Pull to lay its prompt out, "
				 "or add beats by hand."),
			*Definition->GetName());
		return EMotionPromptSync::Unreadable;
	}

	// Compared against the definition's own fields rather than re-read through the resolver, because
	// the resolver would answer with the sequence - which is what we are trying to compare *to*.
	bool bSame = (Definition->Control.BeatSeconds.Num() == Beats.Num());

	if (bSame)
	{
		const UMovieScene* MovieScene = Sequence->GetMovieScene();
		const FFrameRate Tick = MovieScene ? MovieScene->GetTickResolution() : FFrameRate(24000, 1);

		for (int32 Index = 0; Index < Beats.Num() && bSame; ++Index)
		{
			const UMovieSceneMotionPromptSection* Beat = Beats[Index];
			if (!Beat->HasStartFrame() || !Beat->HasEndFrame())
			{
				bSame = false;
				break;
			}

			const double Seconds = Tick.AsSeconds(
				FFrameTime(Beat->GetExclusiveEndFrame() - Beat->GetInclusiveStartFrame()));

			// A twentieth of a second, because the two numbers travel through different rounding on
			// the way here and an exact comparison would report every pull as immediately diverged.
			bSame = FMath::Abs(Seconds - Definition->Control.BeatSeconds[Index]) < 0.05;
		}
	}

	if (bSame)
	{
		OutDetail = FString::Printf(
			TEXT("These beats match '%s'. Editing them here is what changes the clip - the sequence "
				 "wins while it is assigned."),
			*Definition->GetName());
		return EMotionPromptSync::Matches;
	}

	OutDetail = FString::Printf(
		TEXT("%d beat(s) here against %d on '%s'. This sequence is what generates - that is the "
			 "normal state, not a fault. Bake to write these back onto the asset, for the day the "
			 "sequence goes away."),
		Beats.Num(), Definition->Control.BeatSeconds.Num(), *Definition->GetName());

	return EMotionPromptSync::Ahead;
}

FString UMotionForgeSubsystem::CreatePromptSequence(
	const FString& DefinitionPath,
	const FString& SequenceAssetPath,
	FString& OutError)
{
	using namespace MotionPromptPrivate;

	OutError.Reset();

	UMotionDef* Def = LoadDef(DefinitionPath);
	if (Def == nullptr)
	{
		OutError = FString::Printf(TEXT("No motion definition at '%s'."), *DefinitionPath);
		return FString();
	}

	// The two refusals, and each says which it is. Everything else about a definition can be filled
	// in later; without a prompt there are no beats to lay out, and without a skeleton the sequence
	// has no character to pose and is half of what it should be from the day it is made.
	if (Def->Prompt.TrimStartAndEnd().IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("'%s' has an empty prompt. Write what the motion should be first - the sequence lays "
				 "that prompt out in time, it does not invent one."),
			*Def->GetName());
		return FString();
	}

	USkeleton* Skeleton = nullptr;
	USkeletalMesh* UnusedMesh = nullptr;
	if (!ResolveCharacterMesh(*Def, Skeleton, UnusedMesh, OutError))
	{
		return FString();
	}

	// The provider is asked for its frame rate and nothing else, so a prompt can be laid out with the
	// runner off, the API key absent and the GPU cold. Authoring must not need the thing it authors
	// for to be running.
	const TSharedPtr<IMotionProvider> Provider = FindProvider(Def->ProviderId);
	const int32 FrameRate = ResolveFrameRate(Provider);

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	FString PackagePath = SequenceAssetPath;
	if (PackagePath.IsEmpty())
	{
		PackagePath = Settings->GetSequencesPath()
			/ ObjectTools::SanitizeObjectName(TEXT("LS_") + Def->GetName());
	}

	const FString AssetName = FPackageName::GetShortName(PackagePath);

	ULevelSequence* Sequence =
		LoadObject<ULevelSequence>(nullptr, *(PackagePath + TEXT(".") + AssetName));

	const bool bIsNew = (Sequence == nullptr);

	if (bIsNew)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		if (Package == nullptr)
		{
			OutError = FString::Printf(TEXT("Could not create package '%s'."), *PackagePath);
			return FString();
		}

		Sequence = NewObject<ULevelSequence>(
			Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);

		if (Sequence == nullptr)
		{
			OutError = FString::Printf(TEXT("Could not create a Level Sequence at '%s'."), *PackagePath);
			return FString();
		}

		Sequence->Initialize();
	}

	if (!FMotionPromptSequence::Populate(Sequence, Def, FrameRate, OutError))
	{
		return FString();
	}

	FString CharacterError;
	if (!FMotionPromptSequence::EnsureCharacter(Sequence, Def, CharacterError))
	{
		// Not a refusal. The beats are the point and they are complete without a character; the
		// sequence is simply not yet usable for constraint poses, and saying so once is better than
		// leaving that discovered at harvest time.
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *CharacterError);
	}

	// The poses the definition already has, onto the rig - because this is the moment they would
	// otherwise vanish. Building a sequence from a definition with constraints used to show none of
	// them, and the first key put on the rig afterwards made the sequence win and stopped the authored
	// ones being sent at all. Both routes push, so neither can be the one that forgets.
	FString ConstraintError;
	FMotionPromptSequence::PushConstraintsToRig(Sequence, Def, FrameRate, ConstraintError);

	if (!ConstraintError.IsEmpty())
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *ConstraintError);
	}

	if (bIsNew)
	{
		FAssetRegistryModule::AssetCreated(Sequence);
	}

	// Link both ways, through the one function that does it, so this route and the one that starts
	// from an open sequence cannot drift apart. This is the moment the sequence becomes the truth:
	// from here the track supplies the prompt and its beats, and the definition's own fields are
	// ignored rather than merged.
	FString LinkError;
	if (!FMotionPromptSequence::LinkDefinition(FMotionPromptSequence::FindTrack(Sequence), Def, LinkError))
	{
		OutError = LinkError;
		return FString();
	}

	// And the take it already has, on the animation row.
	//
	// A definition generated before anyone thought to build a sequence for it still has its clip, and
	// leaving the row empty means either spending a generation to fill it or wiring it up by hand -
	// the second being the kind of manual step that quietly attaches the wrong animation.
	//
	// After the link, and against the sequence in hand rather than the one the definition points at.
	// This used to run above, where the definition did not yet point anywhere, so it found nothing
	// and every newly created sequence opened with an empty animation row.
	PlaceTakeOnSequence(Sequence, Def->ImportedSequence.LoadSynchronous());

	SaveAsset(Sequence);
	SaveAsset(Def);

	return Sequence->GetPathName();
}

FMotionPromptRead UMotionForgeSubsystem::ReadPromptSequence(
	const FString& DefinitionPath, FString& OutError) const
{
	OutError.Reset();

	FMotionPromptRead Result;

	const UMotionDef* Def = LoadDef(DefinitionPath);
	if (Def == nullptr)
	{
		OutError = FString::Printf(TEXT("No motion definition at '%s'."), *DefinitionPath);
		return Result;
	}

	const TSharedPtr<IMotionProvider> Provider = FindProvider(Def->ProviderId);
	FMotionPromptSequence::Resolve(Def, ResolveFrameRate(Provider), Result, OutError);

	return Result;
}

bool UMotionForgeSubsystem::RefreshPromptSequenceTake(const FString& DefinitionPath, FString& OutError)
{
	OutError.Reset();

	UMotionDef* Def = LoadDef(DefinitionPath);
	if (Def == nullptr)
	{
		OutError = FString::Printf(TEXT("No motion definition at '%s'."), *DefinitionPath);
		return false;
	}

	// Neither a missing sequence nor a missing clip is a fault - this is called speculatively every
	// time somebody opens a timeline, and refusing would turn "nothing to do" into an error to read.
	PlaceTakeOnPromptSequence(Def, Def->ImportedSequence.LoadSynchronous());
	return true;
}

bool UMotionForgeSubsystem::BakePromptSequence(const FString& DefinitionPath, FString& OutError)
{
	OutError.Reset();

	UMotionDef* Def = LoadDef(DefinitionPath);
	if (Def == nullptr)
	{
		OutError = FString::Printf(TEXT("No motion definition at '%s'."), *DefinitionPath);
		return false;
	}

	const TSharedPtr<IMotionProvider> Provider = FindProvider(Def->ProviderId);
	const int32 FrameRate = ResolveFrameRate(Provider);

	FMotionPromptRead Read;
	if (!FMotionPromptSequence::Resolve(Def, FrameRate, Read, OutError))
	{
		return false;
	}

	if (!Read.bFromSequence)
	{
		OutError = FString::Printf(
			TEXT("'%s' has no prompt sequence, so there is nothing to bake back. Its own Prompt and "
				 "Beat Seconds are already the truth."),
			*Def->GetName());
		return false;
	}

	Def->Prompt = Read.Prompt;

	Def->Control.BeatSeconds.Reset(Read.Beats.Num());
	for (const FMotionPromptBeat& Beat : Read.Beats)
	{
		Def->Control.BeatSeconds.Add(Beat.Seconds);
	}

	// Length follows, because the beats are the clip. Left disagreeing, it would be the number a
	// reader trusts and the one nothing uses.
	Def->Length = FMath::Max(1, FMath::RoundToInt(Read.TotalSeconds));

	// The poses too, or "bake everything, drop the binding, it survives" is a promise the button does
	// not keep. Beats without their constraints is half a clip's recipe.
	//
	// Harvested rather than copied, because the poses are not stored anywhere as poses - they live in
	// whatever the sequence uses to drive the character, and only evaluating it turns that into bones.
	if (ULevelSequence* Sequence = Def->Control.ConstraintSequence.LoadSynchronous())
	{
		USkeleton* Skeleton = nullptr;
		USkeletalMesh* Mesh = nullptr;
		FString CharacterError;

		if (MotionPromptPrivate::ResolveCharacterMesh(*Def, Skeleton, Mesh, CharacterError))
		{
			TArray<FMotionConstraint> Baked;

			double DurationSeconds = 0.0;
			TArray<FString> Warnings;
			FString HarvestError;

			const EMotionHarvestResult Result = FMotionSequenceConstraints::Harvest(
				Sequence, GEditor ? GEditor->GetEditorWorldContext().World() : nullptr,
				Skeleton, FrameRate, Def->Control.ConstraintSequenceType,
				Baked, DurationSeconds, Warnings, HarvestError);

			if (Result == EMotionHarvestResult::Ok && Baked.Num() > 0)
			{
				int32 Keys = 0;
				for (const FMotionConstraint& Constraint : Baked)
				{
					Keys += Constraint.Keys.Num();
				}

				// Replaced, not appended. A second bake of the same sequence is the same poses, and
				// merging would leave the definition pinned to the moments of two different edits.
				Def->Control.Constraints = MoveTemp(Baked);

				UE_LOG(LogMotionForge, Log,
					TEXT("Baked %d constraint key(s) across %d type(s) from '%s' onto '%s'."),
					Keys, Def->Control.Constraints.Num(), *Sequence->GetName(), *Def->GetName());
			}
			else if (Result == EMotionHarvestResult::Failed)
			{
				// Said rather than swallowed. The beats went back and the poses did not, so a bake
				// that reported nothing would leave the definition looking complete and half-baked.
				UE_LOG(LogMotionForge, Warning,
					TEXT("Beats were baked onto '%s' but its poses were not: %s"),
					*Def->GetName(), *HarvestError);
			}
		}
		else
		{
			UE_LOG(LogMotionForge, Warning,
				TEXT("Beats were baked onto '%s' but its poses were not: %s"),
				*Def->GetName(), *CharacterError);
		}
	}

	Def->MarkPackageDirty();
	SaveAsset(Def);

	UE_LOG(LogMotionForge, Log,
		TEXT("Baked %d beat(s) totalling %.2fs from '%s' onto '%s'. The sequence still wins while it "
			 "is set; this is what survives deleting it."),
		Read.Beats.Num(), Read.TotalSeconds, *Read.SequencePath, *Def->GetName());

	return true;
}

void UMotionForgeSubsystem::PlaceTakeOnPromptSequence(UMotionDef* Def, UAnimSequence* Clip)
{
	if (Def == nullptr)
	{
		return;
	}

	PlaceTakeOnSequence(Def->Control.ConstraintSequence.LoadSynchronous(), Clip);
}

void UMotionForgeSubsystem::PlaceTakeOnSequence(ULevelSequence* Sequence, UAnimSequence* Clip)
{
	if (Sequence == nullptr || Clip == nullptr)
	{
		return;
	}

	if (FMotionPromptSequence::FindTrack(Sequence) == nullptr)
	{
		return;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene == nullptr)
	{
		return;
	}

	const TArray<FMovieSceneBinding>& Bindings = const_cast<const UMovieScene*>(MovieScene)->GetBindings();
	if (Bindings.Num() == 0)
	{
		return;
	}

	const FGuid Binding = Bindings[0].GetObjectGuid();

	// Replaced, not appended. This row is the answer to "what came back", and a stack of every take
	// ever imported would stop being that on the second generation.
	if (UMovieSceneSkeletalAnimationTrack* Existing =
			MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(Binding))
	{
		MovieScene->RemoveTrack(*Existing);
	}

	UMovieSceneSkeletalAnimationTrack* Track =
		MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Binding);

	if (Track == nullptr)
	{
		return;
	}

	Track->AddNewAnimation(FFrameNumber(0), Clip);

	SaveAsset(Sequence);

	UE_LOG(LogMotionForge, Log, TEXT("Put '%s' on '%s', under the beats that asked for it."),
		*Clip->GetName(), *Sequence->GetName());
}
